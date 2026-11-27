/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "RawLightNotifier"

#include "RawLightNotifier.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <poll.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "SensorNotifierUtils.h"
#include "SscCalApi.h"

static const std::string kPanelPowerState =
        "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/panel_power_state";

// SDE_MODE_DPMS_ON
static const int kPanelPowerOn = 0;

// sensor: xiaomi.sensor.ambientlight.raw
static const uint32_t kSensorTypeAmbientlightRaw = 33171111;

// Report lux and cct values
static const uint32_t kNotifyTypeReportValue = 201;

using android::hardware::Return;
using android::hardware::Void;
using android::hardware::sensors::V1_0::Event;

namespace {

class RawLightSensorCallback : public IEventQueueCallback {
 public:
    RawLightSensorCallback(
            std::shared_ptr<UDalsBleedCompensator> compensator,
            std::weak_ptr<CwbClient> cwbClient)
        : mCompensator(std::move(compensator)),
          mCwbClient(std::move(cwbClient)) {}

    Return<void> onEvent(const Event& e) override {
        const float rawLux = e.u.vec4.y;
        float compensatedLux = rawLux;

        if (auto cwb = mCwbClient.lock()) {
            cwb->onRawLuxSample(rawLux);
        }

        if (mCompensator != nullptr &&
            !mCompensator->compensate(rawLux, &compensatedLux)) {
            return Void();
        }

        _oem_msg msg{};
        msg.notifyType = kNotifyTypeReportValue;
        msg.value = compensatedLux;
        msg.notifyTypeFloat = msg.notifyType;
        msg.unknown1 = 2;
        msg.unknown2 = 5;
        msg.sensorType = kSensorTypeAmbientlightRaw;

        SscCalApiWrapper::getInstance().processMsg(&msg);

        return Void();
    }

 private:
    std::shared_ptr<UDalsBleedCompensator> mCompensator;
    std::weak_ptr<CwbClient> mCwbClient;
};

bool readPanelSysfsUint(const char* attr, uint32_t* out) {
    std::string content;
    std::string path =
            std::string("/sys/class/backlight/panel0-backlight/") + attr;

    if (android::base::ReadFileToString(path, &content)) {
        uint32_t value = 0;
        if (android::base::ParseUint(android::base::Trim(content), &value)) {
            *out = value;
            return true;
        }
    }

    return false;
}

std::optional<uint32_t> readCurrentBrightness() {
    uint32_t brightness = 0;

    if (readPanelSysfsUint("brightness", &brightness)) {
        return brightness;
    }

    return std::nullopt;
}

}  // namespace

void RawLightNotifier::resolveBacklightScale() {
    if (mDeviceNormalMaxBacklight) {
        return;
    }

    uint32_t sysfsMax = 0;
    if (readPanelSysfsUint("max_brightness", &sysfsMax) && sysfsMax > 0) {
        uint64_t tableMax = 
                (mCompensator != nullptr) ? mCompensator->getNormalMaxBacklight() : 2047u;

        mDeviceNormalMaxBacklight =
                (sysfsMax > tableMax + tableMax / 2) ? sysfsMax / 2 : sysfsMax;
    }
}

uint32_t RawLightNotifier::normalizeBacklight(uint32_t raw) const {
    uint32_t targetMax = (mCompensator != nullptr) ? mCompensator->getNormalMaxBacklight() : 2047u;

    if (!mDeviceNormalMaxBacklight || *mDeviceNormalMaxBacklight == 0 ||
        *mDeviceNormalMaxBacklight == targetMax) {
        return raw;
    }

    uint32_t deviceMax = *mDeviceNormalMaxBacklight;

    return static_cast<uint32_t>((static_cast<uint64_t>(raw) * targetMax + (deviceMax / 2)) /
                                 deviceMax);
}

void RawLightNotifier::updateBacklight(uint32_t rawBacklight) {
    if (mCompensator == nullptr) {
        return;
    }

    uint32_t normalized = normalizeBacklight(rawBacklight);
    uint32_t previous = mCompensator->getBacklight();

    mCompensator->setBacklight(normalized);

    if (previous == 0 && normalized > 0 && mCwbClient != nullptr) {
        mCwbClient->notifyBacklightChanged();
    }
}

RawLightNotifier::RawLightNotifier(sp<ISensorManager> manager) : SensorNotifier(manager) {
    auto compensator = std::make_shared<UDalsBleedCompensator>();
    UdalsGeometry geo;

    static constexpr const char* kConfigPaths[] = {
        "/odm/etc/sensors/config/lightSensorConfig.json",
        "/vendor/etc/sensors/config/lightSensorConfig.json",
    };

    bool configLoaded = false;

    for (const char* path : kConfigPaths) {
        if (compensator->loadConfig(path, &geo)) {
            LOG(INFO) << "Loaded UDALS configuration from " << path;
            configLoaded = true;
            break;
        }
    }

    if (configLoaded) {
        mCompensator = compensator;
        resolveBacklightScale();
        mCwbClient = std::make_shared<CwbClient>(geo, mCompensator);
    } else {
        LOG(INFO) << "UDALS configuration unavailable";
    }

    initializeSensorQueue(
            "xiaomi.sensor.ambientlight.factory",
            false,
            new RawLightSensorCallback(mCompensator, mCwbClient));
}

RawLightNotifier::~RawLightNotifier() {
    deactivate();

    if (mCwbClient != nullptr) {
        mCwbClient->stop();
    }
}

void RawLightNotifier::notify() {
    Result res;
    int previousPanelState = -1;

    if (mQueue == nullptr) {
        LOG(ERROR) << "failed to initialize sensor queue";
        mActive = false;
        return;
    }

    android::base::unique_fd fd(open(kPanelPowerState.c_str(), O_RDONLY));
    if (fd.get() == -1) {
        PLOG(ERROR) << "failed to open " << kPanelPowerState;
        mActive = false;
        return;
    }

    struct pollfd powerStatePoll = {
            .fd = fd.get(),
            .events = POLLERR | POLLPRI,
            .revents = POLLPRI,
    };

    while (mActive) {
        int value;
        if (readInt(fd.get(), &value)) {
            if (value != previousPanelState) {
                if (value == kPanelPowerOn) {
                    if (mCompensator != nullptr) {
                        mCompensator->reset();
                    }

                    if (auto brightness = readCurrentBrightness()) {
                        updateBacklight(*brightness);
                    }

                    if (mCwbClient != nullptr) {
                        mCwbClient->start();
                    }
                } else {
                    if (mCwbClient != nullptr) {
                        mCwbClient->stop();
                    }

                    if (mCompensator != nullptr) {
                        mCompensator->reset();
                    }
                }

                previousPanelState = value;
            }

            if (value == kPanelPowerOn) {
                res = mQueue->enableSensor(mSensorHandle, 20000 /* sample period */,
                                           0 /* latency */);
                if (res != Result::OK) {
                    LOG(ERROR) << "failed to enable sensor";
                }
            } else {
                res = mQueue->disableSensor(mSensorHandle);
                if (res != Result::OK) {
                    LOG(ERROR) << "failed to disable sensor";
                }
            }
        }

        if (previousPanelState == kPanelPowerOn && mCompensator != nullptr) {
            if (auto brightness = readCurrentBrightness()) {
                updateBacklight(*brightness);
            }
        }

        if (poll(&powerStatePoll, 1, 500) < 0) {
            PLOG(ERROR) << "failed to poll " << kPanelPowerState;
        }
    }
}

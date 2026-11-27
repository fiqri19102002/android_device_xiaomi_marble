/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UDalsBleedCompensator"

#include "UDalsBleedCompensator.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace {

constexpr auto kWakeGateTimeout = std::chrono::milliseconds(250);
constexpr auto kTransitionHoldTimeout = std::chrono::milliseconds(250);
constexpr auto kBacklightRampDownHold = std::chrono::milliseconds(150);
constexpr size_t kMaxBacklightHistoryEntries = 64;
constexpr float kTransitionBleedCeilingFactor = 1.5f;
constexpr float kTransitionBleedCeilingMarginLux = 5.0f;
constexpr float kLowGammaKneeFraction = 0.025f;
constexpr size_t kResidualMedianWindow = 5;
constexpr float kLowLuxFilterCeiling = 5.0f;
constexpr float kLowLuxEmaCeiling = 3.0f;
constexpr float kEmaStepBypassDelta = 1.5f;
constexpr float kLowLuxEmaTauSeconds = 0.35f;

float bleedCeilingLux(float bleed) {
    return bleed * kTransitionBleedCeilingFactor + kTransitionBleedCeilingMarginLux;
}

template <size_t N>
bool parseCsvInts(const std::string& str, std::array<int, N>* out) {
    auto parts = android::base::Split(str, ",");
    if (parts.size() != N) {
        return false;
    }
    for (size_t i = 0; i < N; ++i) {
        if (!android::base::ParseInt(android::base::Trim(parts[i]), &(*out)[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

UDalsBleedCompensator::UDalsBleedCompensator() {
    std::string bleedProp =
            android::base::GetProperty("ro.vendor.sensors.notifier.udals.max_bleed_rgb", "");
    if (bleedProp.empty()) {
        return;
    }
    auto parts = android::base::Split(bleedProp, ",");
    if (parts.size() != 3) {
        return;
    }
    std::array<float, 3> rgb{};
    for (size_t c = 0; c < 3; ++c) {
        if (!android::base::ParseFloat(android::base::Trim(parts[c]), &rgb[c]) ||
            !std::isfinite(rgb[c]) || rgb[c] <= 0.0f) {
            return;
        }
    }
    mPeakBleedLux = rgb;
    LOG(INFO) << "Configured custom UDALS max bleed RGB: " << rgb[0] << ", " << rgb[1] << ", "
              << rgb[2];
}

bool UDalsBleedCompensator::loadConfig(const std::string& path, UdalsGeometry* outGeo) {
    if (outGeo == nullptr) {
        return false;
    }
    if (!mPeakBleedLux) {
        LOG(INFO) << "ro.vendor.sensors.notifier.udals.max_bleed_rgb not configured; "
                  << "skipping CWB compensation";
        return false;
    }

    std::string content;
    if (!android::base::ReadFileToString(path, &content)) {
        LOG(INFO) << "No light sensor config found at " << path;
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &errs)) {
        LOG(ERROR) << "Failed to parse " << path << ": " << errs;
        return false;
    }

    if (!root.isObject() || !root["Als"].isObject() || !root["Als"]["cwbInfo"].isObject()) {
        LOG(INFO) << "Config does not contain Als.cwbInfo; skipping CWB compensation";
        return false;
    }

    const auto& als = root["Als"];
    const auto& cwb = als["cwbInfo"];
    if (!cwb["Center"].isString() || !cwb["Radius"].isString()) {
        return false;
    }

    std::array<int, 2> center{};
    std::array<int, 3> radii{};
    if (!parseCsvInts(cwb["Center"].asString(), &center) ||
        !parseCsvInts(cwb["Radius"].asString(), &radii) || center[0] <= 0 || center[1] <= 0 ||
        center[0] > 4096 || center[1] > 4096 || radii[0] <= 0 || radii[1] <= radii[0] ||
        radii[2] <= radii[1] || radii[2] > 512) {
        LOG(ERROR) << "Failed to parse numeric geometry from " << path;
        return false;
    }

    UdalsGeometry geo{};
    geo.cx = center[0];
    geo.cy = center[1];
    geo.r1 = radii[0];
    geo.r2 = radii[1];
    geo.r3 = radii[2];

    const int halfSpan = (geo.r3 + 3) & ~3;
    geo.left = std::max(0, geo.cx - halfSpan);
    geo.top = std::max(0, geo.cy - halfSpan);
    geo.right = geo.cx + halfSpan;
    geo.bottom = geo.cy + halfSpan;

    // Align ROI dimensions to multiples of 8 so width * height * 4 (RGBA_8888)
    // stays divisible by 256 bytes for SDM even when left or top clamp to 0.
    geo.right = geo.left + ((geo.right - geo.left + 7) & ~7);
    geo.bottom = geo.top + ((geo.bottom - geo.top + 7) & ~7);

    auto parseCurve = [](const Json::Value& arr, size_t colIndex, std::vector<CaliPoint>* out) {
        out->clear();
        if (!arr.isArray()) {
            return false;
        }
        for (const auto& item : arr) {
            if (!item.isString()) {
                continue;
            }
            auto cols = android::base::Split(item.asString(), ",");
            float b = 0.0f;
            float clear = 0.0f;
            if (cols.size() > colIndex &&
                android::base::ParseFloat(android::base::Trim(cols[0]), &b) &&
                android::base::ParseFloat(android::base::Trim(cols[colIndex]), &clear) &&
                std::isfinite(b) && std::isfinite(clear) && b >= 0.0f && clear >= 0.0f &&
                (out->empty() || b > out->back().brightness)) {
                out->push_back({b, clear});
            }
        }
        return !out->empty() && out->back().bleedLux > 0.0f;
    };

    const auto peakBleed = *mPeakBleedLux;
    auto loadCurves = [&parseCurve, &peakBleed](const Json::Value& obj, CurveSet* out) {
        static constexpr std::array<const char*, 3> kKeys = {"data_r", "data_g", "data_b"};
        for (size_t c = 0; c < 3; ++c) {
            auto& curve = (*out)[c];
            if (!parseCurve(obj[kKeys[c]], 1, &curve) &&
                !parseCurve(obj["data_w"], c + 2, &curve) &&
                !parseCurve(obj["data_w"], 1, &curve)) {
                return false;
            }
            const float maxClear = curve.back().bleedLux;
            for (auto& pt : curve) {
                pt.bleedLux = (pt.bleedLux / maxClear) * peakBleed[c];
            }
        }
        return true;
    };

    if (!als["panel_Info_cali_ori"].isObject()) {
        LOG(ERROR) << "Missing panel_Info_cali_ori object";
        return false;
    }

    const auto& norm = als["panel_Info_cali_ori"];
    CurveSet normalCurves{};
    if (!loadCurves(norm, &normalCurves)) {
        LOG(ERROR) << "Failed to parse panel_Info_cali_ori tables";
        return false;
    }

    const float maxBkl = std::max({
            normalCurves[0].back().brightness,
            normalCurves[1].back().brightness,
            normalCurves[2].back().brightness,
    });
    if (!std::isfinite(maxBkl) || maxBkl < 1.0f || maxBkl > 65535.0f) {
        LOG(ERROR) << "Invalid normal max backlight in " << path << ": " << maxBkl;
        return false;
    }
    const uint32_t normalMaxBkl = static_cast<uint32_t>(std::lround(maxBkl));

    {
        std::lock_guard<std::mutex> lock(mLock);
        mNormalCurves = std::move(normalCurves);
        mNormalMaxBacklight = normalMaxBkl;
    }
    *outGeo = geo;

    LOG(INFO) << "Loaded UDALS calibration: ROI=[" << geo.left << "," << geo.top << "," << geo.right
              << "," << geo.bottom << "] radii=[" << geo.r1 << "," << geo.r2 << "," << geo.r3
              << "] maxBkl=" << normalMaxBkl;

    return true;
}

void UDalsBleedCompensator::reset() {
    std::lock_guard<std::mutex> lock(mLock);
    mBacklight = 0;
    mSample.reset();
    mHold = {HoldMode::kWake, 0, false, 0.0f, SteadyClock::now()};
    mLastSettledCompLux.reset();
    ++mSampleSeq;
    mBacklightHistory.clear();
    resetResidualFilterLocked();
}

void UDalsBleedCompensator::clearSample() {
    std::lock_guard<std::mutex> lock(mLock);
    mSample.reset();
    if (mHold.mode != HoldMode::kWake) {
        mHold = {};
    }
    ++mSampleSeq;
    resetResidualFilterLocked();
}

void UDalsBleedCompensator::pruneBacklightHistoryLocked(TimePoint now) {
    while (!mBacklightHistory.empty() &&
           (now - mBacklightHistory.front().endTime) >= kBacklightRampDownHold) {
        mBacklightHistory.pop_front();
    }
}

uint32_t UDalsBleedCompensator::peakBacklightLocked(TimePoint now) const {
    if (mBacklight == 0) {
        return 0;
    }
    for (const auto& entry : mBacklightHistory) {
        if ((now - entry.endTime) < kBacklightRampDownHold) {
            return std::max(mBacklight, entry.brightness);
        }
    }
    return mBacklight;
}

uint32_t UDalsBleedCompensator::effectiveBacklightLocked(TimePoint now) {
    if (mBacklight == 0) {
        return 0;
    }
    pruneBacklightHistoryLocked(now);
    return peakBacklightLocked(now);
}

void UDalsBleedCompensator::setBacklight(uint32_t brightness) {
    std::lock_guard<std::mutex> lock(mLock);
    auto now = SteadyClock::now();
    pruneBacklightHistoryLocked(now);
    if (brightness < mBacklight && mBacklight > 0) {
        // Record when the previous higher backlight level ended so dimming ramps hold
        // that level across the ~100-150 ms hardware ALS photon integration window.
        while (!mBacklightHistory.empty() && mBacklightHistory.back().brightness <= mBacklight) {
            mBacklightHistory.pop_back();
        }
        mBacklightHistory.push_back({now, mBacklight});
        while (mBacklightHistory.size() > kMaxBacklightHistoryEntries) {
            mBacklightHistory.pop_front();
        }
    }
    mBacklight = brightness;
}

uint64_t UDalsBleedCompensator::getSampleSeq() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mSampleSeq;
}

uint32_t UDalsBleedCompensator::getBacklight() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mBacklight;
}

uint32_t UDalsBleedCompensator::getNormalMaxBacklight() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mNormalMaxBacklight;
}

void UDalsBleedCompensator::setSampledRgb(float rHigh, float gHigh, float bHigh, float rLow,
                                          float gLow, float bLow) {
    std::lock_guard<std::mutex> lock(mLock);
    mSample = SampledRgb{
            {std::clamp(rHigh, 0.0f, 1.0f), std::clamp(gHigh, 0.0f, 1.0f),
             std::clamp(bHigh, 0.0f, 1.0f)},
            {std::clamp(rLow, 0.0f, 1.0f), std::clamp(gLow, 0.0f, 1.0f),
             std::clamp(bLow, 0.0f, 1.0f)},
    };
    mHold = {};
    ++mSampleSeq;
}

bool UDalsBleedCompensator::isTransitionBleedPlausibleLocked(int8_t dir, uint32_t effBkl,
                                                             float rawLux,
                                                             float anchorRawLux) const {
    if (dir > 0) {
        return rawLux <= bleedCeilingLux(calculateMaxWhiteBleedLocked(effBkl));
    }
    if (dir < 0) {
        float currentBleed = calculateBleedLuxLocked(effBkl);
        if (currentBleed <= 0.0f) {
            return false;
        }
        return rawLux < currentBleed || (anchorRawLux - rawLux) <= bleedCeilingLux(currentBleed);
    }
    return false;
}

void UDalsBleedCompensator::updateTransitionHold(int8_t pendingDir, float rawLux,
                                                 float anchorRawLux, uint64_t expectedSeq) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHold.mode == HoldMode::kWake) {
        return;
    }
    if (pendingDir == 0) {
        mHold = {};
        return;
    }

    if (mSampleSeq != expectedSeq || !mSample || mBacklight == 0 || !mLastSettledCompLux) {
        return;
    }

    const int8_t normalizedDir = (pendingDir > 0) ? 1 : -1;
    if (mHold.mode == HoldMode::kTransition && mHold.dir != normalizedDir) {
        mHold = {};
    }

    auto now = SteadyClock::now();
    uint32_t effBkl = effectiveBacklightLocked(now);
    float effAnchor = (mHold.mode == HoldMode::kTransition) ? mHold.anchorRawLux : anchorRawLux;
    if (!isTransitionBleedPlausibleLocked(normalizedDir, effBkl, rawLux, effAnchor)) {
        mHold = {};
        return;
    }

    if (mHold.mode == HoldMode::kNone && !mHold.timedOut) {
        mHold = {HoldMode::kTransition, normalizedDir, false, anchorRawLux, now};
    }
}

void UDalsBleedCompensator::clearTransitionHold() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHold.mode != HoldMode::kWake) {
        mHold = {};
    }
}

bool UDalsBleedCompensator::isUnambiguousAmbient(float rawLux) const {
    if (!std::isfinite(rawLux) || rawLux <= 0.0f) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mLock);
    uint32_t peak = peakBacklightLocked(SteadyClock::now());
    return rawLux > bleedCeilingLux(calculateMaxWhiteBleedLocked(peak));
}

float UDalsBleedCompensator::interpolateTable(const std::vector<CaliPoint>& table,
                                              float brightness) {
    if (table.empty() || brightness <= 0.0f) {
        return 0.0f;
    }
    if (brightness <= table.front().brightness) {
        if (table.front().brightness > 1e-4f) {
            return (brightness / table.front().brightness) * table.front().bleedLux;
        }
        return table.front().bleedLux;
    }
    if (brightness >= table.back().brightness) {
        return table.back().bleedLux;
    }

    auto it = std::lower_bound(table.begin(), table.end(), brightness,
                               [](const CaliPoint& pt, float val) { return pt.brightness < val; });
    if (it == table.begin() || it == table.end()) {
        return 0.0f;
    }
    const auto& p0 = *(it - 1);
    const auto& p1 = *it;
    const float delta = p1.brightness - p0.brightness;
    const float t = (delta > 1e-4f) ? (brightness - p0.brightness) / delta : 0.0f;
    return std::lerp(p0.bleedLux, p1.bleedLux, t);
}

float UDalsBleedCompensator::calculateBleedLuxForRgbLocked(uint32_t backlight,
                                                           const std::array<float, 3>& rgb) const {
    if (backlight == 0 || !mPeakBleedLux ||
        (rgb[0] <= 0.001f && rgb[1] <= 0.001f && rgb[2] <= 0.001f)) {
        return 0.0f;
    }

    if (mNormalCurves[0].empty() || mNormalCurves[1].empty() || mNormalCurves[2].empty()) {
        return 0.0f;
    }

    const bool isHbm = (mNormalMaxBacklight > 0 && backlight > mNormalMaxBacklight);
    const float hbmScale = isHbm ? std::min(static_cast<float>(backlight) /
                                                    static_cast<float>(mNormalMaxBacklight),
                                            3.0f)
                                 : 0.0f;
    const float bklFloat = static_cast<float>(backlight);

    float totalBleed = 0.0f;
    for (size_t c = 0; c < 3; ++c) {
        const float channelBleed = isHbm ? hbmScale * (*mPeakBleedLux)[c]
                                         : interpolateTable(mNormalCurves[c], bklFloat);
        totalBleed += rgb[c] * channelBleed;
    }
    return std::max(0.0f, totalBleed);
}

float UDalsBleedCompensator::calculateBleedLuxLocked(uint32_t effectiveBacklight) const {
    if (effectiveBacklight == 0 || !mSample) {
        return 0.0f;
    }

    // Blend high-BKL (gamma 2.0) and low-BKL (gamma 1.6) linear bases to model low-DBV shadow lift.
    float knee = (mNormalMaxBacklight > 0)
                         ? std::max(1.0f,
                                    kLowGammaKneeFraction * static_cast<float>(mNormalMaxBacklight))
                         : 51.0f;
    float r = static_cast<float>(effectiveBacklight) / knee;
    float lowBlend = 1.0f / (1.0f + r * std::sqrt(r));
    std::array<float, 3> effRgb{};
    for (size_t c = 0; c < 3; ++c) {
        effRgb[c] = std::lerp(mSample->high[c], mSample->low[c], lowBlend);
    }

    return calculateBleedLuxForRgbLocked(effectiveBacklight, effRgb);
}

float UDalsBleedCompensator::calculateMaxWhiteBleedLocked(uint32_t effectiveBacklight) const {
    return calculateBleedLuxForRgbLocked(effectiveBacklight, {1.0f, 1.0f, 1.0f});
}

void UDalsBleedCompensator::resetResidualFilterLocked() {
    mResidualWindow.clear();
    mResidualEma.reset();
    mLastResidualSampleTime.reset();
}

float UDalsBleedCompensator::filterSignedResidualLocked(float signedRes, float bleed,
                                                        TimePoint now) {
    bool passThrough = false;
    if (bleed <= 0.0f) {
        if (mResidualEma && std::fabs(signedRes - *mResidualEma) > kEmaStepBypassDelta) {
            mResidualWindow.clear();
        }
        passThrough = true;
    } else if (mResidualEma && *mResidualEma >= kLowLuxFilterCeiling) {
        // Clamp stale bright-room entries whenever ambient drops sharply out of the bright regime.
        if (signedRes < *mResidualEma - kEmaStepBypassDelta) {
            for (float& val : mResidualWindow) {
                val = std::min(val, signedRes);
            }
        }
        // Pass bright ambient (>= 5 lux) through directly while keeping the median window current.
        passThrough = (signedRes >= kLowLuxFilterCeiling);
    }

    // Filter signed (raw - bleed) before clamping at 0 so symmetric PWM ripple cancels out.
    mResidualWindow.push_back(signedRes);
    while (mResidualWindow.size() > kResidualMedianWindow) {
        mResidualWindow.pop_front();
    }

    if (passThrough) {
        mResidualEma = signedRes;
        mLastResidualSampleTime = now;
        return std::max(0.0f, signedRes);
    }

    std::array<float, kResidualMedianWindow> sorted{};
    size_t n = mResidualWindow.size();
    for (size_t i = 0; i < n; ++i) {
        sorted[i] = mResidualWindow[i];
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    float med = sorted[(n - 1) / 2];

    // Check EMA step-bypass on the median output so brief spikes stay rejected.
    if (!mResidualEma || !mLastResidualSampleTime ||
        std::fabs(med - *mResidualEma) > kEmaStepBypassDelta || med >= kLowLuxEmaCeiling) {
        mResidualEma = med;
    } else {
        float dtSec = std::chrono::duration<float>(now - *mLastResidualSampleTime).count();
        dtSec = std::clamp(dtSec, 0.005f, 2.0f);
        float alpha = 1.0f - std::exp(-dtSec / kLowLuxEmaTauSeconds);
        mResidualEma = alpha * med + (1.0f - alpha) * *mResidualEma;
    }
    mLastResidualSampleTime = now;

    return std::max(0.0f, *mResidualEma);
}

bool UDalsBleedCompensator::compensate(float rawLux, float* outLux) {
    if (outLux == nullptr || !std::isfinite(rawLux) || rawLux < 0.0f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mLock);
    auto now = SteadyClock::now();

    // Drop ALS events on wake until the first CWB capture completes or times out.
    if (mHold.mode == HoldMode::kWake) {
        if ((now - mHold.startTime) < kWakeGateTimeout) {
            return false;
        }
        mHold = {};
    }

    uint32_t effBkl = effectiveBacklightLocked(now);

    // Hold the last settled lux while a bleed-plausible CWB capture is pending.
    if (mHold.mode == HoldMode::kTransition) {
        auto elapsed = now - mHold.startTime;
        if (mSample && effBkl > 0 && mLastSettledCompLux &&
            isTransitionBleedPlausibleLocked(mHold.dir, effBkl, rawLux, mHold.anchorRawLux) &&
            elapsed < kTransitionHoldTimeout) {
            *outLux = std::min(*mLastSettledCompLux, rawLux);
            return true;
        }
        const bool timedOut = (elapsed >= kTransitionHoldTimeout);
        mHold = {HoldMode::kNone, 0, timedOut, 0.0f, {}};
    }

    float bleed = calculateBleedLuxLocked(effBkl);
    float filtered = std::min(filterSignedResidualLocked(rawLux - bleed, bleed, now), rawLux);
    mLastSettledCompLux = filtered;
    *outLux = filtered;
    return true;
}

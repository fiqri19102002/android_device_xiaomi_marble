/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UDalsCwbClient"

#include "CwbClient.h"

#include <aidl/vendor/qti/hardware/display/config/BnDisplayConfigCallback.h>
#include <aidl/vendor/qti/hardware/display/config/IDisplayConfig.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware_buffer.h>
#include <pthread.h>
#include <vndk/hardware_buffer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using aidl::android::hardware::common::NativeHandle;
using aidl::vendor::qti::hardware::display::config::Attributes;
using aidl::vendor::qti::hardware::display::config::BnDisplayConfigCallback;
using aidl::vendor::qti::hardware::display::config::CameraSmoothOp;
using aidl::vendor::qti::hardware::display::config::Concurrency;
using aidl::vendor::qti::hardware::display::config::DisplayType;
using aidl::vendor::qti::hardware::display::config::IDisplayConfig;
using aidl::vendor::qti::hardware::display::config::Rect;
using aidl::vendor::qti::hardware::display::config::TUIEventType;

namespace {

constexpr int32_t kPrimaryDisplayId = static_cast<int32_t>(DisplayType::PRIMARY);
constexpr float kMaxBleedRelevantLux = 15000.0f;
constexpr float kMinTriggerDeltaLux = 1.5f;
constexpr float kSustainedMinTriggerDeltaLux = 0.40f;
constexpr uint32_t kSustainedLowLuxSamples = 2u;
constexpr auto kSustainedMinDuration = std::chrono::milliseconds(80);
constexpr float kMaxTriggerDeltaLux = 250.0f;
constexpr float kTriggerRelativeDelta = 0.08f;
constexpr auto kMinDispatchInterval = std::chrono::milliseconds(100);
constexpr auto kRetryBackoffInterval = std::chrono::milliseconds(250);
constexpr auto kInitialWaitTimeout = std::chrono::milliseconds(40);
constexpr auto kPokeWaitTimeout = std::chrono::milliseconds(100);
constexpr auto kDrainTimeout = std::chrono::milliseconds(150);
constexpr auto kLockoutProbeBaseInterval = std::chrono::seconds(2);
constexpr uint32_t kMaxLockoutProbeShift = 2u;
constexpr uint32_t kLockoutThreshold = 3u;

constexpr float kGammaHigh = 2.0f;
constexpr float kGammaLow = 1.6f;

std::array<float, 256> initGammaLut(float gamma) {
    std::array<float, 256> lut{};
    for (size_t i = 0; i < 256; ++i) {
        lut[i] = std::pow(static_cast<float>(i) / 255.0f, gamma);
    }
    return lut;
}

struct ScopedHardwareBufferLock {
    ScopedHardwareBufferLock(AHardwareBuffer* buffer, uint64_t usage) : mBuffer(buffer) {
        if (mBuffer != nullptr) {
            void* addr = nullptr;
            if (AHardwareBuffer_lock(mBuffer, usage, -1, nullptr, &addr) == 0) {
                if (addr != nullptr) {
                    mAddr = addr;
                } else {
                    LOG(ERROR) << "AHardwareBuffer_lock succeeded with null address";
                    AHardwareBuffer_unlock(mBuffer, nullptr);
                }
            }
        }
    }

    ~ScopedHardwareBufferLock() {
        if (mAddr != nullptr && mBuffer != nullptr) {
            AHardwareBuffer_unlock(mBuffer, nullptr);
        }
    }

    ScopedHardwareBufferLock(const ScopedHardwareBufferLock&) = delete;
    ScopedHardwareBufferLock& operator=(const ScopedHardwareBufferLock&) = delete;

    void* address() const { return mAddr; }

  private:
    AHardwareBuffer* mBuffer = nullptr;
    void* mAddr = nullptr;
};

}  // namespace

class CwbCallbackHandler : public BnDisplayConfigCallback {
  public:
    ndk::ScopedAStatus notifyCWBBufferDone(int32_t error,
                                           const NativeHandle& /* buffer */) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mPending) {
            return ndk::ScopedAStatus::ok();
        }
        mError = error;
        mDone = true;
        mPending = false;
        mCv.notify_all();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyQsyncChange(bool, int32_t, int32_t) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyIdleStatus(bool) override { return ndk::ScopedAStatus::ok(); }

    ndk::ScopedAStatus notifyCameraSmoothInfo(CameraSmoothOp, int32_t) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyResolutionChange(int32_t, const Attributes&) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyFpsMitigation(int32_t, const Attributes&, Concurrency) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyTUIEventDone(int32_t, DisplayType, TUIEventType) override {
        return ndk::ScopedAStatus::ok();
    }

    bool prepareForCapture(uint64_t gen) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mAbortedGen >= gen) {
            return false;
        }
        mPostedGen = gen;
        mDone = false;
        mError = 0;
        mPending = true;
        return true;
    }

    void cancelPending() {
        std::lock_guard<std::mutex> lock(mMutex);
        mPending = false;
    }

    enum class Status : uint8_t {
        kIdle,
        kReady,
        kPendingCurrent,
        kPendingStale,
    };

    void clearDone() {
        std::lock_guard<std::mutex> lock(mMutex);
        mDone = false;
    }

    bool isPending() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mPending;
    }

    Status pollStatus(uint64_t currentGen, int32_t* outError) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mDone) {
            mDone = false;
            if (mPostedGen == currentGen) {
                *outError = mError;
                return Status::kReady;
            }
        }
        if (mPending) {
            return (mPostedGen == currentGen) ? Status::kPendingCurrent : Status::kPendingStale;
        }
        return Status::kIdle;
    }

    bool waitForDone(std::chrono::milliseconds timeout, uint64_t currentGen, int32_t* outError) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mCv.wait_for(lock, timeout, [this, currentGen] {
                return (mDone && mPostedGen == currentGen) || mAbortedGen >= currentGen;
            })) {
            return false;
        }
        if (mAbortedGen >= currentGen || !mDone || mPostedGen != currentGen) {
            return false;
        }
        *outError = mError;
        mDone = false;
        return true;
    }

    void abort(uint64_t gen) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (gen > mAbortedGen) {
            mAbortedGen = gen;
            mCv.notify_all();
        }
    }

    bool waitForDrain(std::chrono::milliseconds timeout, uint64_t currentGen) {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait_for(lock, timeout,
                     [this, currentGen] { return !mPending || mAbortedGen >= currentGen; });
        return !mPending;
    }

  private:
    mutable std::mutex mMutex;
    std::condition_variable mCv;
    uint64_t mPostedGen = 0;
    uint64_t mAbortedGen = 0;
    bool mDone = false;
    bool mPending = false;
    int32_t mError = 0;
};

void CwbClient::HardwareBufferDeleter::operator()(AHardwareBuffer* buffer) const {
    if (buffer != nullptr) {
        AHardwareBuffer_release(buffer);
    }
}

CwbClient::CwbClient(UdalsGeometry geo, std::shared_ptr<UDalsBleedCompensator> compensator)
    : mGeo(geo),
      mRoiWidth(geo.right - geo.left),
      mRoiHeight(geo.bottom - geo.top),
      mCenterRelX(geo.cx - geo.left),
      mCenterRelY(geo.cy - geo.top),
      mRadiusSq{geo.r1 * geo.r1, geo.r2 * geo.r2, geo.r3 * geo.r3},
      mCompensator(std::move(compensator)) {}

CwbClient::~CwbClient() {
    std::shared_ptr<CwbCallbackHandler> cb;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        mRunning = false;
        mSession = {};
        gen = ++mCaptureGen;
        cb = mActiveCallback;
    }

    if (cb != nullptr) {
        cb->abort(gen);
    }

    mCv.notify_all();

    if (mWorker.joinable()) {
        mWorker.join();
    }

    releaseCwbWorker(/*freeBuffer=*/true);
}

bool CwbClient::hasActiveBacklight() const {
    return mCompensator == nullptr || mCompensator->getBacklight() > 0;
}

bool CwbClient::isActiveGen(uint64_t gen) const {
    std::lock_guard<std::mutex> lock(mControlMutex);
    return mRunning && mSession.mode != SessionMode::kStopped && mCaptureGen == gen;
}

bool CwbClient::canRunWorkerLocked() const {
    return mSession.mode != SessionMode::kStopped && !mInvalidRoi.load() && hasActiveBacklight() &&
           !mSession.aboveBleedRelevantLux &&
           (mSession.captureRequested || mSession.mode == SessionMode::kLockout);
}

void CwbClient::notifyBacklightChanged() {
    std::lock_guard<std::mutex> lock(mControlMutex);
    if (mRunning && canRunWorkerLocked()) {
        mCv.notify_all();
    }
}

bool CwbClient::evaluateTriggerLocked(float rawLux, TimePoint now) {
    if (!mSession.anchorRawLux) {
        mSession.clearTriggerTracking();
        return true;
    }

    const float anchor = *mSession.anchorRawLux;
    const float delta = rawLux - anchor;
    const float absDelta = std::fabs(delta);
    const float immediateThreshold =
            std::clamp(kTriggerRelativeDelta * anchor, kMinTriggerDeltaLux, kMaxTriggerDeltaLux);
    const float sustainedThreshold = std::clamp(kTriggerRelativeDelta * anchor,
                                                kSustainedMinTriggerDeltaLux, kMaxTriggerDeltaLux);

    if (absDelta >= immediateThreshold) {
        mSession.clearSustainedDelta();
        mSession.pendingTransitionDir = (delta > 0.0f) ? 1 : -1;
        return true;
    }
    if (absDelta >= sustainedThreshold) {
        const int8_t sign = (delta > 0.0f) ? 1 : -1;
        if (sign == mSession.sustainedDeltaSign && mSession.sustainedDeltaCount > 0) {
            ++mSession.sustainedDeltaCount;
        } else {
            mSession.sustainedDeltaSign = sign;
            mSession.sustainedDeltaCount = 1;
            mSession.sustainedStartTime = now;
        }
        if (mSession.pendingTransitionDir != 0 && mSession.pendingTransitionDir != sign) {
            mSession.pendingTransitionDir = 0;
        }
        if (mSession.sustainedDeltaCount >= kSustainedLowLuxSamples &&
            (now - mSession.sustainedStartTime) >= kSustainedMinDuration) {
            mSession.clearSustainedDelta();
            mSession.pendingTransitionDir = sign;
            return true;
        }
        return false;
    }

    mSession.clearTriggerTracking();
    return false;
}

void CwbClient::onRawLuxSample(float rawLux) {
    if (!std::isfinite(rawLux) || rawLux < 0.0f || mInvalidRoi.load()) {
        return;
    }

    uint64_t seqBefore = (mCompensator != nullptr) ? mCompensator->getSampleSeq() : 0;
    bool unambiguousAmbient =
            (mCompensator != nullptr) && mCompensator->isUnambiguousAmbient(rawLux);
    bool shouldNotify = false;
    int8_t pendingDir = 0;
    float anchorRawLux = rawLux;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        if (!mRunning || mSession.mode == SessionMode::kStopped) {
            return;
        }
        auto now = SteadyClock::now();
        if (rawLux >= kMaxBleedRelevantLux) {
            bool changed = !mSession.aboveBleedRelevantLux;
            mSession.aboveBleedRelevantLux = true;
            mSession.wasUnambiguousAmbientInLockout = false;
            mSession.latestRawLux.reset();
            mSession.anchorRawLux.reset();
            mSession.seedAnchorOnNextSample = false;
            mSession.captureRequested = false;
            mSession.clearTriggerTracking();
            shouldNotify = changed;
        } else {
            bool resumedFromHighLux = mSession.aboveBleedRelevantLux;
            mSession.aboveBleedRelevantLux = false;
            mSession.latestRawLux = LuxSample{rawLux, now};
            if (mSession.mode == SessionMode::kLockout) {
                mSession.clearTriggerTracking();
                bool ambientRisingEdge =
                        unambiguousAmbient && !mSession.wasUnambiguousAmbientInLockout;
                mSession.wasUnambiguousAmbientInLockout = unambiguousAmbient;
                if (resumedFromHighLux || ambientRisingEdge) {
                    if (mSession.lockoutProbeShift > 0) {
                        mSession.lockoutProbeShift = 0;
                        shouldNotify = true;
                    } else if (resumedFromHighLux) {
                        shouldNotify = true;
                    }
                }
            } else if (mSession.seedAnchorOnNextSample) {
                mSession.anchorRawLux = rawLux;
                mSession.seedAnchorOnNextSample = false;
                mSession.clearTriggerTracking();
            } else {
                anchorRawLux = mSession.anchorRawLux.value_or(rawLux);
                if (evaluateTriggerLocked(rawLux, now) && !mSession.captureRequested) {
                    mSession.captureRequested = true;
                    shouldNotify = true;
                }

                if (hasActiveBacklight()) {
                    if (mSession.pendingTransitionDir != 0) {
                        pendingDir = mSession.pendingTransitionDir;
                    } else if (mSession.sustainedDeltaCount > 0) {
                        pendingDir = mSession.sustainedDeltaSign;
                    }
                }
            }
        }
    }
    if (mCompensator != nullptr) {
        mCompensator->updateTransitionHold(pendingDir, rawLux, anchorRawLux, seqBefore);
    }
    if (shouldNotify) {
        mCv.notify_one();
    }
}

bool CwbClient::triggerRefreshWorker() {
    if (mCwbClient != nullptr) {
        ndk::ScopedAStatus status = mCwbClient->refreshScreen();
        if (!status.isOk() && status.getStatus() == STATUS_DEAD_OBJECT) {
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }
    }
    return true;
}

void CwbClient::resetCwbBufferWorker() {
    mCwbBufferHandle = nullptr;
    mBufferStride = 0;
    mCwbBuffer.reset();
}

bool CwbClient::ensureCwbInitializedWorker() {
    if (mInvalidRoi.load() || mGeo.left < 0 || mGeo.top < 0 || mRoiWidth <= 0 || mRoiHeight <= 0) {
        return false;
    }

    if (mCwbClient != nullptr && mCwbBufferHandle != nullptr) {
        return true;
    }

    auto now = SteadyClock::now();
    if (mTiming.initFailures >= 15 && mLastInitAttempt &&
        (now - *mLastInitAttempt) < std::chrono::seconds(5)) {
        return false;
    }
    mLastInitAttempt = now;

    if (mCwbClient == nullptr) {
        static std::once_flag sBinderThreadPoolOnce;
        std::call_once(sBinderThreadPoolOnce, []() {
            ABinderProcess_setThreadPoolMaxThreadCount(1);
            ABinderProcess_startThreadPool();
        });

        const std::string instance = std::string(IDisplayConfig::descriptor) + "/default";
        ndk::SpAIBinder binder(AServiceManager_checkService(instance.c_str()));
        std::shared_ptr<IDisplayConfig> client = IDisplayConfig::fromBinder(binder);
        if (client == nullptr) {
            uint32_t attempt = ++mTiming.initFailures;
            LOG(WARNING) << "Failed to connect to " << instance << " (attempt " << attempt << ")";
            releaseCwbWorker(/*freeBuffer=*/false);
            return false;
        }
        auto callbackHandler = ndk::SharedRefBase::make<CwbCallbackHandler>();
        mCallbackHandler = std::move(callbackHandler);
        {
            std::lock_guard<std::mutex> lock(mControlMutex);
            mActiveCallback = mCallbackHandler;
        }
        mCwbClient = std::move(client);
    }

    if (!mDisplayBoundsChecked) {
        Attributes attrs{};
        if (!mCwbClient->getActiveBuiltinDisplayAttributes(&attrs).isOk() || attrs.xRes <= 0 ||
            attrs.yRes <= 0) {
            uint32_t attempt = ++mTiming.initFailures;
            LOG(WARNING) << "Failed to query active builtin display attributes (attempt " << attempt
                         << ")";
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }
        mDisplayBoundsChecked = true;
        if (mGeo.right > attrs.xRes || mGeo.bottom > attrs.yRes) {
            LOG(ERROR) << "UDALS CWB ROI (" << mGeo.left << "," << mGeo.top << "," << mGeo.right
                       << "," << mGeo.bottom << ") exceeds active display resolution " << attrs.xRes
                       << "x" << attrs.yRes << "; disabling CWB";
            mInvalidRoi = true;
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }
    }

    if (mCwbBuffer == nullptr || mCwbBufferHandle == nullptr) {
        AHardwareBuffer_Desc desc = {
                .width = static_cast<uint32_t>(mRoiWidth),
                .height = static_cast<uint32_t>(mRoiHeight),
                .layers = 1,
                .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                         AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
                .stride = 0,
                .rfu0 = 0,
                .rfu1 = 0,
        };

        AHardwareBuffer* rawBuffer = nullptr;
        if (AHardwareBuffer_allocate(&desc, &rawBuffer) != 0 || rawBuffer == nullptr) {
            uint32_t attempt = ++mTiming.initFailures;
            LOG(WARNING) << "Failed to allocate CWB buffer (attempt " << attempt << ")";
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }
        mCwbBuffer.reset(rawBuffer);

        AHardwareBuffer_describe(mCwbBuffer.get(), &desc);
        mBufferStride = desc.stride;
        if (mBufferStride < static_cast<uint32_t>(mRoiWidth) ||
            desc.height < static_cast<uint32_t>(mRoiHeight)) {
            ++mTiming.initFailures;
            LOG(ERROR) << "Allocated CWB buffer smaller than ROI";
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }

        mCwbBufferHandle = AHardwareBuffer_getNativeHandle(mCwbBuffer.get());
        if (mCwbBufferHandle == nullptr) {
            ++mTiming.initFailures;
            LOG(WARNING) << "Failed to get native handle for CWB buffer";
            releaseCwbWorker(/*freeBuffer=*/true);
            return false;
        }

        LOG(INFO) << "CWB client initialized successfully: " << mRoiWidth << "x" << mRoiHeight
                  << " stride=" << mBufferStride;
    }

    return true;
}

void CwbClient::releaseCwbWorker(bool freeBuffer) {
    uint64_t abortGen = 0;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        abortGen = mCaptureGen;
        mActiveCallback.reset();
    }

    std::shared_ptr<CwbCallbackHandler> retired = std::move(mCallbackHandler);
    if (retired != nullptr) {
        retired->abort(abortGen);
        retired->cancelPending();
    }

    mCwbClient.reset();

    if (freeBuffer) {
        resetCwbBufferWorker();
    }
}

bool CwbClient::start() {
    if (mInvalidRoi.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        ++mCaptureGen;
        mSession = {};
        mSession.mode = SessionMode::kActive;
        mSession.captureRequested = true;
        if (!mRunning) {
            mRunning = true;
            mWorker = std::thread(&CwbClient::captureWorkerThread, this);
        }
    }
    mCv.notify_all();
    return true;
}

void CwbClient::stop() {
    std::shared_ptr<CwbCallbackHandler> cb;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        if (!mRunning || mSession.mode == SessionMode::kStopped) {
            return;
        }
        mSession = {};
        gen = ++mCaptureGen;
        cb = mActiveCallback;
    }

    if (cb != nullptr) {
        cb->abort(gen);
    }
    if (mCompensator != nullptr) {
        mCompensator->clearTransitionHold();
    }
    mCv.notify_all();
}

std::optional<CwbClient::DualRgb> CwbClient::computeWeightedRgb(const uint8_t* pixels) const {
    static constexpr std::array<float, 3> kRingWeights = {0.68f, 0.20f, 0.12f};

    static const std::array<float, 256> kGammaLutHigh = initGammaLut(kGammaHigh);
    static const std::array<float, 256> kGammaLutLow = initGammaLut(kGammaLow);

    std::array<std::array<float, 3>, 3> sumHigh{};
    std::array<std::array<float, 3>, 3> sumLow{};
    std::array<uint32_t, 3> ringCounts{};

    const int32_t yStart = std::max(0, mCenterRelY - mGeo.r3);
    const int32_t yEnd = std::min(mRoiHeight, mCenterRelY + mGeo.r3 + 1);
    const int32_t xStart = std::max(0, mCenterRelX - mGeo.r3);
    const int32_t xEnd = std::min(mRoiWidth, mCenterRelX + mGeo.r3 + 1);

    for (int32_t y = yStart; y < yEnd; ++y) {
        const int32_t dy = y - mCenterRelY;
        const int32_t dy2 = dy * dy;
        const uint8_t* row = pixels + (static_cast<size_t>(y) * mBufferStride * 4);

        for (int32_t x = xStart; x < xEnd; ++x) {
            const int32_t dx = x - mCenterRelX;
            const int32_t distSq = dx * dx + dy2;
            if (distSq > mRadiusSq[2]) {
                continue;
            }

            size_t ring = 2;
            if (distSq <= mRadiusSq[0]) {
                ring = 0;
            } else if (distSq <= mRadiusSq[1]) {
                ring = 1;
            }

            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            for (size_t c = 0; c < 3; ++c) {
                sumHigh[ring][c] += kGammaLutHigh[px[c]];
                sumLow[ring][c] += kGammaLutLow[px[c]];
            }
            ++ringCounts[ring];
        }
    }

    float activeWeightSum = 0.0f;
    DualRgb combined{};
    for (size_t ring = 0; ring < 3; ++ring) {
        if (ringCounts[ring] == 0 || kRingWeights[ring] <= 0.0f) {
            continue;
        }
        const float invCount = 1.0f / static_cast<float>(ringCounts[ring]);
        const float w = kRingWeights[ring];
        for (size_t c = 0; c < 3; ++c) {
            combined.high[c] += w * (sumHigh[ring][c] * invCount);
            combined.low[c] += w * (sumLow[ring][c] * invCount);
        }
        activeWeightSum += w;
    }

    if (activeWeightSum <= 0.0f) {
        return std::nullopt;
    }

    const float invWeight = 1.0f / activeWeightSum;
    for (size_t c = 0; c < 3; ++c) {
        combined.high[c] = std::clamp(combined.high[c] * invWeight, 0.0f, 1.0f);
        combined.low[c] = std::clamp(combined.low[c] * invWeight, 0.0f, 1.0f);
    }
    return combined;
}

CwbClient::CaptureResult CwbClient::requestCwbSample(uint64_t currentGen) {
    std::optional<float> dispatchRawLux;
    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        if (!mRunning || mSession.mode == SessionMode::kStopped || mCaptureGen != currentGen ||
            mCompensator == nullptr || mSession.aboveBleedRelevantLux || !hasActiveBacklight()) {
            return CaptureResult::kAborted;
        }
        if (mSession.latestRawLux) {
            dispatchRawLux = mSession.latestRawLux->lux;
        }
        mSession.clearSustainedDelta();
        mSession.captureRequested = false;
    }

    int32_t cwbError = 0;
    bool bufferReady = false;
    bool wasPending = false;

    if (mCallbackHandler != nullptr) {
        switch (mCallbackHandler->pollStatus(currentGen, &cwbError)) {
            case CwbCallbackHandler::Status::kReady:
                bufferReady = true;
                break;
            case CwbCallbackHandler::Status::kPendingCurrent:
                wasPending = true;
                break;
            case CwbCallbackHandler::Status::kPendingStale: {
                // Drain in-flight pre-sleep capture across off->on wake before dispatching anew.
                if (!triggerRefreshWorker() || mCallbackHandler == nullptr) {
                    return isActiveGen(currentGen) ? CaptureResult::kFailed
                                                   : CaptureResult::kAborted;
                }
                bool drained = mCallbackHandler->waitForDrain(kDrainTimeout, currentGen);
                if (!isActiveGen(currentGen)) {
                    return CaptureResult::kAborted;
                }
                if (drained) {
                    mCallbackHandler->clearDone();
                } else {
                    // Allocate a fresh buffer (new BUFFER_ID) on the next attempt after drain fail.
                    releaseCwbWorker(/*freeBuffer=*/true);
                    return CaptureResult::kFailed;
                }
                break;
            }
            case CwbCallbackHandler::Status::kIdle:
                break;
        }
    }

    if (!bufferReady && !wasPending) {
        if (!ensureCwbInitializedWorker() || mCwbBufferHandle == nullptr ||
            mCallbackHandler == nullptr) {
            return isActiveGen(currentGen) ? CaptureResult::kFailed : CaptureResult::kAborted;
        }

        Rect roiRect = {
                .left = mGeo.left,
                .top = mGeo.top,
                .right = mGeo.right,
                .bottom = mGeo.bottom,
        };

        if (!isActiveGen(currentGen) || !mCallbackHandler->prepareForCapture(currentGen)) {
            return CaptureResult::kAborted;
        }
        mTiming.lastDispatchTime = SteadyClock::now();

        ndk::ScopedAStatus status =
                mCwbClient->setCWBOutputBuffer(mCallbackHandler, kPrimaryDisplayId, roiRect, true,
                                               ::android::dupToAidl(mCwbBufferHandle));
        if (!status.isOk()) {
            mCallbackHandler->cancelPending();
            if (status.getStatus() == STATUS_DEAD_OBJECT) {
                releaseCwbWorker(/*freeBuffer=*/false);
            }
            return isActiveGen(currentGen) ? CaptureResult::kFailed : CaptureResult::kAborted;
        }
    }

    if (!bufferReady) {
        if (!mCallbackHandler->waitForDone(kInitialWaitTimeout, currentGen, &cwbError)) {
            if (!isActiveGen(currentGen) || mCompensator == nullptr) {
                return CaptureResult::kAborted;
            }
            if (wasPending) {
                return CaptureResult::kFailed;
            }
            // Idle SurfaceFlinger may defer CWB completion until a commit occurs:
            // poke refreshScreen once and wait immediately without dispatch cooldown.
            if (!triggerRefreshWorker() || mCallbackHandler == nullptr ||
                !mCallbackHandler->waitForDone(kPokeWaitTimeout, currentGen, &cwbError)) {
                return isActiveGen(currentGen) ? CaptureResult::kFailed : CaptureResult::kAborted;
            }
        }
    }

    if (!isActiveGen(currentGen) || mCompensator == nullptr || mCwbBuffer == nullptr) {
        return CaptureResult::kAborted;
    }

    if (cwbError != 0) {
        return CaptureResult::kFailed;
    }

    std::optional<DualRgb> sampledRgb;
    {
        ScopedHardwareBufferLock scopedLock(mCwbBuffer.get(), AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
        if (scopedLock.address() == nullptr) {
            return CaptureResult::kFailed;
        }
        sampledRgb = computeWeightedRgb(static_cast<const uint8_t*>(scopedLock.address()));
        if (!sampledRgb) {
            return CaptureResult::kFailed;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mControlMutex);
        if (!mRunning || mSession.mode == SessionMode::kStopped || mCaptureGen != currentGen ||
            mCompensator == nullptr) {
            return CaptureResult::kAborted;
        }
        mCompensator->setSampledRgb(sampledRgb->high[0], sampledRgb->high[1], sampledRgb->high[2],
                                    sampledRgb->low[0], sampledRgb->low[1], sampledRgb->low[2]);
        mSession.mode = SessionMode::kActive;
        mSession.wasUnambiguousAmbientInLockout = false;
        mSession.lockoutProbeShift = 0;
        mSession.seedAnchorOnNextSample = false;
        mSession.captureRequested = false;
        mSession.clearTriggerTracking();

        if (dispatchRawLux) {
            if (mSession.latestRawLux) {
                mSession.anchorRawLux = *dispatchRawLux;
                mSession.captureRequested = evaluateTriggerLocked(mSession.latestRawLux->lux,
                                                                  mSession.latestRawLux->time);
            } else {
                // Ambient lux crossed >= kMaxBleedRelevantLux mid-capture.
                mSession.anchorRawLux.reset();
            }
        } else if (mSession.latestRawLux) {
            mSession.anchorRawLux = mSession.latestRawLux->lux;
        } else {
            mSession.anchorRawLux.reset();
            mSession.seedAnchorOnNextSample = !mSession.aboveBleedRelevantLux;
        }
    }

    // Transition SDM out of configured CWB / CWB frequency boost after a 1-shot capture so idle
    // SurfaceFlinger does not remain in active CWB state until the next UI redraw.
    (void)triggerRefreshWorker();

    mTiming.consecutiveErrors = 0;
    mTiming.pendingStuckCount = 0;
    mTiming.initFailures = 0;
    mTiming.retryAfter.reset();

    return CaptureResult::kSuccess;
}

void CwbClient::captureWorkerThread() {
    pthread_setname_np(pthread_self(), "UDalsCwbWorker");

    while (true) {
        uint64_t currentGen = 0;
        SessionMode mode = SessionMode::kStopped;
        uint32_t lockoutShift = 0;
        {
            std::unique_lock<std::mutex> lock(mControlMutex);
            mCv.wait(lock, [this] { return !mRunning || canRunWorkerLocked(); });
            if (!mRunning) {
                break;
            }
            currentGen = mCaptureGen;
            mode = mSession.mode;
            lockoutShift = mSession.lockoutProbeShift;
        }

        if (currentGen != mLastSeenGen) {
            mTiming = {};
            mLastSeenGen = currentGen;
        }

        auto now = SteadyClock::now();
        TimePoint nextAttempt = now;
        if (mode == SessionMode::kLockout) {
            if (mTiming.lastLockoutProbeTime) {
                auto probeInterval = kLockoutProbeBaseInterval *
                                     (1u << std::min(lockoutShift, kMaxLockoutProbeShift));
                nextAttempt = std::max(nextAttempt, *mTiming.lastLockoutProbeTime + probeInterval);
            }
        } else {
            bool isPending = (mCallbackHandler != nullptr && mCallbackHandler->isPending());
            if (!isPending && mTiming.lastDispatchTime) {
                nextAttempt =
                        std::max(nextAttempt, *mTiming.lastDispatchTime + kMinDispatchInterval);
            }
            if (mTiming.retryAfter) {
                nextAttempt = std::max(nextAttempt, *mTiming.retryAfter);
            }
        }

        if (nextAttempt > now) {
            std::unique_lock<std::mutex> lock(mControlMutex);
            auto shouldStillWait = [&] {
                return mRunning && canRunWorkerLocked() && mCaptureGen == currentGen &&
                       mSession.mode == mode &&
                       (mode != SessionMode::kLockout ||
                        mSession.lockoutProbeShift == lockoutShift);
            };
            if (shouldStillWait()) {
                mCv.wait_until(lock, nextAttempt, [&] { return !shouldStillWait(); });
            }
            if (!mRunning) {
                break;
            }
            continue;
        }

        auto attemptStart = SteadyClock::now();
        CaptureResult result = requestCwbSample(currentGen);

        if (mInvalidRoi.load() || result == CaptureResult::kSuccess ||
            result == CaptureResult::kAborted) {
            continue;
        }

        ++mTiming.consecutiveErrors;
        bool enterLockout = (mTiming.consecutiveErrors >= kLockoutThreshold);
        if (mCallbackHandler != nullptr && mCallbackHandler->isPending()) {
            ++mTiming.pendingStuckCount;
        } else {
            mTiming.pendingStuckCount = 0;
        }
        {
            std::lock_guard<std::mutex> lock(mControlMutex);
            if (!mRunning) {
                break;
            }
            if (mSession.mode != SessionMode::kStopped && mCaptureGen == currentGen) {
                if (enterLockout) {
                    if (mSession.mode != SessionMode::kLockout) {
                        if (mCompensator != nullptr) {
                            mCompensator->clearSample();
                        }
                        mSession.mode = SessionMode::kLockout;
                        mSession.wasUnambiguousAmbientInLockout = false;
                        mSession.lockoutProbeShift = 0;
                    } else if (mode == SessionMode::kLockout &&
                               mSession.lockoutProbeShift < kMaxLockoutProbeShift) {
                        ++mSession.lockoutProbeShift;
                    }
                    mSession.clearTriggerTracking();
                    mSession.captureRequested = false;
                } else if (mSession.mode != SessionMode::kLockout) {
                    mSession.captureRequested = true;
                }
            }
        }

        if (enterLockout && mTiming.pendingStuckCount >= kLockoutThreshold) {
            // A pending callback never arrived (lost HWC request or composer crash mid-flight):
            // tear down the stuck session and allocate a fresh buffer (new BUFFER_ID) on probe.
            releaseCwbWorker(/*freeBuffer=*/true);
            mTiming.pendingStuckCount = 0;
        }

        if (enterLockout) {
            mTiming.lastLockoutProbeTime = SteadyClock::now();
            mTiming.retryAfter.reset();
        } else {
            mTiming.retryAfter = attemptStart + kRetryBackoffInterval;
        }
    }
}

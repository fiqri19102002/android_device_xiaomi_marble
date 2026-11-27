/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "UDalsBleedCompensator.h"

struct AHardwareBuffer;
struct native_handle;

namespace aidl::vendor::qti::hardware::display::config {
class IDisplayConfig;
}
class CwbCallbackHandler;

class CwbClient {
  public:
    CwbClient(UdalsGeometry geo, std::shared_ptr<UDalsBleedCompensator> compensator);
    ~CwbClient();
    CwbClient(const CwbClient&) = delete;
    CwbClient& operator=(const CwbClient&) = delete;
    CwbClient(CwbClient&&) = delete;
    CwbClient& operator=(CwbClient&&) = delete;

    bool start();
    void stop();
    void notifyBacklightChanged();
    void onRawLuxSample(float rawLux);

  private:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    enum class CaptureResult : uint8_t {
        kSuccess,
        kFailed,
        kAborted,
    };

    enum class SessionMode : uint8_t {
        kStopped,
        kActive,
        kLockout,
    };

    struct LuxSample {
        float lux = 0.0f;
        TimePoint time{};
    };

    struct DualRgb {
        std::array<float, 3> high{};
        std::array<float, 3> low{};
    };

    struct HardwareBufferDeleter {
        void operator()(AHardwareBuffer* buffer) const;
    };

    struct SessionState {
        SessionMode mode = SessionMode::kStopped;
        bool captureRequested = false;
        bool wasUnambiguousAmbientInLockout = false;
        bool aboveBleedRelevantLux = false;
        bool seedAnchorOnNextSample = false;
        uint32_t sustainedDeltaCount = 0;
        int8_t sustainedDeltaSign = 0;
        int8_t pendingTransitionDir = 0;
        uint32_t lockoutProbeShift = 0;
        std::optional<LuxSample> latestRawLux;
        std::optional<float> anchorRawLux;
        TimePoint sustainedStartTime{};

        void clearSustainedDelta() {
            sustainedDeltaCount = 0;
            sustainedDeltaSign = 0;
            sustainedStartTime = {};
        }

        void clearTriggerTracking() {
            clearSustainedDelta();
            pendingTransitionDir = 0;
        }
    };

    struct CaptureTimingState {
        uint32_t initFailures = 0;
        uint32_t consecutiveErrors = 0;
        uint32_t pendingStuckCount = 0;
        std::optional<TimePoint> lastDispatchTime;
        std::optional<TimePoint> retryAfter;
        std::optional<TimePoint> lastLockoutProbeTime;
    };

    void captureWorkerThread();
    bool hasActiveBacklight() const;
    bool isActiveGen(uint64_t gen) const;
    bool canRunWorkerLocked() const;
    bool evaluateTriggerLocked(float rawLux, TimePoint now);
    bool ensureCwbInitializedWorker();
    void resetCwbBufferWorker();
    void releaseCwbWorker(bool freeBuffer);
    bool triggerRefreshWorker();
    CaptureResult requestCwbSample(uint64_t currentGen);
    std::optional<DualRgb> computeWeightedRgb(const uint8_t* pixels) const;

    UdalsGeometry mGeo;
    int32_t mRoiWidth;
    int32_t mRoiHeight;
    int32_t mCenterRelX;
    int32_t mCenterRelY;
    std::array<int32_t, 3> mRadiusSq;

    std::shared_ptr<UDalsBleedCompensator> mCompensator;
    std::atomic<bool> mInvalidRoi{false};

    mutable std::mutex mControlMutex;
    std::condition_variable mCv;
    bool mRunning = false;
    uint64_t mCaptureGen = 0;
    SessionState mSession{};

    // Worker-thread-exclusive buffer, timing, and client state.
    // Declared before callback holders, mCwbClient, and mWorker so reverse-order
    // destruction releases callbacks and client before AHardwareBuffer_release().
    std::unique_ptr<AHardwareBuffer, HardwareBufferDeleter> mCwbBuffer;
    const struct native_handle* mCwbBufferHandle = nullptr;
    uint32_t mBufferStride = 0;
    uint64_t mLastSeenGen = 0;
    bool mDisplayBoundsChecked = false;
    std::optional<TimePoint> mLastInitAttempt;
    CaptureTimingState mTiming{};

    std::shared_ptr<aidl::vendor::qti::hardware::display::config::IDisplayConfig> mCwbClient;
    std::shared_ptr<CwbCallbackHandler> mCallbackHandler;
    std::shared_ptr<CwbCallbackHandler> mActiveCallback;
    std::thread mWorker;
};

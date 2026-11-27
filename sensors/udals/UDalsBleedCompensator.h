/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct UdalsGeometry {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    int32_t r1 = 0;
    int32_t r2 = 0;
    int32_t r3 = 0;
};

class UDalsBleedCompensator {
  public:
    UDalsBleedCompensator();
    ~UDalsBleedCompensator() = default;
    UDalsBleedCompensator(const UDalsBleedCompensator&) = delete;
    UDalsBleedCompensator& operator=(const UDalsBleedCompensator&) = delete;
    UDalsBleedCompensator(UDalsBleedCompensator&&) = delete;
    UDalsBleedCompensator& operator=(UDalsBleedCompensator&&) = delete;

    bool loadConfig(const std::string& path, UdalsGeometry* outGeo);

    void reset();
    void clearSample();
    void setBacklight(uint32_t brightness);
    void setSampledRgb(float rHigh, float gHigh, float bHigh, float rLow, float gLow, float bLow);
    void updateTransitionHold(int8_t pendingDir, float rawLux, float anchorRawLux,
                              uint64_t expectedSeq);
    void clearTransitionHold();
    bool isUnambiguousAmbient(float rawLux) const;
    bool compensate(float rawLux, float* outLux);
    uint64_t getSampleSeq() const;
    uint32_t getBacklight() const;
    uint32_t getNormalMaxBacklight() const;

  private:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    struct CaliPoint {
        float brightness;
        float bleedLux;
    };
    using CurveSet = std::array<std::vector<CaliPoint>, 3>;

    struct SampledRgb {
        std::array<float, 3> high{};
        std::array<float, 3> low{};
    };

    struct BacklightHoldEntry {
        TimePoint endTime;
        uint32_t brightness;
    };

    enum class HoldMode : uint8_t {
        kNone,
        kWake,
        kTransition,
    };

    struct HoldState {
        HoldMode mode = HoldMode::kNone;
        int8_t dir = 0;
        bool timedOut = false;
        float anchorRawLux = 0.0f;
        TimePoint startTime{};
    };

    void pruneBacklightHistoryLocked(TimePoint now);
    uint32_t peakBacklightLocked(TimePoint now) const;
    uint32_t effectiveBacklightLocked(TimePoint now);
    float calculateBleedLuxForRgbLocked(uint32_t backlight, const std::array<float, 3>& rgb) const;
    float calculateBleedLuxLocked(uint32_t effectiveBacklight) const;
    float calculateMaxWhiteBleedLocked(uint32_t effectiveBacklight) const;
    bool isTransitionBleedPlausibleLocked(int8_t dir, uint32_t effBkl, float rawLux,
                                          float anchorRawLux) const;
    void resetResidualFilterLocked();
    float filterSignedResidualLocked(float signedRes, float bleed, TimePoint now);
    static float interpolateTable(const std::vector<CaliPoint>& table, float brightness);

    mutable std::mutex mLock;
    uint32_t mBacklight = 0;
    uint32_t mNormalMaxBacklight = 2047u;
    uint64_t mSampleSeq = 0;
    HoldState mHold{};
    std::optional<SampledRgb> mSample;
    std::optional<float> mLastSettledCompLux;
    std::optional<float> mResidualEma;
    std::optional<TimePoint> mLastResidualSampleTime;
    std::optional<std::array<float, 3>> mPeakBleedLux;
    std::deque<BacklightHoldEntry> mBacklightHistory;
    std::deque<float> mResidualWindow;
    CurveSet mNormalCurves{};
};

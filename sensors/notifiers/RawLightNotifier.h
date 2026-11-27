/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "SensorNotifier.h"
#include "../udals/CwbClient.h"
#include "../udals/UDalsBleedCompensator.h"

class RawLightNotifier : public SensorNotifier {
  public:
    RawLightNotifier(sp<ISensorManager> manager);
    ~RawLightNotifier();

  protected:
    void notify() override;

  private:
    uint32_t normalizeBacklight(uint32_t raw) const;
    void resolveBacklightScale();
    void updateBacklight(uint32_t rawBacklight);

    std::optional<uint32_t> mDeviceNormalMaxBacklight;
    std::shared_ptr<UDalsBleedCompensator> mCompensator;
    std::shared_ptr<CwbClient> mCwbClient;
};

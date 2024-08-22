/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SensorNotifier"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include "SscCalApi.h"
#include "notifiers/RawLightNotifier.h"

int main() {
    sp<ISensorManager> manager = ISensorManager::getService();
    if (manager == nullptr) {
        LOG(ERROR) << "failed to get ISensorManager";
        return EXIT_FAILURE;
    }

    SscCalApiWrapper::getInstance().initCurrentSensors(
            android::base::GetBoolProperty("persist.vendor.debug.ssccalapi", false));

    // Always assume don't use more than one notifier
    std::unique_ptr<RawLightNotifier> rawLightNotifier =
            std::make_unique<RawLightNotifier>(manager);
    rawLightNotifier->activate();

    while (true) {
        // Sleep to keep the notifiers alive
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    // Should never reach this
    return EXIT_SUCCESS;
}

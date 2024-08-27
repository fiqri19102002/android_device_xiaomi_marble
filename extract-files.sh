#!/bin/bash
#
# Copyright (C) 2016 The CyanogenMod Project
# Copyright (C) 2017-2023 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=marble
VENDOR=xiaomi

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

# Default to sanitizing the vendor folder before extraction
CLEAN_VENDOR=true

ONLY_FIRMWARE=
KANG=
SECTION=

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        --only-firmware )
                ONLY_FIRMWARE=true
                ;;
        -n | --no-cleanup )
                CLEAN_VENDOR=false
                ;;
        -k | --kang )
                KANG="--kang"
                ;;
        -s | --section )
                SECTION="${2}"; shift
                CLEAN_VENDOR=false
                ;;
        * )
                SRC="${1}"
                ;;
    esac
    shift
done

if [ -z "${SRC}" ]; then
    SRC="adb"
fi

function blob_fixup() {
    case "${1}" in
        vendor/bin/hw/android.hardware.identity-service-qti|vendor/lib64/libqtiidentitycredential.so)
            grep -q "android.hardware.identity-V3-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.identity-V3-ndk_platform.so" "android.hardware.identity-V3-ndk.so" "${2}"
            grep -q "android.hardware.keymaster-V3-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.keymaster-V3-ndk_platform.so" "android.hardware.keymaster-V3-ndk.so" "${2}"
            ;;
        vendor/bin/hw/android.hardware.security.keymint-service-qti|vendor/lib64/libqtikeymint.so)
            grep -q "android.hardware.security.keymint-V1-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.security.keymint-V1-ndk_platform.so" "android.hardware.security.keymint-V1-ndk.so" "${2}"
            grep -q "android.hardware.security.secureclock-V1-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.security.secureclock-V1-ndk_platform.so" "android.hardware.security.secureclock-V1-ndk.so" "${2}"
            grep -q "android.hardware.security.sharedsecret-V1-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.security.sharedsecret-V1-ndk_platform.so" "android.hardware.security.sharedsecret-V1-ndk.so" "${2}"
            grep -q "android.hardware.security.rkp-V3-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --add-needed "android.hardware.security.rkp-V3-ndk.so" "${2}"
            ;;
        vendor/etc/camera/marble_enhance_motiontuning.xml|vendor/etc/camera/marble_motiontuning.xml)
            sed -i 's/xml=version/xml version/g' "${2}"
            ;;
        vendor/etc/camera/pureView_parameter.xml)
            sed -i 's/=\([0-9]\+\)>/="\1">/g' "${2}"
            ;;
        vendor/etc/init/init.embmssl_server.rc)
            sed -i -n '/interface/!p' "${2}"
            ;;
        vendor/etc/media_codecs_c2_audio.xml)
            sed -i '/media_codecs_dolby_audio/d' "${2}"
            ;;
        vendor/etc/media_codecs_ukee.xml)
            sed -i -E '/media_codecs_(google_audio|google_c2|google_telephony|vendor_audio)/d' "${2}"
            ;;
        vendor/etc/qcril_database/upgrade/config/6.0_config.sql)
            sed -i '/persist.vendor.radio.redir_party_num/ s/true/false/g' "${2}"
            ;;
        vendor/etc/vintf/manifest/c2_manifest_vendor.xml)
            sed -ni '/dolby/!p' "${2}"
            ;;
        vendor/lib64/libcamximageformatutils.so)
            grep -q "vendor.qti.hardware.display.config-V2-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "vendor.qti.hardware.display.config-V2-ndk_platform.so" "vendor.qti.hardware.display.config-V2-ndk.so" "${2}"
            ;;
        vendor/lib64/vendor.qti.hardware.qxr-V1-ndk_platform.so)
            grep -q "android.hardware.common-V2-ndk.so" "${2}" || "${PATCHELF_0_17_2}" --replace-needed "android.hardware.common-V2-ndk_platform.so" "android.hardware.common-V2-ndk.so" "${2}"
            ;;
    esac
}

# Initialize the helper for device
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}" false "${CLEAN_VENDOR}"

if [ -z "${ONLY_FIRMWARE}" ]; then
    extract "${MY_DIR}/proprietary-files.txt" "${SRC}" "${KANG}" --section "${SECTION}"
fi

if [ -z "${SECTION}" ]; then
    extract_firmware "${MY_DIR}/proprietary-firmware.txt" "${SRC}"
fi

"${MY_DIR}/setup-makefiles.sh"

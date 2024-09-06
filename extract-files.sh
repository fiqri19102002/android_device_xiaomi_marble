#!/bin/bash
#
# SPDX-FileCopyrightText: 2016 The CyanogenMod Project
# SPDX-FileCopyrightText: 2017-2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=marble
VENDOR=xiaomi

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."
export TARGET_ENABLE_CHECKELF="true"

# If XML files don't have comments before the XML header, use this flag
# Can still be used with broken XML files by using blob_fixup
export TARGET_DISABLE_XML_FIXING=true

# Define the default patchelf version used to patch blobs
# This will also be used for utility functions like FIX_SONAME
export PATCHELF_VERSION=0_17_2

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
        --only-firmware)
            ONLY_FIRMWARE=true
            ;;
        -n | --no-cleanup)
            CLEAN_VENDOR=false
            ;;
        -k | --kang)
            KANG="--kang"
            ;;
        -s | --section)
            SECTION="${2}"
            shift
            CLEAN_VENDOR=false
            ;;
        *)
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
        system_ext/lib64/libwfdservice.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "android.media.audio.common.types-V2-cpp.so" "android.media.audio.common.types-V3-cpp.so" "${2}"
            ;;
        vendor/bin/hw/android.hardware.security.keymint-service-qti|vendor/lib64/libqtikeymint.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "android.hardware.security.keymint-V1-ndk_platform.so" "android.hardware.security.keymint-V1-ndk.so" "${2}"
            "${PATCHELF}" --replace-needed "android.hardware.security.secureclock-V1-ndk_platform.so" "android.hardware.security.secureclock-V1-ndk.so" "${2}"
            "${PATCHELF}" --replace-needed "android.hardware.security.sharedsecret-V1-ndk_platform.so" "android.hardware.security.sharedsecret-V1-ndk.so" "${2}"
            grep -q "android.hardware.security.rkp-V1-ndk.so" "${2}" || "${PATCHELF}" --add-needed "android.hardware.security.rkp-V1-ndk.so" "${2}"
            ;;
        vendor/bin/qcc-trd)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "libgrpc++_unsecure.so" "libgrpc++_unsecure_prebuilt.so" "${2}"
            ;;
        vendor/etc/media_codecs_c2_audio.xml)
            [ "$2" = "" ] && return 0
            sed -i '/media_codecs_dolby_audio/d' "${2}"
            ;;
        vendor/etc/media_codecs_ukee.xml)
            [ "$2" = "" ] && return 0
            sed -i -E '/media_codecs_(google_audio|google_c2|google_telephony|vendor_audio)/d' "${2}"
            ;;
        vendor/etc/camera/marble_enhance_motiontuning.xml|vendor/etc/camera/marble_motiontuning.xml)
            [ "$2" = "" ] && return 0
            sed -i 's/xml=version/xml version/g' "${2}"
            ;;
        vendor/etc/camera/pureView_parameter.xml)
            [ "$2" = "" ] && return 0
            sed -i 's/=\([0-9]\+\)>/="\1">/g' "${2}"
            ;;
        vendor/etc/sensors/hals.conf)
            [ "$2" = "" ] && return 0
            sed -i '$s/$/\nsensors.xiaomi.v2.so/' "${2}"
            ;;
        vendor/etc/vintf/manifest/c2_manifest_vendor.xml)
            [ "$2" = "" ] && return 0
            sed -i '/dolby/d' "${2}"
            ;;
        vendor/lib64/libcamximageformatutils.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "vendor.qti.hardware.display.config-V2-ndk_platform.so" "vendor.qti.hardware.display.config-V2-ndk.so" "${2}"
            ;;
        vendor/lib64/vendor.libdpmframework.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --add-needed "libhidlbase_shim.so" "${2}"
            ;;
        *)
            return 1
            ;;
    esac

    return 0
}

function blob_fixup_dry() {
    blob_fixup "$1" ""
}

# Initialize the helper
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}" false "${CLEAN_VENDOR}"

if [ -z "${ONLY_FIRMWARE}" ]; then
    extract "${MY_DIR}/proprietary-files.txt" "${SRC}" "${KANG}" --section "${SECTION}"
fi

if [ -z "${SECTION}" ]; then
    extract_firmware "${MY_DIR}/proprietary-firmware.txt" "${SRC}"
fi

"${MY_DIR}/setup-makefiles.sh"

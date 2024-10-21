#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.file import File
from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixup_remove_arch_suffix,
    lib_fixup_vendorcompat,
    lib_fixups_user_type,
    libs_clang_rt_ubsan,
    libs_proto_3_9_1,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/xiaomi/marble',
    'hardware/qcom-caf/sm8450',
    'hardware/qcom-caf/wlan',
    'hardware/xiaomi',
    'vendor/qcom/opensource/commonsys/display',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/qcom/opensource/dataservices',
]

def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None

lib_fixups: lib_fixups_user_type = {
    libs_clang_rt_ubsan: lib_fixup_remove_arch_suffix,
    libs_proto_3_9_1: lib_fixup_vendorcompat,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'vendor.qti.hardware.dpmservice@1.0',
        'vendor.qti.hardware.dpmservice@1.1',
        'vendor.qti.hardware.qccsyshal@1.0',
        'vendor.qti.hardware.qccsyshal@1.1',
        'vendor.qti.hardware.qccvndhal@1.0',
        'vendor.qti.imsrtpservice@3.0',
        'vendor.qti.diaghal@1.0',
    ): lib_fixup_vendor_suffix,
    (
        'libagmclient',
        'libagmmixer',
        'libpalclient',
        'libwpa_client',
        'vendor.qti.hardware.pal@1.0-impl',
    ): lib_fixup_remove,
}

blob_fixups: blob_fixups_user_type = {
    (
        'vendor/bin/hw/android.hardware.security.keymint-service-qti',
        'vendor/lib64/libqtikeymint.so',
    ): blob_fixup()
    .replace_needed(
        'android.hardware.security.keymint-V1-ndk_platform.so',
        'android.hardware.security.keymint-V1-ndk.so',
    )
    .replace_needed(
        'android.hardware.security.secureclock-V1-ndk_platform.so',
        'android.hardware.security.secureclock-V1-ndk.so',
    )
    .replace_needed(
        'android.hardware.security.sharedsecret-V1-ndk_platform.so',
        'android.hardware.security.sharedsecret-V1-ndk.so',
    )
    .add_needed('android.hardware.security.rkp-V1-ndk.so'),
    'vendor/bin/qcc-trd': blob_fixup().replace_needed(
        'libgrpc++_unsecure.so', 'libgrpc++_unsecure_prebuilt.so'
    ),
    'vendor/etc/media_codecs_c2_audio.xml': blob_fixup().regex_replace('.+media_codecs_dolby_audio.+\n', ''),
    'vendor/etc/media_codecs_ukee.xml': blob_fixup().regex_replace(
        '.+media_codecs_(google_audio|google_c2|google_telephony|vendor_audio).+\n', ''
    ),
    (
        'vendor/etc/camera/marble_enhance_motiontuning.xml',
        'vendor/etc/camera/marble_motiontuning.xml',
    ): blob_fixup().regex_replace(r'xml=version', r'xml version'),
    'vendor/etc/camera/pureView_parameter.xml': blob_fixup().regex_replace(r'=(\d+)>', r'="\1">'),
    'vendor/etc/seccomp_policy/c2audio.vendor.ext-arm64.policy': blob_fixup().add_line_if_missing("setsockopt: 1"),
    'vendor/etc/sensors/hals.conf': blob_fixup().add_line_if_missing("sensors.xiaomi.v2.so"),
    'vendor/etc/vintf/manifest/c2_manifest_vendor.xml': blob_fixup().regex_replace('.+dolby.+\n', ''),
    'vendor/lib64/libcamximageformatutils.so': blob_fixup().replace_needed(
        'vendor.qti.hardware.display.config-V2-ndk_platform.so',
        'vendor.qti.hardware.display.config-V2-ndk.so'
    ),
    'vendor/lib64/vendor.libdpmframework.so': blob_fixup().add_needed('libhidlbase_shim.so'),
}  # fmt: skip

module = ExtractUtilsModule(
    'marble',
    'xiaomi',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    check_elf=True,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()

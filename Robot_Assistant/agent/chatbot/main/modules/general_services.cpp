/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "sdkconfig.h"
#include "brookesia/service_manager.hpp"
#include "brookesia/service_helper.hpp"
#include "private/utils.hpp"
#include "general_services.hpp"

using namespace esp_brookesia;

using SNTPHelper = esp_brookesia::service::helper::SNTP;
using AudioHelper = esp_brookesia::service::helper::Audio;
using NVSHelper = esp_brookesia::service::helper::NVS;
using WifiHelper = esp_brookesia::service::helper::Wifi;
using DeviceHelper = esp_brookesia::service::helper::Device;

// 唤醒词模型位于自定义分区 model 中，语言配置 cn 交给 ESP-SR 使用。
constexpr const char *AUDIO_WAKEUP_WORD_MODEL_PARTITION_LABEL = "model";
constexpr const char *AUDIO_WAKEUP_WORD_MN_LANGUAGE = "cn";

bool GeneralServices::init(std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler)
{
    // ServiceManager 只初始化一次。后续各模块通过 helper 绑定服务，
    // 不直接持有底层驱动对象。
    BROOKESIA_CHECK_NULL_RETURN(task_scheduler, false, "Task scheduler is not available");

    if (is_initialized()) {
        BROOKESIA_LOGW("General services is already initialized");
        return true;
    }

    BROOKESIA_LOGI("Initializing service manager...");

    auto &service_manager = service::ServiceManager::get_instance();
    BROOKESIA_CHECK_FALSE_RETURN(service_manager.init(), false, "Failed to initialize service manager");
    BROOKESIA_CHECK_FALSE_RETURN(service_manager.start(), false, "Failed to start service manager");

    task_scheduler_ = task_scheduler;

    BROOKESIA_LOGI("Service manager started successfully");

    return true;
}

void GeneralServices::init_audio()
{
    // 配置播放器、编码器和解码器的任务参数。音频服务随后会使用 AI
    // Agent 设置的 AFE 配置启动唤醒词和录音链路。
    BROOKESIA_CHECK_FALSE_EXIT(is_initialized(), "General services is not initialized");

    if (!AudioHelper::is_available()) {
        BROOKESIA_LOGW("Audio service is not available");
        return;
    }

    AudioHelper::PlaybackConfig playback_config{
        .player_task = {
            .core_id = 0,
            .priority = 5,
            .stack_size = 4 * 1024,
            // When using PSRAM but not using XIP, set stack_in_ext to true to prevent crashes caused by tasks
            // operating on Flash.
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
            .stack_in_ext = true,
#else
            .stack_in_ext = false,
#endif
        }
    };
    auto playback_result = AudioHelper::call_function_sync(
                               AudioHelper::FunctionId::SetPlaybackConfig, BROOKESIA_DESCRIBE_TO_JSON(playback_config).as_object()
                           );
    BROOKESIA_CHECK_FALSE_EXIT(playback_result, "Failed to set playback config: %1%", playback_result.error());

    AudioHelper::EncoderStaticConfig encoder_static_config{};
    auto encoder_static_result = AudioHelper::call_function_sync(
                                     AudioHelper::FunctionId::SetEncoderStaticConfig, BROOKESIA_DESCRIBE_TO_JSON(encoder_static_config).as_object()
                                 );
    BROOKESIA_CHECK_FALSE_EXIT(
        encoder_static_result, "Failed to set encoder static config: %1%", encoder_static_result.error()
    );

    AudioHelper::DecoderStaticConfig decoder_static_config{};
    auto decoder_static_result = AudioHelper::call_function_sync(
                                     AudioHelper::FunctionId::SetDecoderStaticConfig, BROOKESIA_DESCRIBE_TO_JSON(decoder_static_config).as_object()
                                 );
    BROOKESIA_CHECK_FALSE_EXIT(
        decoder_static_result, "Failed to set decoder static config: %1%", decoder_static_result.error()
    );

    auto set_mute_ret = DeviceHelper::call_function_async(DeviceHelper::FunctionId::SetAudioPlayerMute, false);
    BROOKESIA_CHECK_FALSE_EXIT(set_mute_ret, "Failed to set mute off");
}

void GeneralServices::start_sntp()
{
    // SNTP 只需要建立服务绑定；真正的时间同步由服务在联网后执行。
    BROOKESIA_CHECK_FALSE_EXIT(is_initialized(), "General services is not initialized");

    if (!SNTPHelper::is_available()) {
        BROOKESIA_LOGW("SNTP service is not available");
        return;
    }

    auto &service_manager = service::ServiceManager::get_instance();
    auto binding = service_manager.bind(SNTPHelper::get_name().data());
    if (!binding.is_valid()) {
        BROOKESIA_LOGE("Failed to bind SNTP service");
    } else {
        service_bindings_.push_back(std::move(binding));
    }
}

void GeneralServices::start_nvs()
{
    // NVS 保存 WiFi 凭据、音量、亮度等掉电后仍需保留的设置。
    BROOKESIA_CHECK_FALSE_EXIT(is_initialized(), "General services is not initialized");

    if (!NVSHelper::is_available()) {
        BROOKESIA_LOGW("NVS service is not available");
        return;
    }

    auto &service_manager = service::ServiceManager::get_instance();
    auto binding = service_manager.bind(NVSHelper::get_name().data());
    if (!binding.is_valid()) {
        BROOKESIA_LOGE("Failed to bind NVS service");
    } else {
        service_bindings_.push_back(std::move(binding));
    }
}

void GeneralServices::start_device()
{
    // Device service 提供音量、静音、亮度和电源等统一控制接口。
    BROOKESIA_CHECK_FALSE_EXIT(is_initialized(), "General services is not initialized");

    if (!DeviceHelper::is_available()) {
        BROOKESIA_LOGW("Device service is not available");
        return;
    }

    auto &service_manager = service::ServiceManager::get_instance();
    auto binding = service_manager.bind(DeviceHelper::get_name().data());
    if (!binding.is_valid()) {
        BROOKESIA_LOGE("Failed to bind Device service");
    } else {
        service_bindings_.push_back(std::move(binding));
    }
}

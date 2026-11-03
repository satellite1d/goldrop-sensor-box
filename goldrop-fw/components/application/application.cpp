#include "goldrop/application.hpp"
#include "goldrop/boot_validation.hpp"
#include "goldrop/config_store.hpp"
#include "goldrop/nvs_sampling_config_store.hpp"
#include "goldrop/sampling_schedule.hpp"
#include "goldrop/nvs_wifi_config_store.hpp"
#include "goldrop/usb_wifi_console.hpp"
#include "goldrop/wifi_manager.hpp"
#include "goldrop/hardware_sensors.hpp"
#include "goldrop/health.hpp"
#include "goldrop/diagnostic_log.hpp"
#include "goldrop/firmware_identity.hpp"
#include "goldrop/ota_manager.hpp"
#include "goldrop/system_clock.hpp"
#include "goldrop/tcp_transport.hpp"
#include "goldrop/server_config_store.hpp"
#include "goldrop/config_manager.hpp"
#include "goldrop/management_service.hpp"
#include "goldrop/durable_measurement_store.hpp"
#include "goldrop/sensor_service.hpp"
#include "goldrop/telemetry_service.hpp"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cinttypes>
#include <new>
#include <cstdio>
#include <algorithm>

#if ESP_IDF_VERSION_MAJOR != 6 || ESP_IDF_VERSION_MINOR != 1
#error "Goldrop targets ESP-IDF 6.1"
#endif

namespace goldrop {
namespace {
constexpr const char *kTag = "application";

DeviceId read_device_id() {
    DeviceId identity;
    const esp_err_t result = esp_read_mac(identity.mac.data(), ESP_MAC_WIFI_STA);
    if (result != ESP_OK || !identity.valid()) {
        ESP_LOGE(kTag, "Wi-Fi station MAC unavailable: %s; device_id will be null",
                 result == ESP_OK ? "invalid MAC" : esp_err_to_name(result));
        return {};
    }
    return identity;
}

void rollback_failed_startup() {
    BootValidation validation;
    const esp_err_t result = validation.begin();
    if (result == ESP_OK)
        validation.poll(false, 0, 0, 0);
    else
        ESP_LOGE("ota", "Could not inspect image after startup failure: %s", esp_err_to_name(result));
}
} // namespace

// Runtime allocated once at boot; adapters initialize peripherals on the first sample.
class Application::Runtime : public DeviceRuntimeControl {
  public:
    Runtime()
        : boot_id_((static_cast<uint64_t>(esp_random()) << 32) | esp_random()), device_id_(read_device_id()),
          sensors_(hardware_sensors_, clock_, boot_id_, device_id_), telemetry_(history_, transport_),
          ota_(device_id_), configuration_(device_id_, managed_config_store_, wifi_, transport_, this),
          management_(device_id_, configuration_),
          usb_config_(wifi_config_store_, wifi_, &server_config_store_, &transport_, &ota_config_store_,
                      &ota_, &configuration_) {}

    ~Runtime() {
        if (sensor_task_ != nullptr)
            vTaskDelete(sensor_task_);
        if (telemetry_task_ != nullptr)
            vTaskDelete(telemetry_task_);
        if (queue_ != nullptr)
            vQueueDelete(queue_);
    }

    bool set_sampling_interval(uint32_t interval_ms) override {
        if (!config_store_.save(interval_ms))
            return false;
        sampling_interval_ms_.store(interval_ms, std::memory_order_release);
        if (sensor_task_)
            xTaskNotifyGive(sensor_task_);
        ESP_LOGI("sampling", "Sampling interval set to %" PRIu32 " ms", interval_ms);
        return true;
    }
    bool request_firmware_check() override { return ota_.request_check(); }

    esp_err_t start() {
        const esp_err_t nvs_result = nvs_flash_init();
        if (nvs_result != ESP_OK) {
            ESP_LOGW("storage", "NVS unavailable: %s; using defaults, preserving flash",
                     esp_err_to_name(nvs_result));
        }
        core_initialized_ = nvs_result == ESP_OK;
        if (config_store_.load(config_) == ConfigLoadResult::Failed) {
            core_initialized_ = false;
            ESP_LOGE("sampling", "Saved interval unavailable; using default");
        }
        sampling_interval_ms_.store(config_.interval_ms);

        const esp_err_t validation_result = boot_validation_.begin();
        if (validation_result != ESP_OK) {
            core_initialized_ = false;
            ESP_LOGE("ota", "Could not inspect boot state: %s", esp_err_to_name(validation_result));
        }
        const bool reset_history = validation_result == ESP_OK && !boot_validation_.ready();
        if (reset_history)
            ESP_LOGW("history",
                     "First boot after OTA: deleting buffered measurements from telemetry partition");
        if (!history_.begin(reset_history)) {
            ESP_LOGE("history", "Flash journal unavailable; using bounded RAM fallback");
            if (reset_history) {
                core_initialized_ = false;
                ESP_LOGE("history", "OTA history reset failed; image will not pass boot validation");
            }
        } else if (reset_history)
            ESP_LOGI("history", "OTA history reset complete; new measurements start with empty buffer");
        const ConfigLoadResult wifi_loaded = wifi_config_store_.load_wifi(boot_wifi_config_);
        if (wifi_loaded != ConfigLoadResult::Loaded) {
            ESP_LOGW("wifi", "No readable Wi-Fi configuration; measurements continue, configure via USB");
        }
        const auto server_loaded = server_config_store_.load_server(boot_server_config_);
        if (server_loaded != ConfigLoadResult::Loaded) {
            ESP_LOGW("server", "No readable TCP server configuration; configure address/port via USB");
        }
        ConfigBundle legacy;
        if (wifi_loaded == ConfigLoadResult::Loaded) {
            legacy.wifi = boot_wifi_config_;
            legacy.present |= 1;
        }
        if (server_loaded == ConfigLoadResult::Loaded) {
            legacy.server = boot_server_config_;
            legacy.present |= 2;
        }
        if (!configuration_.begin(legacy, wifi_loaded != ConfigLoadResult::Failed &&
                                              server_loaded != ConfigLoadResult::Failed)) {
            ESP_LOGE("configuration",
                     "Combined configuration unavailable; preserving NVS; USB config recover required");
        }
        legacy.clear();
        boot_wifi_config_.clear();
        boot_server_config_ = {};
        validation_start_ms_ = clock_.uptime_ms();
        if (!ota_.begin())
            ESP_LOGE("ota", "OTA attempt storage unavailable; acquisition continues");
        OtaConfig ota_config;
        const auto ota_loaded = ota_config_store_.load_ota(ota_config);
        if (ota_loaded == ConfigLoadResult::Failed) {
            core_initialized_ = false;
            ESP_LOGE("ota", "Saved OTA configuration unreadable; flash preserved");
        }
        if (ota_loaded == ConfigLoadResult::Loaded && !ota_.configure_ota(ota_config, clock_.uptime_ms())) {
            core_initialized_ = false;
            ESP_LOGE("ota", "Cannot apply saved OTA configuration");
        }
        ESP_LOGI(kTag, "Firmware %s, IDF %s, reset reason %d, boot_id=%" PRIu64,
                 esp_app_get_description()->version, esp_get_idf_version(),
                 static_cast<int>(esp_reset_reason()), boot_id_);
        char mac[18];
        ESP_LOGI(kTag, "ID urzadzenia (MAC Wi-Fi STA): %s", device_id_.format(mac) ? mac : "niedostepny");
        ESP_LOGI(kTag, "Hardware sensors: SHT45 GPIO%d/%d, DS18B20 GPIO%d", settings::kShtSdaGpio,
                 settings::kShtSclGpio, settings::kDsDataGpio);
        ESP_LOGI(kTag, "Hardware revision: %s; measurements require durable backend receipts",
                 kHardwareRevision);
        ESP_LOGI(kTag, "Sampling every %" PRIu32 " ms; history capacity=%u; persistent=%s; restored=%u",
                 config_.interval_ms, static_cast<unsigned>(history_.capacity()),
                 history_.persistent() ? "true" : "false", static_cast<unsigned>(history_.size()));
        queue_ = xQueueCreate(settings::kAcquisitionQueueCapacity, sizeof(Measurement));
        if (queue_ == nullptr)
            return ESP_ERR_NO_MEM;
        if (xTaskCreate(telemetry_entry, "telemetry_task", settings::kTelemetryTaskStackBytes, this,
                        settings::kTelemetryPriority, &telemetry_task_) != pdPASS) {
            vQueueDelete(queue_);
            queue_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(sensor_entry, "sensor_task", settings::kTaskStackBytes, this,
                        settings::kSensorPriority, &sensor_task_) != pdPASS) {
            // Task is still waiting at the startup gate; no resource is held.
            vTaskDelete(telemetry_task_);
            telemetry_task_ = nullptr;
            vQueueDelete(queue_);
            queue_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        xTaskNotifyGive(telemetry_task_);
        xTaskNotifyGive(sensor_task_);
        return ESP_OK;
    }

  private:
    static void sensor_entry(void *context) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        static_cast<Runtime *>(context)->sensor_loop();
    }
    static void telemetry_entry(void *context) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        static_cast<Runtime *>(context)->telemetry_loop();
    }
    void sensor_loop() {

        for (;;) {
            if (pause_acquisition_.load(std::memory_order_acquire)) {
                acquisition_paused_.store(true, std::memory_order_release);
                while (pause_acquisition_.load(std::memory_order_acquire))
                    vTaskDelay(pdMS_TO_TICKS(settings::kServiceWakeMs));
                acquisition_paused_.store(false, std::memory_order_release);
            }
            const int64_t cycle_start = clock_.uptime_ms();
            const Measurement measurement = sensors_.sample();
            health_.acquired.fetch_add(1, std::memory_order_relaxed);
            if (!measurement.sht45_valid || !measurement.ds18b20_valid) {
                health_.sensor_failures.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGW("sensors", "Invalid reading: sht45=%s ds18b20=%s",
                         sensor_error_name(measurement.sht45_error),
                         sensor_error_name(measurement.ds18b20_error));
            }
            if (xQueueSend(queue_, &measurement, 0) != pdTRUE) {
                health_.acquisition_drops.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGW("sensors", "Acquisition queue full; dropped sequence=%" PRIu64,
                         measurement.sequence);
            }
            const auto interval = [&] {
                return sampling_interval_after_acquisition(
                    sampling_interval_ms_.load(std::memory_order_acquire),
                    health_.acquired.load(std::memory_order_relaxed),
                    configuration_trial_.load(std::memory_order_acquire));
            };
            int64_t next = cycle_start + interval();
            if (next <= clock_.uptime_ms())
                next = clock_.uptime_ms() + interval();
            for (;;) {
                const int64_t now = clock_.uptime_ms();
                if (now >= next)
                    break;
                const auto remaining = uint32_t(next - now);
                if (ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(remaining)))) {
                    if (pause_acquisition_.load(std::memory_order_acquire))
                        break;
                    next = clock_.uptime_ms() + interval();
                }
            }
        }
    }
    void telemetry_loop() {
        if (!usb_config_.begin())
            ESP_LOGW(kTag, "USB configuration unavailable; acquisition continues");
        if (!configuration_.apply_active(clock_.uptime_ms()))
            ESP_LOGE("configuration", "Cannot apply active configuration; acquisition continues");
        int64_t next_telemetry_poll_ms = 0;
        int64_t next_status_ms = 0;
        OtaState reported_ota = OtaState::NotConfigured;
        uint64_t reported_configuration = 0;
        bool history_was_persistent = history_.persistent();
        for (;;) {
            wifi_.poll(clock_.uptime_ms());
            const bool wifi_ready = wifi_.status().state == WifiState::Online;
            clock_.poll(wifi_ready);
            configuration_.set_busy(ota_.busy() || !boot_validation_.ready());
            usb_config_.poll(clock_.uptime_ms());
            transport_.poll(clock_.uptime_ms(), wifi_ready);
            Measurement written{};
            if (transport_.take_written(written)) {
                ESP_LOGI("server", "Pomiar #%" PRIu64 ": wyslany, oczekiwanie na zapis w bazie",
                         written.sequence);
            }
            Measurement confirmed{};
            if (transport_.take_acknowledged(confirmed) && telemetry_.acknowledge(confirmed)) {
                ESP_LOGI("server", "Pomiar #%" PRIu64 ": backend potwierdzil zapis w bazie",
                         confirmed.sequence);
            }
            Measurement measurement{};
            if (xQueueReceive(queue_, &measurement, pdMS_TO_TICKS(settings::kServiceWakeMs)) == pdTRUE) {
                const AppendResult stored = telemetry_.ingest(measurement);
                if (stored == AppendResult::DroppedOldest) {
                    health_.history_drops = history_.dropped();
                    ESP_LOGW("telemetry", "History rotated; oldest measurements dropped, total=%" PRIu32,
                             health_.history_drops);
                } else if (stored == AppendResult::Failed) {
                    ESP_LOGE("telemetry", "Measurement store append failed");
                }
                if (stored != AppendResult::Failed)
                    ++health_.buffered;
                log_measurement(measurement, stored != AppendResult::Failed);
                ESP_LOGI("telemetry", "Serwer TCP: %s", server_state_name(transport_.server_state()));
                log_health(health_, history_.size(), clock_.uptime_ms(), esp_get_free_heap_size(),
                           wifi_.status(), history_.capacity(), history_.persistent());
                ESP_LOGI("ota", "state=%s progress=%u error=%s", ota_state_name(ota_.state()),
                         ota_.progress(), ota_.last_error());
            }
            const int64_t now = clock_.uptime_ms();
            if (history_was_persistent && !history_.persistent()) {
                ESP_LOGE("history",
                         "Flash journal fault; RAM fallback active, preserved records=%u; OTA suspended",
                         static_cast<unsigned>(history_.unavailable_flash_records()));
                history_was_persistent = false;
            }
            if (now >= next_telemetry_poll_ms) {
                telemetry_.poll(now);
                next_telemetry_poll_ms = now + settings::kTelemetryWakeMs;
            }
            boot_validation_.poll(core_initialized_ && configuration_.configuration_healthy() &&
                                      history_.persistent(),
                                  health_.acquired.load(std::memory_order_relaxed), health_.buffered,
                                  clock_.uptime_ms() - validation_start_ms_);
            const bool boot_ready =
                boot_validation_.ready() && core_initialized_ && configuration_.configuration_healthy() &&
                now - validation_start_ms_ >= settings::kBootObservationMs &&
                health_.acquired.load(std::memory_order_relaxed) >= 2 && health_.buffered >= 2;
            configuration_.set_busy(ota_.busy() || !boot_validation_.ready());
            configuration_.poll(now, boot_ready && !ota_.busy());
            management_.poll(now, wifi_.status().state == WifiState::Online, clock_.synchronized(),
                             boot_ready && !ota_.busy());
            ota_.poll(now, wifi_ready, clock_.synchronized(), boot_ready, history_.persistent(),
                      configuration_.allows_ota(now) && !management_.active());
            const auto &configuration = configuration_.config_state();
            // Trials need fresh backend ACKs even when the saved interval is one hour.
            const bool testing = configuration.phase == ConfigPhase::Testing;
            if (configuration_trial_.exchange(testing, std::memory_order_acq_rel) != testing && sensor_task_)
                xTaskNotifyGive(sensor_task_);
            if (configuration.report_sequence != reported_configuration) {
                ESP_LOGI("configuration", "phase=%s result=%s error=%s active=%" PRIu64 " pending=%" PRIu64,
                         config_phase_name(configuration.phase), config_result_name(configuration.result),
                         config_error_name(configuration.error), configuration.active_revision,
                         configuration.pending_revision);
            }
            if (transport_.connected() && (now >= next_status_ms || ota_.state() != reported_ota ||
                                           configuration.report_sequence != reported_configuration)) {
                char status[settings::kJsonBufferSize], mac[18];
                if (device_id_.format(mac)) {
                    const int size = std::snprintf(
                        status, sizeof(status),
                        "{\"schema_version\":1,\"message_type\":\"status\",\"device_id\":\"%s\",\"firmware_"
                        "version\":\"%s\",\"hardware_revision\":\"%s\",\"ota_state\":\"%s\",\"ota_progress\":"
                        "%u,\"ota_error\":\"%s\",\"persistent_history\":%s,\"buffered\":%u,\"utc_"
                        "synchronized\":%s,\"delivery_confirmation\":\"backend_ack\",\"config_phase\":\"%s\","
                        "\"config_result\":\"%s\",\"config_error\":"
                        "\"%s\",\"config_revision\":\"%" PRIu64 "\",\"config_pending_revision\":\"%" PRIu64
                        "\",\"sampling_interval_ms\":%" PRIu32 "}",
                        mac, kFirmwareVersion, kHardwareRevision, ota_state_name(ota_.state()),
                        ota_.progress(), ota_.last_error(), history_.persistent() ? "true" : "false",
                        static_cast<unsigned>(history_.size()), clock_.synchronized() ? "true" : "false",
                        config_phase_name(configuration.phase), config_result_name(configuration.result),
                        config_error_name(configuration.error), configuration.active_revision,
                        configuration.pending_revision,
                        sampling_interval_ms_.load(std::memory_order_acquire));
                    if (size > 0 && static_cast<std::size_t>(size) < sizeof(status) &&
                        transport_.try_send_status({status, static_cast<std::size_t>(size)})) {
                        next_status_ms = now + 30'000;
                        reported_ota = ota_.state();
                    }
                }
            }
            reported_configuration = configuration.report_sequence;
            if (ota_.state() == OtaState::AwaitingRestart) {
                if (!pause_acquisition_.exchange(true, std::memory_order_acq_rel) && sensor_task_)
                    xTaskNotifyGive(sensor_task_);
                if (acquisition_paused_.load(std::memory_order_acquire) &&
                    uxQueueMessagesWaiting(queue_) == 0 && history_.persistent())
                    ota_.allow_restart();
            } else if (ota_.state() == OtaState::ReadyToReboot) {
                ESP_LOGI("ota", "Measurements persisted; rebooting into verified image");
                esp_restart();
            } else
                pause_acquisition_.store(false, std::memory_order_release);
        }
    }

    NvsSamplingConfigStore config_store_;
    std::atomic<bool> configuration_trial_{false};
    std::atomic<uint32_t> sampling_interval_ms_{settings::kSamplingIntervalMs};
    SamplingConfig config_;
    SystemClock clock_;
    const uint64_t boot_id_;
    const DeviceId device_id_;
    HardwareSensors hardware_sensors_;
    SensorService sensors_;
    DurableMeasurementStore history_;
    TcpTransport transport_;
    TelemetryService telemetry_;
    NvsWifiConfigStore wifi_config_store_;
    WifiCredentials boot_wifi_config_;
    WifiManager wifi_;
    NvsServerConfigStore server_config_store_;
    ServerConfig boot_server_config_;
    NvsOtaConfigStore ota_config_store_;
    OtaManager ota_;
    NvsManagedConfigStore managed_config_store_;
    ConfigManager configuration_;
    ManagementService management_;
    UsbWifiConsole usb_config_;
    Health health_;
    BootValidation boot_validation_;
    bool core_initialized_{};
    int64_t validation_start_ms_{};
    QueueHandle_t queue_{};
    TaskHandle_t sensor_task_{};
    TaskHandle_t telemetry_task_{};
    std::atomic<bool> pause_acquisition_{false}, acquisition_paused_{false};
};

Application::Application() = default;
Application::~Application() = default;

esp_err_t Application::start() {
    if (runtime_)
        return ESP_ERR_INVALID_STATE;
    runtime_.reset(new (std::nothrow) Runtime);
    if (!runtime_) {
        rollback_failed_startup();
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = runtime_->start();
    if (result != ESP_OK) {
        runtime_.reset();
        rollback_failed_startup();
    }
    return result;
}
} // namespace goldrop

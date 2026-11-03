#include "goldrop/diagnostic_log.hpp"
#include "goldrop/health.hpp"
#include "goldrop/measurement.hpp"
#include "goldrop/network_control.hpp"
#include "goldrop/settings.hpp"
#include "goldrop/utc_timestamp.hpp"
#include "esp_log.h"
#include <cinttypes>

namespace goldrop {
namespace {
const char *sensor_error_description(SensorError error) {
    switch (error) {
    case SensorError::None:
        return "brak bledu";
    case SensorError::Disconnected:
        return "czujnik niepodlaczony";
    case SensorError::Timeout:
        return "przekroczony czas oczekiwania";
    case SensorError::Crc:
        return "blad CRC";
    case SensorError::InvalidValue:
        return "nieprawidlowa wartosc";
    case SensorError::NotInitialized:
        return "czujnik niezainicjalizowany";
    case SensorError::Communication:
        return "blad komunikacji";
    }
    return "nieznany blad";
}

const char *wifi_state_description(WifiState state) {
    switch (state) {
    case WifiState::Unconfigured:
        return "brak konfiguracji sieci";
    case WifiState::Starting:
        return "uruchamianie";
    case WifiState::Connecting:
        return "laczenie z siecia";
    case WifiState::WaitingIp:
        return "oczekiwanie na adres IP";
    case WifiState::Online:
        return "polaczone";
    case WifiState::Backoff:
        return "oczekiwanie na ponowna probe";
    case WifiState::Stopping:
        return "zatrzymywanie";
    }
    return "nieznany stan";
}
} // namespace

void log_measurement(const Measurement &measurement, bool buffered) {
    constexpr const char *tag = "telemetry";
    ESP_LOGI(tag, "---------- Pomiar #%" PRIu64 " ----------", measurement.sequence);
    char mac[18];
    ESP_LOGI(tag, "ID urzadzenia (MAC Wi-Fi STA): %s",
             measurement.device_id.format(mac) ? mac : "niedostepny");
    if (measurement.sht45_valid) {
        ESP_LOGI(tag, "Temperatura SHT45: %.3f C", measurement.temperature_sht_c);
        ESP_LOGI(tag, "Wilgotnosc SHT45:  %.3f %%", measurement.humidity_rh);
    } else {
        ESP_LOGI(tag, "Temperatura SHT45: brak odczytu");
        ESP_LOGI(tag, "Wilgotnosc SHT45:  brak odczytu");
        ESP_LOGW(tag, "Stan SHT45: %s", sensor_error_description(measurement.sht45_error));
    }
    if (measurement.ds18b20_valid) {
        ESP_LOGI(tag, "Temperatura DS18B20: %.3f C", measurement.temperature_ds18_c);
    } else {
        ESP_LOGI(tag, "Temperatura DS18B20: brak odczytu");
        ESP_LOGW(tag, "Stan DS18B20: %s", sensor_error_description(measurement.ds18b20_error));
    }
    ESP_LOGI(tag, "Czas pomiaru od startu: %.3f s", measurement.uptime_ms / 1000.0);
    char utc[kUtcTimestampCapacity];
    if (measurement.timestamp_valid && format_utc_timestamp(measurement.timestamp_ms, utc, sizeof(utc))) {
        ESP_LOGI(tag, "Czas pomiaru UTC: %s", utc);
    } else {
        ESP_LOGI(tag, "Czas UTC: brak synchronizacji");
    }
    ESP_LOGI(tag, "ID uruchomienia: %" PRIu64, measurement.boot_id);
    ESP_LOGI(tag, "Zapis w buforze pomiarow: %s", buffered ? "OK" : "blad");
}

void log_health(const Health &health, std::size_t buffered, int64_t uptime_ms, uint32_t free_heap,
                const NetworkStatus &network, std::size_t capacity, bool persistent) {
    constexpr const char *tag = "health";
    ESP_LOGI(tag, "Czas pracy: %.3f s", uptime_ms / 1000.0);
    ESP_LOGI(tag, "Bufor %s: %u / %u pomiarow", persistent ? "flash" : "awaryjny RAM",
             static_cast<unsigned>(buffered), static_cast<unsigned>(capacity));
    ESP_LOGI(tag, "Wykonane pomiary: %" PRIu32, health.acquired.load(std::memory_order_relaxed));
    ESP_LOGI(tag, "Utracone pomiary (kolejka): %" PRIu32,
             health.acquisition_drops.load(std::memory_order_relaxed));
    ESP_LOGI(tag, "Utracone pomiary (bufor historii): %" PRIu32, health.history_drops);
    ESP_LOGI(tag, "Wolna pamiec RAM: %" PRIu32 " B", free_heap);
    ESP_LOGI(tag, "Wi-Fi: %s", wifi_state_description(network.state));
    ESP_LOGI(tag, "Adres IP: %s", network.state == WifiState::Online ? network.ip : "brak");
    if (network.rssi_valid) {
        ESP_LOGI(tag, "Sygnal Wi-Fi (RSSI): %d dBm", network.rssi_dbm);
    } else {
        ESP_LOGI(tag, "Sygnal Wi-Fi (RSSI): niedostepny");
    }
}
} // namespace goldrop

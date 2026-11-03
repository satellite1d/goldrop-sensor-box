#include "goldrop/durable_measurement_store.hpp"
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace goldrop {
namespace {
bool report(esp_err_t e) {
    if (e == ESP_OK)
        return true;
    ESP_LOGE("history", "Flash error: %s", esp_err_to_name(e));
    return false;
}
} // namespace
bool PartitionFlashIo::begin() {
    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
                                          "telemetry");
    return partition_ && !partition_->encrypted && partition_->size == 786432;
}
std::size_t PartitionFlashIo::size() const {
    return partition_ ? partition_->size : 0;
}
bool PartitionFlashIo::read(std::size_t p, void *b, std::size_t n) {
    return partition_ && report(esp_partition_read(partition_, p, b, n));
}
bool PartitionFlashIo::write(std::size_t p, const void *b, std::size_t n) {
    return partition_ && report(esp_partition_write(partition_, p, b, n));
}
bool PartitionFlashIo::erase(std::size_t p, std::size_t n) {
    return partition_ && report(esp_partition_erase_range(partition_, p, n));
}
void PartitionFlashIo::yield() {
    vTaskDelay(1);
}
} // namespace goldrop

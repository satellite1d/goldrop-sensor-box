#pragma once
#include "goldrop/flash_journal.hpp"
#include "goldrop/ram_measurement_store.hpp"
#include "goldrop/settings.hpp"
#include "esp_partition.h"
namespace goldrop {
class PartitionFlashIo final : public FlashIo {
  public:
    bool begin();
    std::size_t size() const override;
    bool read(std::size_t, void *, std::size_t) override;
    bool write(std::size_t, const void *, std::size_t) override;
    bool erase(std::size_t, std::size_t) override;
    void yield() override;

  private:
    const esp_partition_t *partition_{};
};
class DurableMeasurementStore final : public MeasurementStore {
  public:
    DurableMeasurementStore() : flash_(io_) {}
    bool begin(bool reset_after_update = false) {
        return io_.begin() && (reset_after_update ? flash_.reset_after_update() : flash_.mount());
    }
    AppendResult append(const Measurement &m) override {
        if (flash_.persistent()) {
            const auto result = flash_.append(m);
            if (result != AppendResult::Failed)
                return result;
        }
        const auto result = ram_.append(m);
        if (result == AppendResult::DroppedOldest)
            ++ram_drops_;
        return result;
    }
    bool peek(Measurement &m) const override {
        return flash_.persistent() && flash_.size() ? flash_.peek(m) : ram_.peek(m);
    }
    bool acknowledge(const Measurement &m) override {
        return flash_.persistent() && flash_.size() ? flash_.acknowledge(m) : ram_.acknowledge(m);
    }
    std::size_t size() const override { return (flash_.persistent() ? flash_.size() : 0) + ram_.size(); }
    std::size_t unavailable_flash_records() const { return flash_.persistent() ? 0 : flash_.size(); }
    bool persistent() const override { return flash_.persistent() && ram_.size() == 0; }
    std::size_t capacity() const {
        return flash_.persistent() ? flash_.capacity() : settings::kRamHistoryCapacity;
    }
    uint32_t dropped() const { return flash_.dropped() + ram_drops_; }

  private:
    PartitionFlashIo io_;
    FlashJournal flash_;
    RamMeasurementStore<settings::kRamHistoryCapacity> ram_;
    uint32_t ram_drops_{};
};
} // namespace goldrop

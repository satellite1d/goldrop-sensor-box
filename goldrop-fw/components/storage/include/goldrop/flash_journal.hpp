#pragma once
#include "goldrop/measurement_store.hpp"
#include <array>
namespace goldrop {
class FlashIo {
  public:
    virtual ~FlashIo() = default;
    virtual std::size_t size() const = 0;
    virtual bool read(std::size_t, void *, std::size_t) = 0;
    virtual bool write(std::size_t, const void *, std::size_t) = 0;
    virtual bool erase(std::size_t, std::size_t) = 0;
    virtual void yield() {}
};
// Explicit little-endian v1 format, 4 KiB sectors, 50 committed 80-byte records per sector.
class FlashJournal final : public MeasurementStore {
  public:
    explicit FlashJournal(FlashIo &io) : io_(io) {}
    bool mount();
    // Destructive: only at first boot of a newly installed OTA image, before acquisition.
    bool reset_after_update();
    AppendResult append(const Measurement &) override;
    bool peek(Measurement &m) const override {
        if (!healthy_ || !head_valid_)
            return false;
        m = head_;
        return true;
    }
    bool acknowledge(const Measurement &) override;
    std::size_t size() const override { return count_; }
    bool persistent() const override { return healthy_; }
    std::size_t capacity() const { return sectors_ * kRecords; }
    uint32_t dropped() const { return dropped_; }
    static constexpr std::size_t kSectorSize = 4096, kHeader = 32, kRecordSize = 80, kRecords = 50,
                                 kMaxSectors = 192;

  private:
    struct Sector {
        uint64_t generation{};
        unsigned pending{}, used{};
    };
    bool open_sector(unsigned);
    bool find_head(bool advance_current = false);
    bool read_record(unsigned, unsigned, std::array<uint8_t, kRecordSize> &);
    bool failed() {
        healthy_ = false;
        return false;
    }
    FlashIo &io_;
    std::array<Sector, kMaxSectors> info_{};
    unsigned sectors_{}, writer_{}, head_sector_{}, head_slot_{};
    uint64_t generation_{};
    std::size_t count_{};
    uint32_t dropped_{};
    bool healthy_{}, head_valid_{};
    Measurement head_{};
};
} // namespace goldrop

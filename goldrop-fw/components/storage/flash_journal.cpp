#include "goldrop/flash_journal.hpp"
#include "goldrop/binary_codec.hpp"
#include <cstring>
#include <limits>
namespace goldrop {
namespace {
constexpr uint32_t kCommit = 0x5aa55aa5;
void put_float(uint8_t *p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    binary::put32(p, v);
}
float get_float(const uint8_t *p) {
    uint32_t v = binary::get32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
bool decode(const uint8_t *b, Measurement &m) {
    if (std::memcmp(b, "GDM1", 4) || b[4] != 1 || b[5] > 7 || b[6] > uint8_t(SensorError::Communication) ||
        b[7] > uint8_t(SensorError::Communication) || binary::get32(b + 64) != binary::crc32(b, 64) ||
        binary::get32(b + 68) != kCommit)
        return false;
    m = {};
    m.sequence = binary::get64(b + 8);
    m.boot_id = binary::get64(b + 16);
    m.uptime_ms = static_cast<int64_t>(binary::get64(b + 24));
    m.timestamp_ms = static_cast<int64_t>(binary::get64(b + 32));
    m.temperature_sht_c = get_float(b + 40);
    m.humidity_rh = get_float(b + 44);
    m.temperature_ds18_c = get_float(b + 48);
    std::memcpy(m.device_id.mac.data(), b + 52, 6);
    m.timestamp_valid = b[5] & 1;
    m.sht45_valid = b[5] & 2;
    m.ds18b20_valid = b[5] & 4;
    m.sht45_error = static_cast<SensorError>(b[6]);
    m.ds18b20_error = static_cast<SensorError>(b[7]);
    return true;
}
} // namespace
bool FlashJournal::read_record(unsigned sector, unsigned slot, std::array<uint8_t, kRecordSize> &b) {
    return io_.read(sector * kSectorSize + kHeader + slot * kRecordSize, b.data(), b.size()) || failed();
}
bool FlashJournal::reset_after_update() {
    healthy_ = false;
    head_valid_ = false;
    count_ = 0;
    if (io_.size() % kSectorSize || io_.size() / kSectorSize < 2 ||
        io_.size() / kSectorSize > kMaxSectors)
        return false;
    for (std::size_t offset = 0; offset < io_.size(); offset += kSectorSize) {
        if (!io_.erase(offset, kSectorSize))
            return false;
        io_.yield();
    }
    return mount();
}
bool FlashJournal::mount() {
    healthy_ = true;
    head_valid_ = false;
    count_ = 0;
    generation_ = 0;
    info_ = {};
    dropped_ = 0;
    if (io_.size() % kSectorSize || io_.size() / kSectorSize < 2 || io_.size() / kSectorSize > kMaxSectors)
        return failed();
    sectors_ = static_cast<unsigned>(io_.size() / kSectorSize);
    for (unsigned s = 0; s < sectors_; ++s) {
        std::array<uint8_t, kHeader> h;
        if (!io_.read(s * kSectorSize, h.data(), h.size()))
            return failed();
        if (binary::erased(h.data(), h.size()))
            continue;
        if (binary::get32(h.data() + 28) == kCommit &&
            (std::memcmp(h.data(), "GDLG", 4) || h[4] != 1 || h[5] != kRecordSize ||
             binary::get32(h.data() + 24) != binary::crc32(h.data(), 24)))
            return failed();
        if (std::memcmp(h.data(), "GDLG", 4) == 0 && h[4] != 1 && h[4] != 0xff)
            return failed(); // Unknown format is preserved.
        if (std::memcmp(h.data(), "GDLG", 4) || h[4] != 1 || h[5] != kRecordSize ||
            binary::get32(h.data() + 28) != kCommit ||
            binary::get32(h.data() + 24) != binary::crc32(h.data(), 24))
            continue; // Uncommitted rotation header.
        auto &sector = info_[s];
        sector.generation = binary::get64(h.data() + 8);
        if (!sector.generation)
            return failed();
        for (unsigned earlier = 0; earlier < s; ++earlier)
            if (info_[earlier].generation == sector.generation)
                return failed();
        if (sector.generation > generation_) {
            generation_ = sector.generation;
            writer_ = s;
        }
        for (unsigned i = 0; i < kRecords; ++i) {
            std::array<uint8_t, kRecordSize> b;
            if (!read_record(s, i, b))
                return false;
            if (!binary::erased(b.data(), b.size()))
                sector.used = i + 1;
            Measurement m;
            if (binary::get32(b.data() + 68) == kCommit && !decode(b.data(), m))
                return failed();
            if (decode(b.data(), m) && binary::get32(b.data() + 72) != 0) {
                ++sector.pending;
                ++count_;
            }
        }
        io_.yield();
    }
    if (!generation_) {
        // Initialization can recover a torn empty header, but never erase possible history.
        for (unsigned s = 0; s < sectors_; ++s)
            for (unsigned i = 0; i < kRecords; ++i) {
                std::array<uint8_t, kRecordSize> b;
                if (!read_record(s, i, b))
                    return false;
                if (!binary::erased(b.data(), b.size()))
                    return failed();
                if (i == kRecords - 1)
                    io_.yield();
            }
        writer_ = 0;
    }
    return find_head();
}
bool FlashJournal::open_sector(unsigned sector) {
    if (generation_ == std::numeric_limits<uint64_t>::max())
        return failed();
    dropped_ += info_[sector].pending;
    count_ -= info_[sector].pending;
    if (!io_.erase(sector * kSectorSize, kSectorSize))
        return failed();
    std::array<uint8_t, kHeader> h{};
    std::memcpy(h.data(), "GDLG", 4);
    h[4] = 1;
    h[5] = kRecordSize;
    binary::put64(h.data() + 8, ++generation_);
    binary::put32(h.data() + 24, binary::crc32(h.data(), 24));
    if (!io_.write(sector * kSectorSize, h.data(), 28))
        return failed();
    binary::put32(h.data() + 28, kCommit);
    if (!io_.write(sector * kSectorSize + 28, h.data() + 28, 4))
        return failed();
    info_[sector] = {generation_, 0, 0};
    writer_ = sector;
    return true;
}
AppendResult FlashJournal::append(const Measurement &m) {
    if (!healthy_)
        return AppendResult::Failed;
    const auto before = dropped_;
    if (!generation_ || info_[writer_].used == kRecords) {
        if (!open_sector(generation_ ? (writer_ + 1) % sectors_ : writer_))
            return AppendResult::Failed;
    }
    const unsigned slot = info_[writer_].used++;
    std::array<uint8_t, kRecordSize> b{};
    std::memcpy(b.data(), "GDM1", 4);
    b[4] = 1;
    b[5] = (m.timestamp_valid ? 1 : 0) | (m.sht45_valid ? 2 : 0) | (m.ds18b20_valid ? 4 : 0);
    b[6] = uint8_t(m.sht45_error);
    b[7] = uint8_t(m.ds18b20_error);
    binary::put64(b.data() + 8, m.sequence);
    binary::put64(b.data() + 16, m.boot_id);
    binary::put64(b.data() + 24, static_cast<uint64_t>(m.uptime_ms));
    binary::put64(b.data() + 32, static_cast<uint64_t>(m.timestamp_ms));
    put_float(b.data() + 40, m.temperature_sht_c);
    put_float(b.data() + 44, m.humidity_rh);
    put_float(b.data() + 48, m.temperature_ds18_c);
    std::memcpy(b.data() + 52, m.device_id.mac.data(), 6);
    binary::put32(b.data() + 64, binary::crc32(b.data(), 64));
    const auto offset = writer_ * kSectorSize + kHeader + slot * kRecordSize;
    if (!io_.write(offset, b.data(), 68)) {
        failed();
        return AppendResult::Failed;
    }
    binary::put32(b.data() + 68, kCommit);
    if (!io_.write(offset + 68, b.data() + 68, 4)) {
        failed();
        return AppendResult::Failed;
    }
    ++info_[writer_].pending;
    ++count_;
    if (!head_valid_ || before != dropped_)
        if (!find_head())
            return AppendResult::Failed;
    return before != dropped_ ? AppendResult::DroppedOldest : AppendResult::Stored;
}
bool FlashJournal::find_head(bool advance_current) {
    const unsigned previous_sector = head_valid_ ? head_sector_ : sectors_;
    const unsigned next_slot = head_slot_ + 1;
    head_valid_ = false;
    unsigned sector = sectors_;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (unsigned s = 0; s < sectors_; ++s)
        if (info_[s].pending && info_[s].generation < oldest) {
            oldest = info_[s].generation;
            sector = s;
        }
    if (sector == sectors_)
        return true;
    // Only acknowledge() advances within the current sector. Mount and rotation
    // start at zero; the on-flash format and corruption checks remain unchanged.
    for (unsigned i = advance_current && sector == previous_sector ? next_slot : 0; i < info_[sector].used;
         ++i) {
        std::array<uint8_t, kRecordSize> b;
        if (!read_record(sector, i, b))
            return false;
        if (binary::get32(b.data() + 72) != 0 && decode(b.data(), head_)) {
            head_sector_ = sector;
            head_slot_ = i;
            head_valid_ = true;
            return true;
        }
    }
    return failed();
}
bool FlashJournal::acknowledge(const Measurement &m) {
    if (!healthy_ || !head_valid_ || !same_measurement(head_, m))
        return false;
    const uint32_t ack = 0;
    if (!io_.write(head_sector_ * kSectorSize + kHeader + head_slot_ * kRecordSize + 72, &ack, 4))
        return failed();
    --info_[head_sector_].pending;
    --count_;
    return find_head(true);
}
} // namespace goldrop

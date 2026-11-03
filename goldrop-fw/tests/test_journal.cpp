#include "goldrop/flash_journal.hpp"
#include "goldrop/binary_codec.hpp"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
namespace {
using namespace goldrop;
void require(bool v, int line) {
    if (!v) {
        std::fprintf(stderr, "FAIL journal test line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
class Flash final : public FlashIo {
  public:
    explicit Flash(unsigned sectors = 4) : bytes(sectors * 4096, 0xff) {}
    std::vector<uint8_t> bytes;
    int writes_before_cut{-1};
    unsigned partial{};
    unsigned reads{}, erases{}, yields{};
    int erases_before_cut{-1};
    void yield() override { ++yields; }
    std::size_t size() const override { return bytes.size(); }
    bool read(std::size_t p, void *b, std::size_t n) override {
        ++reads;
        if (p + n > size())
            return false;
        std::memcpy(b, bytes.data() + p, n);
        return true;
    }
    bool write(std::size_t p, const void *b, std::size_t n) override {
        if (p + n > size())
            return false;
        const auto *source = static_cast<const uint8_t *>(b);
        bool cut = writes_before_cut == 0;
        if (writes_before_cut > 0)
            --writes_before_cut;
        const auto count = cut ? std::min<std::size_t>(n, partial) : n;
        for (std::size_t i = 0; i < count; ++i) {
            if ((bytes[p + i] & source[i]) != source[i])
                return false;
            bytes[p + i] &= source[i];
        }
        return !cut;
    }
    bool erase(std::size_t p, std::size_t n) override {
        ++erases;
        if (erases_before_cut == 0)
            return false;
        if (erases_before_cut > 0)
            --erases_before_cut;
        if (p % 4096 || n % 4096 || p + n > size())
            return false;
        std::fill(bytes.begin() + p, bytes.begin() + p + n, 0xff);
        return true;
    }
};
Measurement record(uint64_t sequence) {
    Measurement m;
    m.sequence = sequence;
    m.boot_id = 42;
    m.timestamp_valid = true;
    m.timestamp_ms = 1'792'953'730'123 + static_cast<int64_t>(sequence) * 10'000;
    m.device_id.mac = {2, 0, 0, 0, 0, 1};
    m.temperature_sht_c = 22.5;
    m.humidity_rh = 45;
    m.sht45_valid = true;
    m.sht45_error = SensorError::None;
    return m;
}
} // namespace
void test_flash_journal() {
    using namespace goldrop;
    // OTA reset removes only this journal; an ordinary mount preserves new records.
    Flash ota_flash;
    FlashJournal old(ota_flash);
    REQUIRE(old.mount());
    REQUIRE(old.append(record(100)) == AppendResult::Stored);
    FlashJournal updated(ota_flash);
    ota_flash.yields = 0;
    REQUIRE(updated.reset_after_update());
    REQUIRE(updated.size() == 0 && updated.persistent() && ota_flash.yields >= 4);
    REQUIRE(updated.append(record(101)) == AppendResult::Stored);
    FlashJournal rebooted(ota_flash);
    Measurement retained;
    REQUIRE(rebooted.mount() && rebooted.size() == 1 && rebooted.peek(retained));
    REQUIRE(retained.sequence == 101);
    ota_flash.erases_before_cut = 0;
    const auto before = ota_flash.bytes;
    REQUIRE(!rebooted.reset_after_update() && !rebooted.persistent());
    REQUIRE(ota_flash.bytes == before);
    ota_flash.erases_before_cut = 1;
    REQUIRE(!rebooted.reset_after_update() && !rebooted.persistent());
    ota_flash.erases_before_cut = -1;
    REQUIRE(rebooted.reset_after_update() && rebooted.size() == 0);

    Flash flash;
    FlashJournal journal(flash);
    REQUIRE(journal.mount() && journal.capacity() == 200);
    REQUIRE(journal.append(record(1)) == AppendResult::Stored);
    REQUIRE(journal.append(record(2)) == AppendResult::Stored);
    Measurement m;
    REQUIRE(journal.peek(m) && m.sequence == 1 && m.temperature_sht_c == 22.5F);
    FlashJournal reboot(flash);
    REQUIRE(reboot.mount() && reboot.size() == 2 && reboot.peek(m) && m.sequence == 1);
    REQUIRE(m.timestamp_valid && m.timestamp_ms == 1'792'953'740'123);
    auto wrong = m;
    ++wrong.device_id.mac[5];
    REQUIRE(!reboot.acknowledge(wrong));
    REQUIRE(reboot.acknowledge(m));
    FlashJournal again(flash);
    REQUIRE(again.mount() && again.size() == 1 && again.peek(m) && m.sequence == 2);
    REQUIRE(again.acknowledge(m));
    REQUIRE(again.size() == 0);
    // Cuts in header body, header commit, record body, record commit, including partially programmed words.
    for (int cut = 0; cut < 4; ++cut)
        for (unsigned partial : {0u, 1u, 3u, 17u}) {
            Flash interrupted;
            interrupted.writes_before_cut = cut;
            interrupted.partial = partial;
            FlashJournal original(interrupted);
            REQUIRE(original.mount());
            original.append(record(1));
            interrupted.writes_before_cut = -1;
            FlashJournal recovered(interrupted);
            REQUIRE(recovered.mount());
            const auto prior = recovered.size();
            REQUIRE(prior <= 1);
            REQUIRE(recovered.append(record(2)) == AppendResult::Stored);
            REQUIRE(recovered.size() == prior + 1);
        }
    Flash ack_flash;
    FlashJournal ack(ack_flash);
    REQUIRE(ack.mount() && ack.append(record(9)) == AppendResult::Stored && ack.peek(m));
    ack_flash.writes_before_cut = 0;
    ack_flash.partial = 2;
    REQUIRE(!ack.acknowledge(m));
    ack_flash.writes_before_cut = -1;
    FlashJournal ack_reboot(ack_flash);
    REQUIRE(ack_reboot.mount() && ack_reboot.size() == 1); // Duplicate allowed, loss forbidden.
    Flash full(192);
    FlashJournal history(full);
    REQUIRE(history.mount() && history.capacity() == 9600);
    for (uint64_t i = 1; i <= 8640; ++i)
        REQUIRE(history.append(record(i)) == AppendResult::Stored);
    FlashJournal day(full);
    REQUIRE(day.mount() && day.size() == 8640 && day.peek(m) && m.sequence == 1);
    for (uint64_t i = 8641; i <= 9600; ++i)
        REQUIRE(day.append(record(i)) == AppendResult::Stored);
    REQUIRE(day.append(record(9601)) == AppendResult::DroppedOldest && day.dropped() == 50 && day.peek(m) &&
            m.sequence == 51);
    // Power loss after erasing an old sector, before committing the replacement header.
    for (uint64_t i = 9602; i <= 9650; ++i)
        REQUIRE(day.append(record(i)) == AppendResult::Stored);
    full.writes_before_cut = 0;
    full.partial = 2;
    REQUIRE(day.append(record(9651)) == AppendResult::Failed);
    full.writes_before_cut = -1;
    FlashJournal wrapped(full);
    REQUIRE(wrapped.mount() && wrapped.size() == 9550 && wrapped.peek(m) && m.sequence == 101);
    for (uint64_t i = 101; i <= 9650; ++i) {
        REQUIRE(wrapped.peek(m) && m.sequence == i);
        REQUIRE(wrapped.acknowledge(m));
    }
    FlashJournal emptied(full);
    REQUIRE(emptied.mount() && emptied.size() == 0);
    // A torn write may leave a hole between valid records. Advancing the head
    // must skip that hole while preserving the next complete measurement.
    Flash hole;
    FlashJournal before_hole(hole);
    REQUIRE(before_hole.mount() && before_hole.append(record(1)) == AppendResult::Stored);
    hole.writes_before_cut = 0;
    hole.partial = 17;
    REQUIRE(before_hole.append(record(2)) == AppendResult::Failed);
    hole.writes_before_cut = -1;
    FlashJournal after_hole(hole);
    REQUIRE(after_hole.mount() && after_hole.append(record(3)) == AppendResult::Stored);
    REQUIRE(after_hole.peek(m) && m.sequence == 1 && after_hole.acknowledge(m));
    REQUIRE(after_hole.peek(m) && m.sequence == 3 && after_hole.acknowledge(m));
    REQUIRE(after_hole.size() == 0);
    Flash unknown;
    unknown.bytes[0] = 'X';
    unknown.bytes[32] = 0;
    FlashJournal preserved(unknown);
    const auto snapshot = unknown.bytes;
    REQUIRE(!preserved.mount() && unknown.bytes == snapshot);
    Flash future;
    FlashJournal initial(future);
    REQUIRE(initial.mount() && initial.append(record(1)) == AppendResult::Stored);
    future.bytes[4] = 2;
    FlashJournal unsupported(future);
    const auto future_bytes = future.bytes;
    REQUIRE(!unsupported.mount() && future.bytes == future_bytes);
    for (unsigned offset : {8u, 24u, 32u + 40u, 32u + 64u}) {
        Flash corrupt;
        FlashJournal original(corrupt);
        REQUIRE(original.mount() && original.append(record(1)) == AppendResult::Stored);
        corrupt.bytes[offset] ^= 1;
        const auto damaged = corrupt.bytes;
        FlashJournal recovery(corrupt);
        REQUIRE(!recovery.mount() && !recovery.persistent() && corrupt.bytes == damaged);
    }
    // Two committed sectors cannot have the same generation, even with valid CRCs.
    Flash duplicate;
    FlashJournal original(duplicate);
    REQUIRE(original.mount() && original.append(record(1)) == AppendResult::Stored);
    std::copy_n(duplicate.bytes.begin(), 4096, duplicate.bytes.begin() + 4096);
    const auto duplicated = duplicate.bytes;
    FlashJournal invalid_order(duplicate);
    REQUIRE(!invalid_order.mount() && duplicate.bytes == duplicated);
    // Draining a sector must not reread all previously acknowledged records.
    Flash drain_flash;
    FlashJournal drain(drain_flash);
    REQUIRE(drain.mount());
    for (uint64_t i = 1; i <= 150; ++i)
        REQUIRE(drain.append(record(i)) == AppendResult::Stored);
    drain_flash.reads = 0;
    const auto erases = drain_flash.erases;
    for (uint64_t i = 1; i <= 150; ++i) {
        REQUIRE(drain.peek(m) && m.sequence == i);
        REQUIRE(drain.acknowledge(m));
    }
    REQUIRE(drain.size() == 0 && drain_flash.erases == erases);
    std::printf("Journal drain: %u reads for 150 receipts\n", drain_flash.reads);
    REQUIRE(drain_flash.reads <= 150);
}

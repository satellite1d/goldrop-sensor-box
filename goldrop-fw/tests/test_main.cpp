#include "goldrop/backoff.hpp"
#include "goldrop/boot_self_test.hpp"
#include "goldrop/config_store.hpp"
#include "goldrop/fake_sensors.hpp"
#include "goldrop/ram_measurement_store.hpp"
#include "goldrop/sensor_service.hpp"
#include "goldrop/serialization.hpp"
#include "goldrop/telemetry_service.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

void test_real_sensor_protocols();
void test_network_and_configuration();
void test_server_transport_and_configuration();
void test_ota_features();
void test_flash_journal();
void test_measurement_timestamps();

namespace {
void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define REQUIRE(expression) require((expression), #expression, __LINE__)

using namespace goldrop;

Measurement record(uint64_t sequence, uint64_t boot_id = 42) {
    Measurement m{};
    m.sequence = sequence;
    m.boot_id = boot_id;
    return m;
}

class FakeClock final : public Clock {
public:
    int64_t now{100};
    bool synchronized{};
    int64_t uptime_ms() const override { return now; }
    bool utc_ms(int64_t& result) const override {
        if (!synchronized) return false;
        result = 1'800'000'000'000 + now;
        return true;
    }
};

class InvalidSensors final : public SensorSource {
public:
    SensorReadings read() override {
        return {std::numeric_limits<float>::quiet_NaN(), 45.0F,
                std::numeric_limits<float>::infinity(), SensorError::None, SensorError::None};
    }
};

class FakeTransport final : public TelemetryTransport {
public:
    bool online{};
    SendResult result{SendResult::Accepted};
    unsigned sends{};
    Measurement last{};
    bool connected() const override { return online; }
    SendResult try_send(const Measurement& m) override {
        ++sends;
        last = m;
        return result;
    }
};

void test_fifo_overflow_and_identity() {
    RamMeasurementStore<2> store;
    Measurement out{};
    REQUIRE(!store.peek(out));
    REQUIRE(!store.acknowledge(record(1)));
    REQUIRE(!store.persistent());
    REQUIRE(store.append(record(1)) == AppendResult::Stored);
    REQUIRE(store.append(record(2)) == AppendResult::Stored);
    REQUIRE(store.append(record(3)) == AppendResult::DroppedOldest);
    REQUIRE(store.size() == 2);
    REQUIRE(store.peek(out) && out.sequence == 2);
    REQUIRE(!store.acknowledge(record(1)));
    REQUIRE(!store.acknowledge(record(2, 43)));
    REQUIRE(store.acknowledge(record(2)));
    REQUIRE(store.peek(out) && out.sequence == 3);
    REQUIRE(store.acknowledge(record(3)));
    REQUIRE(store.size() == 0);
    // Repeated wraparound after draining must preserve FIFO order.
    for (uint64_t n = 4; n < 40; ++n) {
        REQUIRE(store.append(record(n)) == AppendResult::Stored);
        REQUIRE(store.peek(out) && out.sequence == n);
        REQUIRE(store.acknowledge(out));
    }
}

void test_offline_retry_and_backend_ack() {
    RamMeasurementStore<8> store;
    FakeTransport transport;
    TelemetryService service(store, transport, 100);
    for (uint64_t n = 1; n <= 3; ++n) {
        REQUIRE(service.ingest(record(n)) == AppendResult::Stored);
        service.poll(static_cast<int64_t>(n * 1'000));
    }
    REQUIRE(store.size() == 3 && transport.sends == 0);
    transport.online = true;
    service.poll(4'000);
    REQUIRE(transport.sends == 1 && transport.last.sequence == 1);
    REQUIRE(store.size() == 3);  // Enqueue acceptance does not discard a measurement.
    REQUIRE(!service.acknowledge(record(2)));
    REQUIRE(!service.acknowledge(record(1, 99)));
    service.poll(4'099);
    REQUIRE(transport.sends == 1);
    service.poll(4'100);
    REQUIRE(transport.sends == 2 && transport.last.sequence == 1);
    transport.online = false;
    service.poll(9'000);
    REQUIRE(store.size() == 3 && transport.sends == 2);
    transport.online = true;
    service.poll(9'001);
    REQUIRE(transport.sends == 3 && transport.last.sequence == 1);
    REQUIRE(service.acknowledge(record(1)));
    REQUIRE(store.size() == 2);
    REQUIRE(!service.acknowledge(record(1)));
    service.poll(9'002);
    REQUIRE(transport.last.sequence == 2);
}

void test_rejected_send_and_late_ack_after_overflow() {
    RamMeasurementStore<1> store;
    FakeTransport transport;
    transport.online = true;
    transport.result = SendResult::Busy;
    TelemetryService service(store, transport, 100);
    service.ingest(record(1));
    service.poll(0);
    REQUIRE(store.size() == 1);
    REQUIRE(!service.acknowledge(record(1)));
    transport.result = SendResult::Accepted;
    service.poll(1);
    REQUIRE(service.ingest(record(2)) == AppendResult::DroppedOldest);
    REQUIRE(service.acknowledge(record(1)));
    Measurement out{};
    REQUIRE(store.peek(out) && out.sequence == 2);
    service.poll(2);
    REQUIRE(transport.last.sequence == 2);
    REQUIRE(service.acknowledge(record(2)));
    REQUIRE(store.size() == 0);
}

void test_acquisition_validity_and_time() {
    FakeClock clock;
    FakeSensors fake(2);
    SensorService service(fake, clock, 77);
    const Measurement first = service.sample();
    REQUIRE(first.sequence == 1 && first.boot_id == 77 && first.uptime_ms == 100);
    REQUIRE(first.sht45_valid && first.ds18b20_valid);
    REQUIRE(!first.timestamp_valid && first.timestamp_ms == 0);
    clock.now = 10'100;
    clock.synchronized = true;
    const Measurement second = service.sample();
    REQUIRE(second.sequence == 2 && second.timestamp_valid);
    REQUIRE(second.timestamp_ms == 1'800'000'010'100);
    REQUIRE(second.sht45_valid && !second.ds18b20_valid);
    REQUIRE(second.ds18b20_error == SensorError::Disconnected);
    REQUIRE(second.temperature_ds18_c == 0);  // No stale valid value.
    REQUIRE(service.sample().ds18b20_valid);  // Temporary failure recovers.
    InvalidSensors invalid;
    SensorService invalid_service(invalid, clock, 78);
    const Measurement bad = invalid_service.sample();
    REQUIRE(!bad.sht45_valid && !bad.ds18b20_valid);
    REQUIRE(bad.sht45_error == SensorError::InvalidValue);
    REQUIRE(bad.ds18b20_error == SensorError::InvalidValue);
}

void test_serialization() {
    FakeClock clock;
    FakeSensors fake;
    SensorService sensors(fake, clock, 42);
    Measurement m = sensors.sample();
    m.sequence = std::numeric_limits<uint64_t>::max();
    m.ds18b20_valid = false;
    m.ds18b20_error = SensorError::Crc;
    char output[settings::kJsonBufferSize];
    REQUIRE(serialize_measurement(m, output, sizeof(output)));
    REQUIRE(std::strstr(output, "\"device_id\":null") != nullptr);
    REQUIRE(std::strstr(output, "\"sequence\":\"18446744073709551615\"") != nullptr);
    REQUIRE(std::strstr(output, "\"timestamp_ms\":null") != nullptr);
    REQUIRE(std::strstr(output, "\"timestamp_utc\":null") != nullptr);
    REQUIRE(std::strstr(output, "\"temperature_ds18_c\":null") != nullptr);
    REQUIRE(std::strstr(output, "\"ds18b20_error\":\"crc\"") != nullptr);
    REQUIRE(std::strstr(output, "\"temperature_sht_c\":22.500") != nullptr);
    const std::size_t required = std::strlen(output) + 1;
    REQUIRE(serialize_measurement(m, output, required));
    REQUIRE(!serialize_measurement(m, output, required - 1));
    REQUIRE(output[0] == '\0');
    REQUIRE(!serialize_measurement(m, nullptr, 10));
    REQUIRE(!serialize_measurement(m, output, 0));
    m.temperature_sht_c = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(serialize_measurement(m, output, sizeof(output)));
    REQUIRE(std::strstr(output, "\"sht45_valid\":false") != nullptr);
    REQUIRE(std::strstr(output, "\"temperature_sht_c\":null") != nullptr);
    REQUIRE(std::strstr(output, "\"sht45_error\":\"invalid_value\"") != nullptr);
    m.temperature_sht_c = 130;
    m.ds18b20_error = SensorError::None;
    m.ds18b20_valid = true;
    m.temperature_ds18_c = -56;
    REQUIRE(serialize_measurement(m, output, sizeof(output)));
    REQUIRE(std::strstr(output, "\"sht45_error\":\"invalid_value\"") != nullptr);
    REQUIRE(std::strstr(output, "\"ds18b20_error\":\"invalid_value\"") != nullptr);
    REQUIRE(std::strstr(output, "\"temperature_ds18_c\":null") != nullptr);
}

void test_device_identity() {
    const DeviceId identity{{0x02, 0x00, 0x0a, 0x10, 0xef, 0xff}};
    char mac[18];
    REQUIRE(identity.valid() && identity.format(mac));
    REQUIRE(std::strcmp(mac, "02:00:0a:10:ef:ff") == 0);
    DeviceId invalid;
    REQUIRE(!invalid.valid() && !invalid.format(mac) && mac[0] == 0);
    invalid.mac.fill(0xff);
    REQUIRE(!invalid.valid());
    invalid.mac = {1, 0, 0, 0, 0, 1};
    REQUIRE(!invalid.valid());  // Multicast cannot identify a Wi-Fi station.
    FakeClock clock;
    FakeSensors fake;
    SensorService sensors(fake, clock, 42, identity);
    const Measurement sample = sensors.sample();
    REQUIRE(sample.device_id == identity);
    REQUIRE(sensors.sample().device_id == identity);
    char payload[settings::kJsonBufferSize];
    REQUIRE(serialize_measurement(sample, payload, sizeof(payload)));
    REQUIRE(std::strstr(payload, "\"device_id\":\"02:00:0a:10:ef:ff\"") != nullptr);
    REQUIRE(std::strstr(payload, "\"schema_version\":1") != nullptr);
    auto other_board = sample;
    ++other_board.device_id.mac[5];
    REQUIRE(!same_measurement(sample, other_board));
    SensorService rebooted(fake, clock, 43, identity);
    const auto after_reset = rebooted.sample();
    REQUIRE(after_reset.device_id == identity && !same_measurement(sample, after_reset));
    RamMeasurementStore<2> store;
    FakeTransport transport; transport.online = true;
    TelemetryService telemetry(store, transport);
    telemetry.ingest(sample); telemetry.poll(0);
    REQUIRE(!telemetry.acknowledge(other_board) && store.size() == 1);
    REQUIRE(telemetry.acknowledge(sample) && store.size() == 0);
}

void test_backoff_and_config_limits() {
    Backoff backoff;
    REQUIRE(backoff.next_delay_ms() == 1'000);
    REQUIRE(backoff.next_delay_ms() == 2'000);
    for (unsigned n = 0; n < 40; ++n) REQUIRE(backoff.next_delay_ms() <= 60'000);
    REQUIRE(backoff.next_delay_ms() == 60'000);
    backoff.reset();
    REQUIRE(backoff.next_delay_ms() == 1'000);
    Backoff large(std::numeric_limits<uint32_t>::max() - 1, std::numeric_limits<uint32_t>::max());
    large.next_delay_ms();
    REQUIRE(large.next_delay_ms() == std::numeric_limits<uint32_t>::max());
    SamplingConfig config;
    DefaultConfigStore store;
    REQUIRE(store.load(config) == ConfigLoadResult::Defaults);
    REQUIRE(config.valid() && config.interval_ms == 10'000);
    config.interval_ms = 9'999;
    REQUIRE(!config.valid());
    config.interval_ms = settings::kMaximumSamplingIntervalMs + 1;
    REQUIRE(!config.valid());
}

void test_boot_validation() {
    REQUIRE(boot_self_test(true, 0, 0, 0) == BootDecision::Wait);
    REQUIRE(boot_self_test(true, 1, 0, 10) == BootDecision::Wait);
    REQUIRE(boot_self_test(true, 1, 1, 10) == BootDecision::Wait);
    REQUIRE(boot_self_test(true, 2, 2, settings::kBootObservationMs) == BootDecision::Confirm);
    REQUIRE(boot_self_test(false, 1, 1, 10) == BootDecision::Rollback);
    REQUIRE(boot_self_test(true, 0, 0, settings::kBootValidationDeadlineMs - 1) == BootDecision::Wait);
    REQUIRE(boot_self_test(true, 0, 0, settings::kBootValidationDeadlineMs) == BootDecision::Rollback);
}
}  // namespace

int main() {
    test_fifo_overflow_and_identity();
    test_offline_retry_and_backend_ack();
    test_rejected_send_and_late_ack_after_overflow();
    test_acquisition_validity_and_time();
    test_serialization();
    test_device_identity();
    test_backoff_and_config_limits();
    test_boot_validation();
    test_real_sensor_protocols();
    test_network_and_configuration();
    test_server_transport_and_configuration();
    test_ota_features();
    test_flash_journal();
    test_measurement_timestamps();
    std::puts("PASS: 21 host test groups (timestamps, OTA, journal, identity, sensors, networking, provisioning)");
    return 0;
}

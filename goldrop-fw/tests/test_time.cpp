#include "goldrop/utc_timestamp.hpp"
#include "goldrop/sensor_service.hpp"
#include "goldrop/fake_sensors.hpp"
#include "goldrop/ram_measurement_store.hpp"
#include "goldrop/telemetry_service.hpp"
#include "goldrop/serialization.hpp"
#include "goldrop/settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace {
using namespace goldrop;
void require(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "FAIL time test line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
class SampleClock final : public goldrop::Clock {
  public:
    int64_t uptime{1000}, utc{1'792'953'730'123};
    bool synced{};
    int64_t uptime_ms() const override { return uptime; }
    bool utc_ms(int64_t &output) const override {
        if (!synced)
            return false;
        output = utc;
        return true;
    }
};
class Transport final : public TelemetryTransport {
  public:
    bool online{};
    unsigned sends{};
    Measurement sent;
    bool connected() const override { return online; }
    SendResult try_send(const Measurement &measurement) override {
        ++sends;
        sent = measurement;
        return SendResult::Accepted;
    }
};
} // namespace
void test_measurement_timestamps() {
    using namespace goldrop;
    char text[kUtcTimestampCapacity];
    REQUIRE(format_utc_timestamp(0, text, sizeof(text)));
    REQUIRE(std::strcmp(text, "1970-01-01T00:00:00.000Z") == 0);
    REQUIRE(format_utc_timestamp(999, text, sizeof(text)));
    REQUIRE(std::strcmp(text, "1970-01-01T00:00:00.999Z") == 0);
    REQUIRE(format_utc_timestamp(1000, text, sizeof(text)));
    REQUIRE(std::strcmp(text, "1970-01-01T00:00:01.000Z") == 0);
    REQUIRE(format_utc_timestamp(1'709'164'800'123, text, sizeof(text)));
    REQUIRE(std::strcmp(text, "2024-02-29T00:00:00.123Z") == 0);
    REQUIRE(format_utc_timestamp(253402300799999LL, text, sizeof(text)));
    REQUIRE(std::strcmp(text, "9999-12-31T23:59:59.999Z") == 0);
    REQUIRE(!format_utc_timestamp(-1, text, sizeof(text)) && text[0] == 0);
    REQUIRE(!format_utc_timestamp(std::numeric_limits<int64_t>::max(), text, sizeof(text)));
    REQUIRE(!format_utc_timestamp(0, nullptr, 25));
    REQUIRE(!format_utc_timestamp(0, text, sizeof(text) - 1) && text[0] == 0);
    REQUIRE(!format_utc_timestamp(0, text, 0));
    SampleClock clock;
    FakeSensors source;
    SensorService sensors(source, clock, 42);
    const auto before_sync = sensors.sample();
    clock.synced = true;
    const auto measured = sensors.sample();
    char json[settings::kJsonBufferSize];
    REQUIRE(serialize_measurement(measured, json, sizeof(json)));
    REQUIRE(std::strstr(json, "\"timestamp_ms\":1792953730123") != nullptr);
    REQUIRE(std::strstr(json, "\"timestamp_utc\":\"2026-10-25T18:42:10.123Z\"") != nullptr);
    RamMeasurementStore<4> history;
    Transport transport;
    TelemetryService telemetry(history, transport);
    REQUIRE(telemetry.ingest(measured) == AppendResult::Stored);
    clock.utc += 24 * 60 * 60 * 1000;
    clock.uptime += 24 * 60 * 60 * 1000;
    telemetry.poll(clock.uptime);
    REQUIRE(transport.sends == 0);
    transport.online = true;
    telemetry.poll(clock.uptime + 1);
    REQUIRE(transport.sends == 1 && transport.sent.timestamp_ms == measured.timestamp_ms);
    REQUIRE(serialize_measurement(transport.sent, json, sizeof(json)));
    REQUIRE(std::strstr(json, "2026-10-25T18:42:10.123Z") != nullptr);
    // Synchronizing later never fabricates a date for older unknown-time records.
    REQUIRE(serialize_measurement(before_sync, json, sizeof(json)));
    REQUIRE(std::strstr(json, "\"timestamp_ms\":null") && std::strstr(json, "\"timestamp_utc\":null"));
    auto invalid = measured;
    invalid.timestamp_ms = -1;
    REQUIRE(serialize_measurement(invalid, json, sizeof(json)));
    REQUIRE(std::strstr(json, "\"timestamp_ms\":null") && std::strstr(json, "\"timestamp_utc\":null"));
}

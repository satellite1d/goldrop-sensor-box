#include "goldrop/ds18b20.hpp"
#include "goldrop/sensor_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {
using namespace goldrop;
using namespace goldrop::sensor_protocol;
void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL sensor test line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define REQUIRE(expression) require((expression), #expression, __LINE__)

enum class Operation { Reset, Write, Read, Bit };
struct Step {
    Operation operation;
    std::vector<uint8_t> bytes;
    SensorError result{SensorError::None};
};
class ScriptedBus final : public OneWireBus {
public:
    std::deque<Step> steps;
    SensorError reset() override { return take(Operation::Reset).result; }
    SensorError write(const uint8_t* data, std::size_t size) override {
        const Step step = take(Operation::Write);
        REQUIRE(step.bytes == std::vector<uint8_t>(data, data + size));
        return step.result;
    }
    SensorError read(uint8_t* data, std::size_t size) override {
        const Step step = take(Operation::Read);
        if (step.result == SensorError::None) {
            REQUIRE(step.bytes.size() == size);
            std::copy(step.bytes.begin(), step.bytes.end(), data);
        }
        return step.result;
    }
    SensorError read_bit(uint8_t& bit) override {
        const Step step = take(Operation::Bit);
        if (step.result == SensorError::None) bit = step.bytes.at(0);
        return step.result;
    }
private:
    Step take(Operation operation) {
        REQUIRE(!steps.empty());
        Step step = steps.front();
        steps.pop_front();
        REQUIRE(step.operation == operation);
        return step;
    }
};

DsRom rom(uint8_t serial = 1, uint8_t family = 0x28) {
    DsRom value{family, serial, 2, 3, 4, 5, 6, 0};
    value[7] = ds_crc(value.data(), 7);
    return value;
}
DsScratchpad scratchpad(int16_t raw) {
    const uint16_t bits = static_cast<uint16_t>(raw);
    DsScratchpad value{static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
                      kDsAlarmHighMarker, kDsAlarmLowMarker, kDs12BitConfig, 0xFF, 0x0C, 0x10, 0};
    value[8] = ds_crc(value.data(), 8);
    return value;
}
void expect_rom(ScriptedBus& bus, const DsRom& value) {
    bus.steps.push_back({Operation::Reset, {}});
    bus.steps.push_back({Operation::Write, {0x33}});
    bus.steps.push_back({Operation::Read, {value.begin(), value.end()}});
}
void expect_command(ScriptedBus& bus, const DsRom& value, uint8_t command) {
    bus.steps.push_back({Operation::Reset, {}});
    std::vector<uint8_t> bytes{0x55};
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(command);
    bus.steps.push_back({Operation::Write, bytes});
}
void expect_begin(ScriptedBus& bus, const DsRom& value) {
    expect_rom(bus, value);
    expect_command(bus, value, 0x4E);
    bus.steps.push_back({Operation::Write, {0xA5, 0x5A, 0x7F}});
    expect_command(bus, value, 0x44);
    bus.steps.push_back({Operation::Bit, {0}});
}
void expect_finish(ScriptedBus& bus, const DsRom& value, const DsScratchpad& frame) {
    bus.steps.push_back({Operation::Bit, {1}});
    expect_rom(bus, value);
    expect_command(bus, value, 0xBE);
    bus.steps.push_back({Operation::Read, {frame.begin(), frame.end()}});
}

void test_sht_frames() {
    const uint8_t golden[] = {0xBE, 0xEF};
    REQUIRE(sht_crc(golden, 2) == 0x92);  // Sensirion datasheet CRC example.
    ShtFrame frame{0x80, 0x00, 0, 0x80, 0x00, 0};
    frame[2] = sht_crc(frame.data(), 2);
    frame[5] = sht_crc(frame.data() + 3, 2);
    float t = 123, rh = 123;
    REQUIRE(decode_sht(frame, t, rh) == SensorError::None);
    REQUIRE(std::fabs(t - 42.5013F) < 0.001F);
    REQUIRE(std::fabs(rh - 56.501F) < 0.001F);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        ShtFrame corrupt = frame;
        corrupt[i] ^= 1;
        t = rh = 123;
        REQUIRE(decode_sht(corrupt, t, rh) == SensorError::Crc);
        REQUIRE(t == 123 && rh == 123);
    }
    frame[3] = frame[4] = 0;
    frame[5] = sht_crc(frame.data() + 3, 2);
    REQUIRE(decode_sht(frame, t, rh) == SensorError::None && rh == 0);
    frame[3] = frame[4] = 0xFF;
    frame[5] = sht_crc(frame.data() + 3, 2);
    REQUIRE(decode_sht(frame, t, rh) == SensorError::None && rh == 100);
    frame[0] = frame[1] = 0xFF;
    frame[2] = sht_crc(frame.data(), 2);
    REQUIRE(decode_sht(frame, t, rh) == SensorError::InvalidValue);
}

void test_ds_frames() {
    const uint8_t golden[] = {'1','2','3','4','5','6','7','8','9'};
    REQUIRE(ds_crc(golden, sizeof(golden)) == 0xA1);  // CRC-8/MAXIM check vector.
    const DsScratchpad power_on{0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C};
    float t = 0;
    REQUIRE(decode_ds(power_on, t) == SensorError::None && t == 85);
    REQUIRE(validate_ds_rom(rom()) == SensorError::None);
    REQUIRE(validate_ds_rom(rom(1, 0x10)) == SensorError::InvalidValue);
    DsRom corrupt_rom = rom();
    corrupt_rom[1] ^= 1;
    REQUIRE(validate_ds_rom(corrupt_rom) == SensorError::Crc);
    REQUIRE(validate_ds_rom(DsRom{}) == SensorError::InvalidValue);
    for (const int16_t raw : {-880, -162, 0, 401, 1360, 2000}) {
        REQUIRE(decode_ds(scratchpad(raw), t) == SensorError::None);
        REQUIRE(t == raw / 16.0F);
    }
    const DsScratchpad frame = scratchpad(-162);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        DsScratchpad corrupt = frame;
        corrupt[i] ^= 1;
        t = 123;
        REQUIRE(decode_ds(corrupt, t) == SensorError::Crc && t == 123);
    }
    REQUIRE(decode_ds(scratchpad(-881), t) == SensorError::InvalidValue);
    REQUIRE(decode_ds(scratchpad(2001), t) == SensorError::InvalidValue);
    REQUIRE(decode_ds(DsScratchpad{}, t) == SensorError::InvalidValue);
    DsScratchpad low_resolution = scratchpad(407);
    low_resolution[4] = 0x1F;
    low_resolution[8] = ds_crc(low_resolution.data(), 8);
    REQUIRE(decode_ds(low_resolution, t) == SensorError::None && t == 25);
}

void test_ds_presence_and_conversion() {
    ScriptedBus bus;
    Ds18b20 sensor(bus);
    const DsRom identity = rom();
    float t = 123;
    REQUIRE(sensor.read_temperature(t) == SensorError::NotInitialized && t == 123);
    bus.steps.push_back({Operation::Reset, {}, SensorError::Disconnected});
    REQUIRE(sensor.begin_conversion() == SensorError::Disconnected);
    REQUIRE(sensor.read_temperature(t) == SensorError::NotInitialized);

    // Plug in after boot: no restart or driver allocation needed.
    expect_begin(bus, identity);
    expect_finish(bus, identity, scratchpad(401));
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::None && t == 25.0625F);
    REQUIRE(sensor.read_temperature(t) == SensorError::NotInitialized);  // Cannot reuse old sample.

    // Disconnect during conversion, then reconnect on a later cycle.
    expect_begin(bus, identity);
    bus.steps.push_back({Operation::Bit, {1}});
    bus.steps.push_back({Operation::Reset, {}, SensorError::Disconnected});
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    t = 123;
    REQUIRE(sensor.read_temperature(t) == SensorError::Disconnected && t == 123);
    expect_begin(bus, identity);
    expect_finish(bus, identity, scratchpad(-162));
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::None && t == -10.125F);

    // Replacement during conversion must not supply a stale measurement.
    expect_begin(bus, identity);
    bus.steps.push_back({Operation::Bit, {1}});
    expect_rom(bus, rom(2));
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::Disconnected);

    expect_begin(bus, identity);
    bus.steps.push_back({Operation::Bit, {0}});
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::Timeout);

    // A reset/power-on scratchpad is rejected even when CRC and ROM are valid.
    DsScratchpad power_on = scratchpad(1360);
    power_on[2] = 0x4B;
    power_on[3] = 0x46;
    power_on[8] = ds_crc(power_on.data(), 8);
    expect_begin(bus, identity);
    expect_finish(bus, identity, power_on);
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    t = 123;
    REQUIRE(sensor.read_temperature(t) == SensorError::InvalidValue && t == 123);
    expect_begin(bus, identity);
    expect_finish(bus, identity, scratchpad(1360));
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::None && t == 85);  // Real 85 C accepted.

    DsScratchpad corrupt = scratchpad(401);
    corrupt[8] ^= 1;
    expect_begin(bus, identity);
    expect_finish(bus, identity, corrupt);
    REQUIRE(sensor.begin_conversion() == SensorError::None);
    REQUIRE(sensor.read_temperature(t) == SensorError::Crc);

    // Failed conversion command cannot turn a previous value into a fresh sample.
    expect_begin(bus, identity);
    bus.steps.back().bytes = {1};
    REQUIRE(sensor.begin_conversion() == SensorError::InvalidValue);
    REQUIRE(sensor.read_temperature(t) == SensorError::NotInitialized);
    bus.steps.push_back({Operation::Reset, {}, SensorError::Timeout});
    REQUIRE(sensor.begin_conversion() == SensorError::Timeout);
    expect_rom(bus, rom(1, 0x10));
    REQUIRE(sensor.begin_conversion() == SensorError::InvalidValue);
    REQUIRE(bus.steps.empty());
}
}  // namespace

void test_real_sensor_protocols() {
    test_sht_frames();
    test_ds_frames();
    test_ds_presence_and_conversion();
}

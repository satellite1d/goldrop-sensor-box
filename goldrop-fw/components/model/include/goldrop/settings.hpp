#pragma once

#include <cstddef>
#include <cstdint>

namespace goldrop::settings {
inline constexpr int kShtSdaGpio = 4;
inline constexpr int kShtSclGpio = 5;
inline constexpr int kDsDataGpio = 10;
inline constexpr uint8_t kShtI2cAddress = 0x44;
inline constexpr uint32_t kShtI2cSpeedHz = 100'000;
inline constexpr int kI2cTimeoutMs = 100;
inline constexpr uint32_t kShtConversionMs = 10;
inline constexpr uint32_t kDsConversionMs = 800;
inline constexpr uint32_t kSamplingIntervalMs = 10'000;
inline constexpr uint32_t kMinimumSamplingIntervalMs = 10'000;
inline constexpr uint32_t kMaximumSamplingIntervalMs = 3'600'000;
inline constexpr std::size_t kAcquisitionQueueCapacity = 32;
inline constexpr std::size_t kRamHistoryCapacity = 360;
inline constexpr std::size_t kJsonBufferSize = 768;
inline constexpr uint32_t kTelemetryWakeMs = 1'000;
inline constexpr uint32_t kServiceWakeMs = 50; // USB input and queued Wi-Fi events share telemetry_task.
inline constexpr char kNtpServer[] = "pool.ntp.org";
inline constexpr int64_t kTimeSyncRetryMs = 60'000;
inline constexpr int64_t kWifiRssiPollMs = 5'000;
inline constexpr int64_t kEarliestSynchronizedUnixSeconds = 1'704'067'200; // 2024-01-01 UTC
inline constexpr int64_t kLatestSynchronizedUnixSeconds = 4'102'444'800;   // 2100-01-01 UTC
inline constexpr int64_t kAckTimeoutMs = 30'000;
inline constexpr int64_t kTcpConnectTimeoutMs = 5'000;
inline constexpr int64_t kDnsTimeoutMs = 10'000;
inline constexpr std::size_t kMaxServerHostLength = 253;
inline constexpr std::size_t kProvisionLineCapacity = 448;
inline constexpr int64_t kTcpWriteTimeoutMs = 5'000;
inline constexpr int64_t kTcpStableConnectionMs = 30'000;
inline constexpr int kTcpKeepaliveIdleSeconds = 10;
inline constexpr int kTcpKeepaliveIntervalSeconds = 5;
inline constexpr int kTcpKeepaliveProbeCount = 3;
inline constexpr int64_t kBootValidationDeadlineMs = 45'000;
inline constexpr int64_t kBootObservationMs = 20'000;
inline constexpr int kOtaIoTimeoutMs = 5'000;
inline constexpr int64_t kOtaOperationTimeoutMs = 180'000;
inline constexpr int64_t kOtaQuiesceTimeoutMs = 20'000;
inline constexpr uint32_t kOtaTaskStackBytes = 16'384;
inline constexpr uint32_t kTaskStackBytes = 4'096;
inline constexpr uint32_t kTelemetryTaskStackBytes = 8'192;
inline constexpr uint32_t kManagementTaskStackBytes = 8'192;
inline constexpr int kManagementIoTimeoutMs = 4'000;
inline constexpr int64_t kManagementOperationTimeoutUs = 15'000'000;
inline constexpr uint32_t kSensorPriority = 5;
inline constexpr uint32_t kTelemetryPriority = 4;
} // namespace goldrop::settings

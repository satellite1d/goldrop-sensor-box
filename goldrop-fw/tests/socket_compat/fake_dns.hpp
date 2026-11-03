#pragma once
#include <cstddef>
#include <string>
namespace fake_dns {
enum class Mode { Async, Cached, Failed, QueueFull };
void mode(Mode value);
void run_tcpip();
void finish(bool success = true);
std::size_t pending();
const std::string& last_host();
}

#pragma once
#include <cstdint>
#include <string_view>

namespace goldrop {
// One outstanding DNS request. Cancellation discards its result; lwIP may finish later.
class AsyncDns {
public:
    AsyncDns();
    ~AsyncDns();
    AsyncDns(const AsyncDns&) = delete;
    AsyncDns& operator=(const AsyncDns&) = delete;
    bool start(std::string_view host);
    void cancel();
    // Address is in network byte order; zero denotes lookup failure.
    bool take_result(uint32_t& ipv4);
private:
    struct State;
    State* state_;
};
}  // namespace goldrop

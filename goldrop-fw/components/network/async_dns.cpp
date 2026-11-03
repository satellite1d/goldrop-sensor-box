#include "goldrop/async_dns.hpp"
#include "goldrop/server_config.hpp"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/inet.h"
#include <atomic>
#include <cstring>
#include <new>

namespace goldrop {
struct AsyncDns::State {
    std::atomic<unsigned> references{1};
    std::atomic<bool> active{false}, ready{false};
    std::atomic<uint32_t> generation{0}, completed_generation{0}, address{0};
    struct Request {
        State* owner;
        uint32_t generation;
        char host[settings::kMaxServerHostLength + 1]{};
    };
    static void release(State* state) {
        if (state->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state;
    }
    static void complete(const char*, const ip_addr_t* address, void* context) {
        auto* request = static_cast<Request*>(context);
        auto* state = request->owner;
        uint32_t value = address && IP_IS_V4(address) ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
        const auto first_octet = ntohl(value) >> 24;
        if (first_octet == 0 || first_octet >= 224) value = 0;
        state->address.store(value, std::memory_order_relaxed);
        state->completed_generation.store(request->generation, std::memory_order_relaxed);
        state->ready.store(true, std::memory_order_release);
        state->active.store(false, std::memory_order_release);
        release(state);
        delete request;
    }
    static void resolve(void* context) {
        auto* request = static_cast<Request*>(context);
        ip_addr_t address{};
        // Raw DNS APIs run only on the TCP/IP thread, including cached lookups.
        const err_t result = dns_gethostbyname_addrtype(request->host, &address, complete,
                                                       request, LWIP_DNS_ADDRTYPE_IPV4);
        if (result == ERR_OK) complete(nullptr, &address, request);
        else if (result != ERR_INPROGRESS) complete(nullptr, nullptr, request);
    }
};

AsyncDns::AsyncDns() : state_(new (std::nothrow) State) {}
AsyncDns::~AsyncDns() {
    cancel();
    if (state_) State::release(state_);
}
bool AsyncDns::start(std::string_view host) {
    if (!state_ || !ServerConfig::valid_host(host)) return false;
    bool expected = false;
    if (!state_->active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    state_->ready.store(false, std::memory_order_release);
    const uint32_t generation = state_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* request = new (std::nothrow) State::Request{state_, generation, {}};
    if (!request) { state_->active.store(false, std::memory_order_release); return false; }
    std::memcpy(request->host, host.data(), host.size());
    state_->references.fetch_add(1, std::memory_order_relaxed);
    // This enqueue is nonblocking even if the TCP/IP mailbox is full.
    if (tcpip_try_callback(State::resolve, request) != ERR_OK) {
        State::complete(nullptr, nullptr, request);
        return false;
    }
    return true;
}
void AsyncDns::cancel() {
    if (!state_) return;
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
    state_->ready.store(false, std::memory_order_release);
}
bool AsyncDns::take_result(uint32_t& ipv4) {
    if (!state_ || !state_->ready.exchange(false, std::memory_order_acq_rel)) return false;
    if (state_->completed_generation.load(std::memory_order_relaxed) !=
        state_->generation.load(std::memory_order_acquire)) return false;
    ipv4 = state_->address.load(std::memory_order_relaxed);
    return true;
}
}  // namespace goldrop

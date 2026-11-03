#include "fake_dns.hpp"
#include "lwip/tcpip.h"
#include <arpa/inet.h>
#include <cassert>
#include <deque>

namespace {
fake_dns::Mode current_mode{fake_dns::Mode::Async};
bool in_tcpip{};
std::string host;
struct Work { tcpip_callback_fn function; void* context; };
struct Lookup { dns_found_callback function; void* context; };
std::deque<Work> work;
std::deque<Lookup> lookups;
}
err_t tcpip_try_callback(tcpip_callback_fn function, void* context) {
    if (current_mode == fake_dns::Mode::QueueFull) return ERR_MEM;
    work.push_back({function, context});
    return ERR_OK;
}
err_t dns_gethostbyname_addrtype(const char* name, ip_addr_t* address, dns_found_callback found,
                                void* context, int family) {
    assert(in_tcpip && family == LWIP_DNS_ADDRTYPE_IPV4);
    host = name;
    if (current_mode == fake_dns::Mode::Failed) return ERR_ARG;
    if (current_mode == fake_dns::Mode::Cached) { address->address = htonl(0x7f000001); return ERR_OK; }
    lookups.push_back({found, context});
    return ERR_INPROGRESS;
}
namespace fake_dns {
void mode(Mode value) { current_mode = value; }
void run_tcpip() {
    in_tcpip = true;
    while (!work.empty()) { const auto entry = work.front(); work.pop_front(); entry.function(entry.context); }
    in_tcpip = false;
}
void finish(bool success) {
    in_tcpip = true;
    while (!lookups.empty()) {
        const auto entry = lookups.front(); lookups.pop_front();
        const ip_addr_t address{htonl(0x7f000001)};
        entry.function(host.c_str(), success ? &address : nullptr, entry.context);
    }
    in_tcpip = false;
}
std::size_t pending() { return lookups.size() + work.size(); }
const std::string& last_host() { return host; }
}

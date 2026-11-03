#include "goldrop/async_dns.hpp"
#include "fake_dns.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool ok, int line) { if (!ok) { std::fprintf(stderr, "FAIL DNS test line %d\n", line); std::exit(1); } }
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
}
int main() {
    using namespace goldrop;
    AsyncDns dns;
    uint32_t address = 0;
    REQUIRE(!dns.start("https://invalid"));
    REQUIRE(dns.start("r.limanowa.org"));
    REQUIRE(!dns.start("other.example"));  // One outstanding query per instance.
    REQUIRE(!dns.take_result(address));
    fake_dns::run_tcpip();
    REQUIRE(fake_dns::last_host() == "r.limanowa.org");
    REQUIRE(!dns.take_result(address));
    fake_dns::finish();
    REQUIRE(dns.take_result(address) && ntohl(address) == 0x7f000001);
    REQUIRE(!dns.take_result(address));
    REQUIRE(dns.start("r.limanowa.org")); fake_dns::run_tcpip(); dns.cancel();
    REQUIRE(!dns.start("new.example"));
    fake_dns::finish();
    REQUIRE(!dns.take_result(address));  // Stale result after Wi-Fi loss/reconfiguration.
    REQUIRE(dns.start("new.example")); fake_dns::run_tcpip(); fake_dns::finish(false);
    REQUIRE(dns.take_result(address) && address == 0);
    fake_dns::mode(fake_dns::Mode::Cached);
    REQUIRE(dns.start("cached.example")); fake_dns::run_tcpip();
    REQUIRE(dns.take_result(address) && address != 0);
    fake_dns::mode(fake_dns::Mode::Failed);
    REQUIRE(dns.start("failed.example")); fake_dns::run_tcpip();
    REQUIRE(dns.take_result(address) && address == 0);
    fake_dns::mode(fake_dns::Mode::QueueFull);
    REQUIRE(!dns.start("busy.example")); dns.cancel();
    REQUIRE(!dns.take_result(address));
    fake_dns::mode(fake_dns::Mode::Async);
    { AsyncDns temporary; REQUIRE(temporary.start("queued.example")); }
    fake_dns::run_tcpip(); fake_dns::finish();  // Callback after the owner was destroyed.
    { AsyncDns temporary; REQUIRE(temporary.start("pending.example")); fake_dns::run_tcpip(); }
    fake_dns::finish();
    REQUIRE(fake_dns::pending() == 0);
    std::puts("PASS: async DNS, cached/error paths, cancellation, bounded requests, late callbacks");
}

#include "goldrop/wifi_manager.hpp"
#include "fake_wifi.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace goldrop;
int main() {
    WifiManager wifi;
    WifiCredentials credentials;
    assert(credentials.set_ssid("test-network", 12));
    assert(credentials.set_password("test-password", 13));
    assert(wifi.configure(credentials, 0));
    wifi.poll(0);
    assert(fake_wifi::connects == 1);
    fake_wifi::online();
    wifi.poll(1);
    for (unsigned i = 0; i < 1000; ++i) {
        const auto status = wifi.status();
        assert(status.state == WifiState::Online && status.rssi_valid && status.rssi_dbm == -50);
    }
    std::printf("Wi-Fi: %u driver RSSI calls for 1000 status reads\n", fake_wifi::ap_reads);
    assert(fake_wifi::ap_reads == 1);
    fake_wifi::rssi = -70;
    wifi.poll(4999);
    assert(wifi.status().rssi_dbm == -50);
    wifi.poll(5001);
    assert(wifi.status().rssi_dbm == -70 && fake_wifi::ap_reads == 2);
    fake_wifi::ap_available = false;
    wifi.poll(10001);
    assert(!wifi.status().rssi_valid && fake_wifi::ap_reads == 3);
    fake_wifi::ap_available = true;
    wifi_event_sta_disconnected_t lost{1};
    fake_wifi::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &lost);
    wifi.poll(10002);
    assert(wifi.status().state == WifiState::Backoff && !wifi.status().rssi_valid);
    wifi.poll(11002);
    fake_wifi::online();
    wifi.poll(11003);
    assert(wifi.status().rssi_valid && fake_wifi::ap_reads == 4);
    assert(wifi.configure(credentials, 11004));
    assert(wifi.status().state == WifiState::Stopping && !wifi.status().rssi_valid);
    wifi.poll(11004);
    fake_wifi::online();
    wifi.poll(11005);
    assert(wifi.status().state == WifiState::Online && wifi.status().rssi_valid && fake_wifi::ap_reads == 5);
    wifi.disable(11006);
    wifi.poll(11006);
    assert(wifi.status().state == WifiState::Unconfigured && !wifi.status().rssi_valid);
}

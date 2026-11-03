#pragma once
#include "goldrop/server_config.hpp"
#include <array>
#include <string_view>
namespace goldrop {
inline constexpr std::size_t kOtaUrlCapacity = 384;
struct HttpUrl {
    ServerConfig origin;
    bool tls{};
    bool parse(std::string_view url) {
        const bool secure = url.substr(0, 8) == "https://";
        const std::size_t prefix = secure ? 8 : 7;
        if ((!secure && url.substr(0, 7) != "http://") || url.size() <= prefix ||
            url.size() >= kOtaUrlCapacity)
            return false;
        for (char c : url)
            if (c < 0x21 || c > 0x7e || c == '\\' || c == '#' || c == '@')
                return false;
        const auto end = url.find('/', prefix);
        const auto authority = url.substr(prefix, end == url.npos ? url.size() - prefix : end - prefix);
        const auto colon = authority.find(':');
        const auto host = authority.substr(0, colon);
        ServerConfig parsed;
        if (!parsed.set_address(host.data(), host.size()))
            return false;
        parsed.port = secure ? 443 : 80;
        if (colon != authority.npos &&
            !parsed.set_port(authority.data() + colon + 1, authority.size() - colon - 1))
            return false;
        origin = parsed;
        tls = secure;
        return true;
    }
};
struct HttpsUrl : HttpUrl {
    bool parse(std::string_view url) { return url.substr(0, 8) == "https://" && HttpUrl::parse(url); }
};
inline bool same_http_origin(std::string_view a, std::string_view b) {
    HttpUrl x, y;
    return x.parse(a) && y.parse(b) && x.tls == y.tls && x.origin.host == y.origin.host &&
           x.origin.port == y.origin.port;
}
inline bool same_https_origin(std::string_view a, std::string_view b) {
    HttpsUrl x, y;
    return x.parse(a) && y.parse(b) && x.origin.host == y.origin.host && x.origin.port == y.origin.port;
}
struct OtaConfig {
    std::array<char, kOtaUrlCapacity> manifest_url{};
    std::array<char, 17> channel{"stable"};
    uint32_t interval_seconds{900};
    bool enabled{};
    bool set_url(std::string_view url) {
        HttpUrl parsed;
        if (!parsed.parse(url))
            return false;
        manifest_url.fill(0);
        std::memcpy(manifest_url.data(), url.data(), url.size());
        return true;
    }
    bool set_channel(std::string_view text) {
        if (text.empty() || text.size() > 16)
            return false;
        for (char c : text)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return false;
        channel.fill(0);
        std::memcpy(channel.data(), text.data(), text.size());
        return true;
    }
    bool valid() const {
        const auto *end = static_cast<const char *>(std::memchr(manifest_url.data(), 0, manifest_url.size()));
        const auto *ch = static_cast<const char *>(std::memchr(channel.data(), 0, channel.size()));
        if (!end || !ch || interval_seconds < 60 || interval_seconds > 86400)
            return false;
        OtaConfig test;
        return test.set_url({manifest_url.data(), std::size_t(end - manifest_url.data())}) &&
               test.set_channel({channel.data(), std::size_t(ch - channel.data())});
    }
};
} // namespace goldrop

#pragma once
#include <cstdint>
using err_t = int;
inline constexpr err_t ERR_OK = 0, ERR_INPROGRESS = -5, ERR_ARG = -16, ERR_MEM = -1;
inline constexpr int LWIP_DNS_ADDRTYPE_IPV4 = 0;
struct ip_addr_t { uint32_t address{}; };
inline bool IP_IS_V4(const ip_addr_t*) { return true; }
inline const ip_addr_t* ip_2_ip4(const ip_addr_t* address) { return address; }
inline uint32_t ip4_addr_get_u32(const ip_addr_t* address) { return address->address; }
using dns_found_callback = void (*)(const char*, const ip_addr_t*, void*);
err_t dns_gethostbyname_addrtype(const char*, ip_addr_t*, dns_found_callback, void*, int);

#pragma once
#include <cstdio>
// Host-only adapter for compiling the real socket transport in tests.
#define ESP_LOGI(tag, ...) do { (void)(tag); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) ESP_LOGI(tag, __VA_ARGS__)

#pragma once
#include "esp_err.h"
#include <cstdint>
using esp_event_base_t = const char *;
using esp_event_handler_instance_t = void *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, esp_event_handler_t, void *,
                                              esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t, esp_event_handler_instance_t);

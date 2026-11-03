#include "goldrop/application.hpp"
#include "esp_log.h"

extern "C" void app_main() {
    // Static storage owns all dependencies for the lifetime of the tasks.
    static goldrop::Application application;
    const esp_err_t result = application.start();
    if (result != ESP_OK) {
        ESP_LOGE("main", "Application startup failed: %s; no restart loop",
                 esp_err_to_name(result));
    }
}

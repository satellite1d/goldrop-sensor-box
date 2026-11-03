#pragma once
#include "esp_err.h"
#include <memory>

namespace goldrop {
class Application {
public:
    esp_err_t start();
    Application();
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
private:
    class Runtime;
    std::unique_ptr<Runtime> runtime_;
};
}  // namespace goldrop

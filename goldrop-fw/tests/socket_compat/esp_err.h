#pragma once
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NOT_FOUND=1;
inline const char* esp_err_to_name(esp_err_t e){return e==ESP_OK?"ESP_OK":"test_error";}

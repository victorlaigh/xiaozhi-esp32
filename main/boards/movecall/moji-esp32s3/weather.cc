#include "weather.h"
#include "weather_icon_font.h"
// 天气icon

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <material_symbols.h>
#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MojiWeather"

namespace Weather {
namespace {

constexpr size_t kMaxWeatherResponseSize = 2048;
constexpr int64_t kLocationRefreshUs = 12LL * 60 * 60 * 1000000;
constexpr int64_t kLocationRetryUs = 5LL * 60 * 1000000;
constexpr TickType_t kWeatherRefreshTicks = pdMS_TO_TICKS(60 * 60 * 1000);
constexpr TickType_t kWeatherRetryTicks = pdMS_TO_TICKS(5 * 60 * 1000);

struct HttpResponse {
    std::string body;
    bool too_large = false;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || response == nullptr) {
        return ESP_OK;
    }

    if (event->data_len < 0 ||
        response->body.size() + static_cast<size_t>(event->data_len) > kMaxWeatherResponseSize) {
        response->too_large = true;
        return ESP_FAIL;
    }

    response->body.append(static_cast<const char*>(event->data),
                          static_cast<size_t>(event->data_len));
    return ESP_OK;
}

bool GetJson(const std::string& url, const char* service_name, std::string& body) {
    HttpResponse response;
    esp_http_client_config_t config = {
        .url = url.c_str(),
        .timeout_ms = 15000,
        .event_handler = OnHttpEvent,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTPS client for %s", service_name);
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        if (response.too_large) {
            ESP_LOGE(TAG, "%s response exceeded %u bytes", service_name,
                     static_cast<unsigned>(kMaxWeatherResponseSize));
        } else {
            ESP_LOGW(TAG, "HTTPS request to %s failed: %s", service_name, esp_err_to_name(err));
        }
        return false;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "%s returned HTTP status %d", service_name, status_code);
        return false;
    }
    if (response.body.empty()) {
        ESP_LOGW(TAG, "%s returned an empty response", service_name);
        return false;
    }

    body = std::move(response.body);
    return true;
}

bool ReadJsonNumber(const cJSON* object, const char* key, double& value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
        return false;
    }
    value = item->valuedouble;
    return true;
}

}  // namespace

// 代码对应的天气描述和图标，参考了 https://www.weatherapi.com/docs/weather_conditions.json
const char* Description(int code) {
    switch (code) {
        case 0:
            return "晴";
        case 1:
            return "大部晴朗";
        case 2:
        case 116:
            return "局部多云";
        case 3:
            return "阴";
        case 45:
        case 48:
            return "雾";
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
            return "毛毛雨";
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
            return "雨";
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return "雪";
        case 80:
        case 81:
        case 82:
            return "阵雨";
        case 95:
        case 96:
        case 99:
            return "雷雨";
        default:
            return "天气";
    }
}

const char* Icon(int code) {
    switch (code) {
        case 0:
        case 1:
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        case 2:
            return MojiWeatherIcons::kPartlyCloudy;
        case 3:
            return MojiWeatherIcons::kOvercast;
        case 45:
        case 48:
            return MATERIAL_SYMBOLS_CLOUD_OFF;
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
            return MojiWeatherIcons::kDrizzle;
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
        case 80:
        case 81:
        case 82:
            return MojiWeatherIcons::kRain;
        case 116:
            // return MATERIAL_SYMBOLS_BRIGHTNESS_6;
            return MojiWeatherIcons::kPartlyCloudy;
            // 局部多云
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return MojiWeatherIcons::kSnow;
        default:
            return MATERIAL_SYMBOLS_CLOUD_DOWNLOAD;
    }
}

void UpdateLocationLabel(Display* display, lv_obj_t* label, const std::string& city) {
    if (display == nullptr || label == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    if (!lock) {
        return;
    }
    lv_label_set_text(label, city.c_str());
}

void UpdateConditions(Display* display, lv_obj_t* icon_label, lv_obj_t* weather_label,
                      lv_obj_t* high_low_label, int code, int temperature, int high, int low) {
    if (display == nullptr || icon_label == nullptr || weather_label == nullptr ||
        high_low_label == nullptr) {
        ESP_LOGW(TAG, "Cannot update weather before the standby UI is ready");
        return;
    }

    char weather_text[48];
    char high_low_text[64];
    snprintf(weather_text, sizeof(weather_text), "%d°  %s", temperature, Description(code));
    snprintf(high_low_text, sizeof(high_low_text), "最低 %d°  最高 %d°", low, high);

    DisplayLockGuard lock(display);
    if (!lock) {
        return;
    }
    lv_label_set_text(icon_label, Icon(code));
    lv_label_set_text(weather_label, weather_text);
    lv_label_set_text(high_low_label, high_low_text);
}

bool Service::Start(Display* display, lv_obj_t* city_label, lv_obj_t* icon_label,
                    lv_obj_t* weather_label, lv_obj_t* high_low_label) {
    if (started_) {
        return true;
    }
    if (display == nullptr || city_label == nullptr || icon_label == nullptr ||
        weather_label == nullptr || high_low_label == nullptr) {
        return false;
    }

    display_ = display;
    city_label_ = city_label;
    icon_label_ = icon_label;
    weather_label_ = weather_label;
    high_low_label_ = high_low_label;
    if (xTaskCreate(Task, "moji_weather", 8192, this, 1, nullptr) != pdPASS) {
        display_ = nullptr;
        city_label_ = nullptr;
        icon_label_ = nullptr;
        weather_label_ = nullptr;
        high_low_label_ = nullptr;
        return false;
    }

    started_ = true;
    return true;
}

void Service::Task(void* arg) {
    static_cast<Service*>(arg)->RunWeatherTask();
}

bool Service::RefreshLocation() {
    std::string body;
    if (!GetJson("https://ipwho.is/?lang=zh-CN", "IP geolocation", body)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "IP geolocation returned invalid JSON");
        return false;
    }

    const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    const cJSON* city_item = cJSON_GetObjectItemCaseSensitive(root, "city");
    double latitude = 0.0;
    double longitude = 0.0;
    bool valid = cJSON_IsTrue(success) && cJSON_IsString(city_item) &&
                 city_item->valuestring != nullptr && std::strlen(city_item->valuestring) > 0 &&
                 std::strlen(city_item->valuestring) <= 64 &&
                 ReadJsonNumber(root, "latitude", latitude) &&
                 ReadJsonNumber(root, "longitude", longitude) && latitude >= -90.0 &&
                 latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
    if (valid) {
        latitude_ = latitude;
        longitude_ = longitude;
        UpdateLocationLabel(display_, city_label_, city_item->valuestring);
        // 改外部引用统一函数, 使用display_在city_label_上更新成city_item->valuestring
        ESP_LOGI(TAG, "Updated approximate weather location");
    } else {
        ESP_LOGW(TAG, "IP geolocation response was missing valid city coordinates");
    }
    cJSON_Delete(root);
    return valid;
}

bool Service::RefreshWeather() {
    char url[320];
    int url_length =
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
                 "&current=temperature_2m,weather_code"
                 "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
                 latitude_, longitude_);
    if (url_length < 0 || static_cast<size_t>(url_length) >= sizeof(url)) {
        ESP_LOGE(TAG, "Weather request URL exceeded its buffer");
        return false;
    }

    std::string body;
    if (!GetJson(url, "weather service", body)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Weather service returned invalid JSON");
        return false;
    }

    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON* daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    double temperature = 0.0;
    double weather_code = 0.0;
    const cJSON* high_values = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON* low_values = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    bool valid = ReadJsonNumber(current, "temperature_2m", temperature) &&
                 ReadJsonNumber(current, "weather_code", weather_code) &&
                 cJSON_IsArray(high_values) && cJSON_IsArray(low_values) &&
                 cJSON_IsNumber(cJSON_GetArrayItem(high_values, 0)) &&
                 cJSON_IsNumber(cJSON_GetArrayItem(low_values, 0));
    if (valid) {
        double high = cJSON_GetArrayItem(high_values, 0)->valuedouble;
        double low = cJSON_GetArrayItem(low_values, 0)->valuedouble;
        valid = std::isfinite(high) && std::isfinite(low) && temperature >= -80.0 &&
                temperature <= 80.0 && high >= -80.0 && high <= 80.0 && low >= -80.0 &&
                low <= 80.0 && weather_code >= 0.0 &&
                (weather_code <= 99.0 || weather_code == 116.0) &&
                weather_code == std::floor(weather_code);
        if (valid) {    // 符合条件才更新天气,不然提示未ready
            int code = static_cast<int>(weather_code);
            UpdateConditions(display_, icon_label_, weather_label_, high_low_label_, code,
                             static_cast<int>(std::lround(temperature)),
                             static_cast<int>(std::lround(high)),
                             static_cast<int>(std::lround(low)));
            ESP_LOGI(TAG, "Updated current weather");
        }
    }
    if (!valid) {
        ESP_LOGW(TAG, "Weather service response was missing valid current or daily data");
    }
    cJSON_Delete(root);
    return valid;
}

void Service::RunWeatherTask() {    // 位置跟天气更新服务
    vTaskDelay(pdMS_TO_TICKS(20000));
    // 先延迟20秒,避免开机时网络还没连上就去获取天气,导致失败
    bool has_location = false;
    bool weather_updated = false;
    while (true) {  // 位置跟天气更新服务
        int64_t now = esp_timer_get_time();
        if (now >= next_location_refresh_us_) { // 12小时刷新一次位置,失败就5分钟后再试
            if (RefreshLocation()) {
                has_location = true;
                next_location_refresh_us_ = now + kLocationRefreshUs;
            } else {
                next_location_refresh_us_ =
                    //now + (has_location ? kLocationRefreshUs : kLocationRetryUs);
                    now + kLocationRetryUs;  // 失败就5分钟后再试,避免又等12h
            }
        }

        //if (has_location && !RefreshWeather()) { 
        if (has_location) { //有位置之后更新天气
            weather_updated = RefreshWeather(); // 执行,顺便记录结果
            if (!weather_updated) {
                ESP_LOGW(TAG, "Weather refresh failed; retaining the last displayed values");
            }
        }
        //vTaskDelay(has_location ? kWeatherRefreshTicks : kWeatherRetryTicks);
        vTaskDelay(has_location && weather_updated ? kWeatherRefreshTicks : kWeatherRetryTicks); 
        // 加上 && weather_updated, 避免天气一小时才刷新一次。
    }
}

}  // namespace Weather
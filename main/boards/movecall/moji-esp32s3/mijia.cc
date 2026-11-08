#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "qrcodegen.h"

namespace {

constexpr char TAG[] = "MojiMcp";
constexpr size_t kMaxMijiaResponseSize = 8192;
constexpr int kMaxQrVersion = 15;
constexpr int kQrQuietZone = 4;

struct MijiaHttpResponse {
    std::string body;
    std::string redirect_url;
    std::map<std::string, std::string>* cookies = nullptr;
    bool too_large = false;
};

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void SaveCookie(const std::string& header, std::map<std::string, std::string>& cookies) {
    size_t end = header.find(';');
    std::string pair = header.substr(0, end);
    size_t equals = pair.find('=');
    if (equals == std::string::npos || equals == 0) {
        return;
    }
    cookies[pair.substr(0, equals)] = pair.substr(equals + 1);
}

esp_err_t OnMijiaHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<MijiaHttpResponse*>(event->user_data);
    if (response == nullptr) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
        event->header_value != nullptr) {
        const std::string key = Lowercase(event->header_key);
        if (key == "set-cookie" && response->cookies != nullptr) {
            SaveCookie(event->header_value, *response->cookies);
        } else if (key == "location") {
            response->redirect_url = event->header_value;
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (event->data_len < 0 ||
            response->body.size() + static_cast<size_t>(event->data_len) > kMaxMijiaResponseSize) {
            response->too_large = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data),
                              static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}

bool IsXiaomiHttpsUrl(const std::string& url) {
    static constexpr char kHttpsPrefix[] = "https://";
    if (url.compare(0, sizeof(kHttpsPrefix) - 1, kHttpsPrefix) != 0) {
        return false;
    }
    size_t host_start = sizeof(kHttpsPrefix) - 1;
    size_t host_end = url.find_first_of("/:?#", host_start);
    std::string host = Lowercase(url.substr(host_start, host_end - host_start));
    return host == "xiaomi.com" ||
           (host.size() > 11 && host.compare(host.size() - 11, 11, ".xiaomi.com") == 0);
}

std::string MakeCookieHeader(const std::map<std::string, std::string>& cookies) {
    std::string header;
    for (const auto& [name, value] : cookies) {
        if (!header.empty()) {
            header += "; ";
        }
        header += name + "=" + value;
    }
    return header;
}

bool GetMijiaUrl(const std::string& url, const std::string& user_agent,
                 std::map<std::string, std::string>& cookies, int timeout_ms, std::string& body,
                 std::string& error) {
    std::string request_url = url;
    for (int redirect_count = 0; redirect_count <= 5; ++redirect_count) {
        if (!IsXiaomiHttpsUrl(request_url)) {
            error = "小米登录服务返回了不受信任的 HTTPS 地址";
            return false;
        }

        MijiaHttpResponse response;
        response.cookies = &cookies;
        esp_http_client_config_t config = {
            .url = request_url.c_str(),
            .user_agent = user_agent.c_str(),
            .method = HTTP_METHOD_GET,
            .timeout_ms = timeout_ms,
            .disable_auto_redirect = true,
            .event_handler = OnMijiaHttpEvent,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .user_data = &response,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            error = "无法初始化小米登录 HTTPS 连接";
            return false;
        }

        esp_err_t header_err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (header_err == ESP_OK) {
            header_err = esp_http_client_set_header(client, "Content-Type",
                                                    "application/x-www-form-urlencoded");
        }
        std::string cookie_header = MakeCookieHeader(cookies);
        if (header_err == ESP_OK && !cookie_header.empty()) {
            header_err = esp_http_client_set_header(client, "Cookie", cookie_header.c_str());
        }
        if (header_err != ESP_OK) {
            esp_http_client_cleanup(client);
            error = "无法设置小米登录请求头";
            return false;
        }

        esp_err_t request_err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (request_err != ESP_OK) {
            error = response.too_large ? "小米登录响应超出大小限制" : "连接小米登录服务失败";
            return false;
        }

        if (status_code == HttpStatus_MovedPermanently || status_code == HttpStatus_Found ||
            status_code == HttpStatus_SeeOther || status_code == HttpStatus_TemporaryRedirect ||
            status_code == HttpStatus_PermanentRedirect) {
            if (response.redirect_url.empty() || redirect_count == 5) {
                error = "小米登录服务重定向失败";
                return false;
            }
            request_url = std::move(response.redirect_url);
            continue;
        }
        if (status_code != HttpStatus_Ok) {
            error = "小米登录服务返回 HTTP " + std::to_string(status_code);
            return false;
        }

        body = std::move(response.body);
        return true;
    }

    error = "小米登录服务重定向次数过多";
    return false;
}

cJSON* ParseMijiaJson(const std::string& response) {
    size_t start = response.find("&&&START&&&");
    const char* json = response.c_str();
    if (start != std::string::npos) {
        json += start + sizeof("&&&START&&&") - 1;
    }
    return cJSON_Parse(json);
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        return value->valuestring;
    }
    if (cJSON_IsNumber(value)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f", value->valuedouble);
        return buffer;
    }
    return "";
}

int JsonCode(const cJSON* object) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, "code");
    return cJSON_IsNumber(value) ? value->valueint : 0;
}

std::string RandomString(size_t length, const char* alphabet) {
    std::string result(length, '\0');
    size_t alphabet_size = std::strlen(alphabet);
    for (char& ch : result) {
        ch = alphabet[esp_random() % alphabet_size];
    }
    return result;
}

struct MijiaLoginContext {
    std::string device_id;
    std::string pass_o;
    std::string user_agent;
    std::string login_url;
    std::string poll_url;
    std::map<std::string, std::string> cookies;
};

class MijiaQrLogin {
public:
    explicit MijiaQrLogin(Display* display) : display_(display) {}

    void DismissQr() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        ++generation_;
        active_ = false;
        HideQr();
    }

    ToolResult Start() {
        if (display_ == nullptr) {
            return std::unexpected("当前板子没有可用屏幕");
        }
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (active_) {
            return std::unexpected("米家二维码登录已经在进行中");
        }
        active_ = true;
        const uint32_t generation = ++generation_;
        auto* task_args = new (std::nothrow) LoginTaskArgs{this, generation};
        if (task_args == nullptr) {
            active_ = false;
            return std::unexpected("无法分配米家登录任务参数");
        }

        if (xTaskCreate(
                [](void* arg) {
                    auto* args = static_cast<LoginTaskArgs*>(arg);
                    MijiaQrLogin* owner = args->owner;
                    const uint32_t generation = args->generation;
                    delete args;
                    owner->Run(generation);
                },
                "mijia_qr_login", 12288, task_args, 4, nullptr) != pdPASS) {
            delete task_args;
            active_ = false;
            return std::unexpected("无法创建米家登录任务");
        }
        return std::string("米家登录二维码正在准备，请查看设备屏幕并使用米家 App 扫码");
    }

private:
    struct LoginTaskArgs {
        MijiaQrLogin* owner;
        uint32_t generation;
    };

    bool IsCurrent(uint32_t generation) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        return active_ && generation_ == generation;
    }

    bool BeginLogin(MijiaLoginContext& context, std::string& error, uint32_t generation) {
        static constexpr char kDeviceAlphabet[] =
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-";
        static constexpr char kHexAlphabet[] = "0123456789ABCDEF";
        context.device_id = RandomString(16, kDeviceAlphabet);
        context.pass_o = RandomString(16, "0123456789abcdef");
        std::string ua_id1 = RandomString(40, kHexAlphabet);
        std::string ua_id2 = RandomString(32, kHexAlphabet);
        std::string ua_id3 = RandomString(32, kHexAlphabet);
        std::string ua_id4 = RandomString(40, kHexAlphabet);
        context.user_agent = "Android-15-11.0.701-Xiaomi-23046RP50C-OS2.0.212.0.VMYCNXM-" + ua_id1 +
                             "-CN-" + ua_id3 + "-" + ua_id2 + "-SmartHome-MI_APP_STORE-" + ua_id1 +
                             "|" + ua_id4 + "|" + context.pass_o + "-64";
        context.cookies["deviceId"] = context.device_id;
        context.cookies["pass_o"] = context.pass_o;
        context.cookies["uLocale"] = "zh_CN";
        context.cookies["locale"] = "zh_CN";
        context.cookies["channel"] = "MI_APP_STORE";

        std::string body;
        if (!GetMijiaUrl(
                "https://account.xiaomi.com/pass/serviceLogin?_json=true&sid=mijia&_locale=zh_CN",
                context.user_agent, context.cookies, 15000, body, error)) {
            return false;
        }
        if (!IsCurrent(generation)) {
            return false;
        }
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> service_data(ParseMijiaJson(body),
                                                                     cJSON_Delete);
        if (!service_data) {
            error = "无法解析小米账号登录响应";
            return false;
        }

        const std::string location = JsonString(service_data.get(), "location");
        size_t query_start = location.find('?');
        if (location.empty() || query_start == std::string::npos) {
            error = "小米账号未返回二维码登录信息";
            return false;
        }
        std::string query = location.substr(query_start + 1);
        size_t fragment_start = query.find('#');
        if (fragment_start != std::string::npos) {
            query.resize(fragment_start);
        }
        if (query.empty()) {
            error = "小米账号返回了空的二维码登录参数";
            return false;
        }

        context.login_url = "https://account.xiaomi.com/longPolling/loginUrl?" + query +
                            "&theme=&bizDeviceType=&_hasLogo=false&_qrsize=240&_dc=" +
                            std::to_string(esp_timer_get_time() / 1000);
        if (!GetMijiaUrl(context.login_url, context.user_agent, context.cookies, 15000, body,
                         error)) {
            return false;
        }
        if (!IsCurrent(generation)) {
            return false;
        }
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> qr_data(ParseMijiaJson(body), cJSON_Delete);
        if (!qr_data || JsonCode(qr_data.get()) != 0) {
            error = qr_data ? "小米账号拒绝创建登录二维码" : "无法解析小米二维码响应";
            return false;
        }

        context.login_url = JsonString(qr_data.get(), "loginUrl");
        context.poll_url = JsonString(qr_data.get(), "lp");
        if (!IsXiaomiHttpsUrl(context.login_url) || !IsXiaomiHttpsUrl(context.poll_url)) {
            error = "小米账号返回了无效的二维码或轮询地址";
            return false;
        }
        return true;
    }

    bool ShowQr(const std::string& login_url, std::string& error, uint32_t generation) {
        if (!IsCurrent(generation)) {
            return false;
        }
        constexpr int kQuietZone = kQrQuietZone;
        std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxQrVersion)> qr{};
        std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxQrVersion)> temp{};
        if (!qrcodegen_encodeText(login_url.c_str(), temp.data(), qr.data(), qrcodegen_Ecc_LOW,
                                  qrcodegen_VERSION_MIN, kMaxQrVersion, qrcodegen_Mask_AUTO,
                                  true)) {
            error = "小米二维码内容超出屏幕二维码容量";
            return false;
        }

        const int qr_size = qrcodegen_getSize(qr.data());
        const int image_size = qr_size + 2 * kQuietZone;
        size_t buffer_size = static_cast<size_t>(image_size) * image_size * sizeof(uint16_t);
        auto* pixels = static_cast<uint16_t*>(heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT));
        if (pixels == nullptr) {
            error = "内存不足，无法显示米家登录二维码";
            return false;
        }
        std::fill_n(pixels, static_cast<size_t>(image_size) * image_size, 0xFFFF);
        for (int y = 0; y < qr_size; ++y) {
            for (int x = 0; x < qr_size; ++x) {
                if (qrcodegen_getModule(qr.data(), x, y)) {
                    pixels[(y + kQuietZone) * image_size + x + kQuietZone] = 0x0000;
                }
            }
        }

        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!active_ || generation_ != generation) {
            heap_caps_free(pixels);
            return false;
        }
        DisplayLockGuard lock(display_);
        if (!lock) {
            heap_caps_free(pixels);
            error = "无法锁定屏幕显示二维码";
            return false;
        }

        if (overlay_ != nullptr) {
            lv_obj_del(overlay_);
            overlay_ = nullptr;
            qr_canvas_ = nullptr;
        }
        if (qr_pixels_ != nullptr) {
            heap_caps_free(qr_pixels_);
            qr_pixels_ = nullptr;
        }
        overlay_ = lv_obj_create(lv_screen_active());
        if (overlay_ == nullptr) {
            heap_caps_free(pixels);
            error = "无法创建米家登录界面";
            return false;
        }
        lv_obj_remove_style_all(overlay_);
        lv_obj_set_size(overlay_, display_->width(), display_->height());
        lv_obj_set_style_bg_color(overlay_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(overlay_, 0, 0);

        lv_obj_t* title = lv_label_create(overlay_);
        lv_obj_t* caption = lv_label_create(overlay_);
        qr_canvas_ = lv_canvas_create(overlay_);
        if (title == nullptr || caption == nullptr || qr_canvas_ == nullptr) {
            lv_obj_del(overlay_);
            overlay_ = nullptr;
            qr_canvas_ = nullptr;
            heap_caps_free(pixels);
            error = "无法创建米家二维码界面控件";
            return false;
        }

        lv_label_set_text(title, "扫码登录");
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_label_set_text(caption, "请使用米家 App 扫码");
        lv_obj_set_style_text_color(caption, lv_color_black(), 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -8);

        lv_canvas_set_buffer(qr_canvas_, pixels, image_size, image_size, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(qr_canvas_, image_size, image_size);
        lv_image_set_antialias(qr_canvas_, false);
        int max_scale = std::min(display_->width() - 20, display_->height() - 56) / image_size;
        uint32_t scale = static_cast<uint32_t>(std::max(1, std::min(3, max_scale)) * 256);
        lv_image_set_scale(qr_canvas_, scale);
        lv_obj_align(qr_canvas_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_move_foreground(overlay_);
        qr_pixels_ = pixels;
        return true;
    }

    bool CompleteLogin(MijiaLoginContext& context, std::string& error, uint32_t generation) {
        std::string body;
        if (!GetMijiaUrl(context.poll_url, context.user_agent, context.cookies, 120000, body,
                         error)) {
            return false;
        }
        if (!IsCurrent(generation)) {
            return false;
        }
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> login_data(ParseMijiaJson(body),
                                                                   cJSON_Delete);
        if (!login_data || JsonCode(login_data.get()) != 0) {
            error = login_data ? "二维码登录未获小米账号确认" : "无法解析米家扫码结果";
            return false;
        }

        const std::string user_id = JsonString(login_data.get(), "userId");
        const std::string c_user_id = JsonString(login_data.get(), "cUserId");
        const std::string pass_token = JsonString(login_data.get(), "passToken");
        const std::string ssecurity = JsonString(login_data.get(), "ssecurity");
        const std::string psecurity = JsonString(login_data.get(), "psecurity");
        const std::string nonce = JsonString(login_data.get(), "nonce");
        const std::string callback_url = JsonString(login_data.get(), "location");
        if (user_id.empty() || c_user_id.empty() || pass_token.empty() || ssecurity.empty() ||
            psecurity.empty() || nonce.empty() || !IsXiaomiHttpsUrl(callback_url)) {
            error = "小米账号返回的登录凭据不完整";
            return false;
        }

        context.cookies["userId"] = user_id;
        context.cookies["cUserId"] = c_user_id;
        context.cookies["passToken"] = pass_token;
        if (!GetMijiaUrl(callback_url, context.user_agent, context.cookies, 15000, body, error)) {
            return false;
        }
        if (!IsCurrent(generation)) {
            return false;
        }

        std::string service_token;
        auto service_cookie = context.cookies.find("serviceToken");
        if (service_cookie != context.cookies.end()) {
            service_token = service_cookie->second;
        }
        if (service_token.empty()) {
            auto alternate_cookie = context.cookies.find("yetAnotherServiceToken");
            if (alternate_cookie != context.cookies.end()) {
                service_token = alternate_cookie->second;
            }
        }
        if (service_token.empty()) {
            error = "登录完成但小米账号未返回服务令牌";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (!active_ || generation_ != generation) {
                return false;
            }
            Settings settings("mijia_auth", true);
            settings.SetString("user_id", user_id);
            settings.SetString("cuser_id", c_user_id);
            settings.SetString("pass_token", pass_token);
            settings.SetString("service_tok", service_token);
            settings.SetString("ssecurity", ssecurity);
            settings.SetString("psecurity", psecurity);
            settings.SetString("nonce", nonce);
            settings.SetString("device_id", context.device_id);
            settings.SetString("pass_o", context.pass_o);
            settings.SetString("user_agent", context.user_agent);
            Settings saved_auth("mijia_auth");
            if (saved_auth.GetString("service_tok") != service_token) {
                error = "无法保存米家登录凭据到 NVS";
                return false;
            }
        }
        return true;
    }

    void HideQr() {
        DisplayLockGuard lock(display_);
        if (lock && overlay_ != nullptr) {
            lv_obj_del(overlay_);
            overlay_ = nullptr;
            qr_canvas_ = nullptr;
            if (qr_pixels_ != nullptr) {
                heap_caps_free(qr_pixels_);
                qr_pixels_ = nullptr;
            }
        }
    }

    void Run(uint32_t generation) {
        MijiaLoginContext context;
        std::string error;
        if (!BeginLogin(context, error, generation) ||
            !ShowQr(context.login_url, error, generation)) {
            Finish(generation, error);
            return;
        }

        if (!CompleteLogin(context, error, generation)) {
            Finish(generation, error);
            return;
        }
        Finish(generation, "米家账号登录成功");
    }

    void Finish(uint32_t generation, const std::string& message) {
        bool is_current = false;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            is_current = active_ && generation_ == generation;
            if (is_current) {
                HideQr();
                active_ = false;
            }
        }
        if (!is_current) {
            vTaskDelete(nullptr);
            return;
        }
        display_->ShowNotification(message, 5000);
        if (message == "米家账号登录成功") {
            ESP_LOGI(TAG, "Mijia QR login completed");
        } else {
            ESP_LOGW(TAG, "Mijia QR login failed: %s", message.c_str());
        }
        vTaskDelete(nullptr);
    }

    Display* display_;
    std::mutex lifecycle_mutex_;
    uint32_t generation_ = 0;
    bool active_ = false;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* qr_canvas_ = nullptr;
    uint16_t* qr_pixels_ = nullptr;
};

MijiaQrLogin* mijia_login_instance = nullptr;

}  // namespace

void HideMojiMijiaLoginQr() {
    if (mijia_login_instance != nullptr) {
        mijia_login_instance->DismissQr();
    }
}

void InitializeMojiMijiaTool(Display* display) {
    static MijiaQrLogin mijia_login(display);
    mijia_login_instance = &mijia_login;
    McpServer::GetInstance().AddTool(
        "self.mijia.login",
        "在屏幕上显示小米账号二维码，用户使用米家 App 扫码后完成登录。登录凭据会保存在设备 NVS 中。"
        "这是用户主动操作，不要由 AI 自行调用。",
        PropertyList(), [](const PropertyList&) -> ToolResult { return mijia_login.Start(); });
}

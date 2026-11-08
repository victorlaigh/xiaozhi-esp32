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
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <miniz.h>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "qrcodegen.h"

namespace {

constexpr char TAG[] = "MojiMcp";
constexpr size_t kMaxMijiaResponseSize = 8192;
constexpr size_t kMaxMijiaDecodedResponseSize = 32768;
constexpr int kMijiaHttpTxBufferSize = 2048;
constexpr int kMaxQrVersion = 15;
constexpr int kQrQuietZone = 4;
bool mijia_crypto_ready = false;

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
    if (host.empty() || host.find('@') != std::string::npos) {
        return false;
    }
    static constexpr char kTrustedDomains[][12] = {"xiaomi.com", ".xiaomi.com", "mijia.com",
                                                   ".mijia.com", "mi.com",      ".mi.com"};
    for (const char* domain : kTrustedDomains) {
        const size_t domain_size = std::strlen(domain);
        if (host == domain || (domain[0] == '.' && host.size() > domain_size &&
                               host.compare(host.size() - domain_size, domain_size, domain) == 0)) {
            return true;
        }
    }
    return false;
}

bool IsAbsoluteHttpsUrl(const std::string& url) {
    static constexpr char kHttpsPrefix[] = "https://";
    if (url.compare(0, sizeof(kHttpsPrefix) - 1, kHttpsPrefix) != 0) {
        return false;
    }
    const size_t host_start = sizeof(kHttpsPrefix) - 1;
    const size_t host_end = url.find_first_of("/:?#", host_start);
    const std::string host = url.substr(host_start, host_end - host_start);
    return !host.empty() && host.find('@') == std::string::npos;
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
                 std::string& error, bool allow_any_https_host = false, bool send_cookies = true) {
    std::string request_url = url;
    for (int redirect_count = 0; redirect_count <= 5; ++redirect_count) {
        const bool trusted_host = IsXiaomiHttpsUrl(request_url);
        if (!trusted_host && (!allow_any_https_host || !IsAbsoluteHttpsUrl(request_url))) {
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
            .buffer_size_tx = kMijiaHttpTxBufferSize,
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
        if (header_err == ESP_OK && send_cookies && trusted_host && !cookie_header.empty()) {
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


bool Base64Encode(const uint8_t* input, size_t input_size, std::string& output) {
    if (input_size == 0) {
        output.clear();
        return true;
    }
    size_t output_size = 0;
    if (mbedtls_base64_encode(nullptr, 0, &output_size, input, input_size) !=
        MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return false;
    }
    output.resize(output_size);
    if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(output.data()), output.size(),
                              &output_size, input, input_size) != 0) {
        return false;
    }
    output.resize(output_size);
    return true;
}

bool Base64Decode(const std::string& input, std::vector<uint8_t>& output) {
    output.resize(input.size());
    size_t output_size = 0;
    int result =
        mbedtls_base64_decode(output.data(), output.size(), &output_size,
                              reinterpret_cast<const uint8_t*>(input.data()), input.size());
    if (result != 0) {
        return false;
    }
    output.resize(output_size);
    return true;
}

bool MakeApiNonce(std::string& nonce) {
    std::array<uint8_t, 12> nonce_bytes;
    esp_fill_random(nonce_bytes.data(), 8);

    const std::time_t now = std::time(nullptr);
    if (now <= 0) {
        return false;
    }
    uint32_t minutes = static_cast<uint32_t>(now / 60);
    nonce_bytes[8] = static_cast<uint8_t>(minutes >> 24);
    nonce_bytes[9] = static_cast<uint8_t>(minutes >> 16);
    nonce_bytes[10] = static_cast<uint8_t>(minutes >> 8);
    nonce_bytes[11] = static_cast<uint8_t>(minutes);
    return Base64Encode(nonce_bytes.data(), nonce_bytes.size(), nonce);
}

bool MakeSignedNonce(const std::string& ssecurity, const std::string& nonce,
                     std::string& signed_nonce) {
    std::vector<uint8_t> secret_bytes;
    std::vector<uint8_t> nonce_bytes;
    if (!Base64Decode(ssecurity, secret_bytes) || !Base64Decode(nonce, nonce_bytes)) {
        return false;
    }

    std::vector<uint8_t> input = std::move(secret_bytes);
    input.insert(input.end(), nonce_bytes.begin(), nonce_bytes.end());
    std::array<uint8_t, 32> digest;
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr || mbedtls_md(md_info, input.data(), input.size(), digest.data()) != 0) {
        return false;
    }
    return Base64Encode(digest.data(), digest.size(), signed_nonce);
}

bool Rc4Crypt(const std::vector<uint8_t>& key, const std::string& input,
              std::vector<uint8_t>& output) {
    if (key.empty()) {
        return false;
    }
    std::array<uint8_t, 256> state;
    for (size_t i = 0; i < state.size(); ++i) {
        state[i] = static_cast<uint8_t>(i);
    }

    uint8_t j = 0;
    for (size_t i = 0; i < state.size(); ++i) {
        j = static_cast<uint8_t>(j + state[i] + key[i % key.size()]);
        std::swap(state[i], state[j]);
    }

    uint8_t i = 0;
    j = 0;
    auto next_key_byte = [&]() {
        i = static_cast<uint8_t>(i + 1);
        j = static_cast<uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
        return state[static_cast<uint8_t>(state[i] + state[j])];
    };
    for (size_t offset = 0; offset < 1024; ++offset) {
        next_key_byte();
    }

    output.resize(input.size());
    for (size_t offset = 0; offset < input.size(); ++offset) {
        output[offset] = static_cast<uint8_t>(input[offset]) ^ next_key_byte();
    }
    return true;
}

bool EncryptApiParameter(const std::vector<uint8_t>& key, const std::string& input,
                         std::string& output) {
    std::vector<uint8_t> encrypted;
    return Rc4Crypt(key, input, encrypted) &&
           Base64Encode(encrypted.data(), encrypted.size(), output);
}

bool MakeApiSignature(const std::string& uri, const std::string& signed_nonce,
                      const std::string& data, const std::string& rc4_hash,
                      std::string& signature) {
    std::string sign_input = "POST&" + uri + "&data=" + data;
    if (!rc4_hash.empty()) {
        sign_input += "&rc4_hash__=" + rc4_hash;
    }
    sign_input += "&" + signed_nonce;
    std::array<uint8_t, 20> digest;
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md_info == nullptr ||
        mbedtls_md(md_info, reinterpret_cast<const uint8_t*>(sign_input.data()), sign_input.size(),
                   digest.data()) != 0) {
        return false;
    }
    return Base64Encode(digest.data(), digest.size(), signature);
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHexDigits[ch >> 4]);
            encoded.push_back(kHexDigits[ch & 0x0F]);
        }
    }
    return encoded;
}

bool DecompressGzip(const std::vector<uint8_t>& input, std::string& output) {
    if (input.size() < 18 || input[0] != 0x1F || input[1] != 0x8B || input[2] != 8) {
        return false;
    }
    const uint8_t flags = input[3];
    if ((flags & 0xE0) != 0) {
        return false;
    }

    size_t data_start = 10;
    if ((flags & 0x04) != 0) {
        if (input.size() < data_start + 2) {
            return false;
        }
        const size_t extra_size =
            input[data_start] | (static_cast<size_t>(input[data_start + 1]) << 8);
        data_start += 2 + extra_size;
    }
    for (uint8_t flag : {static_cast<uint8_t>(0x08), static_cast<uint8_t>(0x10)}) {
        if ((flags & flag) != 0) {
            while (data_start < input.size() - 8 && input[data_start++] != 0) {
            }
        }
    }
    if ((flags & 0x02) != 0) {
        data_start += 2;
    }
    if (data_start >= input.size() - 8) {
        return false;
    }

    std::vector<uint8_t> decompressed(kMaxMijiaDecodedResponseSize);
    const size_t decompressed_size = tinfl_decompress_mem_to_mem(
        decompressed.data(), decompressed.size(), input.data() + data_start,
        input.size() - data_start - 8, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (decompressed_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || decompressed_size == 0) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(decompressed.data()), decompressed_size);
    return true;
}

bool DecodeApiResponse(const std::string& response, const std::string& signed_nonce,
                       std::string& decoded, std::string& error) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> plain_json(cJSON_Parse(response.c_str()),
                                                               cJSON_Delete);
    if (plain_json) {
        decoded = response;
        return true;
    }

    std::vector<uint8_t> key;
    std::vector<uint8_t> encrypted;
    if (!Base64Decode(signed_nonce, key) || !Base64Decode(response, encrypted)) {
        error = "无法解码米家家庭列表响应";
        return false;
    }
    std::string encrypted_bytes(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    std::vector<uint8_t> decrypted;
    if (!Rc4Crypt(key, encrypted_bytes, decrypted)) {
        error = "无法解密米家家庭列表响应";
        return false;
    }
    if (decrypted.size() >= 2 && decrypted[0] == 0x1F && decrypted[1] == 0x8B) {
        if (!DecompressGzip(decrypted, decoded)) {
            error = "无法解压米家家庭列表响应";
            return false;
        }
    } else {
        decoded.assign(reinterpret_cast<const char*>(decrypted.data()), decrypted.size());
    }
    return true;
}

bool RequestHomes(const std::string& user_agent, const std::string& service_token,
                  const std::string& c_user_id, const std::string& device_id,
                  const std::string& ssecurity, std::string& homes_json, std::string& error,
                  const std::string& target_home_id = "") {
    static constexpr char kUri[] = "/v2/homeroom/gethome_merged";
    static constexpr char kApiUrl[] = "https://api.mijia.tech/app/v2/homeroom/gethome_merged";

    if (!mijia_crypto_ready) {
        error = "米家加密模块初始化失败，请重启设备后重试";
        return false;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> request_data(cJSON_CreateObject(),
                                                                 cJSON_Delete);
    if (request_data == nullptr || !cJSON_AddBoolToObject(request_data.get(), "fg", true) ||
        !cJSON_AddBoolToObject(request_data.get(), "fetch_share", true) ||
        !cJSON_AddBoolToObject(request_data.get(), "fetch_share_dev", true) ||
        !cJSON_AddBoolToObject(request_data.get(), "fetch_cariot", true) ||
        !cJSON_AddNumberToObject(request_data.get(), "limit", 300) ||
        !cJSON_AddNumberToObject(request_data.get(), "app_ver", 7) ||
        !cJSON_AddNumberToObject(request_data.get(), "plat_form", 0)) {
        error = "无法创建米家家庭列表请求";
        return false;
    }

    std::unique_ptr<char, decltype(&std::free)> data_json(
        cJSON_PrintUnformatted(request_data.get()), std::free);
    std::string nonce;
    std::string signed_nonce;
    std::vector<uint8_t> encryption_key;
    if (!data_json || !MakeApiNonce(nonce) || !MakeSignedNonce(ssecurity, nonce, signed_nonce) ||
        !Base64Decode(signed_nonce, encryption_key)) {
        error = "无法生成米家 API 请求签名";
        return false;
    }

    std::string rc4_hash;
    std::string encrypted_data;
    std::string encrypted_rc4_hash;
    std::string signature;
    if (!MakeApiSignature(kUri, signed_nonce, data_json.get(), "", rc4_hash) ||
        !EncryptApiParameter(encryption_key, rc4_hash, encrypted_rc4_hash) ||
        !EncryptApiParameter(encryption_key, data_json.get(), encrypted_data) ||
        !MakeApiSignature(kUri, signed_nonce, encrypted_data, encrypted_rc4_hash, signature)) {
        error = "无法加密米家 API 请求";
        return false;
    }

    const std::string form_data =
        "data=" + UrlEncode(encrypted_data) + "&rc4_hash__=" + UrlEncode(encrypted_rc4_hash) +
        "&signature=" + UrlEncode(signature) + "&ssecurity=" + UrlEncode(ssecurity) +
        "&_nonce=" + UrlEncode(nonce);
    if (form_data.size() > INT_MAX) {
        error = "米家 API 请求超出大小限制";
        return false;
    }

    std::map<std::string, std::string> cookies = {
        {"cUserId", c_user_id},
        {"yetAnotherServiceToken", service_token},
        {"serviceToken", service_token},
        {"timezone", "GMT+08:00"},
        {"is_daylight", "0"},
        {"dst_offset", "0"},
        {"channel", "MI_APP_STORE"},
        {"countryCode", "CN"},
        {"PassportDeviceId", device_id},
        {"locale", "zh_CN"},
    };
    MijiaHttpResponse response;
    response.cookies = &cookies;
    esp_http_client_config_t config = {
        .url = kApiUrl,
        .user_agent = user_agent.c_str(),
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .disable_auto_redirect = true,
        .event_handler = OnMijiaHttpEvent,
        .buffer_size = 2048,
        .buffer_size_tx = kMijiaHttpTxBufferSize,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        error = "无法初始化米家家庭列表 HTTPS 连接";
        return false;
    }

    esp_err_t header_err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (header_err == ESP_OK) {
        header_err =
            esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    }
    if (header_err == ESP_OK) {
        header_err = esp_http_client_set_header(client, "miot-accept-encoding", "GZIP");
    }
    if (header_err == ESP_OK) {
        header_err = esp_http_client_set_header(client, "miot-encrypt-algorithm", "ENCRYPT-RC4");
    }
    if (header_err == ESP_OK) {
        header_err =
            esp_http_client_set_header(client, "x-xiaomi-protocal-flag-cli", "PROTOCAL-HTTP2");
    }
    const std::string cookie_header = MakeCookieHeader(cookies);
    if (header_err == ESP_OK) {
        header_err = esp_http_client_set_header(client, "Cookie", cookie_header.c_str());
    }
    if (header_err == ESP_OK) {
        header_err = esp_http_client_set_post_field(client, form_data.c_str(),
                                                    static_cast<int>(form_data.size()));
    }
    if (header_err != ESP_OK) {
        esp_http_client_cleanup(client);
        error = "无法设置米家家庭列表请求参数";
        return false;
    }

    const esp_err_t request_err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (request_err != ESP_OK) {
        error = response.too_large ? "米家家庭列表响应超出大小限制" : "连接米家家庭列表服务失败";
        return false;
    }
    if (status_code != HttpStatus_Ok) {
        error = "米家家庭列表服务返回 HTTP " + std::to_string(status_code);
        return false;
    }

    return DecodeApiResponse(response.body, signed_nonce, homes_json, error);
}

// 定义一个米家凭证结构体
struct MijiaAuthCredentials {
    std::string service_token;
    std::string c_user_id;
    std::string device_id;
    std::string user_agent;
    std::string ssecurity;
};

// 公共的凭证加载与校验函数
// 如果未登录，直接返回带有错误信息的 std::unexpected；成功则返回凭证对象
std::expected<MijiaAuthCredentials, ToolResult> LoadMijiaAuth() {
    Settings auth("mijia_auth");
    MijiaAuthCredentials creds{
        auth.GetString("service_tok"),
        auth.GetString("cuser_id"),
        auth.GetString("device_id"),
        auth.GetString("user_agent"),
        auth.GetString("ssecurity")
    };

    if (creds.service_token.empty() || creds.c_user_id.empty() || 
        creds.device_id.empty() || creds.user_agent.empty() || 
        creds.ssecurity.empty()) {
        return std::unexpected(std::unexpected("尚未登录米家，请先调用 self.mijia.login 完成扫码登录"));
    }

    return creds;
}

// 凭证区域结束

ToolResult ListHomes() {
// 1. 一行代码完成鉴权检查与加载
    auto auth_result = LoadMijiaAuth();
    if (!auth_result) {
        return auth_result.error(); // 直接返回登录提示
    }
    const auto& creds = *auth_result; // 获取凭证：creds.service_token, creds.user_agent 等

    std::string response;
    std::string error;
    // 使用 creds 里的字段发起请求
    if (!RequestHomes(creds.user_agent, creds.service_token, creds.c_user_id, 
                      creds.device_id, creds.ssecurity, response, error)) {
        return std::unexpected(error);
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(ParseMijiaJson(response), cJSON_Delete);
    if (!result) {
        return std::unexpected("无法解析米家家庭列表响应");
    }

    const cJSON* result_data = cJSON_GetObjectItemCaseSensitive(result.get(), "result");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(result.get(), "code");
    if (!cJSON_IsNumber(code) || JsonCode(result.get()) != 0 || !cJSON_IsObject(result_data)) {
        std::string message = JsonString(result.get(), "message");
        if (message.empty()) {
            message = JsonString(result.get(), "desc");
        }
        if (cJSON_IsNumber(code) && message == "米家家庭列表请求失败") {
            message += "，错误码 " + std::to_string(code->valueint);
        }
        if (message.empty()) {
            message = "米家家庭列表请求失败，错误码 " +
                      (cJSON_IsNumber(code) ? std::to_string(code->valueint) : "未知");
        }
        const std::string code_text =
            cJSON_IsNumber(code) ? std::to_string(code->valueint) : "unknown";
        ESP_LOGW(TAG, "Mijia homes API rejected request: code=%s, message=%s", code_text.c_str(),
                 message.c_str());
        return std::unexpected(std::move(message));
    }
    const cJSON* homes = cJSON_GetObjectItemCaseSensitive(result_data, "homelist");
    if (!cJSON_IsArray(homes)) {
        return std::unexpected("米家家庭列表响应缺少 homelist");
    }

    // === 【优化核心】在这里对庞大的 JSON 进行裁剪，只提取核心轻量数据 ===
    cJSON* slim_homes_array = cJSON_CreateArray();
    cJSON* home_item = NULL;
    
    cJSON_ArrayForEach(home_item, homes) {
        cJSON* h_id = cJSON_GetObjectItemCaseSensitive(home_item, "id");
        cJSON* h_name = cJSON_GetObjectItemCaseSensitive(home_item, "name");
        
        // 创建一个精简版的家庭对象
        cJSON* slim_home = cJSON_CreateObject();
        if (h_id) cJSON_AddStringToObject(slim_home, "id", h_id->valuestring ? h_id->valuestring : std::to_string(h_id->valuedouble).c_str());
        // 如果id是数字或者是字符串，适配安全取法：
        // 建议使用 cJSON_IsString(h_id) ? h_id->valuestring : std::to_string((long long)h_id->valuedouble).c_str()
        if (h_name) cJSON_AddStringToObject(slim_home, "name", h_name->valuestring ? h_name->valuestring : "");
        
        // 如果你还需要房间列表，可以只保留房间的名字和id，丢弃几十个设备的 dids 数组：
        const cJSON* rooms = cJSON_GetObjectItemCaseSensitive(home_item, "roomlist");
        if (cJSON_IsArray(rooms)) {
            cJSON* slim_rooms_array = cJSON_CreateArray();
            cJSON* room_item = NULL;
            cJSON_ArrayForEach(room_item, rooms) {
                cJSON* r_id = cJSON_GetObjectItemCaseSensitive(room_item, "id");
                cJSON* r_name = cJSON_GetObjectItemCaseSensitive(room_item, "name");
                
                cJSON* slim_room = cJSON_CreateObject();
                // 简化填充...
                cJSON_AddItemToArray(slim_rooms_array, slim_room);
            }
            cJSON_AddItemToObject(slim_home, "rooms", slim_rooms_array);
        }
        
        cJSON_AddItemToArray(slim_homes_array, slim_home);
    }
   
    // std::unique_ptr<char, decltype(&std::free)> homes_string(cJSON_PrintUnformatted(homes),
    //                                                         std::free);
     // 原本得json改成序列化精简后的 JSON
    std::unique_ptr<char, decltype(&std::free)> homes_string(cJSON_PrintUnformatted(slim_homes_array),
                                                             std::free);
    cJSON_Delete(slim_homes_array); // 清理临时创建的 cJSON 对象                                                         

    if (!homes_string) {
        return std::unexpected("无法序列化精简后的米家家庭列表");
    }
    return std::string(homes_string.get());
}

ToolResult ListHomesOnDedicatedStack() {
    struct TaskContext {
        SemaphoreHandle_t completed;
        ToolResult result;
    };

    SemaphoreHandle_t completed = xSemaphoreCreateBinary();
    if (completed == nullptr) {
        return std::unexpected("无法创建米家家庭列表任务同步对象");
    }

    TaskContext context{completed, std::unexpected("米家家庭列表任务未完成")};
    const BaseType_t task_status = xTaskCreate(
        [](void* arg) {
            auto* context = static_cast<TaskContext*>(arg);
            context->result = ListHomes();
            ESP_LOGI(TAG, "Mijia homes task stack remaining: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            xSemaphoreGive(context->completed);
            vTaskDelete(nullptr);
        },
        "mijia_homes", 24576, &context, 4, nullptr);
    if (task_status != pdPASS) {
        vSemaphoreDelete(completed);
        return std::unexpected("无法创建米家家庭列表任务");
    }

    xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return std::move(context.result);
}

ToolResult GetDevicesListInternal(const std::string& target_home_id) {

    // 1. 同样一行代码完成鉴权检查
    auto auth_result = LoadMijiaAuth();
    if (!auth_result) {
        return auth_result.error();
    }
    const auto& creds = *auth_result;

    std::string response;
    std::string error;
    // 调用RequestHomes获取设备列表 
    if (!RequestHomes(creds.user_agent, creds.service_token, creds.c_user_id, 
                        creds.device_id, creds.ssecurity, response, error, target_home_id)) {
        return std::unexpected(error.empty() ? "获取米家设备列表失败" : error);
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(ParseMijiaJson(response), cJSON_Delete);
    if (!result) {
        return std::unexpected("无法解析米家设备列表响应");
    }

    const cJSON* result_data = cJSON_GetObjectItemCaseSensitive(result.get(), "result");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(result.get(), "code");
    if (!cJSON_IsNumber(code) || JsonCode(result.get() ) != 0 || !cJSON_IsObject(result_data)) {
        std::string message = JsonString(result.get(), "message");
        if (message.empty()) message = "米家设备列表请求失败";
        return std::unexpected(message);
    }

    // 假设返回的设备列表在 "list" 或 "devices" 数组中（根据实际米家 API 结构调整）
    const cJSON* devices = cJSON_GetObjectItemCaseSensitive(result_data, "list");
    if (!cJSON_IsArray(devices)) {
        devices = cJSON_GetObjectItemCaseSensitive(result_data, "devices"); // 兼容不同字段名
    }
    if (!cJSON_IsArray(devices)) {
        return std::unexpected("米家设备列表响应缺少设备数组");
    }

    // === 【核心优化：数据瘦身】只提取大模型和 MCP 需要的关键字段 ===
    cJSON* slim_devices_array = cJSON_CreateArray();
    cJSON* dev_item = NULL;

    cJSON_ArrayForEach(dev_item, devices) {
        cJSON* d_did = cJSON_GetObjectItemCaseSensitive(dev_item, "did");
        cJSON* d_name = cJSON_GetObjectItemCaseSensitive(dev_item, "name");
        cJSON* d_model = cJSON_GetObjectItemCaseSensitive(dev_item, "model");
        cJSON* d_room_id = cJSON_GetObjectItemCaseSensitive(dev_item, "room_id");
        cJSON* d_status = cJSON_GetObjectItemCaseSensitive(dev_item, "status"); // 在线状态等

        cJSON* slim_dev = cJSON_CreateObject();
        if (d_did && d_did->valuestring) cJSON_AddStringToObject(slim_dev, "did", d_did->valuestring);
        if (d_name && d_name->valuestring) cJSON_AddStringToObject(slim_dev, "name", d_name->valuestring);
        if (d_model && d_model->valuestring) cJSON_AddStringToObject(slim_dev, "model", d_model->valuestring);
        if (d_room_id) {
            if (cJSON_IsString(d_room_id) && d_room_id->valuestring) {
                cJSON_AddStringToObject(slim_dev, "room_id", d_room_id->valuestring);
            } else if (cJSON_IsNumber(d_room_id)) {
                cJSON_AddStringToObject(slim_dev, "room_id", std::to_string((long long)d_room_id->valuedouble).c_str());
            }
        }
        if (cJSON_IsNumber(d_status)) cJSON_AddNumberToObject(slim_dev, "status", d_status->valueint);

        cJSON_AddItemToArray(slim_devices_array, slim_dev);
    }

    std::unique_ptr<char, decltype(&std::free)> devices_string(cJSON_PrintUnformatted(slim_devices_array), std::free);
    cJSON_Delete(slim_devices_array);

    if (!devices_string) {
        return std::unexpected("无法序列化精简后的米家设备列表");
    }
    return std::string(devices_string.get());
}

ToolResult GetDevicesListOnDedicatedStack(const std::string& home_id) {
    struct TaskContext {
        std::string home_id;
        SemaphoreHandle_t completed;
        ToolResult result;
    };

    SemaphoreHandle_t completed = xSemaphoreCreateBinary();
    if (completed == nullptr) {
        return std::unexpected("无法创建米家设备列表任务同步对象");
    }

    TaskContext context{home_id, completed, std::unexpected("米家设备列表任务未完成")};
    const BaseType_t task_status = xTaskCreate(
        [](void* arg) {
            auto* ctx = static_cast<TaskContext*>(arg);
            ctx->result = GetDevicesListInternal(ctx->home_id);
            ESP_LOGI(TAG, "Mijia devices task stack remaining: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            xSemaphoreGive(ctx->completed);
            vTaskDelete(nullptr);
        },
        "mijia_devices", 24576, &context, 4, nullptr);

    if (task_status != pdPASS) {
        vSemaphoreDelete(completed);
        return std::unexpected("无法创建米家设备列表任务");
    }

    xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return std::move(context.result);
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

        lv_label_set_text(caption, "请使用米家 App 扫码");
        lv_obj_set_style_text_color(caption, lv_color_black(), 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_canvas_set_buffer(qr_canvas_, pixels, image_size, image_size, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(qr_canvas_, image_size, image_size);
        lv_image_set_antialias(qr_canvas_, false);
        int max_scale = std::min(display_->width() - 20, display_->height() - 56) / image_size;
        uint32_t scale = static_cast<uint32_t>(std::max(1, std::min(3, max_scale)) * 256);
        lv_image_set_scale(qr_canvas_, scale);
        lv_obj_align(qr_canvas_, LV_ALIGN_CENTER, 0, 6);
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
        if (user_id.empty() || c_user_id.empty() || pass_token.empty() || ssecurity.empty()) {
            error = "小米账号扫码成功，但未返回必要登录凭据";
            return false;
        }
        if (!IsAbsoluteHttpsUrl(callback_url)) {
            error = callback_url.empty()
                        ? "小米账号扫码成功，但长轮询响应缺少 location 回调地址"
                        : "小米账号扫码成功，但返回的 location 不是有效的 HTTPS 地址";
            return false;
        }

        context.cookies["userId"] = user_id;
        context.cookies["cUserId"] = c_user_id;
        context.cookies["passToken"] = pass_token;
        if (!GetMijiaUrl(callback_url, context.user_agent, context.cookies, 15000, body, error,
                         true, IsXiaomiHttpsUrl(callback_url))) {
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
            if (psecurity.empty()) {
                settings.EraseKey("psecurity");
            } else {
                settings.SetString("psecurity", psecurity);
            }
            if (nonce.empty()) {
                settings.EraseKey("nonce");
            } else {
                settings.SetString("nonce", nonce);
            }
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
    const psa_status_t psa_status = psa_crypto_init();
    mijia_crypto_ready = psa_status == PSA_SUCCESS;
    if (!mijia_crypto_ready) {
        ESP_LOGE(TAG, "PSA crypto initialization failed: %ld", static_cast<long>(psa_status));
    }

    static MijiaQrLogin mijia_login(display);
    mijia_login_instance = &mijia_login;
    McpServer::GetInstance().AddTool(
        "self.mijia.login",
        "在屏幕上显示小米账号二维码，用户使用米家 App 扫码后完成登录。登录凭据会保存在设备 NVS 中。"
        "这是用户主动操作，不要由 AI 自行调用。",
        PropertyList(), [](const PropertyList&) -> ToolResult { return mijia_login.Start(); });
        
    McpServer::GetInstance().AddTool(
        "self.mijia.list_homes",
        "列出当前已登录米家账号的家庭列表。需要先完成米家扫码登录。",
        PropertyList(),
        [](const PropertyList&) -> ToolResult { return ListHomesOnDedicatedStack(); });


    
    McpServer::GetInstance().AddTool(
        "self.mijia.get_devices_list",
        "获取米家设备列表。可选参数 home_id：传入则筛选该家庭下的设备，不传则获取全部。",

        []() {
            PropertyList p;
            p.AddProperty(Property("home_id", kPropertyTypeString, std::string("")));
            return p;
        }(), // 这是一个立即执行的 Lambda，直接返回构造好的 PropertyList

        [](const PropertyList& args) -> ToolResult {
            std::string home_id = args["home_id"].value<std::string>();
            return GetDevicesListOnDedicatedStack(home_id);
        });
}

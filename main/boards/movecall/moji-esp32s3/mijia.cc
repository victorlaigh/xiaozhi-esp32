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
// login之类的才会用到

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

// 封装成Request 登录信息那些包含
bool RequestApi(const std::string& uri,  cJSON* request_data_obj, 
                std::string& response_json, std::string& error) {
    std::string api_base_url = "https://api.mijia.tech/app";    //base url
    std::string api_url = api_base_url + uri;
    //ESP_LOGI(TAG, "%s", api_url);
    //ESP_LOGI(TAG, "%s", api_url.c_str());

    if (!mijia_crypto_ready) {  // 等加密模块初始化
        error = "米家加密模块初始化失败，请重启设备后重试";
        cJSON_Delete(request_data_obj);
        return false;
    }

    auto auth_result = LoadMijiaAuth();    // 按需加载当前最新的登录凭证
    if (!auth_result.has_value()) {
        error = "尚未登录米家，请先完成扫码登录";
        cJSON_Delete(request_data_obj);
        return false;
    }
    const auto& creds = auth_result.value();

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> request_data(request_data_obj, cJSON_Delete);
    if (request_data == nullptr) {
        error = "无效的请求数据";
        return false;
    }

    std::unique_ptr<char, decltype(&std::free)> data_json(
        cJSON_PrintUnformatted(request_data.get()), std::free);
    std::string nonce;
    std::string signed_nonce;
    std::vector<uint8_t> encryption_key;
    // 使用加载出来的 creds.ssecurity
    if (!data_json || !MakeApiNonce(nonce) || !MakeSignedNonce(creds.ssecurity, nonce, signed_nonce) ||
        !Base64Decode(signed_nonce, encryption_key)) {
        error = "无法生成米家 API 请求签名";
        return false;
    }

    std::string rc4_hash;
    std::string encrypted_data;
    std::string encrypted_rc4_hash;
    std::string signature;
    if (!MakeApiSignature(uri.c_str(), signed_nonce, data_json.get(), "", rc4_hash) ||
        !EncryptApiParameter(encryption_key, rc4_hash, encrypted_rc4_hash) ||
        !EncryptApiParameter(encryption_key, data_json.get(), encrypted_data) ||
        !MakeApiSignature(uri.c_str(), signed_nonce, encrypted_data, encrypted_rc4_hash, signature)) {
        error = "无法加密米家 API 请求";
        return false;
    }

    const std::string form_data =
        "data=" + UrlEncode(encrypted_data) + "&rc4_hash__=" + UrlEncode(encrypted_rc4_hash) +
        "&signature=" + UrlEncode(signature) + "&ssecurity=" + UrlEncode(creds.ssecurity) +
        "&_nonce=" + UrlEncode(nonce);

    // 组装 Cookies
    std::map<std::string, std::string> cookies = {
        {"cUserId", creds.c_user_id},
        {"yetAnotherServiceToken", creds.service_token},
        {"serviceToken", creds.service_token},
        {"timezone", "GMT+08:00"},
        {"is_daylight", "0"},
        {"dst_offset", "0"},
        {"channel", "MI_APP_STORE"},
        {"countryCode", "CN"},
        {"PassportDeviceId", creds.device_id},
        {"locale", "zh_CN"},
    };

    MijiaHttpResponse response;
    response.cookies = &cookies;
    esp_http_client_config_t config = {
        .url = api_url.c_str(),
        .user_agent = creds.user_agent.c_str(),
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
        error = "无法初始化 HTTPS 连接";
        return false;
    }

    // 设置 Headers
    esp_err_t header_err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (header_err == ESP_OK) header_err = esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    if (header_err == ESP_OK) header_err = esp_http_client_set_header(client, "miot-accept-encoding", "GZIP");
    if (header_err == ESP_OK) header_err = esp_http_client_set_header(client, "miot-encrypt-algorithm", "ENCRYPT-RC4");
    if (header_err == ESP_OK) header_err = esp_http_client_set_header(client, "x-xiaomi-protocal-flag-cli", "PROTOCAL-HTTP2");
    
    const std::string cookie_header = MakeCookieHeader(cookies);
    if (header_err == ESP_OK) header_err = esp_http_client_set_header(client, "Cookie", cookie_header.c_str());
    if (header_err == ESP_OK) header_err = esp_http_client_set_post_field(client, form_data.c_str(), static_cast<int>(form_data.size()));

    if (header_err != ESP_OK) {
        esp_http_client_cleanup(client);
        error = "无法设置请求参数";
        return false;
    }

    const esp_err_t request_err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (request_err != ESP_OK) {
        error = response.too_large ? "响应超出大小限制" : "连接服务失败";
        return false;
    }
    if (status_code != HttpStatus_Ok) {
        error = "服务返回 HTTP " + std::to_string(status_code);
        return false;
    }

    return DecodeApiResponse(response.body, signed_nonce, response_json, error);
}

// 取出results
//std::expected<const cJSON*, std::string> RequestResults(std::string& response){
bool RequestResults(std::string& response, cJSON** results){
   
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(ParseMijiaJson(response), cJSON_Delete);
    if (!result) {
        //return std::unexpected("无法解析米家家庭列表响应");
        ESP_LOGE(TAG, "无法解析列表响应"); 
        return false;
    }

    cJSON* result_data = cJSON_GetObjectItemCaseSensitive(result.get(), "result");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(result.get(), "code");
    if (!cJSON_IsNumber(code) || JsonCode(result.get()) != 0 || !cJSON_IsObject(result_data)) {
        std::string message = JsonString(result.get(), "message");
        if (message.empty()) {
            message = JsonString(result.get(), "desc");
        }
        if (cJSON_IsNumber(code) && message == "Result读取失败") {
            message += "，错误码 " + std::to_string(code->valueint);
        }
        if (message.empty()) {
            message = "Result读取失败，错误码 " +
                      (cJSON_IsNumber(code) ? std::to_string(code->valueint) : "未知");
        }
        const std::string code_text =
            cJSON_IsNumber(code) ? std::to_string(code->valueint) : "unknown";
        ESP_LOGW("RequestResults", "解析Results失败: code=%s, message=%s", code_text.c_str(),
                 message.c_str());
        //return std::unexpected(std::move(message));
        return NULL;
    }

    *results = cJSON_Duplicate(result_data, true);

    return (*results != nullptr);
}

bool RequestHomes(std::string& homes_json, std::string& error) {
    static constexpr char kUri[] = "/v2/homeroom/gethome_merged";
    cJSON* request_data = cJSON_CreateObject(); // 在requestApi声明过删除参数
    if (request_data == nullptr || !cJSON_AddBoolToObject(request_data, "fg", true) ||
        !cJSON_AddBoolToObject(request_data, "fetch_share", true) ||
        !cJSON_AddBoolToObject(request_data, "fetch_share_dev", true) ||
        !cJSON_AddBoolToObject(request_data, "fetch_cariot", true) ||
        !cJSON_AddNumberToObject(request_data, "limit", 300) ||
        !cJSON_AddNumberToObject(request_data, "app_ver", 7) ||
        !cJSON_AddNumberToObject(request_data, "plat_form", 0)) {
        if (request_data) cJSON_Delete(request_data);
        error = "无法创建米家家庭列表请求";
        return false;
    }
    return RequestApi(kUri, request_data, homes_json, error);
}

bool RequestDevices(std::string& devices, std::string& error, const std::string& target_home_id = "") {
  
    if (target_home_id.empty()) {
        std::string response;
        std::string error;
        if (!RequestHomes(response, error)) {
            //return std::unexpected(error.empty() ? "获取米家设备列表失败" : error);
            //return std::unexpected(error);  
            return false;
        }

        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(ParseMijiaJson(response), cJSON_Delete);

            
        cJSON* home_list = cJSON_GetObjectItemCaseSensitive(result.get(), "homelist");
        if (!cJSON_IsArray(home_list)) {
            error = "米家家庭列表响应缺少 homelist";
            return false;
        }

        cJSON* homes_id_array = cJSON_CreateArray();
        cJSON* home_item = NULL;
        cJSON_ArrayForEach(home_item, home_list) {
            cJSON* h_id = cJSON_GetObjectItemCaseSensitive(home_item, "id");
            cJSON* id_array = cJSON_CreateObject();
            if (h_id) cJSON_AddStringToObject(id_array, "id", h_id->valuestring ? h_id->valuestring : std::to_string(h_id->valuedouble).c_str());
        // 如果id是数字或者是字符串，适配安全取法：
        // 建议使用 cJSON_IsString(h_id) ? h_id->valuestring : std::to_string((long long)h_id->valuedouble).c_str()
        
        cJSON_AddItemToArray(homes_id_array, id_array);
        }
    
    // 测试一下这个先返回string  后续继续的要用array执行下一步
     // 原本得json改成序列化精简后的 JSON
    std::unique_ptr<char, decltype(&std::free)> id_string(cJSON_PrintUnformatted(homes_id_array),
                                                             std::free);
    cJSON_Delete(homes_id_array); // 清理临时创建的 cJSON 对象                                                         

    if (!id_string) {
        //return std::unexpected("米家家庭ID列表失败");
        return false;
    }
    //return std::string(id_string.get());
    devices = std::string(id_string.get());
    return true;
    }
  
    /*
    ////
            if home_id is None:
            home_list = self.get_homes_list()
            devices = []
            for home in home_list:
                devices.extend(self._get_devices_list(home["id"]))
            return devices
        else:
            return self._get_devices_list(home_id)    
    ////

    static constexpr char kUri[] = "/home/home_device_list";
    std::string start_did = "";

    cJSON* request_data = cJSON_CreateObject(); // 在requestApi声明过删除参数

    std::vector<std::vector<int>> device_data; // 这里是一个二维数组，存储设备数据
    
    if (request_data == nullptr || 
        //!cJSON_AddNumberToObject(request_data, "home_owner", "xxxx") ||   //从homde id 去取 get home owner做一个函数取
        !cJSON_AddStringToObject(request_data, "home_id", target_home_id.c_str()) ||
        !cJSON_AddNumberToObject(request_data, "limit", 200) ||
        !cJSON_AddStringToObject(request_data, "start_did", start_did.c_str()) ||
        !cJSON_AddBoolToObject(request_data, "get_split_device", true) ||
        !cJSON_AddBoolToObject(request_data, "support_smart_home", true) ||
        !cJSON_AddBoolToObject(request_data, "get_cariot_device", true) ||
        !cJSON_AddBoolToObject(request_data, "get_third_device", true)
    ) {
        if (request_data) cJSON_Delete(request_data);
        error = "无法创建米家设备列表请求";
        return false;
    }
    request_data = RequestApi(kUri, request_data, devices, error);
    if (request_data && cJSON_GetObjectItemCaseSensitive(request_data, "device_info"))
    {
        devices.insert(devices.end(), ret["device_info"].begin(), ret["device_info"].end());

    }
///
       
                devices.extend(ret["device_info"])
                start_did = ret.get("max_did", "")
                has_more = ret.get("has_more", False) and start_did != ""
            else:
                has_more = False
                ///
    ///
            cJSON* h_id = cJSON_GetObjectItemCaseSensitive(home_item, "id");
        cJSON* h_name = cJSON_GetObjectItemCaseSensitive(home_item, "name");
        
        // 创建一个精简版的家庭对象
        cJSON* slim_home = cJSON_CreateObject();
        if (h_id) cJSON_AddStringToObject(slim_home, "id", h_id->valuestring ? h_id->valuestring : std::to_string(h_id->valuedouble).c_str());
        // 如果id是数字或者是字符串，适配安全取法：
        // 建议使用 cJSON_IsString(h_id) ? h_id->valuestring : std::to_string((long long)h_id->valuedouble).c_str()
        if (h_name) cJSON_AddStringToObject(slim_home, "name", h_name->valuestring ? h_name->valuestring : "");
       
        ///

    return RequestApi(kUri, request_data, devices, error);

    */
   return true;
}

// 精简米家家庭列表中的房间和设备信息，生成一个只包含家庭ID和UID的数组
static void s_make_id_array(const char *key, const char *uid, const cJSON *source_list, cJSON *target_array)
{
    if (!key || !source_list || !target_array) {
        return;
    }

    if (!cJSON_IsArray(source_list) || !cJSON_IsArray(target_array)) {
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, source_list) {
        cJSON *val_obj = cJSON_GetObjectItemCaseSensitive(item, key);
        cJSON *val_uid = cJSON_GetObjectItemCaseSensitive(item, uid);

        std::string str_val;
        if (val_obj) {
            if (cJSON_IsString(val_obj) && val_obj->valuestring) {
                str_val = val_obj->valuestring;
            } else if (cJSON_IsNumber(val_obj)) {
                // 注意：to_string(double) 可能产生多余小数位，视业务需求可改用 snprintf
                str_val = std::to_string(val_obj->valuedouble);
            }
        }

        std::string uid_val;
        if (val_uid) {
            if (cJSON_IsString(val_uid) && val_uid->valuestring) {
                uid_val = val_uid->valuestring;
            } else if (cJSON_IsNumber(val_uid)) {
                // 注意：to_string(double) 可能产生多余小数位，视业务需求可改用 snprintf
                uid_val = std::to_string(val_uid->valuedouble);
            }
        }

        // 创建新对象 {"key": "value"}
        cJSON *new_obj = cJSON_CreateObject();
        if (new_obj) {
            cJSON_AddStringToObject(new_obj, key, str_val.c_str());
            cJSON_AddStringToObject(new_obj, uid, uid_val.c_str());
            cJSON_AddItemToArray(target_array, new_obj);
        }
    }
}

ToolResult ListHomes() {

    std::string response;
    std::string error;
    if (!RequestHomes(response, error)) {
        return std::unexpected(error);  
        // return std::unexpected(error.empty() ? "获取米家设备列表失败" : error);
    }
    
    cJSON* result_data = nullptr;     // 这个需要另外调用删除函数吗？
    RequestResults(response, &result_data);
    if(!result_data){
        return std::unexpected("家庭列表获取result失败");  
    } // 包含parse 返回result内容
         

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
                // 好像得自己补全 参考上面home得
                // if (r_id) cJSON_AddStringToObject(slim_room, "id", r_id->valuestring ? r_id->valuestring : std::to_string(r_id->valuedouble).c_str());
                // if (r_name) cJSON_AddStringToObject(slim_room, "name", r_name->valuestring ? r_name->valuestring : "");
                // 上面两行自动补全的 要检查一下
                cJSON_AddItemToArray(slim_rooms_array, slim_room);
            }
            cJSON_AddItemToObject(slim_home, "rooms", slim_rooms_array);
        }
        
        cJSON_AddItemToArray(slim_homes_array, slim_home);

    }
           // 【重要】因为 result_data 是 Duplicate 出来的独立对象，必须手动释放
        cJSON_Delete(result_data);
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

static void request_devices(const char *id, const char *uid, std::string& devices_json, std::string& error){
    static constexpr char kUri[] = "/home/home_device_list";
    
    std::string start_did = "";
    cJSON* request_data = cJSON_CreateObject(); // 在requestApi声明过删除参数
    std::vector<std::vector<int>> device_data; // 这里是一个二维数组，存储设备数据
    // owner id 是 Homes的uid

    if (request_data == nullptr || 
        !cJSON_AddStringToObject(request_data, "home_owner", uid) || 
        !cJSON_AddStringToObject(request_data, "home_id", id) ||
        !cJSON_AddNumberToObject(request_data, "limit", 200) ||
        !cJSON_AddStringToObject(request_data, "start_did", start_did.c_str()) ||
        !cJSON_AddBoolToObject(request_data, "get_split_device", true) ||
        !cJSON_AddBoolToObject(request_data, "support_smart_home", true) ||
        !cJSON_AddBoolToObject(request_data, "get_cariot_device", true) ||
        !cJSON_AddBoolToObject(request_data, "get_third_device", true)
    ) {
        if (request_data) cJSON_Delete(request_data);
        error = "无法创建米家设备列表请求";
        return;
    }
    RequestApi(kUri, request_data, devices_json, error);    // 获取资料
    //ESP_LOGI("request_devices", "执行完，如果错误： %s", error.c_str());
}

ToolResult GetDevicesListInternal(const std::string& target_home_id) {
    std::string response;
    std::string error;
        
    if (!RequestHomes(response, error)) {   // Request空白报错
        return std::unexpected(error.empty() ? "获取米家设备列表失败" : error);
        ESP_LOGE("MIJIA", "获取家庭列表失败: %s", error.c_str());
    }
    
    cJSON* result_data = nullptr;
    RequestResults(response, &result_data); // 包含parse 返回result内容,要调用删除函数
    if(!result_data){   // 取出得Result空白报错
        return std::unexpected("Result内容读取失败");  
    } 

    // 深拷贝，使其独立于 result_data 现在可以安全地释放原始对象
    cJSON *result_homelist = cJSON_Duplicate(result_data, true);
    cJSON_Delete(result_data);  // 【重要】因为是 Duplicate 出来的独立对象，必须手动释放
    // 继续使用 result_homelist...
    const cJSON* home_list = cJSON_GetObjectItemCaseSensitive(result_homelist, "homelist");
    if (!cJSON_IsArray(home_list)) {    //如果取出来不是列表报错
        return std::unexpected("获取设备时家庭列表响应缺少 homelist");
    }
    
    cJSON* homes_id_array = cJSON_CreateArray();    // id uid列表, 用完之后要delete
    if (!target_home_id.empty()) {   // 如果id不空白,也是放homes_id_array 但是就一组 可能也能用名字？
        int array_size = cJSON_GetArraySize(home_list);
        int uid_int = 0;
        for (int i = 0; i < array_size; i++) {  // 遍历数组
            // 获取数组中的第 i 个元素（这是一个对象）
            cJSON *item = cJSON_GetArrayItem(home_list, i);
            if (item == NULL || !cJSON_IsObject(item)) {
                continue; // 跳过非对象元素
            }

            // 获取当前对象的 "id" 字段
            cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(item, "id");
            // 跳过检查 id 字段是否存在且是数字类型 if (id_obj != NULL && cJSON_IsNumber(id_obj)) {
            // 比较 id 值
            //if (id_obj->valueint == target_id) {
            if (id_obj->valuestring == target_home_id) {

                // 匹配项，提取 "uid"
                cJSON *uid_obj = cJSON_GetObjectItemCaseSensitive(item, "uid");
                uid_int = uid_obj->valueint;
                /* 数字判断之类 先不用
                if (uid_obj != NULL && cJSON_IsString(uid_obj) && uid_obj->valuestring != NULL) {
                    printf("Found! ID: %d, UID: %s\n", target_id, uid_obj->valuestring);
                    found = 1;
                    break; // 找到后通常可以退出循环
                } else {
                    printf("Found ID: %d, but 'uid' field is missing or invalid.\n", target_id);
                    found = 1;
                    break;
                }*/
                cJSON *new_obj = cJSON_CreateObject();
                if (new_obj) {
                    cJSON_AddStringToObject(new_obj, "id", target_home_id.c_str());
                    cJSON_AddNumberToObject(new_obj, "uid", (double)uid_int);
                    cJSON_AddItemToArray(homes_id_array, new_obj);
                }
                break; // 找到后退出循环
            }   
        }
    }

    else if (target_home_id.empty()) {   // 如果id空白就列表     
        s_make_id_array("id","uid", home_list, homes_id_array); // 可能之后要增加名字 方便用名字直接控制
    }
      
    // cJSON* device_array = cJSON_CreateObject(); // 创建一个设备主列表, 用完要delete
    cJSON* device_array = cJSON_CreateArray(); // 创建一个设备主列表, 用完要delete
    cJSON *devitem = NULL; 
    cJSON_ArrayForEach(devitem, homes_id_array) {   // 获取每个id设备列表 加到主列表
        cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(devitem, "id");
        cJSON *uid_obj = cJSON_GetObjectItemCaseSensitive(devitem, "uid");

        if ( !(id_obj || cJSON_IsString(id_obj)  ||  id_obj->valuestring != NULL ||
            uid_obj || cJSON_IsString(uid_obj) || uid_obj->valuestring != NULL)) {
            return std::unexpected("获取id,uid失败");
        }
            const char *id_str = id_obj->valuestring;
            const char *uid_str = uid_obj->valuestring; // 取出id uid
            ESP_LOGI("cJSON_ArrayForEach", "ID： %s", id_str);
            /*学习一下三元写法
            #define SAFE_GET_STRING(item) ((item) && cJSON_IsString(item) ? (item)->valuestring : NULL)
            const char *id_str = SAFE_GET_STRING(id_obj);
            const char *id_str =  ((id_obj) && cJSON_IsString(id_obj) ? (id_obj)->valuestring : NULL);*/
            
            std::string devices_json;   // 获取设备列表
            request_devices(id_str, uid_str, devices_json, error);  //id, owner, 设备列表, 错误
            /*         之后改成bool 前面套一个 if(!)     
            if (!request_devices(id_str, uid_str, devices_json, error)) {
                    return std::unexpected(error.empty() ? "获取米家设备列表失败" : error);
                    ESP_LOGE("MIJIA", "获取家庭列表失败: %s", error.c_str());
            }*/
            cJSON* result_data = NULL;
            RequestResults(devices_json, &result_data); // 包含parse 返回result内容,需要另外调用删除
            cJSON* device_info = cJSON_GetObjectItemCaseSensitive(result_data, "device_info");
            // 取出result内device_info

        // 下面开始精简
        if (!cJSON_IsArray(device_info)) {  // device_info空白报错
        return std::unexpected("米家设备列表响应缺少设备数组");
        }

        int devsize = cJSON_GetArraySize(device_info);
        for (int i = 0; i < devsize; i++) { // 遍历数组，精简完添加到device_array
            // 获取数组中的第 i 个元素（这是一个对象）
            cJSON *device_obj = cJSON_GetArrayItem(device_info, i);
            if (device_obj != NULL && cJSON_IsObject(device_obj)) {
                // === 【核心优化：数据瘦身】只提取大模型和 MCP 需要的关键字段 ===
                cJSON* d_did = cJSON_GetObjectItemCaseSensitive(device_obj, "did");	//string
                cJSON* d_name = cJSON_GetObjectItemCaseSensitive(device_obj, "name");	//string
                cJSON* d_model = cJSON_GetObjectItemCaseSensitive(device_obj, "model");	//string
                cJSON* d_spec_type = cJSON_GetObjectItemCaseSensitive(device_obj, "spec_type"); //string
                cJSON* d_status = cJSON_GetObjectItemCaseSensitive(device_obj, "isOnline"); // True False bool值
                // 这边的uid是数字 需要的话用前面的就好
                // const char *name_str = d_name->valuestring;
                // ESP_LOGI("cJSON_ArrayForEach", "name： %s", name_str);
                // 安全校验并读取
                if (d_model != NULL && cJSON_IsString(d_model)) {
                    const char *model_str = d_model->valuestring;
                    ESP_LOGI("cJSON_ArrayForEach", "model: %s", model_str);
                } 

                //创建精简版对象
                // cJSON* slim_dev = cJSON_CreateObject();
                cJSON* slim_dev = cJSON_CreateArray();
                if (d_did && d_did->valuestring) cJSON_AddStringToObject(slim_dev, "did", d_did->valuestring);
                if (d_name && d_name->valuestring) cJSON_AddStringToObject(slim_dev, "name", d_name->valuestring);
                if (d_model && d_model->valuestring) cJSON_AddStringToObject(slim_dev, "model", d_model->valuestring);
                if (d_spec_type && d_spec_type->valuestring) cJSON_AddStringToObject(slim_dev, "spec_type", d_spec_type->valuestring);
                if (d_status && cJSON_IsBool(d_status)) {
                        if (cJSON_IsTrue(d_status)) cJSON_AddBoolToObject(slim_dev, "isOnline", true);
                        else cJSON_AddBoolToObject(slim_dev, "isOnline", false);
                }
                cJSON_AddStringToObject(slim_dev, "id", id_str);    // 加入id
                // 之后看看要不要再加入home name
                // 如果id是数字或者是字符串，适配安全取法：
                // 建议使用 cJSON_IsString(h_id) ? h_id->valuestring : std::to_string((long long)h_id->valuedouble).c_str() 
                cJSON_AddItemToArray(device_array, slim_dev);    // 加到device_array内
            }
        }
        ///上面是精简
    }
    cJSON_Delete(result_data);
    cJSON_Delete(result_homelist);  // 循环完释放循环列表
    cJSON_Delete(homes_id_array); // 释放内存：只需释放根节点，子节点会递归释放

    std::unique_ptr<char, decltype(&std::free)> dev_string(cJSON_PrintUnformatted(device_array),
                                                            std::free);
    cJSON_Delete(device_array); // 清理临时创建的 cJSON 对象   
        // 【重要】因为 result_data 是 Duplicate 出来的独立对象，必须手动释放
        // cJSON_Delete(result_data);
        //return std::string(dev_string.get()); //返回dev string
        if (dev_string) {
            // 使用 .get() 获取原始 char* 指针，配合 %s 输出
            ESP_LOGI(TAG, "设备info: %s", dev_string.get());
        } else {
            ESP_LOGE(TAG, "设备info解析失败");
        }

       //         return std::string(dev_string.get()); //返回dev string
    /* 
                devices.extend(ret["device_info"])
                start_did = ret.get("max_did", "")
                has_more = ret.get("has_more", False) and start_did != ""
            else:
                has_more = False
                ///                            
    return std::string(id_string.get());
    }
*/
    // 【重要】因为 result_data 是 Duplicate 出来的独立对象，必须手动释放
    //cJSON_Delete(result_data);
    return "1"; // 这里是一个占位符，表示获取设备列表的逻辑还未实现
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
        "获取米家设备列表。可选参数 home_id：传入则筛选该家庭下的设备，不传则获取全部。"
        "用户没有指定哪个家庭的话,默认就不传入参数,指定的话用list_homes工具查看id传入",

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

#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <esp_random.h>
#include <esp_log.h>

// 定义标签，用于 ESP_LOG 输出
static const char *TAG = "UserAgentGen";

class DeviceConfig {
public:
    // 模拟 auth_data 字典
    std::map<std::string, std::string> auth_data;
    
    // 模拟 locale 和 pass_o
    std::string locale;
    std::string pass_o;

    DeviceConfig() : locale("zh_CN"), pass_o("default_pass_hash") {}

    /**
     * @brief 生成指定长度的随机十六进制字符串
     * 
     * @param length 需要的字符长度 (必须是偶数，因为每个字节产生2个hex字符)
     * @return std::string 随机十六进制字符串
     */
    std::string generateRandomHex(size_t length) {
        if (length % 2 != 0) {
            ESP_LOGW(TAG, "Length should be even for hex generation, adjusting...");
            length++; 
        }
        
        size_t byte_count = length / 2;
        uint8_t *buffer = new uint8_t[byte_count];
        
        // 使用 ESP-IDF 硬件随机数生成器
        esp_fill_random(buffer, byte_count);
        
        char hex_str[length + 1];
        for (size_t i = 0; i < byte_count; i++) {
            sprintf(&hex_str[i * 2], "%02X", buffer[i]);
        }
        hex_str[length] = '\0';
        
        std::string result(hex_str);
        delete[] buffer; // 释放临时缓冲区
        return result;
    }

    /**
     * @brief 获取地区代码 (例如从 "zh_CN" 提取 "CN")
     */
    std::string getLocaleCode() {
        if (locale.empty()) {
            return "CN";
        }
        size_t pos = locale.find('_');
        if (pos != std::string::npos && pos + 1 < locale.length()) {
            return locale.substr(pos + 1);
        }
        return "CN"; // 默认 fallback
    }

    /**
     * @brief 生成或获取 User Agent
     */
    std::string getUserAgent() {
        // 1. 缓存检查
        auto it = auth_data.find("ua");
        if (it != auth_data.end() && !it->second.empty()) {
            return it->second;
        }

        // 2. 生成随机 ID
        // Python: k=40 -> 20 bytes
        std::string ua_id1 = generateRandomHex(40);
        // Python: k=32 -> 16 bytes
        std::string ua_id2 = generateRandomHex(32);
        std::string ua_id3 = generateRandomHex(32);
        std::string ua_id4 = generateRandomHex(40);

        // 地区代码CN
        std::string locale_code = "getLocaleCode()";

        // 4. 构建 UA 字符串
        // 预估最大长度，避免频繁 realloc
        char ua_buffer[512];
        
        int len = snprintf(ua_buffer, sizeof(ua_buffer),
            "Android-15-11.0.701-Xiaomi-23046RP50C-OS2.0.212.0.VMYCNXM-"
            "%s-%s-"
            "%s-%s-SmartHome-MI_APP_STORE-"
            "%s|%s|%s-64",
            ua_id1.c_str(),
            locale_code.c_str(),
            ua_id3.c_str(),
            ua_id2.c_str(),
            ua_id1.c_str(),
            ua_id4.c_str(),
            pass_o.c_str()
        );

        if (len < 0 || len >= sizeof(ua_buffer)) {
            ESP_LOGE(TAG, "UA generation failed or truncated");
            return "";
        }

        std::string ua_str(ua_buffer);
        
        // 5. 缓存结果
        auth_data["ua"] = ua_str;
        
        ESP_LOGD(TAG, "Generated UA: %s", ua_str.c_str());
        return ua_str;
    }
};

// 示例用法 (通常在 app_main 中调用)
extern "C" void app_main(void) {
    DeviceConfig config;
    
    // 第一次调用：生成并缓存
    std::string ua1 = config.getUserAgent();
    printf("UA 1: %s\n", ua1.c_str());
    
    // 第二次调用：直接返回缓存
    std::string ua2 = config.getUserAgent();
    printf("UA 2: %s\n", ua2.c_str());
    
    // 验证是否一致
    if (ua1 == ua2) {
        printf("Cache working correctly.\n");
    }
}

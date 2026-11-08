#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"
#include "weather.h"
#include "weather_icon_font.h"
// weather icon跟description移到这了

#include <material_symbols.h>
#include <driver/i2c_master.h>
#include <esp_efuse_table.h>
#include <esp_log.h>
#include <esp_timer.h>
// 为了计时启动后隐藏待命

#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define TAG "MovecallMojiESP32S3"

void InitializeMojiMijiaTool(Display* display);
void HideMojiMijiaLoginQr();
// 待机时还没登录也隐藏QR用

LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(lv_font_montserrat_48); //声明字体,尺寸
// 启用方式：运行 idf.py menuconfig，进入 Component config → LVGL configuration → Fonts → Montserrat 48 打开它，保存配置后重新构建。

namespace {

enum class MojiStandbyStyle {
    Minimal,
    Weather,
    Instrument,
};

const char* StandbyStyleName(MojiStandbyStyle style) {
    switch (style) {
        case MojiStandbyStyle::Weather:
            return "moji-weather";
        case MojiStandbyStyle::Instrument:
            return "moji-instrument";
        case MojiStandbyStyle::Minimal:
        default:
            return "moji-minimal";
    }
}

bool ParseStandbyStyle(const std::string& name, MojiStandbyStyle& style) {
    if (name == "moji-minimal") {
        style = MojiStandbyStyle::Minimal;
        return true;
    }
    if (name == "moji-weather") {
        style = MojiStandbyStyle::Weather;
        return true;
    }
    if (name == "moji-instrument") {
        style = MojiStandbyStyle::Instrument;
        return true;
    }
    return false;
}

MojiStandbyStyle DefaultStandbyStyle() {
#if CONFIG_MOJI_STANDBY_STYLE_WEATHER
    return MojiStandbyStyle::Weather;
#elif CONFIG_MOJI_STANDBY_STYLE_INSTRUMENT
    return MojiStandbyStyle::Instrument;
#else
    return MojiStandbyStyle::Minimal;
#endif
}

void RegisterMojiStandbyThemes() {
    static std::vector<std::unique_ptr<LvglTheme>> themes;
    auto& manager = LvglThemeManager::GetInstance();
    auto base = manager.GetTheme("dark");
    if (base == nullptr || !themes.empty()) {
        return;
    }

    for (const char* name : {"moji-minimal", "moji-weather", "moji-instrument"}) {
        auto theme = std::make_unique<LvglTheme>(name);
        theme->set_background_color(base->background_color());
        theme->set_text_color(base->text_color());
        theme->set_chat_background_color(base->chat_background_color());
        theme->set_user_bubble_color(base->user_bubble_color());
        theme->set_assistant_bubble_color(base->assistant_bubble_color());
        theme->set_system_bubble_color(base->system_bubble_color());
        theme->set_system_text_color(base->system_text_color());
        theme->set_border_color(base->border_color());
        theme->set_low_battery_color(base->low_battery_color());
        theme->set_text_font(base->text_font());
        theme->set_icon_font(base->icon_font());
        theme->set_large_icon_font(base->large_icon_font());
        theme->set_emoji_font(base->emoji_font());
        manager.RegisterTheme(name, theme.get());
        themes.push_back(std::move(theme));
    }
}

}  // namespace

class CustomLcdDisplay : public SpiLcdDisplay {
    friend class MovecallMojiESP32S3;
    // 将 MovecallMojiESP32S3 声明为友元类，保留板级代码访问显示私有成员的能力。
    // Weather::Service 现在由 SetupUI() 启动，并直接接收天气相关标签。

private:
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* weather_icon_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* high_low_label_ = nullptr;
    lv_obj_t* weather_card_ = nullptr;
    lv_obj_t* instrument_ring_ = nullptr;
    // 增加 weather service_ , 代码放weather.cc
    Weather::Service weather_service_;
    // 启动后计时隐藏待命用
    esp_timer_handle_t status_rehide_timer_ = nullptr;
    std::atomic<bool> standby_mode_{false};
    bool use_standard_theme_colors_ = false;
    int last_displayed_clock_minute_ = -1;
    MojiStandbyStyle standby_style_ = DefaultStandbyStyle();

    void ApplyMojiStyle() {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        if (!lock) {
            return;
        }

        lv_color_t background = lv_color_hex(0x10151D);
        lv_color_t foreground = lv_color_hex(0xF2F4F7);
        lv_color_t secondary = lv_color_hex(0xAAB4C1);
        lv_color_t accent = lv_color_hex(0xD6A36B);
        lv_color_t card = lv_color_hex(0x10151D);
        lv_opa_t card_opacity = LV_OPA_TRANSP;

        switch (standby_style_) {
            case MojiStandbyStyle::Weather:
                background = lv_color_hex(0x15344F);
                foreground = lv_color_hex(0xF4F8FC);
                secondary = lv_color_hex(0xD0E2F0);
                accent = lv_color_hex(0xFFD18A);
                card = lv_color_hex(0xD9EDFF);
                card_opacity = LV_OPA_20;
                break;
            case MojiStandbyStyle::Instrument:
                background = lv_color_hex(0x111D18);
                foreground = lv_color_hex(0xE8F0E7);
                secondary = lv_color_hex(0xA8B9A8);
                accent = lv_color_hex(0xD8B477);
                break;
            case MojiStandbyStyle::Minimal:
            default:
                break;
        }

        if (use_standard_theme_colors_ && current_theme_ != nullptr) {
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            background = theme->background_color();
            foreground = theme->text_color();
            secondary = theme->text_color();
        }

        lv_obj_set_style_bg_color(lv_screen_active(), background, 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, background, 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_grad_color(
                container_,
                standby_style_ == MojiStandbyStyle::Weather && !use_standard_theme_colors_
                    ? lv_color_hex(0x0D1A31)
                    : background,
                0);
            lv_obj_set_style_bg_grad_dir(
                container_,
                standby_style_ == MojiStandbyStyle::Weather && !use_standard_theme_colors_
                    ? LV_GRAD_DIR_VER
                    : LV_GRAD_DIR_NONE,
                0);
        }
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_color(top_bar_, background, 0);
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        }
        if (status_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        }
        for (lv_obj_t* label : {network_label_, status_label_, notification_label_, mute_label_,
                                battery_label_, emoji_label_, chat_message_label_}) {
            if (label != nullptr) {
                lv_obj_set_style_text_color(label, foreground, 0);
            }
        }


        auto text_font = current_theme_ != nullptr ? current_theme_->GetTextFont() : nullptr;
        if (text_font != nullptr && text_font->font() != nullptr) {
            for (lv_obj_t* label : {city_label_, date_label_, weather_label_, high_low_label_}) {
                if (label != nullptr) {
                    lv_obj_set_style_text_font(label, text_font->font(), 0);
                }
            }
        }
        // 下面这几个设置颜色而已
        if (city_label_ != nullptr) {
            lv_obj_set_style_text_color(city_label_, secondary, 0);
        }
        if (date_label_ != nullptr) {
            lv_obj_set_style_text_color(date_label_, secondary, 0);
        }
        if (time_label_ != nullptr) {
            lv_obj_set_style_text_color(time_label_, foreground, 0);
        }
        if (weather_icon_label_ != nullptr) {
            lv_obj_set_style_text_color(weather_icon_label_, accent, 0);
        }
        if (weather_label_ != nullptr) {
            lv_obj_set_style_text_color(weather_label_, foreground, 0);
        }
        if (high_low_label_ != nullptr) {
            lv_obj_set_style_text_color(high_low_label_, secondary, 0);
        }

        if (weather_card_ != nullptr) {
            lv_obj_set_style_bg_color(weather_card_, card, 0);
            lv_obj_set_style_bg_opa(weather_card_, card_opacity, 0);
            lv_obj_set_style_border_color(weather_card_, secondary, 0);
            lv_obj_set_style_border_opa(
                weather_card_,
                standby_style_ == MojiStandbyStyle::Weather ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        }
        if (instrument_ring_ != nullptr) {
            lv_obj_set_style_border_color(instrument_ring_, accent, 0);
            lv_obj_set_style_border_opa(
                instrument_ring_,
                standby_style_ == MojiStandbyStyle::Instrument ? LV_OPA_50 : LV_OPA_TRANSP, 0);
        }

        if (date_label_ != nullptr) {
            lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0,
                         standby_style_ == MojiStandbyStyle::Weather ? 58 : 56);
        }

        if (weather_card_ != nullptr) {
            lv_obj_align(weather_card_, LV_ALIGN_TOP_MID, 0, 155);
            lv_obj_set_size(weather_card_, LV_HOR_RES - 50, 62);
        }

        if (weather_icon_label_ != nullptr) {
            // lv_obj_align(weather_icon_label_, LV_ALIGN_TOP_LEFT, 26,
                        // standby_style_ == MojiStandbyStyle::Weather ? 166 : 160);
            lv_obj_align(weather_icon_label_, LV_ALIGN_LEFT_MID, 20, 0);
        }
        if (high_low_label_ != nullptr) {
                lv_obj_align(high_low_label_, LV_ALIGN_TOP_MID, 0, 188);
        }
        if (weather_card_ != nullptr) {
            bool show_card = standby_mode_.load(std::memory_order_relaxed) &&
                             standby_style_ == MojiStandbyStyle::Weather;
            if (show_card) {
                lv_obj_remove_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (instrument_ring_ != nullptr) {
            bool show_ring = standby_mode_.load(std::memory_order_relaxed) &&
                             standby_style_ == MojiStandbyStyle::Instrument;
            if (show_ring) {
                lv_obj_remove_flag(instrument_ring_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(instrument_ring_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void UpdateClock(bool update_all) {
        if (!standby_mode_.load(std::memory_order_relaxed) || time_label_ == nullptr ||
            date_label_ == nullptr) {
            return;
        }

        time_t now = time(nullptr);
        struct tm local_time = {};
        bool time_is_valid =
            localtime_r(&now, &local_time) != nullptr && local_time.tm_year >= 2025 - 1900;
        int current_minute = time_is_valid ? local_time.tm_hour * 60 + local_time.tm_min : -1;
        if (!update_all && current_minute == last_displayed_clock_minute_) {
            return;
        }
        last_displayed_clock_minute_ = current_minute;

        char time_text[8] = "--:--";
        char date_text[32] = "SYNCING DATE";
        if (time_is_valid) {
            snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour,
                     local_time.tm_min);
            static constexpr const char* weekdays[] = {"星期日", "星期一", "星期二", "星期三",
                                                       "星期四", "星期五", "星期六"};
            snprintf(date_text, sizeof(date_text), "%d月%d日  %s", local_time.tm_mon + 1,
                     local_time.tm_mday, weekdays[local_time.tm_wday]);
        }

        DisplayLockGuard lock(this);
        if (!lock) {
            return;
        }
        lv_label_set_text(time_label_, time_text);
        lv_label_set_text(date_label_, date_text);
    }

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y, bool mirror_x,
                     bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {
        // 计时隐藏待命
        esp_timer_create_args_t status_rehide_timer_args = {
            .callback =
                [](void* arg) {
                    auto* display = static_cast<CustomLcdDisplay*>(arg);
                    DisplayLockGuard lock(display);
                    if (lock && display->standby_mode_.load(std::memory_order_relaxed) &&
                        display->status_label_ != nullptr) {
                        lv_obj_add_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "moji_status_rehide",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&status_rehide_timer_args, &status_rehide_timer_));
        // 到这边
        RegisterThemesAndLoadStyle();
    }

    // 计时隐藏待命用
    ~CustomLcdDisplay() override {
        if (status_rehide_timer_ != nullptr) {
            esp_timer_stop(status_rehide_timer_);
            esp_timer_delete(status_rehide_timer_);
        }
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        // 这个父类定义在lcd_display.cc 实际调用的是基类LcdDisplay的实现
        SpiLcdDisplay::SetupUI();

        {
            DisplayLockGuard lock(this);
            // 获取显示锁，避免在 LVGL 正在操作界面时修改对象
            if (status_bar_ != nullptr) {
                lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.33, 0);
                lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.33, 0);

            }

            auto screen = lv_screen_active();
            city_label_ = lv_label_create(screen);
            lv_obj_set_width(city_label_, LV_HOR_RES - 32);
            lv_label_set_long_mode(city_label_, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(city_label_, &font_noto_sans_basic_20_4, 0);
            lv_obj_set_style_text_align(city_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(city_label_, "定位中");
            lv_obj_align(city_label_, LV_ALIGN_TOP_MID, 0, 30);
            // 水平居中，距离顶部30px,比原本少8

            date_label_ = lv_label_create(screen);
            lv_obj_set_width(date_label_, LV_HOR_RES - 32);
            lv_obj_set_style_text_font(date_label_, &font_noto_sans_basic_20_4, 0);
            lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(date_label_, "日期同步中");
            lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 56);
            // 水平居中，距离顶部56px,比原本少8

            time_label_ = lv_label_create(screen);
            lv_obj_set_width(time_label_, LV_HOR_RES - 24);
            lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
            // 创建时间标签时，使用 Montserrat 字体，大小为 48px
            lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_letter_space(time_label_, 1, 0);
            lv_label_set_text(time_label_, "--:--");
            lv_obj_align(time_label_, LV_ALIGN_CENTER, 5, -5);

            weather_icon_label_ = lv_label_create(screen);
            lv_obj_set_width(weather_icon_label_, 36);
            lv_obj_set_style_text_font(weather_icon_label_, &moji_weather_icon_font, 0);
            // lv_obj_set_style_text_font(weather_icon_label_, &font_material_symbols_30_4, 0);
            // 30px的 Material Symbols 图标字体，每像素使用 4-bit 灰阶
            lv_obj_set_style_text_align(weather_icon_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(weather_icon_label_, MATERIAL_SYMBOLS_CLOUD_OFF);
            lv_obj_align(weather_icon_label_, LV_ALIGN_LEFT_MID, 24, 6);
            // lv_obj_align(weather_icon_label_, LV_ALIGN_TOP_LEFT, 30, 152);
            // 左上角对齐，距离左边30px，距离顶部152px, 本来是26,160

            weather_label_ = lv_label_create(screen);
            lv_obj_set_width(weather_label_, LV_HOR_RES - 72);
            lv_obj_set_style_text_font(weather_label_, &font_noto_sans_basic_20_4, 0);
            lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(weather_label_, "天气更新中");
            lv_obj_align(weather_label_, LV_ALIGN_TOP_MID, 0, 160);
            // 本来是162

            high_low_label_ = lv_label_create(screen);
            lv_obj_set_width(high_low_label_, LV_HOR_RES - 32);
            lv_obj_set_style_text_font(high_low_label_, &font_noto_sans_basic_20_4, 0);
            lv_obj_set_style_text_align(high_low_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(high_low_label_, "");
            lv_obj_align(high_low_label_, LV_ALIGN_TOP_MID, 0, 180);
            // 本来190,改180

            weather_card_ = lv_obj_create(screen);
            lv_obj_remove_style_all(weather_card_);
            lv_obj_set_style_radius(weather_card_, 20, 0);
            lv_obj_set_style_border_width(weather_card_, 1, 0);
            lv_obj_move_to_index(weather_card_, 1);
            // lv_obj_add_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);

            instrument_ring_ = lv_obj_create(screen);
            lv_obj_remove_style_all(instrument_ring_);
            lv_obj_set_size(instrument_ring_, 220, 220);
            lv_obj_set_style_radius(instrument_ring_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(instrument_ring_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(instrument_ring_, 1, 0);
            lv_obj_align(instrument_ring_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_move_to_index(instrument_ring_, 2);
            lv_obj_add_flag(instrument_ring_, LV_OBJ_FLAG_HIDDEN);

            for (lv_obj_t* object : {city_label_, date_label_, time_label_,
                                     weather_icon_label_, weather_label_, high_low_label_
                                    }) {
                if (object != nullptr) {
                    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
                }
                // 如果标签未创建就跳过，避免卡死
            }
        }

        ApplyMojiStyle();
        if (!weather_service_.Start(this, city_label_, weather_icon_label_, weather_label_,
                                    high_low_label_)) {
            ESP_LOGE(TAG, "Failed to start weather refresh task");
        }
    }

    void SetTheme(Theme* theme) override {
        if (theme == nullptr) {
            ESP_LOGW(TAG, "Ignoring null theme");
            return;
        }

        MojiStandbyStyle style;
        if (ParseStandbyStyle(theme->name(), style)) {
            use_standard_theme_colors_ = false;
            standby_style_ = style;
            Settings settings("display", true);
            settings.SetString("standby_style", StandbyStyleName(style));
            ApplyMojiStyle();
            return;
        }

        SpiLcdDisplay::SetTheme(theme);
        use_standard_theme_colors_ = true;
        ApplyMojiStyle();
    }

    bool SetTextFont(std::shared_ptr<LvglFont> text_font) override {
        if (text_font == nullptr || text_font->font() == nullptr) {
            return false;
        }

        auto& manager = LvglThemeManager::GetInstance();
        for (const char* name : {"moji-minimal", "moji-weather", "moji-instrument"}) {
            auto theme = manager.GetTheme(name);
            if (theme != nullptr) {
                theme->set_text_font(text_font);
            }
        }
        return SpiLcdDisplay::SetTextFont(std::move(text_font));
    }

    void SetStatus(const char* status) override {
        SpiLcdDisplay::SetStatus(status);
        const bool is_standby_status =
            status != nullptr && std::strcmp(status, Lang::Strings::STANDBY) == 0;
        if ((standby_mode_.load(std::memory_order_relaxed) || is_standby_status) && 
            status_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (lock) {
                lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // 计时隐藏待命用
    void ShowNotification(const char* notification, int duration_ms) override {
        SpiLcdDisplay::ShowNotification(notification, duration_ms);
        if (status_rehide_timer_ == nullptr || notification_label_ == nullptr ||
            duration_ms < 0) {
            return;
        }

        if (esp_timer_is_active(status_rehide_timer_)) {
            ESP_ERROR_CHECK(esp_timer_stop(status_rehide_timer_));
        }
        static constexpr uint64_t kRehideDelayAfterNotificationUs = 100000;
        const uint64_t delay_us =
            static_cast<uint64_t>(duration_ms) * 1000 + kRehideDelayAfterNotificationUs;
        ESP_ERROR_CHECK(esp_timer_start_once(status_rehide_timer_, delay_us));
    }

    bool SetEmotion(const char* emotion) override {
        bool result = SpiLcdDisplay::SetEmotion(emotion);
        bool show_standby = emotion != nullptr &&
                            Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                            std::strcmp(emotion, "neutral") == 0;
        if (show_standby) { //待机就隐藏米家登录二维码
            HideMojiMijiaLoginQr();
        }
        if (!setup_ui_called_) {
            return result;
        }

        {
            DisplayLockGuard lock(this);
            if (!lock) {
                return result;
            }
            standby_mode_.store(show_standby, std::memory_order_relaxed);
            if (show_standby) {
                if (emoji_box_ != nullptr) {
                    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                for (lv_obj_t* object : {city_label_, date_label_, time_label_, 
                                         weather_icon_label_, weather_label_, high_low_label_
                                         }) {
                    if (object != nullptr) {
                        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                if (status_label_ != nullptr) {
                    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                }
                // 待命时隐藏状态栏
            } else {
                if (emoji_box_ != nullptr) {
                    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                for (lv_obj_t* object : {city_label_, date_label_, time_label_,
                                         weather_icon_label_, weather_label_, high_low_label_
                                         }) {
                    if (object != nullptr) {
                        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                if (status_label_ != nullptr && notification_label_ != nullptr &&
                    lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        ApplyMojiStyle();
        if (show_standby) {
            last_displayed_clock_minute_ = -1;
            UpdateClock(true);
        }
        return result;
    }

    void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        UpdateClock(update_all);
    }

    void RegisterThemesAndLoadStyle() {
        RegisterMojiStandbyThemes();

        Settings settings("display", false);
        std::string saved_style = settings.GetString("standby_style", "");
        MojiStandbyStyle loaded_style;
        if (ParseStandbyStyle(saved_style, loaded_style)) {
            standby_style_ = loaded_style;
        }
    }
};

class MovecallMojiESP32S3 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    CustomLcdDisplay* display_ = nullptr;

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    // SPI初始化
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg =
            GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                        DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config =
            GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;  // Set to -1 if not use
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;  // Implemented by LCD command `3Ah` (16/18)

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        display_ = new CustomLcdDisplay(io_handle, panel_handle, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeMojiMijiaTool(display_);
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led_strip(BUILTIN_LED_GPIO);
        return &led_strip;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(MovecallMojiESP32S3);

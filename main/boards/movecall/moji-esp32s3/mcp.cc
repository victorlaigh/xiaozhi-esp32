#include "display.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdint>

namespace {

constexpr char TAG[] = "MojiMcp";

class WhiteSquareTool {
public:
    explicit WhiteSquareTool(Display* display) : display_(display) {
        esp_timer_create_args_t timer_args = {
            .callback = OnTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "moji_white_square",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));
    }

    ToolResult Show() {
        DisplayLockGuard lock(display_);
        if (!lock) {
            return std::unexpected("Failed to lock display");
        }

        bool needs_timer = false;
        if (square_ == nullptr) {
            square_ = lv_obj_create(lv_screen_active());
            if (square_ == nullptr) {
                return std::unexpected("Failed to create white square");
            }
            needs_timer = true;
            lv_obj_remove_style_all(square_);
            lv_obj_set_size(square_, 60, 60);
            lv_obj_set_style_bg_color(square_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(square_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(square_, 0, 0);
            lv_obj_set_style_radius(square_, 0, 0);
            lv_obj_align(square_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_move_foreground(square_);
        }

        expires_at_us_ = esp_timer_get_time() + 5000000;
        if (needs_timer) {
            esp_err_t err = esp_timer_start_once(timer_, 5000000);
            if (err != ESP_OK) {
                lv_obj_del(square_);
                square_ = nullptr;
                return std::unexpected("Failed to start white square timer");
            }
        }
        return true;
    }

private:
    static void OnTimer(void* arg) {
        auto* self = static_cast<WhiteSquareTool*>(arg);
        DisplayLockGuard lock(self->display_);
        if (!lock) {
            return;
        }

        int64_t remaining_us = self->expires_at_us_ - esp_timer_get_time();
        if (remaining_us > 0) {
            if (!esp_timer_is_active(self->timer_)) {
                esp_err_t err = esp_timer_start_once(self->timer_, remaining_us);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to restart white square timer: %s",
                             esp_err_to_name(err));
                    if (self->square_ != nullptr) {
                        lv_obj_del(self->square_);
                        self->square_ = nullptr;
                    }
                }
            }
            return;
        }

        if (self->square_ != nullptr) {
            lv_obj_del(self->square_);
            self->square_ = nullptr;
        }
    }

    Display* display_;
    lv_obj_t* square_ = nullptr;
    int64_t expires_at_us_ = 0;
    esp_timer_handle_t timer_ = nullptr;
};

}  // namespace

void InitializeMojiMcpTools(Display* display) {
    static WhiteSquareTool white_square(display);
    McpServer::GetInstance().AddTool(
        "self.screen.show_white_square",
        "当用户说“试一下自定义工具”时调用。会在屏幕中央显示一个白色方块，并在5秒后自动关闭。",
        PropertyList(), [](const PropertyList&) -> ToolResult { return white_square.Show(); });
}

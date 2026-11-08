#include "weather_icon_font.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

LV_FONT_DECLARE(font_material_symbols_30_4);

namespace {

constexpr int kIconSize = 36;
constexpr int kSampleGrid = 4;
constexpr uint32_t kOvercastCodepoint = 0xE000;
constexpr uint32_t kPartlyCloudyCodepoint = 0xE001;
constexpr uint32_t kDrizzleCodepoint = 0xE002;
constexpr uint32_t kRainCodepoint = 0xE003;
constexpr uint32_t kSnowCodepoint = 0xE004;

using IconBitmap = std::array<uint8_t, kIconSize * kIconSize>;
using IconBitmaps = std::array<IconBitmap, 5>;

bool InCircle(float x, float y, float center_x, float center_y, float radius) {
    const float dx = x - center_x;
    const float dy = y - center_y;
    return dx * dx + dy * dy <= radius * radius;
}

bool InRoundRect(float x, float y, float left, float top, float right, float bottom,
                 float radius) {
    const float center_x = (left + right) * 0.5f;
    const float center_y = (top + bottom) * 0.5f;
    const float half_width = (right - left) * 0.5f - radius;
    const float half_height = (bottom - top) * 0.5f - radius;
    const float dx = std::max(std::abs(x - center_x) - half_width, 0.0f);
    const float dy = std::max(std::abs(y - center_y) - half_height, 0.0f);
    return dx * dx + dy * dy <= radius * radius;
}

bool NearSegment(float x, float y, float x1, float y1, float x2, float y2, float radius) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length_squared = dx * dx + dy * dy;
    const float projection = std::clamp(((x - x1) * dx + (y - y1) * dy) / length_squared,
                                        0.0f, 1.0f);
    const float distance_x = x - (x1 + projection * dx);
    const float distance_y = y - (y1 + projection * dy);
    return distance_x * distance_x + distance_y * distance_y <= radius * radius;
}

bool InCloud(float x, float y, bool large) {
    const float scale = large ? 1.0f : 0.78f;
    const float offset_x = large ? 0.0f : 2.5f;
    const float offset_y = large ? 0.0f : 4.5f;
    x = (x - offset_x) / scale;
    y = (y - offset_y) / scale;

    return InCircle(x, y, 11.0f, 22.0f, 8.5f) ||
           InCircle(x, y, 19.0f, 17.0f, 10.0f) ||
           InCircle(x, y, 27.0f, 22.0f, 7.5f) ||
           InRoundRect(x, y, 4.0f, 20.0f, 32.0f, 30.0f, 5.0f);
}

bool InPartlyCloudy(float x, float y) {
    constexpr float center_x = 25.0f;
    constexpr float center_y = 11.0f;
    if (InCircle(x, y, center_x, center_y, 6.0f)) {
        return true;
    }

    constexpr float rays[][4] = {
        {25.0f, 1.5f, 25.0f, 3.2f},  {25.0f, 18.8f, 25.0f, 20.5f},
        {15.5f, 11.0f, 17.2f, 11.0f}, {32.8f, 11.0f, 34.5f, 11.0f},
        {18.3f, 4.3f, 19.5f, 5.5f}, {30.5f, 16.5f, 31.7f, 17.7f},
        {18.3f, 17.7f, 19.5f, 16.5f}, {30.5f, 5.5f, 31.7f, 4.3f},
    };
    for (const auto& ray : rays) {
        if (NearSegment(x, y, ray[0], ray[1], ray[2], ray[3], 1.0f)) {
            return true;
        }
    }

    return InCloud(x, y, false);
}

bool InDrizzle(float x, float y) {
    if (InCloud(x, y + 5.0f, true)) {
        return true;
    }

    constexpr float drops[][4] = {
        {11.0f, 27.0f, 9.0f, 32.0f},
        {19.0f, 27.0f, 17.0f, 32.0f},
        {27.0f, 27.0f, 25.0f, 32.0f},
    };
    for (const auto& drop : drops) {
        if (NearSegment(x, y, drop[0], drop[1], drop[2], drop[3], 1.2f)) {
            return true;
        }
    }
    return false;
}

bool InRain(float x, float y) {
    if (InCloud(x, y + 5.0f, true)) {
        return true;
    }

    constexpr float drops[][4] = {
        {8.0f, 26.0f, 6.0f, 33.0f},
        {15.0f, 26.0f, 13.0f, 33.0f},
        {22.0f, 26.0f, 20.0f, 33.0f},
        {29.0f, 26.0f, 27.0f, 33.0f},
    };
    for (const auto& drop : drops) {
        if (NearSegment(x, y, drop[0], drop[1], drop[2], drop[3], 1.3f)) {
            return true;
        }
    }
    return false;
}

bool InSnowflake(float x, float y) {
    constexpr float center_x = 18.0f;
    constexpr float center_y = 18.0f;
    constexpr float kArmLength = 12.0f;
    constexpr float kBranchStart = 7.5f;
    constexpr float kBranchLength = 4.0f;
    constexpr float kPi = 3.14159265f;

    for (int arm = 0; arm < 6; ++arm) {
        const float angle = arm * kPi / 3.0f;
        const float direction_x = std::cos(angle);
        const float direction_y = std::sin(angle);
        const float end_x = center_x + kArmLength * direction_x;
        const float end_y = center_y + kArmLength * direction_y;
        if (NearSegment(x, y, center_x, center_y, end_x, end_y, 1.0f)) {
            return true;
        }

        const float branch_x = center_x + kBranchStart * direction_x;
        const float branch_y = center_y + kBranchStart * direction_y;
        for (float turn : {-1.0f, 1.0f}) {
            const float branch_angle = angle + turn * 2.0f * kPi / 3.0f;
            const float tip_x = branch_x + kBranchLength * std::cos(branch_angle);
            const float tip_y = branch_y + kBranchLength * std::sin(branch_angle);
            if (NearSegment(x, y, branch_x, branch_y, tip_x, tip_y, 1.0f)) {
                return true;
            }
        }
    }

    return false;
}

const IconBitmaps& GetIconBitmaps() {
    static const IconBitmaps bitmaps = [] {
        IconBitmaps result{};
        for (int icon = 0; icon < 5; ++icon) {
            for (int y = 0; y < kIconSize; ++y) {
                for (int x = 0; x < kIconSize; ++x) {
                    unsigned covered = 0;
                    for (int sample_y = 0; sample_y < kSampleGrid; ++sample_y) {
                        for (int sample_x = 0; sample_x < kSampleGrid; ++sample_x) {
                            const float px = x + (sample_x + 0.5f) / kSampleGrid;
                            const float py = y + (sample_y + 0.5f) / kSampleGrid;
                            const bool inside = icon == 0    ? InCloud(px, py, true)
                                                 : icon == 1 ? InPartlyCloudy(px, py)
                                                 : icon == 2 ? InDrizzle(px, py)
                                                 : icon == 3 ? InRain(px, py)
                                                             : InSnowflake(px, py);
                            covered += inside ? 1U : 0U;
                        }
                    }
                    result[icon][y * kIconSize + x] =
                        static_cast<uint8_t>(covered * 255U / (kSampleGrid * kSampleGrid));
                }
            }
        }
        return result;
    }();
    return bitmaps;
}

bool GetGlyphDescription(const lv_font_t* font, lv_font_glyph_dsc_t* description, uint32_t letter,
                         uint32_t letter_next) {
    (void)font;
    (void)letter_next;
    if (letter != kOvercastCodepoint && letter != kPartlyCloudyCodepoint &&
        letter != kDrizzleCodepoint && letter != kRainCodepoint &&
        letter != kSnowCodepoint) {
        return false;
    }

    description->adv_w = kIconSize;
    description->box_w = kIconSize;
    description->box_h = kIconSize;
    description->ofs_x = 0;
    description->ofs_y = 0;
    description->stride = kIconSize;
    description->format = LV_FONT_GLYPH_FORMAT_A8;
    description->is_placeholder = false;
    description->gid.index = letter == kOvercastCodepoint ? 0
                             : letter == kPartlyCloudyCodepoint ? 1
                             : letter == kDrizzleCodepoint ? 2
                             : letter == kRainCodepoint ? 3
                                                        : 4;
    return true;
}

const void* GetGlyphBitmap(lv_font_glyph_dsc_t* description, lv_draw_buf_t* draw_buffer) {
    (void)draw_buffer;
    if (description->gid.index > 4) {
        return nullptr;
    }
    return GetIconBitmaps()[description->gid.index].data();
}

}  // namespace

const lv_font_t moji_weather_icon_font = {
    .get_glyph_dsc = GetGlyphDescription,
    .get_glyph_bitmap = GetGlyphBitmap,
    .release_glyph = nullptr,
    .line_height = kIconSize,
    .base_line = 0,
    .subpx = LV_FONT_SUBPX_NONE,
    .kerning = LV_FONT_KERNING_NONE,
    .static_bitmap = 1,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = nullptr,
    .fallback = &font_material_symbols_30_4,
    .user_data = nullptr,
};

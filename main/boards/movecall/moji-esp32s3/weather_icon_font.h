#pragma once

#include <lvgl.h>

namespace MojiWeatherIcons {

inline constexpr char kOvercast[] = "\xEE\x80\x80";
inline constexpr char kPartlyCloudy[] = "\xEE\x80\x81";
inline constexpr char kDrizzle[] = "\xEE\x80\x82";
inline constexpr char kRain[] = "\xEE\x80\x83";
inline constexpr char kSnow[] = "\xEE\x80\x84";

}  // namespace MojiWeatherIcons

LV_FONT_DECLARE(moji_weather_icon_font);

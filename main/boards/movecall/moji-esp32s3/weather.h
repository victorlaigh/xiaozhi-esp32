#pragma once
//防止头文件内容被多次包含。
//告诉编译器在编译过程中只包含一次头文件，即使该通过#include被引用多次。

#include "display/display.h"

#include <cstdint>
#include <string>

namespace Weather {

const char* Description(int code);
const char* Icon(int code);
void UpdateLocationLabel(Display* display, lv_obj_t* label, const std::string& city);
void UpdateConditions(Display* display, lv_obj_t* icon_label, lv_obj_t* weather_label,
                      lv_obj_t* high_low_label, int code, int temperature, int high, int low);

class Service {
public:
    bool Start(Display* display, lv_obj_t* city_label, lv_obj_t* icon_label,
               lv_obj_t* weather_label, lv_obj_t* high_low_label);

private:
    static void Task(void* arg);
    void RunWeatherTask();
    bool RefreshLocation();
    bool RefreshWeather();

    Display* display_ = nullptr;
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* high_low_label_ = nullptr;
    double latitude_ = 0.0;
    double longitude_ = 0.0;
    int64_t next_location_refresh_us_ = 0;
    bool started_ = false;
};

}  // namespace Weather
// LovyanGFX setup for the ESP32-2432S028R: panel on HSPI, XPT2046 touch on its own pins.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
#ifdef PANEL_ST7789
  lgfx::Panel_ST7789 panel_;
#else
  lgfx::Panel_ILI9341 panel_;
#endif
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  LGFX() {
    auto b = bus_.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    bus_.config(b);
    panel_.setBus(&bus_);

    auto p = panel_.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.pin_busy = -1;
    p.panel_width = 240;
    p.panel_height = 320;
    p.readable = true;
    p.invert = PANEL_INVERT;
    p.rgb_order = false;
    p.bus_shared = false;
    panel_.config(p);

    auto l = light_.config();
    l.pin_bl = 21;
    l.freq = 12000;
    l.pwm_channel = 7;
    light_.config(l);
    panel_.setLight(&light_);

    auto t = touch_.config();
    t.spi_host = VSPI_HOST;
    t.freq = 1000000;
    t.pin_sclk = 25;
    t.pin_mosi = 32;
    t.pin_miso = 39;
    t.pin_cs = 33;
    t.pin_int = 36;
    t.bus_shared = false;
    t.x_min = 300;
    t.x_max = 3900;
    t.y_min = 200;
    t.y_max = 3700;
    touch_.config(t);
    panel_.setTouch(&touch_);

    setPanel(&panel_);
  }
};

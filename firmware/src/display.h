// LovyanGFX setup for the ESP32-2432S028R ("Cheap Yellow Display"): panel on HSPI,
// XPT2046 touch on its own pins. Boards ship with either an ILI9341 or an ST7789
// controller, so begin() reads the panel ID and picks the matching driver.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 ili_;
  lgfx::Panel_ST7789 st_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  LGFX() {
    auto b = bus_.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 27000000;  // 40 MHz corrupts rows on some panels
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    bus_.config(b);
    setupPanel(ili_);
    setupPanel(st_);
    setPanel(&ili_);
  }

  // Screen settings. Boards ship with different controllers and the panel ID can't
  // always be read, so the user can also pick one by pressing BOOT.
  enum Mode : uint8_t { Auto, Ili9341, St7789Inv, St7789, St7789InvBgr, St7789Bgr, ModeCount };

  static const char* modeName(uint8_t m) {
    static const char* kNames[] = {"Auto",          "ILI9341",       "ST7789 inverted",
                                   "ST7789 normal", "ST7789 inverted BGR", "ST7789 normal BGR"};
    return m < ModeCount ? kNames[m] : "?";
  }

  // Initializes the display in `mode` (Auto = detect) and returns the mode used.
  uint8_t begin(uint8_t mode) {
    if (mode == Auto || mode >= ModeCount) mode = detect();
    if (mode == Ili9341) {
      setPanel(&ili_);
    } else {
      auto p = st_.config();
      p.invert = mode == St7789Inv || mode == St7789InvBgr;
      p.rgb_order = mode == St7789InvBgr || mode == St7789Bgr;
      st_.config(p);
      setPanel(&st_);
    }
    init();
    return mode;
  }

 private:
  // A genuine ILI9341 answers RDID4 (0xD3) with 0x93 0x41; anything else is
  // treated as the ST7789 that two-USB boards use.
  uint8_t detect() {
    setPanel(&ili_);
    init();
    uint32_t id = ili_.readCommand(0xD3, 0, 4);
    Serial.printf("[display] RDID4 %08lx\n", (unsigned long)id);
    for (int i = 0; i < 3; i++)
      if (((id >> (8 * i)) & 0xFF) == 0x93 && ((id >> (8 * (i + 1))) & 0xFF) == 0x41) return Ili9341;
    return St7789Inv;
  }

  template <typename P>
  void setupPanel(P& panel) {
    panel.setBus(&bus_);
    auto p = panel.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.pin_busy = -1;
    p.panel_width = 240;
    p.panel_height = 320;
    p.readable = true;
    p.invert = false;
    p.rgb_order = false;
    p.bus_shared = false;
    panel.config(p);

    auto l = light_.config();
    l.pin_bl = 21;
    l.freq = 12000;
    l.pwm_channel = 7;
    light_.config(l);
    panel.setLight(&light_);

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
    panel.setTouch(&touch_);
  }
};

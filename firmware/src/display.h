// LovyanGFX setup for the ESP32-2432S028R ("Cheap Yellow Display"): panel on HSPI,
// XPT2046 touch on its own pins. Boards ship with either an ILI9341 or an ST7789
// controller, so begin() reads the panel ID and picks the matching driver.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ILI9341 with the start-up sequence from TFT_eSPI's ILI9341_2_DRIVER, which is
// what working firmware for two-USB CYD boards uses (the stock LovyanGFX sequence
// garbles rows on those panels). `gs` selects the Display Function Control gate
// scan bit: 0xC2 matches LovyanGFX's orientation handling, 0x82 is TFT_eSPI's value.
template <uint8_t gs>
struct Panel_ILI9341_CYD : public lgfx::Panel_ILI9341 {
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x10,              // power control 1
        0xC1, 1, 0x00,              // power control 2
        0xC5, 2, 0x30, 0x30,        // VCOM control 1
        0xC7, 1, 0xB7,              // VCOM control 2
        0xB1, 2, 0x00, 0x1A,        // frame rate
        0xB6, 3, 0x08, gs, 0x27,    // display function control
        0xF2, 1, 0x00,              // 3-gamma off
        0x26, 1, 0x01,              // gamma curve 1
        0xE0, 15, 0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9, 0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0xE1, 15, 0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F,
        0x11, 0 + CMD_INIT_DELAY, 120,  // sleep out
        0x29, 0 + CMD_INIT_DELAY, 100,  // display on
        0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX : public lgfx::LGFX_Device {
  Panel_ILI9341_CYD<0xC2> cyd_;
  Panel_ILI9341_CYD<0x82> cydGs_;
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
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    bus_.config(b);
    setupPanel(cyd_);
    setupPanel(cydGs_);
    setupPanel(ili_);
    setupPanel(st_);
    setPanel(&cyd_);
  }

  // Screen settings. Boards ship with different controllers and the panel ID can't
  // always be read, so the user can also pick one by pressing BOOT.
  enum Mode : uint8_t { Auto, Cyd, CydGs, Ili9341, St7789Inv, St7789, ModeCount };

  static const char* modeName(uint8_t m) {
    static const char* kNames[] = {"Auto", "CYD (ILI9341_2)", "CYD alt scan", "ILI9341 stock",
                                   "ST7789 inverted", "ST7789 normal"};
    return m < ModeCount ? kNames[m] : "?";
  }

  // Initializes the display in `mode` (Auto = the CYD sequence, which works on both
  // board versions) and returns the mode used.
  uint8_t begin(uint8_t mode) {
    if (mode == Auto || mode >= ModeCount) mode = Cyd;
    switch (mode) {
      case Cyd: setPanel(&cyd_); break;
      case CydGs: setPanel(&cydGs_); break;
      case Ili9341: setPanel(&ili_); break;
      default: {
        auto p = st_.config();
        p.invert = mode == St7789Inv;
        st_.config(p);
        setPanel(&st_);
      }
    }
    init();
    return mode;
  }

 private:
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

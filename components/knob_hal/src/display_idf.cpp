#include "display.h"
#include "hal_pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c_bus;

// ─── ST77916 vendor-specific init commands (Waveshare Knob-Touch-LCD-1.8) ───
// Source: Waveshare demo code / Espressif esp_lcd_st77916 component.
// These override the component's built-in defaults with values tuned for
// the specific panel revision used on this board.
static const st77916_lcd_init_cmd_t st77916_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    // Gamma positive
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0,
     (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29,
                 0x15, 0x15, 0x2E, 0x34},
     14, 0},
    // Gamma negative
    {0xE1,
     (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39,
                 0x15, 0x15, 0x2D, 0x33},
     14, 0},
    // Gate/source timing
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    // Source EQ
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    // GOA mapping
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    // Mux mapping
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    // Return to command set 0 + finalize
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    // Display inversion on
    {0x21, (uint8_t[]){0x00}, 1, 0},
    // Sleep out (120ms delay required)
    {0x11, (uint8_t[]){0x00}, 1, 120},
    // Display on
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

static void init_backlight() {
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_cfg.duty_resolution = LEDC_TIMER_10_BIT;
  timer_cfg.timer_num = LEDC_TIMER_0;
  timer_cfg.freq_hz = 25000;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

  ledc_channel_config_t ch_cfg = {};
  ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_cfg.channel = static_cast<ledc_channel_t>(LCD_BL_LEDC_CH);
  ch_cfg.timer_sel = LEDC_TIMER_0;
  ch_cfg.gpio_num = PIN_LCD_BL;
  ch_cfg.duty = 0;
  ch_cfg.hpoint = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

void display_set_backlight(uint8_t percent) {
  uint32_t duty = (1023 * percent) / 100;
  ledc_set_duty(LEDC_LOW_SPEED_MODE,
                static_cast<ledc_channel_t>(LCD_BL_LEDC_CH), duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE,
                   static_cast<ledc_channel_t>(LCD_BL_LEDC_CH));
}

static void init_lcd() {
  init_backlight();

  // Hardware reset — the Knob-Touch-LCD-1.8 has RST on a direct GPIO
  gpio_config_t rst_cfg = {};
  rst_cfg.pin_bit_mask = 1ULL << PIN_LCD_RST;
  rst_cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rst_cfg);
  gpio_set_level(static_cast<gpio_num_t>(PIN_LCD_RST), 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(static_cast<gpio_num_t>(PIN_LCD_RST), 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  // ── QSPI bus ──────────────────────────────────────────────────────────────
  spi_bus_config_t bus_cfg = {};
  bus_cfg.sclk_io_num = PIN_LCD_CLK;
  bus_cfg.data0_io_num = PIN_LCD_SIO0;
  bus_cfg.data1_io_num = PIN_LCD_SIO1;
  bus_cfg.data2_io_num = PIN_LCD_SIO2;
  bus_cfg.data3_io_num = PIN_LCD_SIO3;
  bus_cfg.max_transfer_sz =
      LCD_H_RES * LCD_DRAW_ROWS * 2; // RGB565: 2 bytes/pixel
  bus_cfg.flags = SPICOMMON_BUSFLAG_QUAD;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

  // ── QSPI panel IO ────────────────────────────────────────────────────────
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = PIN_LCD_CS;
  io_cfg.dc_gpio_num = -1; // QSPI: no DC pin
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = 50 * 1000 * 1000; // ST77916 max QSPI is 50MHz
  io_cfg.trans_queue_depth = 3;
  io_cfg.lcd_cmd_bits = 32; // QSPI: 32-bit command (opcode + cmd + dummy)
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_panel_io));

  // ── ST77916 panel driver ──────────────────────────────────────────────────
  st77916_vendor_config_t vendor_cfg = {};
  vendor_cfg.init_cmds = st77916_init_cmds;
  vendor_cfg.init_cmds_size =
      sizeof(st77916_init_cmds) / sizeof(st77916_init_cmds[0]);
  vendor_cfg.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = -1; // Already reset above via direct GPIO
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16; // RGB565
  panel_cfg.vendor_config = &vendor_cfg;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(s_panel_io, &panel_cfg, &s_panel));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  display_set_backlight(80);
  ESP_LOGI(TAG, "LCD initialized (%dx%d)", LCD_H_RES, LCD_V_RES);
}

static void init_touch_hw() {
  i2c_master_bus_config_t i2c_bus_cfg = {};
  i2c_bus_cfg.i2c_port = static_cast<i2c_port_num_t>(TOUCH_I2C_NUM);
  i2c_bus_cfg.sda_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SDA);
  i2c_bus_cfg.scl_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SCL);
  i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_bus_cfg.flags.enable_internal_pullup = true;

  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &s_i2c_bus));

  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.dev_addr = TOUCH_I2C_ADDR;
  io_cfg.scl_speed_hz = TOUCH_I2C_FREQ;
  io_cfg.control_phase_bytes = 1;
  io_cfg.dc_bit_offset = 0;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.disable_control_phase = 1;

  esp_lcd_panel_io_handle_t touch_io;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &touch_io));

  esp_lcd_touch_config_t touch_cfg = {};
  touch_cfg.x_max = LCD_H_RES;
  touch_cfg.y_max = LCD_V_RES;
  touch_cfg.rst_gpio_num = static_cast<gpio_num_t>(PIN_TOUCH_RST);
  touch_cfg.int_gpio_num = static_cast<gpio_num_t>(PIN_TOUCH_INT);
  // Mirror touch to match display orientation (panel mirrors both axes)
  touch_cfg.flags.mirror_x = true;
  touch_cfg.flags.mirror_y = true;

  ESP_ERROR_CHECK(
      esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_cfg, &s_touch));
  ESP_LOGI(TAG, "Touch initialized");
}

i2c_master_bus_handle_t display_get_i2c_bus() { return s_i2c_bus; }

void display_init(lv_display_t **disp, lv_indev_t **touch) {
  init_lcd();
  init_touch_hw();

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_affinity = 1; // Pin LVGL to Core 1 (WiFi runs on Core 0)
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

  // Use field-by-field assignment to avoid C++20 designated initializer
  // ordering issues with esp_lvgl_port struct layout
  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = s_panel_io;
  disp_cfg.panel_handle = s_panel;
  disp_cfg.buffer_size = static_cast<uint32_t>(LCD_H_RES * LCD_DRAW_ROWS * 2);
  disp_cfg.double_buffer = true;
  disp_cfg.hres = LCD_H_RES;
  disp_cfg.vres = LCD_V_RES;
  disp_cfg.monochrome = false;
  disp_cfg.rotation.swap_xy = false;
  disp_cfg.rotation.mirror_x = true;
  disp_cfg.rotation.mirror_y = true;
  disp_cfg.flags.buff_dma = true;
  disp_cfg.flags.buff_spiram = false;
  disp_cfg.flags.sw_rotate = false;
  disp_cfg.flags.swap_bytes = false;
  disp_cfg.flags.full_refresh = false;
  disp_cfg.flags.direct_mode = false;

  *disp = lvgl_port_add_disp(&disp_cfg);

  // Render natively in byte-swapped RGB565 so the flush callback skips the
  // expensive software swap (lv_draw_sw_rgb565_swap on ~500KB/frame).
  // The ST77916 expects big-endian RGB565; ESP32-S3 is little-endian.
  // Instead of swapping after render, we tell LVGL to render swapped.
  lv_display_set_color_format(*disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

  lvgl_port_touch_cfg_t touch_port_cfg = {};
  touch_port_cfg.disp = *disp;
  touch_port_cfg.handle = s_touch;
  *touch = lvgl_port_add_touch(&touch_port_cfg);

  ESP_LOGI(TAG, "LVGL port initialized (RGB565_SWAPPED native, %d rows)",
           LCD_DRAW_ROWS);
}

bool display_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void display_unlock() { lvgl_port_unlock(); }

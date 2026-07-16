#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "charger_state.h"
#include "charger_proto.h"
#include "wifi.h"
#include "font5x7.h"

static const char *TAG = "ui";

// --- hardware --------------------------------------------------------------
#define I2C_SDA        5
#define I2C_SCL        6
#define OLED_ADDR      0x3C
#define LED_GPIO       8
#define LED_ACTIVE_LOW 1        // onboard C3 LED is usually active-low (TBD)

// Visible 72x40 window sits at this offset in the 128x64 framebuffer. This module
// (unlike the datasheet's suggested 12) has its visible band lower: OY tuned to 24
// on real hardware - content above that clips off the top and the lower glass was
// unused.
#define OX 30
#define OY 24
#define UW 72
#define UH 40

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool     s_have_oled;
static uint8_t  s_fb[1024];             // 128x64 / 8

// ==========================================================================
// SSD1306 primitives
// ==========================================================================
static void ssd_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];                     // must fit the 25-byte init + control byte
    if (n + 1 > sizeof(buf)) {
        return;
    }
    buf[0] = 0x00;                       // Co=0, D/C#=0 -> command stream
    memcpy(buf + 1, cmds, n);
    i2c_master_transmit(s_dev, buf, n + 1, 100);
}

static void ssd_flush(void)
{
    static uint8_t tx[1025];
    const uint8_t win[] = { 0x21, 0, 127, 0x22, 0, 7 };  // full col/page range
    ssd_cmds(win, sizeof(win));
    tx[0] = 0x40;                         // data stream
    memcpy(tx + 1, s_fb, sizeof(s_fb));
    i2c_master_transmit(s_dev, tx, sizeof(tx), 200);
}

static bool ssd_init(void)
{
    static const uint8_t init[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    ssd_cmds(init, sizeof(init));
    return true;
}

// ==========================================================================
// Framebuffer drawing (local 72x40 coords; offset applied here)
// ==========================================================================
static inline void px(int x, int y)
{
    int X = x + OX, Y = y + OY;
    if (X < 0 || X >= 128 || Y < 0 || Y >= 64) {
        return;
    }
    s_fb[X + (Y / 8) * 128] |= (uint8_t)(1u << (Y & 7));
}

static void draw_char(int x, int y, char c, int scale)
{
    if (c < 0x20 || c > 0x7F) {
        c = 0x20;
    }
    const uint8_t *g = &FONT5X7[(c - 0x20) * 5];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1u << row)) {
                for (int dx = 0; dx < scale; dx++)
                    for (int dy = 0; dy < scale; dy++)
                        px(x + col * scale + dx, y + row * scale + dy);
            }
        }
    }
}

static int text_w(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale;
}

static void draw_str(int x, int y, const char *s, int scale)
{
    for (; *s; s++) {
        draw_char(x, y, *s, scale);
        x += 6 * scale;
    }
}

static void draw_str_right(int xr, int y, const char *s, int scale)
{
    draw_str(xr - text_w(s, scale), y, s, scale);
}

static void draw_str_center(int y, const char *s, int scale)
{
    draw_str((UW - text_w(s, scale)) / 2, y, s, scale);
}

// ==========================================================================
// Layout
// ==========================================================================
static const char *link_word(charger_ble_link_t l)
{
    switch (l) {
    case CB_LINK_SCANNING:       return "SCAN";
    case CB_LINK_CONNECTING:     return "CONN";
    case CB_LINK_AUTHENTICATING: return "AUTH";
    default:                     return "----";
    }
}

static void render(int phase)
{
    memset(s_fb, 0, sizeof(s_fb));
    charger_state_t s;
    charger_state_get(&s);

    if (s.link != CB_LINK_READY) {
        draw_str_center(3, "no charger", 1);
        draw_str_center(18, link_word(s.link), 2);
        return;
    }

    char t[16];
    snprintf(t, sizeof(t), "%u/8", s.step);          // stage
    draw_str(0, 0, t, 1);
    snprintf(t, sizeof(t), "%u%%", s.capacity_pct);   // capacity
    draw_str_right(UW - 1, 0, t, 1);

    if (s.error) {
        draw_str_center(13, "FAULT", 2);
    } else if (phase) {
        snprintf(t, sizeof(t), "%.1fV", s.voltage_v);
        draw_str_center(13, t, 2);
    } else {
        snprintf(t, sizeof(t), "%.2fA", s.current_a);
        draw_str_center(13, t, 2);
    }

    draw_str(0, 31, cp_program_name(s.program), 1);   // program
    const char *st = s.charging ? "CHG" : (s.battery_present ? "IDLE" : "----");
    draw_str_right(UW - 1, 31, st, 1);
}

// ==========================================================================
// Onboard LED = WiFi state
// ==========================================================================
static void led_set(bool on)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? !on : on);
}

static void led_update(int tick)
{
    switch (wifi_get_state()) {
    case WIFI_CONNECTED: led_set(true);            break;   // solid
    case WIFI_CONNECTING:
    case WIFI_PORTAL:    led_set((tick & 2) == 0); break;   // ~2 Hz blink
    default:             led_set(false);           break;   // off
    }
}

// ==========================================================================
// Task
// ==========================================================================
#define TICK_MS      200
#define PHASE_TICKS  12         // ~2.4 s V<->I alternation

static void ui_task(void *arg)
{
    (void)arg;
    int tick = 0;
    for (;;) {
        if (s_have_oled) {
            render((tick / PHASE_TICKS) & 1);
            ssd_flush();
        }
        led_update(tick);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void ui_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    led_set(false);

    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed; display off (LED still active)");
    } else if (i2c_master_probe(s_bus, OLED_ADDR, 200) != ESP_OK) {
        ESP_LOGW(TAG, "no SSD1306 ACK at 0x%02X on SDA=%d SCL=%d; display off",
                 OLED_ADDR, I2C_SDA, I2C_SCL);
    } else {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) == ESP_OK && ssd_init()) {
            s_have_oled = true;
            ESP_LOGI(TAG, "SSD1306 found at 0x%02X; display on", OLED_ADDR);
        }
    }

    xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);
}

// ui - SSD1306 OLED status display + onboard WiFi LED.
//
// Drives the 0.42" 128x64 SSD1306 (I2C 0x3C on GPIO5/6) whose visible area is a
// 72x40 window at framebuffer offset (30,12). Shows a condensed charge summary
// (capacity, stage n/8, large alternating V/I, program/state) and surfaces link
// and fault conditions. The onboard LED (GPIO8) reflects WiFi state, not charge.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the display + LED task. Requires charger_state_init() and wifi_start().
// Safe to call if no panel is attached (it logs an I2C probe result and the LED
// still works). Non-blocking.
void ui_start(void);

#ifdef __cplusplus
}
#endif

/*----------------------------------------------------------------------------/
  BoardRLCD42 - Waveshare ESP32-S3-RLCD-4.2 board support for AI_StackChan_Ex

  Hardware:
   - ESP32-S3-WROOM-1-N16R8 (16MB Flash / 8MB PSRAM)
   - 4.2" reflective LCD 400x300 mono (ST7305, SPI)
   - ES8311 audio codec (speaker DAC) + ES7210 ADC (dual mic, AEC capable)
   - SHTC3 temp/humidity, PCF85063 RTC (shared I2C)
   - KEY button (GPIO18), BOOT button (GPIO0)
/----------------------------------------------------------------------------*/
#if defined(ARDUINO_WS_RLCD42)

#ifndef _BOARD_RLCD42_H
#define _BOARD_RLCD42_H

#include <stdint.h>

// ---- Pin assignment (Waveshare ESP32-S3-RLCD-4.2) ----
#define RLCD42_LCD_SCLK   11
#define RLCD42_LCD_MOSI   12
#define RLCD42_LCD_CS     40
#define RLCD42_LCD_DC     5
#define RLCD42_LCD_RST    41

#define RLCD42_I2C_SDA    13
#define RLCD42_I2C_SCL    14

#define RLCD42_I2S_BCLK   9
#define RLCD42_I2S_LRCK   45
#define RLCD42_I2S_MCLK   16
#define RLCD42_I2S_DOUT   8    // to ES8311 (speaker)
#define RLCD42_I2S_DIN    10   // from ES7210 (microphone)
#define RLCD42_SPK_EN     46   // speaker amplifier enable (active high)

#define RLCD42_BTN_KEY    18   // KEY button (active low)
#define RLCD42_BTN_BOOT   0    // BOOT button (active low)

#define RLCD42_BAT_ADC    4    // battery voltage ADC (3x divider)

// ---- Audio sample rates ----
// M5Unified I2S: LRCK = cfg.sample_rate / 2, MCLK = cfg.sample_rate * 128.
// Speaker cfg 64000 -> LRCK 32kHz / MCLK 8.192MHz (ES8311 coeff: 8192000/32000)
// Mic cfg 16000 (over_sampling=2) -> LRCK 16kHz / MCLK 4.096MHz (ES7210: ratio 256)
#define RLCD42_SPK_SAMPLE_RATE   64000
#define RLCD42_SPK_CODEC_MCLK    (RLCD42_SPK_SAMPLE_RATE * 128)
#define RLCD42_SPK_CODEC_FS      (RLCD42_SPK_SAMPLE_RATE / 2)
#define RLCD42_MIC_SAMPLE_RATE   16000

#ifndef RLCD42_LCD_SPI_FREQ
// ST7305はSPIクロック上限が低い(Waveshare公式は1MHz推奨、Zephyr定義は最大10MHz)。
// 20MHzでは表示不能/化けの恐れがあるため既定4MHz。動作確認後に10MHzまで上げられる。
#define RLCD42_LCD_SPI_FREQ  4000000
#endif

// Disable M5GFX/M5Unified board autodetection by pre-setting a harmless
// board id on M5.Display. The autodetect probes many GPIOs and crashes /
// misbehaves on this (unknown to M5) board. MUST be called BEFORE M5.begin().
void rlcd42_gfx_skip_autodetect(void);

// Initialize the ST7305 display and attach it to M5.Display.
// Call after rlcd42_gfx_skip_autodetect(), before or after M5.begin().
bool rlcd42_display_init(void);

// Initialize ES8311 (speaker) / ES7210 (mic) codecs via I2C and
// enable the speaker amplifier. Must be called BEFORE M5.Mic/M5.Speaker use.
bool rlcd42_audio_init(void);

// Set ES8311 output volume (0-100).
void rlcd42_codec_volume(int volume);

// Reconfigure ES8311 clock (MCLK/LRCK) at runtime. For diagnostics/tuning.
bool rlcd42_codec_clock(int mclk_hz, int lrck_hz);

// Fully re-initialize ES8311.
//  mclk_from_pin=false : SCLK(ビットクロック)を内部MCLKとして使う(MCLKピン不要)
//  mclk_from_pin=true  : MCLKピン入力を使う(mclk_hz指定)
bool rlcd42_codec_reinit(bool mclk_from_pin, int mclk_hz, int lrck_hz);

// Dump key ES8311 registers to Serial (diagnostics).
void rlcd42_codec_dump(void);

// Write a single ES8311 register directly (diagnostics).
void rlcd42_es8311_write(uint8_t reg, uint8_t val);

#endif // _BOARD_RLCD42_H

#endif // ARDUINO_WS_RLCD42

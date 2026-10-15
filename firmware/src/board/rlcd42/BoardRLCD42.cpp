/*----------------------------------------------------------------------------/
  BoardRLCD42 - Waveshare ESP32-S3-RLCD-4.2 board support for AI_StackChan_Ex
/----------------------------------------------------------------------------*/
#if defined(ARDUINO_WS_RLCD42)

#include <Arduino.h>
#include <M5Unified.h>
#include <driver/i2c.h>

#include "BoardRLCD42.h"
#include "Panel_ST7305.hpp"

extern "C" {
#include "es8311.h"
#include "es7210.h"
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

static lgfx::Bus_SPI      _bus;
static lgfx::Panel_ST7305 _panel;

// M5GFX::init_impl は getBoard() が board_unknown 以外なら自動判定せず即returnする。
// 未知ボードでの自動判定はGPIOを探ってクラッシュするため、ダミーのボードIDを
// 事前に設定して判定処理そのものを無効化する。
// (board_M5UnitLCD はメインボードのピン割当を持たないIDなのでM5Unified側も無害)
void rlcd42_gfx_skip_autodetect(void)
{
  struct GfxBoardSetter : public m5gfx::M5GFX {
    void force(m5gfx::board_t b) { _board = b; }
  };
  static_cast<GfxBoardSetter&>(M5.Display).force(m5gfx::board_t::board_M5UnitLCD);
  Serial.println("[RLCD42] M5GFX autodetect disabled");
}

bool rlcd42_display_init(void)
{
  {
    auto cfg = _bus.config();
    cfg.spi_host   = SPI2_HOST;
    cfg.spi_mode   = 0;
    cfg.freq_write = RLCD42_LCD_SPI_FREQ;
    cfg.freq_read  = 4000000;
    cfg.spi_3wire  = false;
    cfg.use_lock   = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk   = RLCD42_LCD_SCLK;
    cfg.pin_mosi   = RLCD42_LCD_MOSI;
    cfg.pin_miso   = -1;
    cfg.pin_dc     = RLCD42_LCD_DC;
    _bus.config(cfg);
  }
  {
    auto cfg = _panel.config();
    cfg.pin_cs   = RLCD42_LCD_CS;
    cfg.pin_rst  = RLCD42_LCD_RST;
    cfg.pin_busy = -1;
    cfg.panel_width   = 400;
    cfg.panel_height  = 300;
    cfg.memory_width  = 400;
    cfg.memory_height = 300;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
#ifdef RLCD42_OFFSET_ROTATION
    cfg.offset_rotation = RLCD42_OFFSET_ROTATION;  // set 2 if the image appears upside down
#else
    cfg.offset_rotation = 0;
#endif
    cfg.readable   = false;
    cfg.invert     = false;
    cfg.rgb_order  = false;
    cfg.bus_shared = false;
    _panel.config(cfg);
  }
  _panel.setBus(&_bus);

  // M5GFX public API for attaching a user panel: sets the panel and runs
  // LGFX_Device::init_impl() directly (bypasses M5 board autodetect).
  bool res = M5.Display.init(&_panel);
  if (res) {
    M5.Display.setRotation(0);
  }
  Serial.printf("[RLCD42] display init %s\n", res ? "OK" : "FAILED");
  return res;
}

// ---------------------------------------------------------------------------
// Audio codecs (ES8311 speaker DAC / ES7210 microphone ADC)
// ---------------------------------------------------------------------------

static es8311_handle_t     _es8311 = nullptr;
static es7210_dev_handle_t _es7210 = nullptr;

static bool _i2c_init(void)
{
  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = (gpio_num_t)RLCD42_I2C_SDA;
  conf.scl_io_num = (gpio_num_t)RLCD42_I2C_SCL;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400000;
  if (i2c_param_config(I2C_NUM_0, &conf) != ESP_OK) { return false; }
  esp_err_t err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
  if (err == ESP_ERR_INVALID_STATE) { err = ESP_OK; }  // already installed
  return err == ESP_OK;
}

// ES8311へ直接レジスタを書く(esp-bspドライバが書かない補完用)
static void _es8311_write_raw(uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = { reg, val };
  i2c_master_write_to_device(I2C_NUM_0, ES8311_ADDRESS_0, buf, 2, pdMS_TO_TICKS(100));
}

// Waveshare公式デモ(esp_codec_dev版es8311)は書くが、esp-bsp版es8311は書かない
// レジスタ群の補完。特にREG10/REG11(電源管理)とREG44(内部リファレンス)は
// チップのデフォルト値がロットにより異なり、欠けるとDACが無音になる。
static void _es8311_supplement_init(void)
{
  _es8311_write_raw(0x44, 0x08);  // I2Cノイズ耐性 (公式は2回書く)
  _es8311_write_raw(0x44, 0x08);
  _es8311_write_raw(0x16, 0x24);
  _es8311_write_raw(0x04, 0x10);
  _es8311_write_raw(0x05, 0x00);
  _es8311_write_raw(0x0B, 0x00);
  _es8311_write_raw(0x0C, 0x00);
  _es8311_write_raw(0x10, 0x1F);  // SYSTEM power
  _es8311_write_raw(0x11, 0x7F);  // SYSTEM power
  _es8311_write_raw(0x1B, 0x0A);
  _es8311_write_raw(0x17, 0xBF);  // ADC volume
  _es8311_write_raw(0x0E, 0x02);
  _es8311_write_raw(0x12, 0x00);  // DAC power-up
  _es8311_write_raw(0x14, 0x1A);  // MIC sel/PGA
  _es8311_write_raw(0x0D, 0x01);  // analog power-up
  _es8311_write_raw(0x15, 0x40);
  _es8311_write_raw(0x37, 0x08);
  _es8311_write_raw(0x45, 0x00);
  _es8311_write_raw(0x44, 0x58);  // 内部リファレンス(ADCL+DACR)
}

// ★Waveshare公式デモ(esp_codec_dev版es8311)から移植した初期化スクリプト。
//   esp-bspドライバ(es8311_init)の初期化列では、このボードのES8311は
//   無音になることを実機のマイク自動計測で確認済み(本スクリプトは動作確認済み)。
//   クロック分周はMCLK/LRCK比256用の値で、絶対周波数には依存しないため、
//   スピーカー(MCLK8.192MHz/LRCK32k)・マイク(4.096MHz/16k)の両セッション共通。
static const uint8_t ES8311_INIT_SCRIPT[][2] = {
  // -- open (電源・リファレンス・フォーマット) --
  {0x44,0x08},{0x44,0x08},{0x01,0x30},{0x02,0x00},{0x03,0x10},{0x16,0x24},
  {0x04,0x10},{0x05,0x00},{0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7F},
  {0x00,0x80},{0x00,0x80},{0x13,0x10},{0x1B,0x0A},{0x1C,0x6A},{0x44,0x58},
  {0x09,0x0C},{0x0A,0x0C},                          // I2S standard 16bit
  // -- clock (MCLKモード, 比256: pre_div=1, multi=x1, ADC/DAC OSR 0x10/0x20) --
  {0x01,0x3F},{0x02,0x00},{0x05,0x00},{0x03,0x10},{0x04,0x20},{0x07,0x00},{0x08,0xFF},{0x06,0x03},
  // -- start (アナログ/DAC電源ON, 音量, ミュート解除) --
  {0x17,0xBF},{0x0E,0x02},{0x12,0x00},{0x14,0x1A},{0x0D,0x01},{0x15,0x40},
  {0x37,0x08},{0x45,0x00},{0x32,0xC8},{0x31,0x00},
};

static bool _es8311_init(void)
{
  _es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);  // 音量API用にハンドルは維持
  if (_es8311 == nullptr) { return false; }

  _es8311_write_raw(0x00, 0x1F);  // reset
  delay(20);
  _es8311_write_raw(0x00, 0x00);
  _es8311_write_raw(0x00, 0x80);  // power-on, slave mode
  for (auto& p : ES8311_INIT_SCRIPT) {
    _es8311_write_raw(p[0], p[1]);
  }
  Serial.println("[RLCD42] es8311 init (Waveshare demo script / MCLK ratio 256)");
  return true;
}

static bool _es7210_init(void)
{
  es7210_i2c_config_t i2c_conf = {};
  i2c_conf.i2c_port = I2C_NUM_0;
  i2c_conf.i2c_addr = 0x40;
  if (es7210_new_codec(&i2c_conf, &_es7210) != ESP_OK) {
    Serial.println("[RLCD42] es7210_new_codec failed");
    return false;
  }

  es7210_codec_config_t codec_conf = {};
  codec_conf.sample_rate_hz = RLCD42_MIC_SAMPLE_RATE;   // 16 kHz (LRCK)
  codec_conf.mclk_ratio     = 256;                      // MCLK = 4.096 MHz
  codec_conf.i2s_format     = ES7210_I2S_FMT_I2S;
  codec_conf.bit_width      = ES7210_I2S_BITS_16B;
  codec_conf.mic_bias       = ES7210_MIC_BIAS_2V87;
  codec_conf.mic_gain       = ES7210_MIC_GAIN_30DB;
  codec_conf.flags.tdm_enable = false;

  if (es7210_config_codec(_es7210, &codec_conf) != ESP_OK) {
    Serial.println("[RLCD42] es7210_config_codec failed");
    return false;
  }
  es7210_config_volume(_es7210, 0);
  return true;
}

bool rlcd42_audio_init(void)
{
  bool ok = _i2c_init();
  if (!ok) {
    Serial.println("[RLCD42] i2c init failed");
    return false;
  }
  bool ok8311 = _es8311_init();
  bool ok7210 = _es7210_init();

  // Speaker amplifier enable (M5.begin() already drives GPIO46 high as the
  // "power hold" pin on ESP32-S3, but set it explicitly to be safe.)
  pinMode(RLCD42_SPK_EN, OUTPUT);
  digitalWrite(RLCD42_SPK_EN, HIGH);

  Serial.printf("[RLCD42] audio init ES8311:%s ES7210:%s\n",
                ok8311 ? "OK" : "NG", ok7210 ? "OK" : "NG");
  return ok8311 && ok7210;
}

void rlcd42_codec_volume(int volume)
{
  if (_es8311 == nullptr) { return; }
  if (volume < 0) { volume = 0; }
  if (volume > 100) { volume = 100; }
  es8311_voice_volume_set(_es8311, volume, nullptr);
}

bool rlcd42_codec_clock(int mclk_hz, int lrck_hz)
{
  if (_es8311 == nullptr) { return false; }
  esp_err_t err = es8311_sample_frequency_config(_es8311, mclk_hz, lrck_hz);
  es8311_voice_volume_set(_es8311, 90, nullptr);
  es8311_voice_mute(_es8311, false);
  digitalWrite(RLCD42_SPK_EN, HIGH);
  Serial.printf("[RLCD42] codec clock mclk=%d lrck=%d -> %s\n", mclk_hz, lrck_hz,
                err == ESP_OK ? "OK" : "NG(coeff not found)");
  return err == ESP_OK;
}

bool rlcd42_codec_reinit(bool mclk_from_pin, int mclk_hz, int lrck_hz)
{
  if (_es8311 == nullptr) { return false; }
  es8311_clock_config_t clk_cfg = {};
  clk_cfg.mclk_inverted      = false;
  clk_cfg.sclk_inverted      = false;
  clk_cfg.mclk_from_mclk_pin = mclk_from_pin;
  clk_cfg.mclk_frequency     = mclk_hz;
  clk_cfg.sample_frequency   = lrck_hz;
  esp_err_t err = es8311_init(_es8311, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  _es8311_supplement_init();
  es8311_voice_volume_set(_es8311, 90, nullptr);
  es8311_voice_mute(_es8311, false);
  digitalWrite(RLCD42_SPK_EN, HIGH);
  Serial.printf("[RLCD42] codec reinit %s mclk=%d lrck=%d -> %s\n",
                mclk_from_pin ? "MCLKpin" : "SCLKsrc", mclk_hz, lrck_hz,
                err == ESP_OK ? "OK" : "NG");
  return err == ESP_OK;
}

void rlcd42_es8311_write(uint8_t reg, uint8_t val)
{
  _es8311_write_raw(reg, val);
}

void rlcd42_codec_dump(void)
{
  static const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,
                                 0x0B,0x0C,0x0D,0x0E,0x10,0x11,0x12,0x13,0x14,0x17,0x31,
                                 0x32,0x37,0x44,0x45,0xFD,0xFE,0xFF};
  Serial.print("[RLCD42] ES8311 regs:");
  for (uint8_t r : regs) {
    uint8_t v = 0;
    i2c_master_write_read_device(I2C_NUM_0, ES8311_ADDRESS_0, &r, 1, &v, 1, pdMS_TO_TICKS(100));
    Serial.printf(" %02X=%02X", r, v);
  }
  Serial.println();
}

#endif // ARDUINO_WS_RLCD42

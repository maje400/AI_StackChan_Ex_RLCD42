// ============================================================
// オーディオ自己診断ファームウェア (env: ws-rlcd42-min)
//
// I2Sを全二重(TX+RX)で常時動作させ、ES8311の設定候補A〜Eを順に適用しながら
// 1kHzトーンを再生。その音をマイク(ES7210)で拾って音量を自動測定する。
// → スピーカーが実際に鳴った候補は画面の peak 値が大きくなる(数千〜)。
//    人間の耳に頼らず正解設定を特定できる。
//
// 候補:
//  A: esp-bspドライバのMCLKモード (MCLK=4.096MHz)
//  B: Waveshare公式デモ移植版 MCLKモード
//  C: B + SCLK極性反転
//  D: Waveshare公式デモ移植版 SCLKソースモード(×8逓倍, MCLK不使用)
//  E: D + SCLK極性反転
// ============================================================
#if defined(RLCD42_MINIMAL_TEST)

#include <Arduino.h>
#include <M5Unified.h>
#include <nvs_flash.h>
#include <driver/i2s.h>
#include <math.h>
#include "BoardRLCD42.h"

static void band(int idx) {
  M5.Display.fillRect(0, idx * 20, 400, 16, TFT_WHITE);
}

// ---------- ES8311 レジスタスクリプト ----------
typedef uint8_t RegPair[2];

static const RegPair SCRIPT_OPEN[] = {
  {0x44,0x08},{0x44,0x08},{0x01,0x30},{0x02,0x00},{0x03,0x10},{0x16,0x24},
  {0x04,0x10},{0x05,0x00},{0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7F},
  {0x00,0x80},{0x00,0x80},{0x13,0x10},{0x1B,0x0A},{0x1C,0x6A},{0x44,0x58},
  {0x09,0x0C},{0x0A,0x0C},
};
// 公式デモの係数 (MCLK=4.096MHz, LRCK=16kHz): pre_div=1 multi=x1 osr=0x10/0x20 bclkdiv=4
static const RegPair SCRIPT_MCLK[] = {
  {0x01,0x3F},{0x02,0x00},{0x05,0x00},{0x03,0x10},{0x04,0x20},{0x07,0x00},{0x08,0xFF},{0x06,0x03},
};
// SCLKソース(MCLK不使用): REG01 bit7=1, REG02 pre_multi=x8 (公式デモのuse_mclk=falseパス)
static const RegPair SCRIPT_SCLK[] = {
  {0x01,0xBF},{0x02,0x18},{0x05,0x00},{0x03,0x10},{0x04,0x20},{0x07,0x00},{0x08,0xFF},{0x06,0x03},
};
static const RegPair SCRIPT_START[] = {
  {0x17,0xBF},{0x0E,0x02},{0x12,0x00},{0x14,0x1A},{0x0D,0x01},{0x15,0x40},
  {0x37,0x08},{0x45,0x00},{0x32,0xC8},{0x31,0x00},
};

static void run_script(const RegPair* p, size_t n) {
  for (size_t i = 0; i < n; ++i) { rlcd42_es8311_write(p[i][0], p[i][1]); }
}
#define RUN(s) run_script(s, sizeof(s)/sizeof(s[0]))

static void codec_reset(void) {
  rlcd42_es8311_write(0x00, 0x1F);
  delay(20);
  rlcd42_es8311_write(0x00, 0x00);
  rlcd42_es8311_write(0x00, 0x80);
}

static void apply_candidate(int idx) {
  switch (idx) {
    case 0:  // A: esp-bsp MCLKモード
      rlcd42_codec_reinit(true, 4096000, 16000);
      rlcd42_es8311_write(0x32, 0xC8);
      break;
    case 1:  // B: デモ移植 MCLK
      codec_reset(); RUN(SCRIPT_OPEN); RUN(SCRIPT_MCLK); RUN(SCRIPT_START);
      break;
    case 2:  // C: B + SCLK反転
      codec_reset(); RUN(SCRIPT_OPEN); RUN(SCRIPT_MCLK); RUN(SCRIPT_START);
      rlcd42_es8311_write(0x06, 0x23);
      break;
    case 3:  // D: デモ移植 SCLKソース
      codec_reset(); RUN(SCRIPT_OPEN); RUN(SCRIPT_SCLK); RUN(SCRIPT_START);
      break;
    case 4:  // E: D + SCLK反転
      codec_reset(); RUN(SCRIPT_OPEN); RUN(SCRIPT_SCLK); RUN(SCRIPT_START);
      rlcd42_es8311_write(0x06, 0x23);
      break;
  }
  digitalWrite(RLCD42_SPK_EN, HIGH);
}

static const char* cand_names[] = {
  "A bsp MCLK", "B demo MCLK", "C demo MCLK inv", "D demo SCLK", "E demo SCLK inv"
};

// ---------- 全二重I2S ----------
static bool i2s_ready = false;

static bool fullduplex_i2s_init(void) {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 240;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // MCLK = 4.096MHz
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { return false; }
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = RLCD42_I2S_MCLK;
  pins.bck_io_num   = RLCD42_I2S_BCLK;
  pins.ws_io_num    = RLCD42_I2S_LRCK;
  pins.data_out_num = RLCD42_I2S_DOUT;
  pins.data_in_num  = RLCD42_I2S_DIN;
  return i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
}

// トーンを流しつつマイクで拾い、ピーク値を返す
static int32_t play_and_measure(uint32_t ms) {
  static int16_t txbuf[480];   // 240フレーム(L/R)
  static int16_t rxbuf[480];
  int phase = 0;
  int32_t peak = 0;
  uint32_t start = millis();
  while (millis() - start < ms) {
    for (int i = 0; i < 240; ++i) {
      int16_t s = (int16_t)(22000.0f * sinf(2.0f * 3.14159265f * 1000.0f * phase / 16000.0f));
      txbuf[i * 2] = s;
      txbuf[i * 2 + 1] = s;
      phase++;
    }
    size_t n;
    i2s_write(I2S_NUM_0, txbuf, sizeof(txbuf), &n, portMAX_DELAY);
    i2s_read(I2S_NUM_0, rxbuf, sizeof(rxbuf), &n, 0);
    if (millis() - start > 300) {   // 立ち上がり300msは測定しない
      for (size_t i = 0; i < n / 2; ++i) {
        int32_t a = abs(rxbuf[i]);
        if (a > peak) { peak = a; }
      }
    }
  }
  return peak;
}

void setup() {
  delay(1000);
  rlcd42_display_init();
  M5.Display.fillScreen(TFT_BLACK);
  band(0);
  Serial.begin(115200);

  {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
  }
  band(1);

  rlcd42_gfx_skip_autodetect();
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  band(2);

  rlcd42_audio_init();   // ES8311 + ES7210(クロック設定あり) 初期化
  band(3);

  i2s_ready = fullduplex_i2s_init();
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 90);
  M5.Display.print(i2s_ready ? "I2S full-duplex OK" : "I2S INIT FAILED!");
  delay(800);
}

void loop() {
  static uint32_t cycle = 0;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 4);
  M5.Display.printf("codec matrix cyc%lu", (unsigned long)cycle);

  int32_t best_peak = -1;
  int best = -1;
  for (int i = 0; i < 5; ++i) {
    M5.Display.setCursor(4, 30 + i * 40);
    M5.Display.printf("%s ...", cand_names[i]);

    apply_candidate(i);
    delay(100);
    int32_t peak = i2s_ready ? play_and_measure(1500) : -1;

    M5.Display.setCursor(4, 30 + i * 40);
    M5.Display.printf("%s pk=%ld    ", cand_names[i], (long)peak);
    Serial.printf("[TEST] cand %s peak=%ld\n", cand_names[i], (long)peak);
    if (peak > best_peak) { best_peak = peak; best = i; }
    delay(400);
  }

  M5.Display.setTextSize(3);
  M5.Display.setCursor(4, 240);
  if (best_peak > 2000) {
    M5.Display.printf("WINNER: %s", cand_names[best]);
  } else {
    M5.Display.print("no sound detected");
  }
  delay(8000);
  cycle++;
}

#endif  // RLCD42_MINIMAL_TEST

#include <Arduino.h>
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include "share/Version.h"
#include "share/Mutex.h"
#include "share/SDUtil.h"
#include "share/DefaultParams.h"
#include <M5Unified.h>
#include <nvs.h>
#include <Avatar.h>
#include <faces/CatFace.h>
#include "StackchanExConfig.h" 
#include "Robot.h"
#include "mod/ModManager.h"
#include "mod/ModBase.h"
#include "mod/AiStackChan/AiStackChanMod.h"
#include "mod/AiStackChan/RealtimeAiMod.h"
#include "mod/Pomodoro/PomodoroMod.h"
#include "mod/PhotoFrame/PhotoFrameMod.h"
#include "mod/StatusMonitor/StatusMonitorMod.h"
#include "mod/VolumeSetting/VolumeSettingMod.h"
#include "mod/QRdisplay/QRdisplayMod.h"
#include "mod/EspNowRemote/EspNowRemoteMod.h"

#include "driver/PlayMP3.h"   //lipSync
#include "driver/TapDetect.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include <ESP8266FtpServer.h>

#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "llm/ChatHistory.h"
#include "llm/Gemini/GeminiLive.h"

#include "WebAPI.h"

#if defined( ENABLE_CAMERA )
#include "driver/Camera.h"
#endif    //ENABLE_CAMERA

#include "driver/WatchDog.h"
#include "SDUpdater.h"
#include "DebugTools.h"

#if defined(USE_AUDIO_MODULE)
#include "driver/M5AudioModule.h"
#endif

#if defined(ARDUINO_WS_RLCD42)
#include "board/rlcd42/BoardRLCD42.h"
static m5::Button_Class BtnKey;   // KEY button (GPIO18)
static m5::Button_Class BtnBoot;  // BOOT button (GPIO0)
#endif

StackchanExConfig system_config;
Robot* robot;
bool isOffline = false;


// NTP接続情報　NTP connection information.
const char* NTPSRV      = "ntp.jst.mfeed.ad.jp";    // NTPサーバーアドレス NTP server address.
const long  GMT_OFFSET  = 9 * 3600;                 // GMT-TOKYO(時差９時間）9 hours time difference.
const int   DAYLIGHT_OFFSET = 0;                    // サマータイム設定なし No daylight saving time setting

//bool servo_home = false;
bool servo_home = true;
volatile bool espnow_remote_servo_override = false;

using namespace m5avatar;
Avatar avatar;
Face* customFace;
const Expression expressions_table[] = {
  Expression::Neutral,
  Expression::Happy,
  Expression::Sleepy,
  Expression::Doubt,
  Expression::Sad,
  Expression::Angry
};

FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP8266FtpServer.h to see ftp verbose on serial


void lipSync(void *args)
{
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef REALTIME_API
#ifdef REALTIME_API_WITH_TTS
    level = robot->tts->getLevel();
#else
    level = ((RealtimeLLMBase*)(robot->llm))->getAudioLevel();
#endif
#else
    level = robot->tts->getLevel();
#endif
    if(level<100) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
    avatar->setRotation(gazeX * 5);
    delay(100);
  }
}


void servo(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef USE_SERVO
    if(espnow_remote_servo_override)
    {
      delay(100);
      continue;
    }

    if(!servo_home)
    {
      avatar->getGaze(&gazeY, &gazeX);
      robot->servo->moveTo((int)(15.0 * gazeX), (int)(10.0 * gazeY));
    } else {
      robot->servo->moveToOrigin();
    }
#endif
    delay(5000);
  }
}

void battery_check(void *args) {
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    int32_t batteryLevel = M5.Power.getBatteryLevel();
    if((batteryLevel < 95) && (batteryLevel != 0)){
      avatar->setBatteryIcon(true);
      avatar->setBatteryStatus(M5.Power.isCharging(), M5.Power.getBatteryLevel());
    }
    else{
      avatar->setBatteryIcon(false);    
    }
    delay(60000);
  }
}

bool Wifi_connection_check() {
  unsigned long start_millis = millis();

  // 前回接続時情報で接続する
  while (WiFi.status() != WL_CONNECTED) {
    M5.Display.print(".");
    Serial.print(".");
    delay(1000);
    // 5秒以上接続できなかったら抜ける
    if ( 5000 < (millis() - start_millis) ) {
      //break;
      return false;
    }
  }
  return true;
}

bool WifiSmartConfig() {
#if defined(USE_LLM_MODULE)
  // LLMモジュール使用時は普通はオフラインが前提のため、Smart Config待ちはしない
  return false;
#else
  unsigned long start_millis = millis();
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  M5.Display.println("Waiting for SmartConfig");
  Serial.println("Waiting for SmartConfig");
  while (!WiFi.smartConfigDone()) {
    delay(1000);
    M5.Display.print("#");
    Serial.print("#");
    // 30秒以上接続できなかったら抜ける
    if ( 30000 < millis() - start_millis) {
      Serial.println("");
      //Serial.println("Reset");
      //ESP.restart();
      return false;
    }
  }
  return true;
#endif
}

void time_sync(const char* ntpsrv, long gmt_offset, int daylight_offset) {
  struct tm timeInfo;
  char buf[60];

  // NTPサーバと同期(予備サーバを含む3系統)
  // 注意: TLS証明書の検証には正しい時刻が必須。時計が1970年のままだと
  //       Gemini等への接続が X509 verify failed (-9984) で失敗する。
  configTime(gmt_offset, daylight_offset, ntpsrv, "time.google.com", "pool.ntp.org");

  // 最大20秒待って同期を確認する
  bool synced = false;
  for (int i = 0; i < 20; ++i) {
    if (getLocalTime(&timeInfo, 1000)) { synced = true; break; }
    Serial.print("n");
  }

  if (synced) {
    Serial.print("NTP : ");                                 // シリアルモニターに表示
    Serial.println(ntpsrv);                                 // シリアルモニターに表示

    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d\n",     // 表示内容の編集
    timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
    timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    Serial.println(buf);                                    // シリアルモニターに表示
  }
  else {
    Serial.println("NTP Sync Error (TLS接続は時刻不正で失敗します)");
  }
}



ModBase* init_mod(void)
{
  ModBase* mod;
  if(!isOffline || robot->isAllOfflineService()){
#if defined(REALTIME_API)
    add_mod(new RealtimeAiMod(isOffline));
#else
    add_mod(new AiStackChanMod(isOffline));
#endif
  }
  add_mod(new StatusMonitorMod());
  add_mod(new VolumeSettingMod());
  //add_mod(new EspNowRemoteMod());
  //add_mod(new PomodoroMod(isOffline));
  //add_mod(new PhotoFrameMod(isOffline));
  //add_mod(new QRdisplayMod());
  mod = get_current_mod();
  mod->init();
  return mod;
}


void sw_tone()
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();
  delay(300);     // AtomS3Rはこのdelayがないと鳴らないときがある
  M5.Speaker.tone(1000, 100);
  delay(500);

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}
  
void alarm_tone()
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();

  for(int i=0; i<5; i++){
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(1000);  
  }

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}

void init_mic_spk()
{
#if defined(USE_AUDIO_MODULE)
  initAudioModule();
#endif

  {
    auto micConfig = M5.Mic.config();
    //micConfig.stereo = false;
    micConfig.sample_rate = 16000;
#if defined(USE_AUDIO_MODULE)
    micConfig.pin_data_in = SYS_I2S_DIN_PIN;
    micConfig.pin_bck = SYS_I2S_SCLK_PIN;
    micConfig.pin_mck = SYS_I2S_MCLK_PIN;
    micConfig.pin_ws = SYS_I2S_LRCK_PIN;
#elif defined(ARDUINO_WS_RLCD42)
    // MCLK必須(ES7210/ES8311とも比256のMCLKで動作することを実機計測で確認済み)
    micConfig.pin_data_in = RLCD42_I2S_DIN;
    micConfig.pin_bck = RLCD42_I2S_BCLK;
    micConfig.pin_mck = RLCD42_I2S_MCLK;
    micConfig.pin_ws = RLCD42_I2S_LRCK;
    micConfig.stereo = false;
#endif
    M5.Mic.config(micConfig);
  }
  M5.Mic.begin();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 64000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;

#if defined(USE_AUDIO_MODULE)
    spk_cfg.pin_data_out = SYS_I2S_DOUT_PIN;
    spk_cfg.pin_bck = SYS_I2S_SCLK_PIN;
    spk_cfg.pin_mck = SYS_I2S_MCLK_PIN;
    spk_cfg.pin_ws = SYS_I2S_LRCK_PIN;
#elif defined(ARDUINO_WS_RLCD42)
    // 実クロック: LRCK = sample_rate/2 = 32kHz, MCLK = 256×LRCK = 8.192MHz
    spk_cfg.sample_rate = RLCD42_SPK_SAMPLE_RATE;
    spk_cfg.pin_data_out = RLCD42_I2S_DOUT;
    spk_cfg.pin_bck = RLCD42_I2S_BCLK;
    spk_cfg.pin_mck = RLCD42_I2S_MCLK;
    spk_cfg.pin_ws = RLCD42_I2S_LRCK;
    spk_cfg.stereo = false;
#endif
    M5.Speaker.config(spk_cfg);
  }
  //M5.Speaker.begin();
}

void setup()
{
  /// シリアル出力のログレベルを VERBOSEに設定
  //M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_VERBOSE);

  auto cfg = M5.config();

#if defined(ARDUINO_M5STACK_ATOMS3R)
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.external_speaker.atomic_echo = true;
#endif
#if defined(ARDUINO_WS_RLCD42)
  cfg.internal_spk = false;   // ES8311/ES7210は自前で初期化する
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
#endif
  cfg.serial_baudrate = 115200;   //M5Unified 0.1.17からデフォルトが0になったため設定

#if defined(ARDUINO_WS_RLCD42)
  // 重要: M5.beginの前に実行すること。
  // 未知ボードに対するM5GFXのボード自動判定はGPIOを探ってクラッシュするため無効化し、
  // ST7305ディスプレイを先にM5.Displayへ接続しておく。
  rlcd42_gfx_skip_autodetect();
  rlcd42_display_init();
#endif

  M5.begin(cfg);

#if defined(ARDUINO_WS_RLCD42)
  // オーディオコーデックとボタンの初期化
  rlcd42_audio_init();
  pinMode(RLCD42_BTN_KEY, INPUT_PULLUP);
  pinMode(RLCD42_BTN_BOOT, INPUT_PULLUP);
  BtnKey.setHoldThresh(1000);
#endif

  ///// Debug /////
#if 0
  check_board();
  Wire.begin(); 
  i2c_scan(Wire);
  Wire1.begin(); 
  i2c_scan(Wire1);
#endif
  /////////////////

#if defined(ARDUINO_M5STACK_ATOMS3R)
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("Ver.%s\n", FW_VERSION);
#else
  M5.Lcd.setFont(&fonts::lgfxJapanGothic_20);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("AI Stack-chan Ex [・＿・]");
  M5.Lcd.printf("Firmware Version: %s\n", FW_VERSION);
#endif

  initMutex();

#if defined(ENABLE_SD_UPDATER)
  // ***** for SD-Updater *********************
  SDU_lobby("AiStackChanEx");
  // ******************************************
#endif

  //auto brightness = M5.Display.getBrightness();
  //Serial.printf("Brightness: %d\n", brightness);

  init_mic_spk();

  /// settings
#if defined(ARDUINO_M5STACK_ATOMS3R) || defined(ARDUINO_WS_RLCD42)
  if (SPIFFS.begin()) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SPIFFS, "/SC_ExConfig.yaml", 2048,
                                     "/SC_SecConfig.yaml", 2048,
                                     "/SC_BasicConfig.yaml", 2048);
#else
  if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SD, "/app/AiStackChanEx/SC_ExConfig.yaml");
#endif
    // Wifi設定読み込み
    wifi_s* wifi_info = system_config.getWiFiSetting();
    Serial.printf("\nSSID: %s\n",wifi_info->ssid.c_str());
    Serial.printf("Key: %s\n",wifi_info->password.c_str());

    // 前回設定で接続
    Serial.println("Connecting to WiFi");
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    if(Wifi_connection_check()){
      Serial.println("Successfully connected to Wi-Fi using the previous settings.");
    }else{
      // 前回設定での接続に失敗。SDカード設定による接続にトライ。
      Serial.println("The previous WiFi connection failed. Attempting to connect using the SD card settings.");
      if(wifi_info->ssid.length() == 0){
        // SDカード設定の取得に失敗。Smart Configをスタート。
        Serial.println("Can't get WiFi settings. Start Smart Config.");
        if(!WifiSmartConfig()){
          // Smart Config失敗。オフラインモード。
          Serial.println("Smart Config failed. Running in offline mode.");
          isOffline = true;
        }
      }else{
        WiFi.begin(wifi_info->ssid.c_str(), wifi_info->password.c_str());
        if(Wifi_connection_check()){
          // SDカード設定による接続に成功。
          Serial.println("Successfully established a Wi-Fi connection via the SD card settings.");
        }else{
          // SDカード設定による接続に失敗。Smart Configをスタート。
          Serial.println("WiFi connection failed due to SD card settings. Start Smart Config.");
          if(!WifiSmartConfig()){
            // Smart Config失敗。オフラインモード。
            Serial.println("Smart Config failed. Running in offline mode.");
            isOffline = true;
          }
        }
      }
    }

    if(!isOffline){
      Serial.println(WiFi.localIP());
      M5.Lcd.println(WiFi.localIP());
      delay(1000);

      //Webサーバ設定
      init_web_server();
      //FTPサーバ設定（SPIFFS用）
      ftpSrv.begin("stackchan","stackchan");    //username, password for ftp.  set ports in ESP8266FtpServer.h  (default 21, 50009 for PASV)
      Serial.println("FTP server started");
      M5.Lcd.println("FTP server started");

      //時刻同期
      time_sync(NTPSRV, GMT_OFFSET, DAYLIGHT_OFFSET);
    }else{
      M5.Lcd.print("Can't connect to WiFi. Start offline mode.\n");
    }

    robot = new Robot(system_config);

    //SD.end();
  } else {
    M5.Lcd.print("Failed to load SD card settings. System reset after 5 seconds.");
    delay(5000);
    ESP.restart();
    //WiFi.begin();
  }
  
  mp3_init();

  //mod設定
  init_mod();

#if defined(ARDUINO_M5STACK_ATOMS3R)
#if defined(CAT_FACE)
  customFace = new CatFace();
  avatar.setFace(customFace);
#endif
  avatar.setScale(0.5);
  avatar.setPosition(-56, -96);
  avatar.init();
#elif defined(ARDUINO_WS_RLCD42)
#if defined(CAT_FACE)
  customFace = new CatFace();
  avatar.setFace(customFace);
#endif
  // 顔の論理サイズ320x240 → 400x300にちょうど収まる倍率
  avatar.setScale(1.25);
  avatar.setPosition(0, 0);
  avatar.init();   // colorDepth=1 (モノクロRLCDに最適)
#else
  //avatar.init();
  avatar.init(16);
#endif

  avatar.addTask(lipSync, "lipSync", 2048, 2);
  avatar.addTask(servo, "servo", 2048);
  avatar.addTask(battery_check, "battery_check", 2048);
  avatar.setSpeechFont(&fonts::efontJA_16);

  Serial.printf("Speaker volume (yaml): %d\n", system_config.getExConfig().audio.speaker_volume);
  if(0 != system_config.getExConfig().audio.speaker_volume){
    robot->spk_volume = system_config.getExConfig().audio.speaker_volume;
  }else{
    robot->spk_volume = DEFAULT_SPEAKER_VOLUME;
  }
  Serial.printf("Speaker volume (set): %d\n", robot->spk_volume);
  M5.Speaker.setVolume(robot->spk_volume);

#if defined(ENABLE_CAMERA)
  camera_init();
  avatar.set_isSubWindowEnable(true);
#endif

#if defined(ENABLE_TAP_DETECT)
  invokeDoubleTapDetectTask();
#endif

  //init_watchdog();

  //ヒープメモリ残量確認(デバッグ用)
  check_heap_free_size();
  check_heap_largest_free_block();

}



void loop()
{
  //get_elapsed_time_micro("loop() start");
  M5.update();
  //get_elapsed_time_micro("M5.update time");
  ModBase* mod = get_current_mod();
  mod->idle();
  //get_elapsed_time_micro("Mod idle time");

#if defined(ARDUINO_WS_RLCD42)
  // KEY(GPIO18): 短押し=会話開始/停止(btnA相当)、長押し(1秒)=ウェイクワード登録(btnB長押し相当)
  // BOOT(GPIO0): 短押し=WebUIのQRコード表示(btnC相当)
  {
    uint32_t ms = millis();
    BtnKey.setRawState(ms, !digitalRead(RLCD42_BTN_KEY));
    BtnBoot.setRawState(ms, !digitalRead(RLCD42_BTN_BOOT));

    if (BtnKey.wasClicked())  { mod->btnA_pressed(); }
    if (BtnKey.wasHold())     { mod->btnB_longPressed(); }
    if (BtnBoot.wasClicked()) { mod->btnC_pressed(); }
  }
#endif

#if !defined(ARDUINO_WS_RLCD42)
  // 注意: 未知ボードではM5UnifiedがBtnAをGPIO0(BOOT)に割り当てるため、
  // 当ボードでは上のKEY/BOOT独自処理と二重反応する。本家のボタン処理は無効化。
  if (M5.BtnA.wasPressed())
  {
    mod->btnA_pressed();
  }

  if (M5.BtnA.pressedFor(2000))
  {
    mod->btnA_longPressed();
  }

  if (M5.BtnB.wasPressed())
  {
    mod->btnB_pressed();
  }

  if (M5.BtnB.pressedFor(2000))
  {
    mod->btnB_longPressed();
  }

  if (M5.BtnC.wasPressed())
  {
    mod->btnC_pressed();
  }
#endif

#if defined(ARDUINO_M5STACK_Core2) || defined( ARDUINO_M5STACK_CORES3 )
  auto count = M5.Touch.getCount();
  if (count)
  {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed())
    {
      mod->display_touched(t.x, t.y);
    }

    if (t.wasFlicked())
    {
      int16_t dx = t.distanceX();
      int16_t dy = t.distanceY();

      // detect flick right/left
      if(abs(dx) >= abs(dy))
      {
        if(dx > 0){
          //Serial.println("Right flicked");
          change_mod(true);
        }
        else{
          //Serial.println("Left flicked");
          change_mod();
        }
      }
    }
  }
#endif

#if defined(ENABLE_TAP_DETECT)
  if(doubleTapDetected){
    Serial.println("loop(): Double tap detected");
    mod->doubleTapped(detectedAcc[0], detectedAcc[1], detectedAcc[2]);
    doubleTapDetected = false;
  }

  // Modで重い処理をしている場合はダブルタップ検出を停止する
  if(mod->isBusy()){
    stopDoubleTapDetectTask();
  }else{
    resumeDoubleTapDetectTask();
  }
#endif
  //get_elapsed_time_micro("Callback process time");

  if(!isOffline){
    web_server_handle_client();
    ftpSrv.handleFTP();
  }

  //get_elapsed_time_micro("Web event process time");
  
  //reset_watchdog();
}

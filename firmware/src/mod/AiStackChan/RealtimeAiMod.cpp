#if defined(REALTIME_API)

#include <Arduino.h>
#include <deque>
#include <SD.h>
#include <SPIFFS.h>
#include "mod/ModManager.h"
#include "RealtimeAiMod.h"
#include <Avatar.h>
#include "Robot.h"
#include "llm/ChatGPT/FunctionCall.h"
#include <WiFiClientSecure.h>
#include "Scheduler.h"
#include "MySchedule.h"
#include "share/SDUtil.h"
#include "share/Mutex.h"
#include "driver/HeadTouchSensor.h"

#if defined(ENABLE_WAKEWORD)
#include "driver/WakeWord.h"
#endif

using namespace m5avatar;


/// 外部参照 ///
extern Avatar avatar;
extern bool servo_home;
extern void sw_tone();
extern void alarm_tone();
#if defined(ENABLE_WAKEWORD)
extern bool wakeword_is_enable;   // AiStackChanMod.cppで定義
#endif
///////////////



RealtimeAiMod::RealtimeAiMod(bool _isOffline)
  : isOffline{_isOffline}
{
  box_servo.setupBox(80, 120, 80, 80);
  box_stt.setupBox(0, 0, M5.Display.width(), 60);
  box_BtnA.setupBox(0, 100, 40, 60);
  box_BtnC.setupBox(280, 100, 40, 60);

  pRtLLM = (RealtimeLLMBase*)robot->llm;
  pRtLLM->invokeWebSocketLoopTask();

#if defined(ENABLE_WAKEWORD)
  if(robot->m_config.getExConfig().wakeword.type == WAKEWORD_TYPE_SIMPLEVOX){
    wakeword_init();
    // 登録済みウェイクワードがあれば起動時から待ち受けを有効化
    if(wakeword_any_registered()){
      mode = 1;
      wakeword_is_enable = true;
      Serial.println("Wakeword: registered data found. Listening enabled.");
    }
  }
#endif

  //servo_home = false;

#if 0
  if(!isOffline){
    //スケジューラ設定
    init_schedule();
  }
#endif
}


void RealtimeAiMod::init(void)
{
  //avatar.setSpeechText("Realtime AI");
  avatar.set_isSubWindowEnable(true);
  pRtLLM->resumeWebSocketLoopTask();
}

void RealtimeAiMod::pause(void)
{
  avatar.set_isSubWindowEnable(false);
  pRtLLM->suspendWebSocketLoopTask();
}


void RealtimeAiMod::update(int page_no)
{

}

void RealtimeAiMod::btnA_pressed(void)
{
#if defined(ARDUINO_M5STACK_ATOMS3R) || defined(ARDUINO_WS_RLCD42)
  Serial.println("Btn A pressed");
  sw_tone();
  toggleRealtimeRecord();
#endif
}

void RealtimeAiMod::btnB_longPressed(void)
{
#if defined(ENABLE_WAKEWORD)
  // ウェイクワード登録モード開始（登録後は自動で待ち受け有効化）
  if(pRtLLM->isRealtimeRecording()){
    pRtLLM->stopRealtimeRecord();
  }
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.tone(1000, 100);
  delay(500);
  M5.Speaker.tone(600, 100);
  delay(1000);
  M5.Speaker.end();
  M5.Mic.begin();
  wakeword_is_enable = false;
  mode = -1;
  avatar.setSpeechText("ウェイクワード登録開始");
#endif
}

void RealtimeAiMod::btnC_pressed(void)
{
  static bool isQrDrawing = false;
  if(!isQrDrawing){
    avatar.setSpeechText("");
    String url = String("http://") + WiFi.localIP().toString();
    avatar.updateSubWindowQrcode(url);
    avatar.set_isSubWindowEnable(true);
    isQrDrawing = true;
  }else{
    avatar.set_isSubWindowEnable(false);
    isQrDrawing = false;
  }
}

void RealtimeAiMod::display_touched(int16_t x, int16_t y)
{
  if (box_stt.contain(x, y))
  {
    sw_tone();
    toggleRealtimeRecord();
  }
#ifdef USE_SERVO
  if (box_servo.contain(x, y))
  {
    sw_tone();
    servo_home = !servo_home;
  }
#endif
  if (box_BtnA.contain(x, y))
  {
    //sw_tone();
  }
  if (box_BtnC.contain(x, y))
  {
    btnC_pressed();
  }

}

void RealtimeAiMod::doubleTapped(float ax, float ay, float az)
{
  Serial.printf("Mod double tapped. ax=%.3f ay=%.3f az=%.3f\n", ax, ay, az);
#if defined(ARDUINO_M5STACK_ATOMS3R)
  sw_tone();
  toggleRealtimeRecord();
#endif
}


void RealtimeAiMod::idle(void)
{
#ifdef REALTIME_API_WITH_TTS

  if(robot->asyncPlaying || (pRtLLM->getOutputTextQueueSize() != 0)){
    // 発話中
    pRtLLM->setSpeaking(true);
  }
  else{
    // 発話停止中かつキューにテキストがない場合はLLM機能に発話終了を通知
    pRtLLM->setSpeaking(false);
  }

#endif  //REALTIME_API_WITH_TTS

#if defined(ENABLE_WAKEWORD)
  wakewordHandler();
#endif

  // Alarm (Function Calling)
  alarmEventHandler();
  updateHeadTouchExpression();

#if 0 
  //スケジューラ処理
  if(!isOffline){
    run_schedule();
  }
#endif

}

#if defined(ENABLE_WAKEWORD)
void RealtimeAiMod::wakewordHandler(void)
{
  if (mode < 0) {
    // 登録モード: ウェイクワードを発話すると録音・再生して登録される
    int idx = wakeword_regist();
    if(idx >= 0){
      String text = String("ウェイクワード#") + String(idx) + String("登録終了");
      avatar.setSpeechText(text.c_str());
      delay(1000);
      avatar.setSpeechText("");
      mode = 1;
      wakeword_is_enable = true;
    }
  }
  else if (mode > 0 && wakeword_is_enable) {
    // 録音中・応答再生中はマイクを触らない(I2Sポート競合で再生が失敗するため)。
    // さらに再生終了後1.5秒は待ち受けを再開しない(自己音声の残響への誤反応防止)。
    static uint32_t quietSinceMs = 0;
    bool busy = pRtLLM->isRealtimeRecording() || pRtLLM->isSpeaking() || M5.Speaker.isRunning();
    if (busy) {
      quietSinceMs = 0;
      return;
    }
    if (quietSinceMs == 0) {
      quietSinceMs = millis();
      clear_buff();   // 溜まった音声バッファを破棄してから待ち受け再開
      return;
    }
    if (millis() - quietSinceMs < 1500) { return; }

    // 再生タスクのMic.end→Speaker.begin遷移と排他してマイクを読む
    enterMutexAudio();
    int idx = wakeword_compare();
    exitMutexAudio();
    if (idx >= 0) {
      Serial.println("Wakeword detected!");
      String text = String("ウェイクワード#") + String(idx);
      avatar.setSpeechText(text.c_str());
      sw_tone();
      avatar.setSpeechText("");
      if (!pRtLLM->isRealtimeRecording()) {
        pRtLLM->startRealtimeRecord();
      }
    }
  }
}
#endif  //ENABLE_WAKEWORD

void RealtimeAiMod::alarmEventHandler()
{
  if(xAlarmTimer != NULL){
    TickType_t xRemainingTime;

    /* Query the period of the timer that expires. */
    xRemainingTime = xTimerGetExpiryTime( xAlarmTimer ) - xTaskGetTickCount();
    avatarText = "Alarm countdown: " + String(xRemainingTime / 1000);
    avatar.set_isSubWindowEnable(true);
    avatar.updateSubWindowTxt(avatarText, 0, 0, 200, 50);
  }

  if (alarmTimerCallbacked) {
    alarmTimerCallbacked = false;
    avatar.set_isSubWindowEnable(false);
    alarm_tone();
  }

  if (alarmTimerCanceled) {
    alarmTimerCanceled = false;
    avatar.set_isSubWindowEnable(false);
  }

}

void RealtimeAiMod::updateHeadTouchExpression(void)
{
  HeadTouchSensor::Gesture gesture = HeadTouchSensor::update();
  if (HeadTouchSensor::isPetGesture(gesture)) {
    headTouchHappyUntilMs = millis() + 3000;
    headTouchHappyActive = true;
    avatar.setExpression(Expression::Happy);
    Serial.printf("[HeadTouch] pet gesture=%s\n", HeadTouchSensor::gestureName(gesture));
  }

  if (headTouchHappyActive && millis() < headTouchHappyUntilMs) {
    avatar.setExpression(Expression::Happy);
    return;
  }

  if (headTouchHappyActive) {
    headTouchHappyActive = false;
    avatar.setExpression(Expression::Neutral);
  }
}

bool RealtimeAiMod::isBusy(void)
{
  if(pRtLLM->isRealtimeRecording() || pRtLLM->isSpeaking()){
    return true;
  }else{
    return false;
  }
}

void RealtimeAiMod::toggleRealtimeRecord(void)
{
  if(pRtLLM->isRealtimeRecording()){
    pRtLLM->stopRealtimeRecord();
  }else{
    pRtLLM->startRealtimeRecord();
  }
}

#endif //REALTIME_API

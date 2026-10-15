# AIスタックチャンEx - Waveshare ESP32-S3-RLCD-4.2 移植版

[AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex)（ベースコミット: `BASE_COMMIT.txt` 参照）を
[Waveshare ESP32-S3-RLCD-4.2](https://docs.waveshare.com/ESP32-S3-RLCD-4.2) に移植したものです。
**Gemini Live API によるリアルタイム音声会話**に対応しています。

## この移植版の構成

- **会話AI**: Gemini Live API（リアルタイム双方向音声。Geminiとの会話はこのRealtime APIモードのみ対応）
- **ディスプレイ**: ST7305 モノクロ反射LCD 400×300（カスタムLovyanGFXパネルドライバ `src/board/rlcd42/Panel_ST7305.*`）
- **オーディオ**: ES8311（スピーカーDAC）+ ES7210（デュアルマイクADC）を直接初期化（`src/board/rlcd42/BoardRLCD42.*`）
- **入力**: KEYボタン + ウェイクワード（SimpleVox・音声登録式）の両対応
- **サーボ**: 非接続（ビルドフラグ `USE_SERVO` 無効）
- **設定**: SDカード不使用。AtomS3R方式（SPIFFSにYAMLを書き込み）

## ボタン操作

| 操作 | 機能 |
|---|---|
| KEY 短押し | 会話の開始 / 停止 |
| KEY 長押し（1秒） | ウェイクワード登録モード開始 |
| BOOT 短押し | Web設定画面のQRコード表示 / 消去 |

## セットアップ手順

### 1. ビルド環境

VS Code + PlatformIO拡張をインストールし、`firmware/` フォルダを開きます。
プロジェクトのパスに日本語などASCII以外の文字が含まれる場合は、`platformio.ini` の `workspace_dir` の行のコメントを外し、ASCIIだけのパスを指定してください（下の「ハマりどころ」4）。

### 2. YAML設定の編集

`firmware/data_rlcd42/` の3ファイルを `firmware/data/` にコピーし（`data` フォルダがなければ作成）、`firmware/data/` 側を編集します。
`firmware/data/` は `.gitignore` の対象なので、Wi-FiのパスワードやAPIキーを書いてもリポジトリには入りません。

- **SC_SecConfig.yaml**: Wi-FiのSSID/パスワード、Gemini APIキー（`apikey: aiservice:`）を記入
  - APIキーは https://aistudio.google.com/apikey で無料取得できます
- **SC_ExConfig.yaml**: `llm: type: 3`（Gemini Realtime）設定済み。音量などはお好みで
- **SC_BasicConfig.yaml**: 基本そのままでOK

### 3. ファームウェア書き込み

1. PCとボードをUSBケーブルで接続
2. PlatformIOで env **`ws-rlcd42-realtime`** を選択
3. **Upload** を実行
   - 書き込みに失敗する場合は、BOOTボタンを押しながらリセット（PWR長押し→入）でダウンロードモードに入れてから再実行

### 4. SPIFFSへの設定書き込み（初回およびYAML変更時）

1. PlatformIOの env `ws-rlcd42-realtime` で **Upload Filesystem Image** を実行
   - `firmware/data/` の3つのYAMLがSPIFFSに書き込まれます
2. 以後は、Wi-Fi接続済みならFTPでも編集可能です（ユーザ名: `stackchan` / パスワード: `stackchan`）

### 5. ウェイクワード「スタックチャン」の登録

SimpleVoxは話者の声をそのまま登録するタイプのウェイクワードエンジンです（事前学習モデル不要）。

1. 起動後、**KEYボタンを1秒長押し** →「ウェイクワード登録開始」と表示される
2. マイクに向かって「**スタックチャン**」と話しかける
3. 録音された音声が再生され、「ウェイクワード#0登録終了」と表示されれば完了
4. 以後は「スタックチャン」と呼びかけるだけで会話が始まります
   - 最大10個まで登録可能（家族の声を追加登録できます）
   - 誤認識が多い場合は `src/driver/WakeWord.h` の `DIST_THRESHOLD` を調整
   - 登録データはSPIFFSに `wakeword#.bin` として保存され、FTPで削除できます

## 使い方

- 「スタックチャン」と呼びかける、またはKEY短押しで会話開始（開始音が鳴ります）
- あとは自由に話しかけるだけ。リアルタイムで応答し、口が動きます
- 無応答30秒で自動的に会話終了。KEY短押しでも終了

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| 画面が乱れる・ノイズが出る | `platformio.ini` の `-DRLCD42_LCD_SPI_FREQ=10000000` を有効化（SPIクロックを下げる） |
| 表示が上下逆 | `-DRLCD42_OFFSET_ROTATION=2` を有効化 |
| 音が出ない | シリアルモニタで `[RLCD42] audio init ES8311:OK ES7210:OK` を確認。NGならI2C配線/電源を確認 |
| マイクが反応しない | 音量（`SC_ExConfig.yaml` の `speaker_volume`）とマイクゲイン（`BoardRLCD42.cpp` の `ES7210_MIC_GAIN_30DB`）を調整 |
| Wi-Fiに繋がらない | 2.4GHz帯のSSIDか確認。SC_SecConfig.yamlの再アップロードを忘れずに |
| 描画をもっと速くしたい | `-DRLCD42_LCD_SPI_FREQ=10000000` を有効化（既定4MHz、上限10MHz目安） |
| 起動しない・原因不明の再起動 | env `ws-rlcd42-min` を書き込むと画面の白帯の本数で故障箇所を診断できます |

## 実装メモ（変更点の概要）

- `firmware/src/board/rlcd42/` — 本ボード用ドライバ一式（新規）
  - `Panel_ST7305.*`: LovyanGFX用パネルドライバ。1bppバッファ+Bayerディザ、フルフレームSPI転送（15KB/フレーム、SPI既定4MHz・上限10MHz）
  - `BoardRLCD42.*`: ピン定義、`M5.Display.init(&panel)` によるM5GFXへの接続、ES8311/ES7210レジスタ初期化
  - `minimal_test.cpp`: 起動診断用ファーム（env `ws-rlcd42-min`。白帯の本数でステージを表示）
- `firmware/lib/esp_codec/` — Espressif esp-bsp のES8311/ES7210ドライバ（Apache-2.0）をvendor
- `firmware/src/main.cpp` — ボード分岐追加（I2Sピン設定、SPIFFS設定読み込み、KEY/BOOTボタン処理、Avatar 1.25倍スケール）
- `firmware/src/mod/AiStackChan/RealtimeAiMod.*` — **Realtimeモードへのウェイクワード対応を追加**（本家はAiStackChanModのみ対応）。起動時に登録済みなら自動で待ち受け開始
- `firmware/src/driver/WakeWord.*` — `wakeword_any_registered()` 追加
- クロック設計: M5UnifiedのI2S実クロックは LRCK=cfg.sample_rate/2, MCLK=cfg.sample_rate×128。
  スピーカー(cfg 64kHz)→ES8311は8.192MHz/32kHz、マイク(cfg 16kHz, os=2)→ES7210は4.096MHz/16kHzで整合

### ハマりどころ（移植で実際に踏んだ問題と対策）

1. **M5GFXのボード自動判定でクラッシュ**: 未知ボードでは`M5.begin()`内の自動判定が各GPIOを
   探ってパニックする。`rlcd42_gfx_skip_autodetect()`でダミーボードID(`board_M5UnitLCD`)を
   事前設定して判定を丸ごとスキップ（**M5.beginより前に呼ぶこと**）
2. **LovyanGFXの自動display再帰**: `endWrite`→`display()`が自動で呼ばれるため、`display()`内で
   `startWrite/endWrite`を使うと無限再帰→スタックオーバーフロー。バス直接操作+送信前の
   dirty rectクリアで解決（Panel_ST7305.cpp参照）
3. **esp-sr(esp_ns.h)はarduino-esp32 2.0.9まで**: ウェイクワードに必要。espressif32@6.3.2固定
4. **日本語を含むプロジェクトパス**: xtensaリンカがmapファイルを開けない。`workspace_dir`で
   ビルドフォルダをASCIIパスへ退避
5. **ST7305のSPIクロック上限は約10MHz**（公式推奨は1MHz、既定4MHzで安定動作）
6. **esp-bsp版ES8311ドライバの初期化列ではこのボードのES8311は無音になる**。
   マイクでスピーカー音を自動計測する診断（`ws-rlcd42-min`）で特定した。
   Waveshare公式デモ(esp_codec_dev版)のレジスタ列を移植した`ES8311_INIT_SCRIPT`
   （BoardRLCD42.cpp）を使うこと。MCLKは正常に機能しており（比256=MCLK/LRCK）、
   ES7210（マイク）はesp-bspドライバのクロック設定込みで動作する。
   M5UnifiedのI2S実クロックは LRCK=cfg.sample_rate/2、MCLK=256×LRCK

## 既知の制約

- タッチパネル非搭載のため、画面タッチ操作（モード切替フリック等）は使えません
- バッテリー残量アイコンは未対応（GPIO4のADC読みは未実装）
- モノクロ表示のため、カラー前提の一部表示（QRコード等は問題なし）は簡略化されます
- Function Callingの一部機能（SDカードへのメモ保存等）はSDカード非使用のため動作しません

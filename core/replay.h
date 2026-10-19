// replay.h — 重播檔：錄下「每一幀的輸入」，之後可以完全重現同一場遊戲
//
// 原理：Comet 是 deterministic（確定性）的——
//   同一個遊戲版本 + 同一個亂數種子 + 同一串輸入 = 每一幀的畫面都一樣
// 所以不必錄影片，只要錄「輸入」：一小時大約數十 KB。
//
// 用途：成就與紀錄驗證（任何人都能重跑確認）、bug 回報（百分之百重現）、
//      分享精彩片段、檢查網頁版和主機結果一致。
//
// ---- 錄影（平台用）----
//   engine_set_seed(種子); replay_record_begin(); engine_load(遊戲);
//   （先開始錄影再載入：遊戲在 _init 裡讀存檔，也要記下來）
//   …每一幀 engine_frame() 時，引擎自動記下輸入…
//   replay_snapshot(); replay_snapshot_size();   到目前為止的重播檔（錄影繼續）
//
// ---- 播放（平台用）----
//   replay_play_begin(重播檔); engine_load(同一個遊戲);
//   …engine_frame() 時，輸入改由重播檔提供，平台交進來的輸入被忽略…
//   replay_state() 變成 REPLAY_FINISHED 後，replay_matched() 告訴你最後的畫面是否一致。
//   播完之後輸入還給平台，玩家可以接著玩。
//
// 檔案格式（所有數字都是 little-endian，第 1 版）：
//   位置  大小  內容
//     0    4   "CRPL"
//     4    2   格式版本（1）
//     6    2   檔頭大小（112）——之後版本加欄位時，舊程式可以跳過看不懂的部分
//     8    4   亂數種子
//    12    4   總幀數
//    16   32   遊戲原始碼的 SHA-256（辨認是不是同一個遊戲、同一個版本）
//    48   32   起始狀態的 SHA-256；全部是 0 代表「從新遊戲開始」（之後分段驗證用）
//    80   32   最後一幀畫面的 SHA-256（播放結束時比對）
//   112   …   事件，一個接一個，直到檔尾
//
//   事件：
//     幀數差（varint：距離上一個事件幾幀）
//     1 byte：bit 0–1 玩家、bit 2 有按鍵、bit 3 有左搖桿、bit 4 有右搖桿、
//             bit 5–7 事件種類（0 = 輸入、1 = 讀存檔、其他保留）
//
//   種類 0（輸入）：
//     按鍵 2 bytes／左搖桿 x、y 各 1 byte／右搖桿 x、y 各 1 byte（有才寫）
//     只在輸入「有變化」時才寫事件，按住不放不佔空間。
//   種類 1（讀存檔：遊戲呼叫 loadsave 時讀到了什麼）：
//     開頭 byte 的 bit 0–2 是欄位（0–7）
//     varint：長度 + 1（0 代表「沒有存檔」）
//     存檔的 bytes
//   同一幀裡，輸入事件在前（幀開始時記），讀存檔事件在後（遊戲執行中才發生）。
//   在 _init 裡讀的存檔算第 0 幀。

#ifndef REPLAY_H
#define REPLAY_H

#include <stddef.h>
#include <stdint.h>

#define REPLAY_VERSION     1
#define REPLAY_HEADER_SIZE 112

enum {
    REPLAY_OFF       = 0,
    REPLAY_RECORDING = 1,
    REPLAY_PLAYING   = 2,
    REPLAY_FINISHED  = 3,   // 播完了（輸入已還給平台）
    REPLAY_ARMED     = 4,   // 已呼叫 replay_record_begin，等 engine_load 載入遊戲後開始錄
};

// ---- 平台用 ----

// 開始錄影：在 engine_set_seed() 之後、engine_load() 之前呼叫。成功回傳 0。
// 載入成功後才真的開始錄（要先知道遊戲指紋）；載入失敗時呼叫 replay_stop()
int replay_record_begin(void);

// 到目前為止的重播檔。錄影會繼續，之後可以再取一次（更長的版本）。
// 回傳的記憶體屬於引擎，下一次 engine_frame() 之前有效
const uint8_t *replay_snapshot(void);
size_t replay_snapshot_size(void);

// 準備播放：在 engine_load() 之前呼叫（引擎要先知道種子）。
// 資料會被複製一份，呼叫完可以馬上釋放。成功回傳 0，格式不對回傳 1
int replay_play_begin(const uint8_t *data, size_t len);

// 停止錄影或播放
void replay_stop(void);

int replay_state(void);
uint32_t replay_frames(void);   // 播放中：總共幾幀

// 播完後：最後一幀的畫面和錄影時一樣回傳 1，不一樣回傳 0
int replay_matched(void);

// ---- 引擎內部用（engine.c 呼叫）----

// engine_load() 時呼叫：播放中就比對遊戲指紋，不是同一個遊戲回傳 1
int replay_on_load(const uint8_t cart_sha[32]);
// 每一幀執行遊戲之前：播放中套用這一幀的輸入；錄影中記下這一幀的輸入
void replay_before_frame(uint32_t frame);
// 每一幀執行完之後：檢查是否播完
void replay_after_frame(uint32_t frames_done);

// 遊戲讀存檔時（save.c 呼叫）。len = -1 代表沒有存檔
// 錄影中：記下讀到的內容
void replay_record_save(uint32_t frame, int slot, const uint8_t *data, int len);
// 播放中：從重播檔取出這次讀到的內容，回傳長度（-1 = 沒有存檔）。
// 重播檔裡下一個事件不是「這一幀、這個欄位的讀存檔」，代表遊戲跑得和錄影時不一樣：
// 停止播放，回傳 -2
int replay_play_save(uint32_t frame, int slot, uint8_t *buf, int cap);

#endif

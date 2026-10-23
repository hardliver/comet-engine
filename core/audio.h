// audio.h — 音效與音樂：引擎自己合成聲音（像 PICO-8、Game Boy 的音效晶片）
//
// 遊戲這邊（Lua）：
//   sfx(名字, 聲道)    播一個音效（comet.json 的 sfx 檔裡的名字）。聲道 0–3 可省略：自動找空的。
//                      回傳用了哪個聲道。sfx(nil, 聲道) 停掉那個聲道；sfx(nil) 全部停掉
//   music(名字)        播一首音樂（一段一段的音效同時播、接著播、循環）；music() 停掉
//
// 一個「音效」最多 32 個音符，每個音符有：
//   音高 0–63（0 = C2，每 1 = 半音，12 = 高八度）、波形 0–4、音量 0–7（0 = 休止）、效果 0–3
//   波形：0 三角波（柔和）、1 方波（GB 的感覺）、2 窄方波（比較尖）、3 鋸齒波（比較粗）、4 雜訊（打擊、爆炸）
//   效果：0 沒有、1 滑音（從上一個音滑過來）、2 抖音（音高微微上下）、3 漸弱（這個音越來越小聲）
//   speed：一個音符多長，單位 1/120 秒（speed 4 = 一個音 1/30 秒）
//   loop：[開始, 結束]（音符的位置，從 0 數）：播到結束就跳回開始，一直重複（例如引擎聲）
//
// 為什麼在 C 核心合成、而不是用瀏覽器的聲音功能：
//   三個平台（Mac、網頁、主機）的聲音一模一樣；主機（Pico）上沒有瀏覽器，本來就要自己合成。
//
// 決定性：每一幀（engine_frame）剛好產生 1/60 秒的聲音（22050 ÷ 60 = 367.5 個取樣，所以一幀 367、一幀 368），
// 聲音完全由「第幾幀、遊戲叫了哪些 sfx」決定。平台只是把這些取樣送到喇叭（網頁：AudioWorklet；主機：PWM），
// Mac 版可以存成 WAV，自動測試比對 Mac 和網頁算出來的聲音逐 byte 相同。
//
// 記憶體（為主機設想）：4 個聲道各自複製一份正在播的音效（每份約 130 bytes），
// 音樂開始時把用到的音效複製進 AUDIO_SONG_SFX 格的池子（約 4 KB）。音效資料在遊戲檔裡是精簡的字串。

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define AUDIO_RATE        22050   // 每秒幾個取樣
#define AUDIO_CHANNELS    4
#define AUDIO_FRAME_MAX   368     // 一幀最多幾個取樣
#define SFX_NOTES         32      // 一個音效最多幾個音符
#define AUDIO_SONG_SFX    32      // 一首音樂最多用到幾個不同的音效
#define AUDIO_SONG_PATS   64      // 一首音樂最多幾段

struct lua_State;

// engine_load() 時呼叫：全部停掉
void audio_reset(void);

// engine_frame() 最後呼叫：算出這一幀的聲音
void audio_frame(uint32_t frame);

// 平台用：這一幀的聲音（16 位元有號、單聲道、22050 Hz），每次 engine_frame() 之後讀
const int16_t *audio_samples(void);
int audio_sample_count(void);

// 把 sfx、music 註冊成 Lua 的全域函式（script.c 建立 Lua 環境時呼叫）
void audio_open_lua(struct lua_State *L);

#endif

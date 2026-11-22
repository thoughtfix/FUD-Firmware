// FUD firmware - Find Unwelcome Devices. Passive surveillance-awareness canary.
// Copyright (C) 2026 thoughtfix. Licensed under the GNU General Public License
// v3.0 (see LICENSE). Contains a subset of SquachWatch-CYD's GPL-3.0 signature
// data; see README attributions.
// Acked items leave the log. Acked hostile goes quiet (silent re-show every
// ~7 min). Trackers get their own section. Hold BACK at home dismisses like a
// phone notification. State-driven idle personality (mood ideas credited to
// Pwnagotchi + M5PORKCHOP; our faces, our code).
// Nav: MAIN short=advance, MAIN hold=enter/confirm, SIDE=back, SIDE-hold@home
// =dismiss, double-tap=ack.

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include "board.h"
#include "gps.h"
// Board profile (pins, rotation, IR), chosen at boot. Stick values until setup() runs.
static const Board* B=&BOARD_STICKS3;
// Onboard IR LED, created in setup() on the board's pin (StickS3 46, Cardputer ADV 44).
// nullptr on a board without a known IR pin: TV-B-Gone then sends nothing.
IRsend* irTx=nullptr;

#define FUD_VERSION "v0.50b"
#include "facevec.h"   // vector Zuse, backported from FUD-Deck (thoughtfix, GPL-3.0)

static const uint16_t AMBER  = 0xFFE0;
static const uint16_t LK_RED = 0xF800;
static const uint16_t BG     = 0x0000;
static uint8_t ROT_PORTRAIT  = 0;   // set from the board in setup()
static uint8_t ROT_LANDSCAPE = 1;

enum Cat { CAT_NONE=0, CAT_TRACKER, CAT_CAMERA, CAT_DRONE, CAT_SUBGHZ, CAT_HACKDEV, CAT_ATTACK };
const char* catTag[]  = { "clr","TRK","CAM","DRN","SUB","HAK","ATK" };
#include "signatures.h"
#include "user_ignore.h"

static const uint32_t EVICT_MS      = 5*60*1000;
static const uint32_t EVICT_TRK_MS  = 30*60*1000;
static const uint32_t TRK_FOLLOW_MS = 10*60*1000;
static const uint32_t DISP_MS       = 4000;
static const uint32_t ALARM_BEEP_MS = 15000;
static const uint32_t ALARM_STANDDOWN_MS = 60000;
static const uint32_t RESHOW_MS     = 7*60*1000;   // silent acked-attack reminder
static const uint32_t MOOD_PERIOD   = 20000;
static const uint32_t MOOD_SHOW     = 2400;

struct Det {
  bool used=false; uint8_t key[6]={0}; Cat cat=CAT_NONE;
  char label[16]={0}; char name[32]={0};
  uint32_t firstSeen=0,lastSeen=0; uint16_t hits=0; int8_t rssi=0; uint16_t place=0;   // place = Wi-Fi area when first seen (following needs a move)
  char conf='M';   // signature confidence: 'H' high, 'M' medium, 'L' low
  bool acked=false, alarm=false, ignored=false, escalated=false, ssidBased=false, active=false;
};
static const int MAXDET=128;
Det dets[MAXDET];
uint32_t cntCam=0,cntHak=0,cntAtk=0,cntTrk=0,cntDrn=0,cntSub=0;
bool ccPresent=false, nrfPresent=false;   // RF Pack radios detected at boot
volatile char activeRadio='-';            // B=BLE W=WiFi S=subGHz N=nRF24  (one at a time)
uint32_t lastDetection=0;
// Where am I, without GPS: the strongest nearby access points. When most of them change for two
// Wi-Fi scans running, you have moved and the place number bumps. A tracker only counts as
// following once it was first seen before a move and is still here after. RAM only, no storage.
volatile uint16_t placeNow=0; volatile bool placeKnown=false;
bool muted=false;   // MUTE: ignore everything (silent + calm), keep logging
// SILENT (development rule 2026-11-19): ON by default. ON = every scan is passive, nothing is
// transmitted (BLE ads only, Wi-Fi beacons only). OFF lets detection modes use active scans.
// Saved in NVS. Settings > Silent; turning it OFF shows the warning screen first (setWarn=1).
// Only Wi-Fi uses it today: BLE scanning stays passive either way.
bool silentMode=true;
int setWarn=0;   // Settings warning screen up: 1 = Silent OFF?, 2 = Wardrive ON?
static int wifiScan(){ return silentMode ? WiFi.scanNetworks(false,true,true,120)   // passive: beacons only
                                         : WiFi.scanNetworks(false,true); }        // active: probe requests

struct Evt { uint8_t key[6]; Cat cat; char label[16]; char name[32]; int8_t rssi; bool ssid; bool active; char conf; };
QueueHandle_t evtQ;

enum Mode { MODE_IDLE, MODE_STATUS, MODE_ZOOM, MODE_LIST, MODE_ITEM, MODE_ATTACK, MODE_WATERFALL, MODE_SETTINGS,
            MODE_TOYS, MODE_SSID, MODE_TRAFFIC, MODE_TVBG, MODE_FINDER, MODE_GPS };
Mode mode=MODE_IDLE;
// Radio toys run from the UI loop and use the radios directly (Wi-Fi scan, CC1101/nRF24 on SPI).
static bool toyMode(){ return mode==MODE_WATERFALL||mode==MODE_SSID||mode==MODE_TRAFFIC; }
// One owner at a time for Wi-Fi scanning and the SPI bus (radios + SD). Two Wi-Fi scans at once
// double-free the scan list (crashed the Stick: SSID Scan opened mid Wi-Fi phase). The scan
// task takes it per phase and rechecks toyMode() inside; the UI only tries (never blocks) and
// skips a frame when it is busy.
static SemaphoreHandle_t radioMtx=nullptr;
struct RadioLock { bool ok; explicit RadioLock(TickType_t w){ ok=radioMtx&&xSemaphoreTake(radioMtx,w)==pdTRUE; } ~RadioLock(){ if(ok) xSemaphoreGive(radioMtx); } };
const char* dispFace=nullptr; uint32_t dispUntil=0;   // transient face for log-tier hits
int  listSel=0,listTop=0,itemAct=0,curDet=-1,attackShow=-1;
int  statusSel=0, zoomCat=CAT_TRACKER, setSel=0, logFilter=-1, toysSel=0, ssidSel=0, ssidTop=0; // -1 = all
struct SsidRow{ char ssid[24]; int8_t rssi; uint8_t ch; bool open; };
SsidRow ssidRows[24]; int ssidN=0; uint32_t ssidLast=0;
// TV-B-Gone blaster state (non-blocking: one code per loop tick)
bool tvbgFiring=false; int tvbgIdx=0; uint32_t tvbgLast=0;
// Traffic graph
bool trafReady=false; int trafNow=0, trafPeak=0;
// Tracker Finder. You pick a target from the recent-detections list, then lock
// onto it and walk toward it by RSSI. (written by scan task, read by UI:
// display-only race is fine)
volatile bool finderHas=false; volatile int8_t finderRssi=-100; volatile Cat finderCat=CAT_NONE;
char finderLabel[16]={0}, finderName[32]={0};
bool finderLocked=false; uint8_t finderTarget[6]={0}; int finderSel=0, finderTop=0;
int finderSmooth=-100, finderBaseline=-100; uint32_t finderSeen=0;
bool textZoom=false;                                   // detail text-zoom overlay
uint32_t lastAlarmBeep=0,lastReshow=0;
const char* moodFace=nullptr; uint32_t moodUntil=0,moodNext=8000;
bool uiDirty=true;

// display power management
static const uint8_t  BRIGHT_LEVELS[]={180,120,70,35};   // HIGH..DIM
static const char*    BRIGHT_NAMES[]={"HIGH","MED","LOW","DIM"};
int brightIdx=0;
static const uint8_t  BRIGHT_NORMAL=BRIGHT_LEVELS[0];    // default = HIGH (first boot; then NVS "bright")
static const uint32_t SCREEN_TIMEOUT=15000;    // idle scanning: sleep quickly
static const uint32_t INTERACT_TIMEOUT=180000; // in a menu / watching a waterfall: 3 min
static const uint32_t ALERT_PEEK=5000;         // a threat lights the screen this long
// Settings > Shake: how hard a handle must be to wake the screen (or keep it on). The measure is
// the summed per-axis change in g between two loop samples (5 ms apart awake, 25 ms asleep).
// Hi wakes on a gentle pick-up; Lo wants a deliberate shake, so walking with it stays dark.
static const float    SHAKE_LEVELS[]={1.4f,0.8f,0.4f};   // Lo, Med, Hi
static const char*    SHAKE_NAMES[]={"Lo","Med","Hi"};
static const int      SHAKE_N=3;
int shakeIdx=1;                                 // NVS "shake", default Med
// Screen lock (Cardputer L, Stick both buttons 1 s): the screen stays off. Motion and double-tap
// do nothing. Threat alerts still light it.
// Any key or button unlocks (and does nothing else). Not saved: a reboot always starts unlocked.
bool screenLock=false;
bool displayOn=true; uint32_t lastWake=0, wakeHold=SCREEN_TIMEOUT;
float prevAx=0,prevAy=0,prevAz=1;
uint8_t userBright=BRIGHT_NORMAL;   // runtime brightness (saved as NVS "bright")

// sound volume: a short ladder scaled against the built-in earcon levels. MED is
// the current feel; two steps louder, two quieter, then MUTE. Persisted.
static const uint8_t VOL_PCT[]  ={150,120,100,65,35,0};
static const char*   VOL_NAMES[]={"MAX","LOUD","MED","LOW","MIN","MUTE"};
static const int VOL_N=6; int volIdx=2;   // default MED
static uint8_t applyVol(uint16_t base){ uint16_t v=(uint16_t)(base*VOL_PCT[volIdx]/100); return v>255?255:(uint8_t)v; }

// alert filter: minimum signature confidence that may take over the screen /
// make sound. Below it, still counted and logged, just quiet. 0=all.
static const char* AF_NAMES[]={"ALL","MED","HIGH"}; static const int AF_N=3; int alertMinConf=0;
static int confRank(char c){ return c=='H'?2:(c=='M'?1:0); }

// mode-change confirmation toast
const char* toastMsg=nullptr; uint32_t toastUntil=0;

// waterfall: source 0 = 2.4 GHz (nRF24), 1 = sub-GHz (CC1101)
M5Canvas wfc(&M5.Display); bool wfReady=false; uint8_t wfrow[126]; int wfSource=0;

// persistent ignore lists (survive reboot): BLE/device keys and WiFi SSIDs
Preferences prefs;
static const int MAXIGN=32;
uint8_t ignoreKeys[MAXIGN][6]; int ignoreN=0;
static const int MAXIGNSS=16;
char ignSSID[MAXIGNSS][33]; int ignSSIDN=0;

// ================= helpers =================
// Zuse at (x,y) in a size-square. Vector only: every face in the set is a vector.
// Works on the panel or a sprite: both satisfy facevec's fillRect/startWrite contract.
// Unknown names draw defaultsmile (facevec::draw handles that). angle turns the features
// (wheel spin); sweep lays a radar beam (degrees, < 0 = none). Both default to off.
template<class G> static void drawZuse(G& g,const char* n,int x,int y,int size,uint16_t bg,float angle=0,float sweep=-1){
  facevec::draw(g,n,x,y,size,(int32_t)bg,angle,sweep);
}
// Home idle wheel/radar state: features turned (angle) or a radar beam (sweep) over the face.
// 0 angle and -1 sweep = none. Driven by the wheel scheduler, read by drawIdle's face draw.
static float g_faceAngle=0, g_faceSweep=-1;
// Serious (active) threats keep their persistent alert even when MUTED, so a
// hacker device or a live deauth is still on screen when you pick the pendant
// up. Mute only silences the sound; passive stuff (evil twin, cameras) just
// logs. So alarmActive() is NOT gated by mute.
static bool alarmActive(){ for(auto&d:dets) if(d.used&&d.alarm&&!d.acked) return true; return false; }
static int  usedCount(){ int c=0; for(auto&d:dets) if(d.used) c++; return c; }
static int  activeAttackIdx(){ int b=-1; uint32_t t=0; for(int i=0;i<MAXDET;i++){ Det&d=dets[i]; if(d.used&&d.alarm&&!d.acked&&d.lastSeen>=t){t=d.lastSeen;b=i;} } return b; }
static const char* alarmFace(){ return "redface"; }  // serious threat face (vector). Hacker device vs other attack share it now that the PNG trio is gone.
// visible log = active threats (non-tracker, not acked, not ignored)
// The log. logFilter<0 = all active threats (acked hidden); logFilter==a Cat =
// every entry of that category (for the ZOOM drill-down, incl trackers).
static void logIdxs(int& n,int idxs[]){ n=0; for(int i=0;i<MAXDET;i++){ Det&d=dets[i]; if(!d.used||d.ignored) continue;
    if(logFilter>=0){ if(d.cat!=logFilter) continue; } else { if(d.cat==CAT_TRACKER||d.acked) continue; }
    idxs[n++]=i; }
  // serious first, then newest arrivals on top; chronic noise sinks
  for(int a=1;a<n;a++){ int v=idxs[a]; int b=a-1;
    while(b>=0 && ( (dets[idxs[b]].active<dets[v].active) ||
        (dets[idxs[b]].active==dets[v].active && dets[idxs[b]].firstSeen<dets[v].firstSeen) )){ idxs[b+1]=idxs[b]; b--; }
    idxs[b+1]=v; } }
// Live count of a category: exactly what that category's list shows (used, not ignored), so a
// tile or count can never point at something you cannot open. cnt* stay as the since-boot tally.
static int  liveCount(Cat c){ int n=0; for(auto&d:dets) if(d.used&&!d.ignored&&d.cat==c) n++; return n; }
static bool ackedAttackPresent(){ uint32_t now=millis(); for(auto&d:dets) if(d.used&&d.active&&d.acked&&(now-d.lastSeen)<ALARM_STANDDOWN_MS) return true; return false; }

static Det* slotFor(const uint8_t key[6]){
  for(auto&d:dets) if(d.used&&!memcmp(d.key,key,6)) return &d;
  Det* old=nullptr; for(auto&d:dets){ if(!d.used){ d=Det(); return &d; } if(!old||d.lastSeen<old->lastSeen) old=&d; } *old=Det(); return old;
}

// ---- persistent ignore list (NVS) ----
static bool isIgnored(const uint8_t key[6]){
  for(int i=0;i<ignoreN;i++) if(!memcmp(ignoreKeys[i],key,6)) return true;
  for(int i=0;i<USER_IGNORE_MACS_N;i++) if(!memcmp(USER_IGNORE_MACS[i],key,6)) return true; // compiled-in
  return false;
}
static bool isIgnoredBleName(const char* nm){ if(!nm||!nm[0]) return false; for(int i=0;i<USER_IGNORE_BLE_NAMES_N;i++) if(strstr(nm,USER_IGNORE_BLE_NAMES[i])) return true; return false; }
static void addIgnore(const uint8_t key[6]){ if(isIgnored(key)||ignoreN>=MAXIGN) return; memcpy(ignoreKeys[ignoreN++],key,6); prefs.putBytes("ign",ignoreKeys,ignoreN*6); }
static bool isIgnoredSSID(const char* s){ if(!s||!s[0]) return false;
  for(int i=0;i<ignSSIDN;i++) if(!strcmp(ignSSID[i],s)) return true;
  for(int i=0;i<USER_IGNORE_SSIDS_N;i++) if(!strcmp(USER_IGNORE_SSIDS[i],s)) return true; // compiled-in
  return false; }
static void addIgnoreSSID(const char* s){ if(!s||!s[0]||isIgnoredSSID(s)||ignSSIDN>=MAXIGNSS) return; strncpy(ignSSID[ignSSIDN],s,32); ignSSID[ignSSIDN][32]=0; ignSSIDN++; prefs.putBytes("ignss",ignSSID,ignSSIDN*33); }
static void loadIgnore(){
  if(prefs.isKey("ign")){ size_t n=prefs.getBytesLength("ign"); if(n>sizeof(ignoreKeys)) n=sizeof(ignoreKeys); if(n){ prefs.getBytes("ign",ignoreKeys,n); ignoreN=n/6; } }
  if(prefs.isKey("ignss")){ size_t n=prefs.getBytesLength("ignss"); if(n>sizeof(ignSSID)) n=sizeof(ignSSID); if(n){ prefs.getBytes("ignss",ignSSID,n); ignSSIDN=n/33; } }
}

// ---- display power ----
static void wake(uint32_t hold=SCREEN_TIMEOUT){ if(!displayOn){ M5.Display.setBrightness(userBright); displayOn=true; uiDirty=true; } lastWake=millis(); wakeHold=hold; }
static uint32_t holdForMode(){ return mode==MODE_IDLE ? SCREEN_TIMEOUT : INTERACT_TIMEOUT; } // idle sleeps fast; interacting stays on
static void toast(const char* m){ toastMsg=m; toastUntil=millis()+1300; uiDirty=true; }
static void goHome();
static void drawLockMsg();
static uint32_t lockAt=0, lockMsgUntil=0;          // lockMsgUntil: the notice is up, backlight off after it
static const uint32_t LOCK_DEBOUNCE=800, LOCK_MSG_MS=1500;
static bool lockDebounced(){ return millis()-lockAt>=LOCK_DEBOUNCE; }   // presses this soon after locking are ignored
static void setLock(bool on){
  screenLock=on;
  if(on){ tvbgFiring=false; finderLocked=false; goHome(); M5.Speaker.tone(700,60);   // from any screen
    lockAt=millis(); lockMsgUntil=lockAt+LOCK_MSG_MS; wake(LOCK_MSG_MS+1000); drawLockMsg(); }
  else { lockMsgUntil=0; M5.Speaker.tone(1500,60); wake(SCREEN_TIMEOUT); uiDirty=true; }
}

// ================= Zuse's voice: 8-bit Derezzed earcons =================
// Monophonic sawtooth riffs reconstructed as (freq,ms), chopped to short
// fragments and played non-blocking via M5.Speaker. Mapped to events.
struct Note { uint16_t f, ms; };
#define NELEM(a) ((int)(sizeof(a)/sizeof((a)[0])))
static const Note RIFF_BOOT[]   = { {311,375},{370,375},{311,125},{622,125},{466,125},{370,125},{277,125},{208,125},{139,125},{156,125},{185,125},{233,125} }; // the phrase
static const Note RIFF_DETECT[] = { {740,31},{370,31},{311,31},{277,31},{233,32},{185,31} };                 // quick zip
static const Note RIFF_BASE[]   = { {466,125},{622,125},{698,125},{740,125} };                                // rising = clean
static const Note RIFF_ATTACK[] = { {932,125},{740,125},{554,125},{415,125},{466,125} };                      // high alarm
static const Note RIFF_ERROR[]  = { {156,375} };                                                              // one low note
const Note* g_riff=nullptr; int g_riffN=0,g_riffI=0,g_riffReps=1; uint32_t g_riffUntil=0; uint8_t g_riffVol=110;
static void playRiff(const Note* r,int n,uint8_t vol=110,int reps=1){ if(muted) return; g_riff=r; g_riffN=n; g_riffI=0; g_riffUntil=0; g_riffVol=vol; g_riffReps=reps; }

// ---- full song (easter egg): all bars + a trimmed arrangement ----
static const Note BAR1[]={{233,125},{185,125},{185,125},{233,125},{185,125},{185,125},{233,125},{185,125},{185,125},{233,125},{185,125},{185,125},{233,125},{185,125},{185,125},{233,125}};
static const Note BAR2[]={{311,125},{311,63},{311,62},{0,125},{311,125},{370,250},{311,125},{370,250},{311,125},{370,31},{740,31},{370,32},{740,31},{370,31},{740,32},{622,31},{370,31},{311,31},{277,31},{233,32},{185,31},{156,31},{156,32},{156,31},{156,31},{156,31},{156,32},{156,31},{156,31},{0,125}};
static const Note BAR3[]={{311,125},{311,63},{311,62},{311,62},{311,63},{370,125},{311,125},{311,63},{311,62},{311,63},{311,62},{370,125},{370,62},{370,63},{370,63},{370,62},{311,125},{311,63},{311,62},{370,125},{370,63},{370,62},{370,63},{370,62},{370,63},{370,62}};
static const Note BAR4[]={{156,125},{156,63},{156,62},{156,62},{156,63},{156,63},{0,62},{233,125},{233,63},{233,62},{466,125},{622,125},{698,125},{740,125},{0,250},{554,125},{523,125},{494,125},{466,125}};
static const Note BAR5[]={{622,125},{311,125},{311,125},{622,125},{311,125},{311,125},{622,125},{311,125},{932,125},{740,125},{554,125},{415,125},{466,125},{554,125},{740,125},{831,125}};
static const Note BAR6[]={{311,125},{311,63},{311,62},{311,62},{311,63},{370,63},{370,62},{370,62},{370,63},{370,63},{370,62},{370,63},{370,62},{370,63},{370,62},{932,125},{740,125},{554,125},{415,125},{466,125},{554,125},{622,63},{622,62},{622,63},{622,62}};
static const Note BAR7[]={{311,63},{622,62},{0,250},{370,375},{311,125},{622,125},{466,125},{370,125},{277,125},{208,125},{139,125},{156,125},{185,125},{233,125}};
static const Note BAR8[]={{932,125},{740,125},{554,125},{415,125},{466,125},{622,63},{0,62},{622,63},{0,62},{622,63},{0,62},{466,125},{370,125},{277,125},{208,125},{233,125},{277,125},{370,125},{466,125}};
static const Note BAR9[]={{932,125},{740,125},{554,125},{415,125},{466,125},{554,125},{415,125},{466,125},{311,375},{370,375},{311,125},{622,125}};
static const Note BAR10[]={{466,125},{370,125},{277,125},{208,125},{139,125},{156,125},{185,125},{233,125},{311,375},{370,375},{311,125},{622,125}};
static const Note BAR11[]={{466,125},{370,125},{277,125},{208,125},{139,125},{156,125},{185,125},{233,125},{932,125},{740,125},{554,125},{415,125},{466,125},{622,63},{0,62},{622,63},{0,62},{622,63},{0,62}};
static const Note BAR12[]={{466,125},{370,125},{277,125},{208,125},{233,125},{277,125},{370,125},{466,125},{932,125},{740,125},{554,125},{415,125},{466,125},{554,125},{415,125},{466,125}};
static const Note BAR13[]={{156,375}};
static const Note* BARS_TBL[]={RIFF_BOOT,BAR1,BAR2,BAR3,BAR4,BAR5,BAR6,BAR7,BAR8,BAR9,BAR10,BAR11,BAR12,BAR13};
static const int BARLEN[]={NELEM(RIFF_BOOT),NELEM(BAR1),NELEM(BAR2),NELEM(BAR3),NELEM(BAR4),NELEM(BAR5),NELEM(BAR6),NELEM(BAR7),NELEM(BAR8),NELEM(BAR9),NELEM(BAR10),NELEM(BAR11),NELEM(BAR12),NELEM(BAR13)};
struct SongStep{uint8_t bar,reps;};
static const SongStep SONG[]={{0,3},{1,1},{0,1},{2,1},{3,1},{4,1},{5,2},{6,1},{7,1},{0,1},{8,1},{9,1},{10,2},{11,1},{12,1},{13,1}}; // trimmed repeats
bool g_songOn=false; int g_songStep=0,g_songRep=0; uint32_t g_songStart=0;
static void songLoadBar(int bar){ g_riff=BARS_TBL[bar]; g_riffN=BARLEN[bar]; g_riffI=0; g_riffUntil=millis(); }
static void songAdvance(){ g_songRep++;
  if(g_songRep>=SONG[g_songStep].reps){ g_songStep++; g_songRep=0; if(g_songStep>=NELEM(SONG)){ g_songOn=false; g_riff=nullptr; M5.Speaker.setVolume(applyVol(80)); return; } }
  songLoadBar(SONG[g_songStep].bar); }
static void playSong(){ g_songOn=true; g_songStart=millis(); g_songStep=0; g_songRep=0; g_riffVol=130; songLoadBar(SONG[0].bar); toast("PLAYING"); uiDirty=true; } // explicit: plays even when muted
static void stopSong(){ if(g_songOn){ g_songOn=false; g_riff=nullptr; M5.Speaker.setVolume(applyVol(80)); toast("STOPPED"); uiDirty=true; } }

static void serviceRiff(){ if(!g_riff) return; uint32_t now=millis();
  if(now>=g_riffUntil){ if(g_riffI>=g_riffN){
      if(g_songOn){ songAdvance(); return; }
      if(--g_riffReps>0){ g_riffI=0; g_riffUntil=now; return; }   // repeat the riff
      g_riff=nullptr; M5.Speaker.setVolume(applyVol(80)); return; }
    Note nt=g_riff[g_riffI++]; if(g_riffI==1) M5.Speaker.setVolume(applyVol(g_riffVol));
    if(nt.f) M5.Speaker.tone(nt.f,nt.ms); g_riffUntil=now+nt.ms; } }

// ================= ingest =================
static void alarmSound();   // fwd decl (defined below, used here)
namespace sdlog { static void note(const Det& d); }   // sdlog.h, included after rfspi
static void ingest(const Evt& e){
  Det* d=slotFor(e.key); uint32_t now=millis(); bool fresh=!d->used;
  if(fresh){ d->used=true; memcpy(d->key,e.key,6); d->cat=e.cat; d->ssidBased=e.ssid; d->active=e.active; d->conf=e.conf;
    strncpy(d->label,e.label,sizeof(d->label)-1); strncpy(d->name,e.name,sizeof(d->name)-1);
    d->firstSeen=now; d->hits=0; d->place=placeNow;
    d->ignored = isIgnored(e.key) || (e.ssid && isIgnoredSSID(e.name)); }  // persistent whitelist
  d->lastSeen=now; d->rssi=e.rssi; d->hits++;
  if(e.name[0]){ d->name[0]=0; strncpy(d->name,e.name,sizeof(d->name)-1); } // keep latest (frame count, src)
  if(d->ignored) return;
  if(fresh){
    lastDetection=now; sdlog::note(*d);
    if(e.cat==CAT_ATTACK) cntAtk++; else if(e.cat==CAT_CAMERA) cntCam++;
    else if(e.cat==CAT_HACKDEV) cntHak++; else if(e.cat==CAT_TRACKER) cntTrk++;
    else if(e.cat==CAT_DRONE) cntDrn++; else if(e.cat==CAT_SUBGHZ) cntSub++;
    bool pass = confRank(e.conf) >= alertMinConf;   // below the filter: log only
    if(e.active && pass){
      // SERIOUS: hacker device present, or a live attack (deauth). Persistent
      // alert that survives mute visually; only the SOUND is muted.
      d->alarm=true; d->acked=false;
      if(!muted){ alarmSound(); lastAlarmBeep=now; wake(ALERT_PEEK); }
    } else if(!e.active && pass && e.cat!=CAT_TRACKER){
      // PASSIVE / log tier: evil twin, cameras. One beep + brief face, or, if
      // muted, just log silently.
      if(!muted){
        playRiff(RIFF_DETECT,NELEM(RIFF_DETECT),110);   // Zuse: "Bio-digital jazz" (riff 3)
        dispFace = (e.cat==CAT_CAMERA)?"swirly-face":(e.cat==CAT_DRONE)?"shockedface":(e.cat==CAT_SUBGHZ)?"tongueface":"redface";
        dispUntil=now+DISP_MS; wake(ALERT_PEEK);
      }
    }
    uiDirty=true;
  }
  // Following: with you a while AND still here after you moved (or Wi-Fi place is unknown, so
  // we fall back to time alone). Sitting at home near the neighbours' AirTags is not following.
  if(d->cat==CAT_TRACKER && !d->escalated && (now-d->firstSeen)>TRK_FOLLOW_MS && (!placeKnown || d->place!=placeNow)){
    d->escalated=true; if(!muted){ playRiff(RIFF_DETECT,NELEM(RIFF_DETECT),110); dispFace="neutralface"; dispUntil=now+DISP_MS; wake(ALERT_PEEK); } uiDirty=true;
  }
}
static void housekeep(){
  uint32_t now=millis();
  for(auto&d:dets){ if(!d.used) continue;
    if(d.alarm&&!d.acked&&(now-d.lastSeen)>ALARM_STANDDOWN_MS){ d.alarm=false; uiDirty=true; }
    uint32_t age=now-d.lastSeen, lim=(d.cat==CAT_TRACKER)?EVICT_TRK_MS:EVICT_MS;
    if(age>lim && !d.alarm){ d=Det(); uiDirty=true; }
  }
}
// Urgent, unmistakable attack alarm: boost volume, play a triple two-tone
// whoop, then restore UI volume shortly after (restore handled in loop()).
uint32_t restoreVolAt=0;
static void alarmSound(){ playRiff(RIFF_ATTACK, NELEM(RIFF_ATTACK), 168); } // Zuse: attack phrase (riff 6)
static void ackAll(bool sound){ bool any=false; for(auto&d:dets) if(d.used&&d.alarm){ d.acked=true; d.alarm=false; any=true; } if(any){ if(sound) M5.Speaker.tone(1200,60); uiDirty=true; } }

// ================= scan task =================
static void enq(const uint8_t key[6],Cat c,const char* label,const char* name,int8_t rssi,bool ssid=false,bool active=false,char conf='M'){
  Evt e; memcpy(e.key,key,6); e.cat=c; e.rssi=rssi; e.ssid=ssid; e.active=active; e.conf=conf;
  strncpy(e.label,label,sizeof(e.label)-1); e.label[sizeof(e.label)-1]=0;
  strncpy(e.name,name?name:"",sizeof(e.name)-1); e.name[sizeof(e.name)-1]=0;
  xQueueSend(evtQ,&e,0);
}
// ---- BLE matching. One matcher, used by logging (classifyBle) and the Finder (classifyBleLite),
// so the two can never disagree. Names come from the advert only: we scan passively, so a name
// that only arrives in a scan response is never seen. Name matches are graded down: a string
// anybody can type, not a registered identifier.
// Signature values and the name rules are credited to SquachWatch-CYD (GPL-3.0) in signatures.h.
static bool hasSvc(const NimBLEAdvertisedDevice* dev,uint16_t u){   // service list OR service data
  NimBLEUUID id(u); if(dev->isAdvertisingService(id)) return true;
  for(int i=0;i<(int)dev->getServiceDataCount();i++) if(dev->getServiceDataUUID(i)==id) return true;
  return false; }
static bool isFmdn(const NimBLEAdvertisedDevice* dev){              // Google Find Hub frame 0x40/0x41
  NimBLEUUID id((uint16_t)FMDN_SVC);
  for(int i=0;i<(int)dev->getServiceDataCount();i++) if(dev->getServiceDataUUID(i)==id){
    std::string d=dev->getServiceData(i); if(d.size()>=1 && ((uint8_t)d[0]==0x40||(uint8_t)d[0]==0x41)) return true; }
  return false; }
static bool flockFsName(const char* n){                             // "FS-" + 4..12 hex: a Flock accessory
  if(strncmp(n,"FS-",3)) return false; int k=0;
  for(const char* p=n+3;*p;p++,k++) if(!isxdigit((unsigned char)*p)) return false;
  return k>=4 && k<=12; }
static bool bareSerial10(const char* n){                            // a Penguin battery's bare 10-digit serial
  for(int i=0;i<10;i++) if(n[i]<'0'||n[i]>'9') return false; return n[10]==0; }
struct BleHit { Cat cat; const char* label; char conf; bool active; };
static bool bleMatch(const NimBLEAdvertisedDevice* dev, BleHit& h){
  std::string nm=dev->getName(); std::string md=dev->getManufacturerData();
  uint16_t company= md.size()>=2 ? ((uint8_t)md[0]|((uint8_t)md[1]<<8)) : 0xFFFF; const char* name=nm.c_str();
  if(isIgnoredBleName(name)) return false;                          // user's own gear
  auto hit=[&](Cat c,const char* l,char cf,bool a){ h.cat=c; h.label=l; h.conf=cf; h.active=a; return true; };
  // Hacker devices (active): Flipper by its fixed service UUIDs, then by name
  for(int i=0;i<FLIPPER_SVC_N;i++) if(hasSvc(dev,FLIPPER_SVC0+i)) return hit(CAT_HACKDEV,"Flipper",'H',true);
  if(nm.find("Flipper")!=std::string::npos) return hit(CAT_HACKDEV,"Flipper",'M',true);
  // Meta/Ray-Ban glasses: COMPOSITE (Meta or Luxottica CID + Meta svc UUID together)
  if((company==META_CID||company==META_CID2) && hasSvc(dev,META_SVC)) return hit(CAT_CAMERA,"Meta glass",'H',false);
  // Apple Find My / AirTag: company 0x004C, offline-finding type byte 0x12
  if(company==APPLE_CID && md.size()>=3 && (uint8_t)md[2]==FINDMY_TYPE) return hit(CAT_TRACKER,"AirTag",'H',false);
  if(isFmdn(dev)) return hit(CAT_TRACKER,"GoogleFMD",'M',false);
  if(dev->isAdvertisingService(NimBLEUUID(FLOCK_UUID128))) return hit(CAT_CAMERA,"Flock",'H',false);
  // Table-driven single-signature matches
  for(int i=0;i<BLE_SIGS_N;i++){ const BleSig& s=BLE_SIGS[i];
    bool svcOk=(s.service==0xFFFF)||hasSvc(dev,s.service);
    bool cidOk=(s.company==0xFFFF)||(company==s.company);
    if(svcOk&&cidOk) return hit(s.cat,s.label,s.conf,false); }
  // Advertised names
  if(name[0]){
    if(strcasestr(name,"Flock")||strcasestr(name,"Penguin")||strcasestr(name,"Pigvision")||flockFsName(name))
      return hit(CAT_CAMERA,"Flock",'M',false);
    if(bareSerial10(name)) return hit(CAT_CAMERA,"Flock?",'L',false);
    if(strcasestr(name,"BodyWorn")) return hit(CAT_CAMERA,"BodyWorn",'M',false);   // Utility Inc body cam remote
    if(strcasestr(name,"Axon")) return hit(CAT_CAMERA,"Axon",'M',false);
    static const char* SKIM[]={"HC-03","HC-05","HC-06","RN42","BT04-A"};           // card skimmer modules
    for(auto sk:SKIM) if(!strcasecmp(name,sk)) return hit(CAT_HACKDEV,"Skimmer",'M',false);
  }
  return false;
}
static void classifyBle(const NimBLEAdvertisedDevice* dev){
  BleHit h; if(!bleMatch(dev,h)) return;
  uint8_t key[6]; memcpy(key,dev->getAddress().getBase()->val,6);
  enq(key,h.cat,h.label,dev->getName().c_str(),dev->getRSSI(),false,h.active,h.conf);
}
// Same matching as classifyBle, but returns the category/label instead of logging. Used by the
// Finder to lock onto a huntable device live.
static Cat classifyBleLite(const NimBLEAdvertisedDevice* dev, char* lbl, int lsz){
  BleHit h; if(!bleMatch(dev,h)) return CAT_NONE;
  strncpy(lbl,h.label,lsz-1); lbl[lsz-1]=0; return h.cat;
}
// Consistent 6-byte cache key from an SSID, so all BSSIDs of one network
// collapse to a single log entry (FNV-1a hash).
static void ssidKey(const char* s, uint8_t k[6]){ uint32_t h=2166136261u; for(const char*p=s;*p;p++){ h^=(uint8_t)*p; h*=16777619u; } k[0]=0xE7; k[1]=h; k[2]=h>>8; k[3]=h>>16; k[4]=h>>24; k[5]=(uint8_t)strlen(s); }
// Hash the strongest PLACE_N access points into a place fingerprint. When over three quarters
// of them have changed for two scans in a row, bump the place number (you moved). Fewer than 3
// APs = can't tell places apart, so placeKnown stays off and following falls back to time.
static void placeUpdate(int n){
  static const int PLACE_N=12;
  static uint32_t placeRef[PLACE_N]; static int placeRefN=0, placeLow=0;
  uint32_t cur[PLACE_N]; int8_t rs[PLACE_N]; int cn=0;
  for(int i=0;i<n;i++){ uint8_t* b=WiFi.BSSID(i); if(!b) continue;
    uint32_t h=2166136261u; for(int k=0;k<6;k++){ h^=b[k]; h*=16777619u; }
    int8_t r=WiFi.RSSI(i);
    if(cn<PLACE_N){ cur[cn]=h; rs[cn++]=r; continue; }
    int w=0; for(int k=1;k<cn;k++) if(rs[k]<rs[w]) w=k;   // replace the weakest kept AP
    if(r>rs[w]){ cur[w]=h; rs[w]=r; } }
  if(cn<3) return;                                         // too few APs to tell places apart
  if(!placeRefN){ memcpy(placeRef,cur,cn*sizeof(uint32_t)); placeRefN=cn; placeKnown=true; return; }
  int hit=0; for(int i=0;i<cn;i++) for(int k=0;k<placeRefN;k++) if(cur[i]==placeRef[k]){ hit++; break; }
  if(hit*4>cn){ placeLow=0; return; }                      // over a quarter still here: same place
  if(++placeLow<2) return;                                 // need two scans running to confirm
  placeLow=0; memcpy(placeRef,cur,cn*sizeof(uint32_t)); placeRefN=cn; placeNow++; placeKnown=true;
}
namespace wd { static void capture(const wifi_ap_record_t* r); }   // wardrive.h, included after rfspi
static void scanWifiOnce(){
  int n=wifiScan();
  for(int i=0;i<n;i++){ wd::capture((const wifi_ap_record_t*)WiFi.getScanInfoByIndex(i));   // wardriving (ON only): every network, before any filter
    String ss=WiFi.SSID(i); if(ss.length()==0||isIgnoredSSID(ss.c_str())) continue;  // SSID whitelist
    uint8_t* b=WiFi.BSSID(i); uint8_t key[6]; if(!b)continue; memcpy(key,b,6);
    // Classic evil twin: same SSID advertised both OPEN and secured (a clone
    // running open to sniff). Encryption-only differences (WPA2 vs WPA3) are
    // normal transitional config, not an attack, so we do not flag those.
    bool iOpen=WiFi.encryptionType(i)==WIFI_AUTH_OPEN, twin=false;
    for(int j=0;j<n;j++){ if(j!=i&&WiFi.SSID(j)==ss&&(WiFi.encryptionType(j)==WIFI_AUTH_OPEN)!=iOpen){twin=true;break;} }
    if(twin){ uint8_t tk[6]; ssidKey(ss.c_str(),tk); enq(tk,CAT_ATTACK,"evil twin",ss.c_str(),WiFi.RSSI(i),true,false,'M'); continue; } // one entry per SSID
    if(!strncasecmp(ss.c_str(),"pwned",5)){ enq(key,CAT_HACKDEV,"pwned",ss.c_str(),WiFi.RSSI(i),true,true,'H'); continue; }   // ESP deauther control AP (forks vary the suffix)
    if(!strcasecmp(ss.c_str(),"Flock")){ enq(key,CAT_CAMERA,"Flock",ss.c_str(),WiFi.RSSI(i),true,false,'M'); continue; }
    bool matched=false;
    for(int k=0;k<SSID_SIGS_N;k++){ if(!strncasecmp(ss.c_str(),SSID_SIGS[k].prefix,strlen(SSID_SIGS[k].prefix))){ enq(key,SSID_SIGS[k].cat,SSID_SIGS[k].label,ss.c_str(),WiFi.RSSI(i),true,SSID_SIGS[k].active,'M'); matched=true; break; } }
    if(!matched) for(int k=0;k<OUI_SIGS_N;k++){ if(!memcmp(key,OUI_SIGS[k].oui,3)){ enq(key,OUI_SIGS[k].cat,OUI_SIGS[k].label,ss.c_str(),WiFi.RSSI(i),false,false,OUI_SIGS[k].conf); break; } } // OUI (ignore by MAC)
  }
  placeUpdate(n);   // update the Wi-Fi place fingerprint from this same scan (following needs a move)
  WiFi.scanDelete();
}
// ---- WiFi promiscuous deauth/disassoc detector (its own radio window) ----
// Deauth-only trips the alarm (disassoc is usually benign roaming/churn); a
// real flood is many, fast. We also capture the source, target-is-broadcast,
// and the 802.11 reason code so a real attack can be told from WiFi noise.
static const uint32_t DEAUTH_THRESH=10;         // DEAUTH frames in a window = attack
static volatile uint32_t g_deauth=0,g_disassoc=0;
static volatile uint8_t  g_deauthSrc[6]={0};
static volatile uint8_t  g_bcast=0; static volatile uint16_t g_reason=0;
// Beacons heard in the same listening window (receive only, the radio sends nothing):
//  - Pwnagotchi: its beacons carry a JSON blob with a "pwnd_tot" handshake counter and its name.
//  - Drone Remote ID over Wi-Fi: a vendor element with the ASD-STAN OUI FA:0B:BC, type 0x0D.
// Idea and both formats credited to SquachWatch-CYD (GPL-3.0); this code is ours. The callback
// only notes what it saw; the window's end logs it.
static volatile bool g_pwnSeen=false, g_ridSeen=false;
static uint8_t g_pwnSrc[6], g_ridSrc[6]; static char g_pwnName[24]; static volatile int8_t g_pwnRssi=0, g_ridRssi=0;
static int findBytes(const uint8_t* h,int hl,const char* n,int nl){ for(int i=0;i+nl<=hl;i++) if(!memcmp(h+i,n,nl)) return i; return -1; }
static void beaconScan(const uint8_t* f,int len,int8_t rssi){
  if(len<=36) return; if(len>1024) len=1024;
  if(!g_ridSeen){ for(int i=36;i+2<=len;){ uint8_t id=f[i], l=f[i+1]; if(i+2+l>len) break;
      if(id==221 && l>=4 && f[i+2]==0xFA && f[i+3]==0x0B && f[i+4]==0xBC && f[i+5]==0x0D){ memcpy(g_ridSrc,f+10,6); g_ridRssi=rssi; g_ridSeen=true; break; }
      i+=2+l; } }
  if(!g_pwnSeen && findBytes(f+36,len-36,"pwnd_tot",8)>=0){
    int k=findBytes(f+36,len-36,"\"name\"",6); char nm[24]={0};
    if(k>=0){ int j=36+k+6; while(j<len&&(f[j]==' '||f[j]==':')) j++;
      if(j<len&&f[j]=='"'){ j++; int o=0; while(j<len&&f[j]!='"'&&o<(int)sizeof(nm)-1){ char c=(char)f[j++]; if(c<0x20||c>0x7E){ o=0; break; } nm[o++]=c; } nm[o]=0; } }
    strncpy(g_pwnName,nm,sizeof(g_pwnName)-1); memcpy(g_pwnSrc,f+10,6); g_pwnRssi=rssi; g_pwnSeen=true; }
}
static void promiscCb(void* buf, wifi_promiscuous_pkt_type_t type){
  if(type!=WIFI_PKT_MGMT) return;
  const uint8_t* f=((wifi_promiscuous_pkt_t*)buf)->payload;
  uint8_t fc=f[0]; uint8_t ftype=(fc&0x0C)>>2, sub=(fc&0xF0)>>4;
  if(ftype!=0) return;
  if(sub==8){ const wifi_promiscuous_pkt_t* pk=(wifi_promiscuous_pkt_t*)buf; int len=(int)pk->rx_ctrl.sig_len-4;   // minus FCS
    beaconScan(f,len,(int8_t)pk->rx_ctrl.rssi); return; }
  if(sub==10){ g_disassoc++; return; }
  if(sub==12){                                   // deauth
    g_deauth++;
    for(int i=0;i<6;i++) g_deauthSrc[i]=f[10+i]; // addr2 = transmitter
    bool bc=true; for(int i=0;i<6;i++) if(f[4+i]!=0xFF) bc=false; // addr1 = dest
    if(bc) g_bcast=1;
    g_reason=(uint16_t)f[24] | ((uint16_t)f[25]<<8); // reason code after 24-byte hdr
  }
}
static void wifiMonitorDeauth(){
  g_deauth=0; g_disassoc=0; g_bcast=0; g_reason=0; g_pwnSeen=false; g_ridSeen=false;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  static const uint8_t chans[]={1,6,11,1,6,11};
  for(uint8_t c:chans){ if(toyMode()) break; esp_wifi_set_channel(c,WIFI_SECOND_CHAN_NONE); vTaskDelay(pdMS_TO_TICKS(900)); }   // a toy wants the radio: stop early
  esp_wifi_set_promiscuous(false);
  if(g_pwnSeen){ uint8_t k[6]; memcpy(k,g_pwnSrc,6); enq(k,CAT_HACKDEV,"Pwnagotchi",g_pwnName[0]?g_pwnName:"pwnagotchi",g_pwnRssi,false,true,'H'); }
  if(g_ridSeen){ uint8_t k[6]; memcpy(k,g_ridSrc,6); enq(k,CAT_DRONE,"RemoteID","Wi-Fi beacon",g_ridRssi,false,false,'M'); }
  uint8_t src[6]; for(int i=0;i<6;i++) src[i]=g_deauthSrc[i];
  // Serial diagnostic every window, so an investigation can watch the counts.
  Serial.printf("[deauth-scan] deauth=%u disassoc=%u bcast=%u reason=%u src=%02X:%02X:%02X:%02X:%02X:%02X\n",
    (unsigned)g_deauth,(unsigned)g_disassoc,(unsigned)g_bcast,(unsigned)g_reason,src[0],src[1],src[2],src[3],src[4],src[5]);
  if(g_deauth>=DEAUTH_THRESH){
    // deauth sources are usually spoofed, so collapse the whole flood into ONE
    // cache entry (fixed key) instead of a new alarm per spoofed MAC.
    static const uint8_t dkey[6]={0xDE,0xAD,0xBE,0xEF,0xDE,0xAD};
    char nm[32]; snprintf(nm,sizeof(nm),"%02X%02X%02X d%u r%u%s",
      src[0],src[1],src[2],(unsigned)g_deauth,(unsigned)g_reason,g_bcast?" B":"");
    enq(dkey,CAT_ATTACK,"deauth",nm,-50,false,true,'H'); // active attack
  }
}
// =================== Pingequa RF Pack S3 (CC1101 + nRF24) ===================
// StickS3 RF Pack: SCK 5, MISO 4, MOSI 6. CC1101 CS 2 / GDO0 3. nRF24 CSN 8 / CE 1.
// Cardputer ADV Hydra RF: SCK 40, MISO 39, MOSI 14 (shared with SD, CS 12). CC1101 CS 13 / GDO0 5.
// nRF24 CSN 6 / CE 4. See board.h.
// Radios are optional: probed at boot; their scan phases run only if present,
// so the same firmware works on a bare StickS3.
// Pins come from the board profile (board.h); these defaults are the StickS3 RF Pack.
static int RF_SCK=5, RF_MISO=4, RF_MOSI=6;
static int CC_CS=2, NRF_CSN=8, NRF_CE=1;
// FSPI (SPI2). The display is on SPI3 (HSPI) on both boards, so the two never share a peripheral.
SPIClass rfspi(FSPI);
#include "sdlog.h"   // SD detection log (shares this bus; written from the scan task)
#include "wardrive.h"   // opt-in WiGLE CSV of scanned networks (SD + GPS boards)
static inline void csHi(int p){ digitalWrite(p,HIGH); }
static inline void csLo(int p){ digitalWrite(p,LOW); }

// ---- CC1101 ----
static void ccStrobe(uint8_t cmd){ csLo(CC_CS); uint32_t t=micros(); while(digitalRead(RF_MISO)&&micros()-t<5000){} rfspi.transfer(cmd); csHi(CC_CS); }
static void ccWrite(uint8_t a,uint8_t v){ csLo(CC_CS); uint32_t t=micros(); while(digitalRead(RF_MISO)&&micros()-t<5000){} rfspi.transfer(a); rfspi.transfer(v); csHi(CC_CS); }
static uint8_t ccStatus(uint8_t a){ csLo(CC_CS); uint32_t t=micros(); while(digitalRead(RF_MISO)&&micros()-t<5000){} rfspi.transfer(0xC0|a); uint8_t v=rfspi.transfer(0); csHi(CC_CS); return v; }
static bool ccProbe(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  csHi(CC_CS); delayMicroseconds(30); csLo(CC_CS); delayMicroseconds(30); csHi(CC_CS); delayMicroseconds(45);
  ccStrobe(0x30); delay(1);                          // SRES
  uint8_t ver=ccStatus(0x31);                        // VERSION
  rfspi.endTransaction();
  return ver!=0x00 && ver!=0xFF;
}
static void ccInit433(){
  ccStrobe(0x30); delay(1);
  ccWrite(0x0D,0x10); ccWrite(0x0E,0xB0); ccWrite(0x0F,0x71); // FREQ 433.92 MHz
  ccWrite(0x10,0x07);                                          // MDMCFG4: widest RX bandwidth
  ccWrite(0x12,0x30);                                          // MDMCFG2: ASK/OOK, no sync (carrier sense)
  ccWrite(0x0B,0x06);                                          // FSCTRL1
  ccStrobe(0x33); delay(1); ccStrobe(0x34); delay(2);          // SCAL, SRX
}
static int ccRssiDbm(){ uint8_t r=ccStatus(0x34); int d=(r>=128)?(r-256):r; return d/2-74; }
// Sub-GHz activity: sample 433 RSSI; a strong, repeated signal = something is
// transmitting (key fob, TPMS, remote, or a Flipper). Informational log tier.
// The 433 ambient floor is ~-75..-104 dBm here, so only a signal clearly above
// it (a close transmitter / Flipper) counts. Peak-based, well over ambient.
static int SUBGHZ_PEAK=-65;    // dBm; a strong nearby sub-GHz transmission
static void ccScanSubGhz(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  ccInit433();
  int peak=-200, floor=200; int strong=0;
  for(int i=0;i<150;i++){ int r=ccRssiDbm(); if(r>peak)peak=r; if(r<floor)floor=r; if(r>SUBGHZ_PEAK)strong++; delay(15); } // ~2.3s
  ccStrobe(0x36);                                             // SIDLE
  rfspi.endTransaction();
  Serial.printf("[subghz] peak=%d floor=%d strong=%d (thr %d)\n",peak,floor,strong,SUBGHZ_PEAK); // tuning diag
  if(strong>=3){                                              // a few strong samples = real close transmitter
    static const uint8_t skey[6]={0x43,0x33,0x00,0x00,0x00,0x00}; // "433"
    char nm[32]; snprintf(nm,sizeof(nm),"433MHz %ddBm",peak);
    enq(skey,CAT_SUBGHZ,"subghz",nm,(int8_t)peak,false,false,'M');
  }
}

// ---- nRF24 ----
static uint8_t nrfRead(uint8_t r){ csLo(NRF_CSN); rfspi.transfer(0x00|(r&0x1F)); uint8_t v=rfspi.transfer(0xFF); csHi(NRF_CSN); return v; }
static void nrfWrite(uint8_t r,uint8_t v){ csLo(NRF_CSN); rfspi.transfer(0x20|(r&0x1F)); rfspi.transfer(v); csHi(NRF_CSN); }
static bool nrfProbe(){
  digitalWrite(NRF_CE,LOW);
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  nrfWrite(0x05,0x4C); uint8_t rb=nrfRead(0x05);
  rfspi.endTransaction();
  return rb==0x4C;
}
// 2.4 GHz sweep. A broadband jammer lights most channels at once (attack). A
// few hot channels is just normal WiFi/BLE.
static void nrfScan24(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  nrfWrite(0x00,0x0B);                                        // PWR_UP, PRX
  int hot=0;
  for(int ch=0;ch<=125;ch++){ nrfWrite(0x05,ch); digitalWrite(NRF_CE,HIGH); delayMicroseconds(200); digitalWrite(NRF_CE,LOW);
    if(nrfRead(0x09)&0x01) hot++; }
  nrfWrite(0x00,0x09);                                        // power down-ish
  rfspi.endTransaction();
  // Ambient Wi-Fi/BLE lights 2..30 channels per sweep and one busy sweep can reach the mid 40s.
  // A jammer lights most channels on every sweep, so it takes two hot sweeps in a row.
  static int hotRun=0; hotRun=(hot>=40)?hotRun+1:0;
  Serial.printf("[nrf24] hot=%d/126 run=%d (thr 40 x2)\n",hot,hotRun); // tuning diag, like [subghz]
  if(hotRun>=2){                                              // broadband, sustained = likely jammer
    static const uint8_t jkey[6]={0x32,0x34,0x00,0x00,0x00,0x00}; // "24"
    char nm[32]; snprintf(nm,sizeof(nm),"2.4G %d ch hot",hot);
    enq(jkey,CAT_ATTACK,"2.4 jam",nm,-40,false,true,'M');         // active attack
  }
}

// nRF24 sweep for the waterfall: 8 carrier-detect samples per channel = a 0..8
// intensity, across all 126 channels (2400-2525 MHz). ~130 ms.
// Coarser waterfall: 60 columns (4 px each) instead of 120, fewer samples per
// column. Lighter on the radio and faster to draw.
static const int WFCOLS=60, WFCW=4;
static void nrfSweepWF(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  nrfWrite(0x00,0x0B);                              // PWR_UP, PRX
  for(int i=0;i<WFCOLS;i++){ int ch=i*2; nrfWrite(0x05,ch); int h=0;
    for(int s=0;s<4;s++){ digitalWrite(NRF_CE,HIGH); delayMicroseconds(130); digitalWrite(NRF_CE,LOW); if(nrfRead(0x09)&0x01) h++; }
    wfrow[i]=h?h+2:0; }
  nrfWrite(0x00,0x09);
  rfspi.endTransaction();
}
// Sub-GHz waterfall: sweep 415-450 MHz (the 433 band the antenna is tuned for)
// in 120 steps, reading true RSSI at each. Antenna is 433, so this window is
// where it is honest. RSSI -110..-30 dBm mapped to 0..8 intensity.
static const uint32_t WF_SUB_LO=415000000UL, WF_SUB_HI=450000000UL;
// Waterfall sources: 0 = 2.4 GHz (nRF24), 1 = 433 window, 2 = 915 window (902-928 MHz ISM, the US
// band; the Hydra RF 924 is tuned for it). 915 is offered on the Cardputer only for now: the
// StickS3 RF Pack is a 433 board. The CC1101 tunes 779-928 MHz, so it is only a different window.
static const uint32_t WF_915_LO=902000000UL, WF_915_HI=928000000UL;
static bool wfHas915();
static uint32_t wfLo(){ return wfSource==2?WF_915_LO:WF_SUB_LO; }
static uint32_t wfHi(){ return wfSource==2?WF_915_HI:WF_SUB_HI; }
static void ccSweepWF(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  ccStrobe(0x30); delay(1);                         // SRES
  ccWrite(0x10,0x07); ccWrite(0x12,0x30); ccWrite(0x0B,0x06); ccWrite(0x18,0x18); // wide BW, OOK, FS_AUTOCAL idle->RX
  uint32_t lo=wfLo(), span=wfHi()-lo;
  for(int i=0;i<WFCOLS;i++){
    uint32_t f=lo+(uint32_t)((uint64_t)span*i/WFCOLS);
    uint32_t rv=(uint32_t)(((uint64_t)f<<16)/26000000ULL);
    ccStrobe(0x36);                                 // IDLE
    ccWrite(0x0D,(rv>>16)&0xFF); ccWrite(0x0E,(rv>>8)&0xFF); ccWrite(0x0F,rv&0xFF);
    ccStrobe(0x34); delayMicroseconds(1200);        // SRX (autocal ~720us) + RSSI settle
    int r=ccRssiDbm(); delayMicroseconds(300); int r2=ccRssiDbm(); if(r2>r) r=r2; // take the stronger
    int v=(r+100)/9; if(v<0)v=0; if(v>8)v=8;        // -100..-30 dBm -> 0..8
    wfrow[i]=v;
  }
  ccStrobe(0x36);
  rfspi.endTransaction();
}
// 2.4 GHz busyness for the traffic graph: one carrier-detect sample on every
// channel, count how many are hot (0..126). ~20 ms.
static int nrfBusy(){
  if(!nrfPresent) return -1;
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  nrfWrite(0x00,0x0B); int hot=0;
  for(int ch=0;ch<=125;ch++){ nrfWrite(0x05,ch); digitalWrite(NRF_CE,HIGH); delayMicroseconds(140); digitalWrite(NRF_CE,LOW); if(nrfRead(0x09)&0x01) hot++; }
  nrfWrite(0x00,0x09);
  rfspi.endTransaction(); return hot;
}

// Sequential scan phases: each mode owns the radio for its window. Missing a
// signal during another phase is acceptable (this is not a just-in-time
// alerter). Easier and lighter than radio coexistence.
static void scanTask(void*){
  // Passive: listen to advertisements only, never send a scan request. The device transmits
  // nothing by default (TV-B-Gone, fired by hand, is the one exception). Flippers are matched
  // by the service UUIDs in their advertisements; names that only arrive in scan responses are
  // not seen. Duty ~50% (window 80 of interval 160).
  NimBLEScan* s=NimBLEDevice::getScan(); s->setActiveScan(false); s->setInterval(160); s->setWindow(80);
  for(;;){
    // Pause background scanning while a radio-toy owns the radios/screen.
    if(toyMode()){ vTaskDelay(pdMS_TO_TICKS(150)); continue; }
    // Finder (locked): fast repeated short scans watching for the ONE chosen
    // MAC, publish its live RSSI for the UI. Stays on this thread so NimBLE is
    // only ever driven from one place. While the picklist is up (unlocked) the
    // normal scan cycle below keeps the detection cache fresh.
    if(mode==MODE_FINDER && finderLocked){
      activeRadio='B'; { RadioLock L(portMAX_DELAY); WiFi.mode(WIFI_OFF); }
      NimBLEScanResults r=s->getResults(1200,false);
      int found=-127;
      for(int i=0;i<r.getCount();i++){ const NimBLEAdvertisedDevice* d=r.getDevice(i);
        if(memcmp(d->getAddress().getBase()->val,finderTarget,6)==0){ found=d->getRSSI(); break; } }
      if(found>-127){ finderRssi=found; finderSmooth=(finderSmooth*2+found)/3; finderSeen=millis(); finderHas=true; }
      s->clearResults(); uiDirty=true; continue;
    }
    // PHASE 1: BLE (~6 s). WiFi radio OFF so only one 2.4 radio is live.
    { RadioLock L(portMAX_DELAY); if(toyMode()) continue; activeRadio='B'; WiFi.mode(WIFI_OFF); }
    NimBLEScanResults r=s->getResults(6000,false);
    for(int i=0;i<r.getCount();i++) classifyBle(r.getDevice(i));
    s->clearResults();
    { RadioLock L(portMAX_DELAY); if(!toyMode()){ sdlog::service(); wd::service(); } }   // SD writes: Wi-Fi off (heap high), bus ours
    // PHASE 2/3: WiFi on for the scan + deauth monitor, then off again.
    { RadioLock L(portMAX_DELAY);
      if(!toyMode()){ activeRadio='W'; WiFi.mode(WIFI_STA); WiFi.disconnect(); esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        scanWifiOnce();
        wifiMonitorDeauth();
        WiFi.mode(WIFI_OFF); } }
    // PHASE 4/5: RF Pack radios (SPI), WiFi already off.
    if(ccPresent){  RadioLock L(portMAX_DELAY); if(!toyMode()){ activeRadio='S'; ccScanSubGhz(); } }
    if(nrfPresent){ RadioLock L(portMAX_DELAY); if(!toyMode()){ activeRadio='N'; nrfScan24(); } }
    activeRadio='-';
    vTaskDelay(pdMS_TO_TICKS(600));   // rest between cycles (cooler + lighter)
  }
}

// ================= mood (idle personality) =================
// Ideas credited to Pwnagotchi (mood faces) and M5PORKCHOP (happiness/activity
// tracking). Faces and mapping are ours. The face is chosen once per mood
// window and held, so it never flips frame to frame.
// idle life: short face animations. Zuse glances around, blinks, yawns, and
// derez-flickers so he feels alive between events.
struct FaceStep{const char* f; uint16_t ms;};
static const FaceStep A_GLANCE[]={{"glance-left",800},{"defaultsmile",250},{"glance-right",800},{"defaultsmile",1}};
static const FaceStep A_BLINK[] ={{"winkingface",140},{"defaultsmile",1}};
static const FaceStep A_YAWN[]  ={{"yawning-face",1600},{"defaultsmile",1}};
static const FaceStep A_FLICK[] ={{"total-blank-face",90},{"defaultsmile",1}};
const FaceStep* g_anim=nullptr; int g_animN=0,g_animI=0; uint32_t g_animUntil=0,animNext=6000; const char* animFaceNow=nullptr;
static void playAnim(const FaceStep* a,int n){ g_anim=a; g_animN=n; g_animI=0; g_animUntil=0; }
static void serviceAnim(){ if(!g_anim){ animFaceNow=nullptr; return; } uint32_t now=millis();
  if(now>=g_animUntil){ if(g_animI>=g_animN){ g_anim=nullptr; animFaceNow=nullptr; uiDirty=true; return; }
    FaceStep s=g_anim[g_animI++]; animFaceNow=s.f; g_animUntil=now+s.ms; uiDirty=true; } }
static const char* currentIdleFace(){
  uint32_t now=millis();
  if(alarmActive()) return alarmFace();                          // serious threat: redface, even muted
  if(dispFace && now<dispUntil) return dispFace;                 // transient log-tier face
  int bat=M5.Power.getBatteryLevel();
  if(bat>=0 && bat<15) return "sadface";                         // low battery
  if(animFaceNow) return animFaceNow;                            // living idle animation
  return "defaultsmile";
}

// ================= drawing =================
// portrait double-buffer (home + status): render to a PSRAM sprite, push once.
M5Canvas pc(&M5.Display); bool pcReady=false;
static void pcEnsure(){ if(!pcReady){ pc.setColorDepth(16); pc.setPsram(true); pcReady=pc.createSprite(135,240); } }
template<class T> static void battGlyph(T& g,int x,int y,int pct,bool chg){ if(pct<0)pct=0; if(pct>100)pct=100;
  uint16_t col=pct<15?LK_RED:pct<40?0xFD20:0x07E0; g.drawRect(x,y,20,11,AMBER); g.fillRect(x+20,y+3,2,5,AMBER); g.fillRect(x+2,y+2,pct*16/100,7,col);
  if(chg){ g.drawLine(x+10,y+2,x+7,y+6,BG); g.drawLine(x+7,y+6,x+11,y+5,BG); g.drawLine(x+11,y+5,x+9,y+9,BG); } }
template<class T> static void muteGlyph(T& g,int x,int y,bool m){ g.fillRect(x,y+3,3,5,AMBER); g.fillTriangle(x+3,y+1,x+3,y+10,x+8,y+5,AMBER);
  if(m) g.drawLine(x,y,x+11,y+11,LK_RED); else { g.drawLine(x+9,y+3,x+11,y+5,AMBER); g.drawLine(x+9,y+7,x+11,y+5,AMBER); } }
// Boards whose screen is fixed landscape (Cardputer ADV) get landscape versions of the
// portrait screens (Home, Status, Zoom, Settings, Toys). Defined after drawToys().
static bool landOnly(){ return ROT_PORTRAIT==ROT_LANDSCAPE; }
static const uint16_t DIM_L=0x8400;   // dim yellow, for secondary text (landscape screens)
static const int HDR_L=18;            // landscape header height
static void drawIdleL(); static void drawStatusL(); static void drawZoomL(); static void drawSettingsL(); static void drawToysL();
static void drawIdle(){
  if(landOnly()){ drawIdleL(); return; }
  M5.Display.setRotation(ROT_PORTRAIT); pcEnsure();
  if(!pcReady){ M5.Display.fillScreen(BG); return; }
  auto& g=pc; bool red=alarmActive(); uint16_t bg=red?LK_RED:BG, fg=red?BG:AMBER;
  g.fillScreen(bg);
  drawZuse(g,currentIdleFace(),0,0,135,bg,g_faceAngle,g_faceSweep);   // vector Zuse (idle wheel/radar when active)
  g.setTextDatum(top_left); g.setTextColor(fg,bg);
  int bat=M5.Power.getBatteryLevel(); bool chg=M5.Power.isCharging(); char b[40];
  if(red){
    int i=activeAttackIdx(); const char* lbl=(i>=0)?(dets[i].name[0]?dets[i].name:dets[i].label):"THREAT";
    g.setTextSize(3); g.drawString("ALERT",6,140);
    g.setTextSize(2); g.drawString(lbl,6,176); g.drawString("FRONT=info",6,206);
  } else {
    g.setTextSize(2); g.drawString("SCAN",6,140);
    { char rc[2]={(char)activeRadio,0}; g.drawString(rc,84,140); }
    if(!silentMode){ g.setTextColor(LK_RED,bg); g.drawString("TX",104,140); g.setTextColor(fg,bg); }   // Silent is OFF
    battGlyph(g,6,167,bat,chg);
    g.setTextSize(1); g.setTextColor((bat>=0&&bat<15&&!chg)?LK_RED:fg,bg);
    snprintf(b,sizeof(b),"%d%%",bat); g.drawString(b,31,168);
    muteGlyph(g,74,166,muted);
    g.setTextColor(fg,bg);
    snprintf(b,sizeof(b),"CAM %d DRN %d HAK %d",liveCount(CAT_CAMERA),liveCount(CAT_DRONE),liveCount(CAT_HACKDEV)); g.drawString(b,6,186);   // live: matches the lists
    snprintf(b,sizeof(b),"ATK %d TRK %d SUB %d",liveCount(CAT_ATTACK),liveCount(CAT_TRACKER),liveCount(CAT_SUBGHZ)); g.drawString(b,6,198);
    unsigned long seen=(unsigned long)cntCam+cntDrn+cntHak+cntAtk+cntTrk+cntSub;
    snprintf(b,sizeof(b),"seen %lu  SIDE=zoom",seen); g.drawString(b,6,210);   // since boot (cumulative, not openable)
    if(!ccPresent&&!nrfPresent){ g.drawString("no RF pack",6,224); }
  }
  if(toastMsg && millis()<toastUntil){
    g.fillRect(0,104,135,24,fg); g.setTextColor(bg,fg); g.setTextDatum(middle_center); g.setTextSize(2);
    g.drawString(toastMsg,67,116); g.setTextDatum(top_left);
  }
  g.pushSprite(0,0);
}
static void drawDetail(Det& d){
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(1);
  char b[64]; int y=26;
  snprintf(b,sizeof(b),"type: %s (%s) conf %c",d.label,catTag[d.cat],d.conf); M5.Display.drawString(b,8,y); y+=13;
  snprintf(b,sizeof(b),"name: %s",d.name[0]?d.name:"(none)"); M5.Display.drawString(b,8,y); y+=13;
  snprintf(b,sizeof(b),"id: %02X:%02X:%02X:%02X:%02X:%02X",d.key[0],d.key[1],d.key[2],d.key[3],d.key[4],d.key[5]); M5.Display.drawString(b,8,y); y+=13;
  snprintf(b,sizeof(b),"rssi:%d hits:%u seen:%lus",d.rssi,d.hits,(unsigned long)((millis()-d.firstSeen)/1000)); M5.Display.drawString(b,8,y);
}
// ---- menus / categories ----
static const char* STATUS_ITEMS[]={"Log","Toys","Settings","Clear cache"};
static const int STATUS_N=4;
static const char* TOYS_ITEMS[]={"Waterfall","SSID Scan","Traffic","TV-B-Gone","Finder","GPS"};
static int TOYS_N=5;   // 6 (adds GPS) on boards with a Grove GPS port, set in setup()
// Settings rows, looked up by name. "SD Log" only on boards with a card slot (built in setup).
static const char* SET_ALL[]={"Brightness","Shake","Volume","Alert Filt","Silent","SD Log","Wardrive","End of Line"};
static const int SET_MAX=8; static const char* SET_ITEMS[SET_MAX]; static int SET_N=0;
static void setRowsBuild(){ SET_N=0; for(const char* r:SET_ALL){ if(!strcmp(r,"SD Log")&&!sdlog::avail) continue; if(!strcmp(r,"Wardrive")&&!wd::avail) continue; SET_ITEMS[SET_N++]=r; } }
static bool setIs(int i,const char* n){ return i>=0&&i<SET_N&&!strcmp(SET_ITEMS[i],n); }
static void setLabel(int i,char* b,size_t n){
  if(setIs(i,"Brightness")) snprintf(b,n,"Bright:%s",BRIGHT_NAMES[brightIdx]);
  else if(setIs(i,"Shake")) snprintf(b,n,"Shake:%s",SHAKE_NAMES[shakeIdx]);
  else if(setIs(i,"Volume")) snprintf(b,n,"Vol:%s",VOL_NAMES[volIdx]);
  else if(setIs(i,"Alert Filt")) snprintf(b,n,"Alert:%s",AF_NAMES[alertMinConf]);
  else if(setIs(i,"Silent")) snprintf(b,n,"Silent:%s",silentMode?"ON":"OFF");
  else if(setIs(i,"Wardrive")) snprintf(b,n,"Wardrive:%s",wd::on?"ON":"OFF");
  else if(setIs(i,"SD Log")) snprintf(b,n,"SD Log:%s",!sdlog::on?"OFF":sdlog::present()?"ON":"NO CARD");
  else snprintf(b,n,"%s",SET_ITEMS[i]);
}
static bool setRed(int i){ return setIs(i,"Silent")&&!silentMode; }
static const Cat ZCATS[]={CAT_CAMERA,CAT_DRONE,CAT_HACKDEV,CAT_ATTACK,CAT_TRACKER,CAT_SUBGHZ};
static const char* ZNAMES[]={"CAMERAS","DRONES","HACK DEV","ATTACKS","TRACKERS","SUB-GHZ"};
static const int ZN=6;

static void drawStatus(){
  if(landOnly()){ drawStatusL(); return; }
  M5.Display.setRotation(ROT_PORTRAIT); pcEnsure(); if(!pcReady){ M5.Display.fillScreen(BG); return; }
  auto& g=pc; g.fillScreen(BG); g.setTextColor(AMBER,BG); g.setTextDatum(top_left);
  g.setTextSize(2); g.drawString("STATUS",6,6);
  g.setTextSize(1); g.setTextDatum(top_right); g.drawString(FUD_VERSION,129,8); g.setTextDatum(top_left);
  int bat=M5.Power.getBatteryLevel(); bool chg=M5.Power.isCharging();
  battGlyph(g,6,34,bat,chg); muteGlyph(g,78,33,muted);
  g.setTextSize(1); g.setTextColor(AMBER,BG);
  char b[20]; snprintf(b,sizeof(b),"%d%%",bat); g.drawString(b,32,35);
  snprintf(b,sizeof(b),"RFP:%s",(ccPresent||nrfPresent)?"YES":"NO"); g.drawString(b,96,35);
  g.drawFastHLine(6,50,123,AMBER);
  g.setTextSize(2);
  for(int i=0;i<STATUS_N;i++){ int yy=58+i*30; bool sel=(i==statusSel);
    if(sel){ g.fillRect(4,yy-2,127,24,AMBER); g.setTextColor(BG,AMBER);} else g.setTextColor(AMBER,BG);
    g.drawString(STATUS_ITEMS[i],10,yy); }
  g.setTextSize(1); g.setTextColor(AMBER,BG); g.drawString("holdSIDE=baseline",6,224);
  g.pushSprite(0,0);
}
static void drawZoom(){
  if(landOnly()){ drawZoomL(); return; }
  M5.Display.setRotation(ROT_PORTRAIT); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(1);
  M5.Display.drawString("hold=open  BACK=out",6,6);
  for(int i=0;i<ZN;i++){ int yy=22+i*35; bool sel=(i==zoomCat);
    if(sel){ M5.Display.fillRect(2,yy-2,131,31,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    M5.Display.setTextSize(2); M5.Display.setTextDatum(top_left); M5.Display.drawString(ZNAMES[i],8,yy+4);
    char b[8]; snprintf(b,sizeof(b),"%d",liveCount(ZCATS[i])); M5.Display.setTextDatum(top_right); M5.Display.drawString(b,128,yy+4); M5.Display.setTextDatum(top_left); }  // live count = what the list opens
}
static void drawSettings(){
  if(landOnly()){ drawSettingsL(); return; }
  M5.Display.setRotation(ROT_PORTRAIT); M5.Display.fillScreen(BG);
  if(setWarn==1){   // the warning before Silent goes OFF (the Stick has no Wardrive)
    static const char* W[]={"Wi-Fi scans will","send probe","requests.","","Nearby receivers","can log this","device being","here, and where."};
    M5.Display.setTextDatum(top_left); M5.Display.setTextColor(LK_RED,BG); M5.Display.setTextSize(2);
    M5.Display.drawString("SILENT",6,6); M5.Display.drawString("OFF?",6,26);
    M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1);
    for(int i=0;i<8;i++) M5.Display.drawString(W[i],6,58+i*14);
    M5.Display.setTextSize(2); M5.Display.drawString("hold=OFF",6,184); M5.Display.drawString("BACK=keep",6,206);
    return; }
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2); M5.Display.drawString("SETTINGS",6,6);
  M5.Display.setTextSize(1); M5.Display.drawString("hold=change BACK=out",6,30);
  for(int i=0;i<SET_N;i++){ int yy=54+i*28; bool sel=(i==setSel);
    if(sel){ M5.Display.fillRect(4,yy-2,127,24,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    M5.Display.setTextSize(2); char b[24]; setLabel(i,b,sizeof(b));
    if(setRed(i)&&!sel) M5.Display.setTextColor(LK_RED,BG);
    M5.Display.drawString(b,10,yy); }
  M5.Display.setTextSize(1); M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("all settings saved",6,220);
}
static void drawListL(); static void drawItemL(); static void drawAttackL(); static void drawSsidL();
static void drawTvbgL(); static void drawFinderListL(); static void drawFinderLockL();
static void drawList(){
  if(landOnly()){ drawListL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2); M5.Display.drawString(logFilter<0?"LOG":catTag[logFilter],8,4);
  M5.Display.setTextSize(1); M5.Display.drawString("NEXT=scroll hold=open BACK=out",8,24);
  int idxs[MAXDET],n; logIdxs(n,idxs);
  if(listSel>=n)listSel=n>0?n-1:0; if(listSel<listTop)listTop=listSel; if(listSel>=listTop+5)listTop=listSel-4;
  uint32_t now=millis();
  for(int r=0;r<5 && listTop+r<n;r++){ int di=idxs[listTop+r]; Det&d=dets[di]; int yy=40+r*17; bool sel=(listTop+r==listSel);
    if(sel){ M5.Display.fillRect(6,yy-1,228,16,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(d.alarm?LK_RED:AMBER,BG);
    const char* nm=d.name[0]?d.name:d.label; char b[52];
    if(d.cat==CAT_TRACKER) snprintf(b,sizeof(b),"%.12s %lum",nm,(unsigned long)((now-d.firstSeen)/60000));
    else if(d.hits>1)      snprintf(b,sizeof(b),"%s %.13s (%u)",catTag[d.cat],nm,(unsigned)d.hits);
    else                   snprintf(b,sizeof(b),"%s %.16s",catTag[d.cat],nm);
    M5.Display.drawString(b,10,yy); }
  if(n==0){ M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("(nothing here)",10,57); }
}
// big, auto-scrolling detail text (zoom overlay for the ALERT screen)
static void drawDetailBig(Det& d){
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  char b[48]; int y=8;
  M5.Display.drawString(d.label,8,y); y+=26;
  const char* nm=d.name[0]?d.name:"(no name)"; int len=strlen(nm), vis=18;
  if(len<=vis) M5.Display.drawString(nm,8,y);
  else { int span=len-vis, off=(millis()/350)%(span*2); if(off>span) off=span*2-off; char buf[24]; strncpy(buf,nm+off,vis); buf[vis]=0; M5.Display.drawString(buf,8,y); }
  y+=26;
  snprintf(b,sizeof(b),"%02X%02X%02X%02X%02X%02X",d.key[0],d.key[1],d.key[2],d.key[3],d.key[4],d.key[5]); M5.Display.drawString(b,8,y); y+=26;
  snprintf(b,sizeof(b),"rssi %d  x%u",d.rssi,d.hits); M5.Display.drawString(b,8,y);
}
// A detection we can home in on with the BLE finder: has its own MAC (not an
// SSID/sentinel key) and is a tracker, camera, or hacker device.
static bool isTrackable(const Det& d){ return d.used && !d.ssidBased &&
  (d.cat==CAT_TRACKER||d.cat==CAT_CAMERA||d.cat==CAT_HACKDEV); }
static void lockFinder(const Det& d){ memcpy(finderTarget,d.key,6);
  strncpy(finderLabel,d.label,sizeof(finderLabel)-1); finderLabel[sizeof(finderLabel)-1]=0;
  strncpy(finderName,d.name,sizeof(finderName)-1); finderName[sizeof(finderName)-1]=0;
  finderCat=d.cat; finderLocked=true; finderHas=false; finderSmooth=finderBaseline=-100; finderSeen=0; mode=MODE_FINDER; }
static void drawItem(){
  if(landOnly()){ drawItemL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  if(curDet<0||!dets[curDet].used){ mode=MODE_LIST; return; }
  Det&d=dets[curDet];
  // biggest text = what was detected (name if it has one, else the type)
  M5.Display.setTextColor(d.alarm?LK_RED:AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString(d.name[0]?String(d.name).substring(0,19):String(d.label),8,4);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1);
  char b[64]; int y=30;
  snprintf(b,sizeof(b),"%s   %02X:%02X:%02X:%02X:%02X:%02X",catTag[d.cat],d.key[0],d.key[1],d.key[2],d.key[3],d.key[4],d.key[5]); M5.Display.drawString(b,8,y); y+=13;
  snprintf(b,sizeof(b),"rssi %d  hits %u  conf %c",d.rssi,d.hits,d.conf); M5.Display.drawString(b,8,y); y+=13;
  snprintf(b,sizeof(b),"seen %lus ago",(unsigned long)((millis()-d.lastSeen)/1000)); M5.Display.drawString(b,8,y);
  bool tk=isTrackable(d);
  const char* acts3[]={"Ack","Ignore","Back"};
  const char* acts4[]={"Ack","Ignore","Find","Back"};
  const char** acts=tk?acts4:acts3; int na=tk?4:3; int step=tk?58:76, w=tk?54:72;
  if(itemAct>=na) itemAct=0;
  for(int i=0;i<na;i++){ int xx=8+i*step,yy=96;
    if(i==itemAct){ M5.Display.fillRect(xx-2,yy-1,w,16,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    M5.Display.drawString(acts[i],xx+2,yy); }
}
static void drawAttack(){
  if(landOnly()&&!textZoom){ drawAttackL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  if(textZoom){ if(attackShow>=0&&dets[attackShow].used) drawDetailBig(dets[attackShow]); return; }
  M5.Display.setTextColor(LK_RED,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString(attackShow>=0&&dets[attackShow].used&&dets[attackShow].cat!=CAT_HACKDEV?"ATTACK":"HACKER",8,4);
  if(attackShow>=0&&dets[attackShow].used) drawDetail(dets[attackShow]);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1); M5.Display.drawString("acked. hold=zoom BACK=home",8,104);
}
// Spectrum-analyzer palette: black -> blue -> cyan -> green -> yellow -> red -> white.
static uint16_t wfHeat(int v){
  static const uint16_t pal[8]={0x0007,0x001F,0x05FF,0x07FF,0x07E0,0xFFE0,0xFC00,0xFFFF};
  if(v<=0) return 0x0000; int t=v; if(t<1)t=1; if(t>8)t=8; return pal[t-1];
}
// Waterfall and traffic share one sprite. 16-bit with PSRAM, 8-bit without (27 KB instead of 54).
// Fixed-landscape boards: FUD-Boy style header, channel ticks under it, a shorter sprite below.
static int wfY(){ return landOnly()?31:22; }
static int wfH(){ return landOnly()?104:112; }
// Without PSRAM the sprite comes from internal heap: only allocate while the toy holds the radio
// lock (the scan task is then out of its Wi-Fi phase), never at the moment the screen opens.
static bool wfMayAlloc=false, wfTried=false;
static void wfEnsure(){ if(wfReady) return; bool ps=ESP.getPsramSize()>0; if(!ps&&!wfMayAlloc) return; wfTried=true;
  wfc.setColorDepth(ps?16:8); wfc.setPsram(ps); wfReady=wfc.createSprite(240,wfH());
  if(wfReady) wfc.fillScreen(0); else Serial.printf("[wf] sprite failed, heap %u max %u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap()); }
template<class G> static void toyHeaderL(G& g,const char* title,const char* val){
  g.fillRect(0,0,240,HDR_L,BG); g.setTextSize(1);
  g.setFont(&fonts::FreeSansBold9pt7b); g.setTextColor(AMBER,BG); g.setTextDatum(middle_left); g.drawString(title,3,HDR_L/2);
  g.setFont(&fonts::Font2); g.setTextDatum(middle_right); g.drawString(val,237,HDR_L/2);
  g.setFont(&fonts::Font0); g.setTextDatum(top_left); g.drawFastHLine(0,HDR_L-1,240,DIM_L);
}
extern bool lastFramePcL;
static void wfInit(){ lastFramePcL=false;   // drawn on the panel, not pcL (shot must read the panel)
  if(landOnly()){
    M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG); wfEnsure();
    toyHeaderL(M5.Display,wfSource==0?"2.4 GHz":(wfSource==2?"915 MHz":"433 MHz"),"");
    M5.Display.setFont(&fonts::Font2); M5.Display.setTextColor(DIM_L,BG);
    if(wfSource==0){ M5.Display.drawString("ch1",14,HDR_L-1); M5.Display.drawString("6",68,HDR_L-1); M5.Display.drawString("11",116,HDR_L-1);
      M5.Display.setTextDatum(top_right); M5.Display.drawString("2525",238,HDR_L-1); M5.Display.setTextDatum(top_left); }
    else if(wfSource==2){ M5.Display.drawString("902",2,HDR_L-1); M5.Display.drawString("915",(int)(240*(915-902)/26)-10,HDR_L-1);
      M5.Display.setTextDatum(top_right); M5.Display.drawString("928",238,HDR_L-1); M5.Display.setTextDatum(top_left); }
    else { M5.Display.drawString("415",2,HDR_L-1); M5.Display.drawString("433.9",(int)(240*(433.92-415)/35)-14,HDR_L-1);
      M5.Display.setTextDatum(top_right); M5.Display.drawString("450",238,HDR_L-1); M5.Display.setTextDatum(top_left); }
    M5.Display.setFont(&fonts::Font0);
    if(wfReady) wfc.pushSprite(0,wfY()); else { M5.Display.setTextColor(wfTried?LK_RED:DIM_L,BG); M5.Display.drawString(wfTried?"no sprite memory":"starting...",4,60); }
    return;
  }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(1);
  M5.Display.drawString(wfSource==0?"2.4GHz   MAIN=band BACK=out":"433MHz   MAIN=band BACK=out",4,2);
  wfEnsure();
  if(wfSource==0){ M5.Display.setTextColor(0x7BEF,BG); M5.Display.drawString("1",22,12); M5.Display.drawString("6",72,12); M5.Display.drawString("11",120,12); } // WiFi ch ticks
}
static void wfRender(){
  if(!wfReady){ M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("no sprite memory",4,60); return; }
  wfc.scroll(0,1);
  int peakI=-1,peakV=0;
  for(int i=0;i<WFCOLS;i++){ int v=wfrow[i]; uint16_t c=wfHeat(v); for(int x=0;x<WFCW;x++) wfc.drawPixel(i*WFCW+x,0,c); if(v>peakV){peakV=v;peakI=i;} }
  wfc.pushSprite(0,wfY());
  char b[20];
  if(peakI<0) snprintf(b,sizeof(b),"quiet");
  else if(wfSource==0) snprintf(b,sizeof(b),"pk %dMHz",2400+peakI*2);
  else snprintf(b,sizeof(b),"pk %luMHz",(unsigned long)((wfLo()+(uint64_t)(wfHi()-wfLo())*peakI/WFCOLS)/1000000));
  if(landOnly()){ M5.Display.setFont(&fonts::Font2); M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(middle_right);
    M5.Display.setTextPadding(110); M5.Display.drawString(b,237,HDR_L/2); M5.Display.setTextPadding(0);
    M5.Display.setFont(&fonts::Font0); M5.Display.setTextDatum(top_left); return; }
  M5.Display.fillRect(150,2,90,10,BG); M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_right); M5.Display.drawString(b,238,2); M5.Display.setTextDatum(top_left);
}
static void drawToys(){
  if(landOnly()){ drawToysL(); return; }
  M5.Display.setRotation(ROT_PORTRAIT); pcEnsure(); if(!pcReady){ M5.Display.fillScreen(BG); return; }
  auto& g=pc; g.fillScreen(BG); g.setTextColor(AMBER,BG); g.setTextDatum(top_left);
  g.setTextSize(2); g.drawString("TOYS",6,6);
  g.setTextSize(1); g.drawString("FRONT=move hold=open",6,28);
  g.setTextSize(2);
  for(int i=0;i<TOYS_N;i++){ int yy=48+i*32; bool sel=(i==toysSel);
    if(sel){ g.fillRect(4,yy-2,127,26,AMBER); g.setTextColor(BG,AMBER);} else g.setTextColor(AMBER,BG);
    g.drawString(TOYS_ITEMS[i],10,yy); }
  g.pushSprite(0,0);
}
// ================= landscape screens (fixed-landscape boards, 240x135) =================
// Home draws into one 240x135 sprite (no flicker). 16-bit with PSRAM, 8-bit without (~32 KB).
M5Canvas pcL(&M5.Display); bool pcLReady=false;
bool lastFramePcL=false;   // the last frame came from pcL (serial `shot` reads it: the ADV panel has no read line)
static void pcLEnsure(){ if(pcLReady) return; bool ps=ESP.getPsramSize()>0;
  pcL.setColorDepth(ps?16:8); pcL.setPsram(ps); pcLReady=pcL.createSprite(240,135); }
// The notice shown for LOCK_MSG_MS when the screen locks, then the backlight goes off.
// Landscape draws into pcL like the other landscape screens (the ADV panel cannot be read back).
static void drawLockMsg(){
  if(landOnly()){ M5.Display.setRotation(ROT_LANDSCAPE); pcLEnsure();
    if(pcLReady){ auto& d=pcL; lastFramePcL=true; d.fillScreen(BG); d.setTextDatum(middle_center); d.setTextColor(AMBER,BG);
      d.setTextSize(3); d.drawString("LOCKING",120,45); d.setTextSize(2); d.drawString("press any key",120,80); d.drawString("to wake",120,102);
      d.setTextDatum(top_left); d.pushSprite(0,0); return; } }
  auto& d=M5.Display; d.setRotation(landOnly()?ROT_LANDSCAPE:ROT_PORTRAIT); d.fillScreen(BG); lastFramePcL=false;
  d.setTextDatum(middle_center); d.setTextColor(AMBER,BG); int cx=d.width()/2, cy=d.height()/2;
  if(landOnly()){ d.setTextSize(3); d.drawString("LOCKING",cx,cy-22); d.setTextSize(2); d.drawString("press any key",cx,cy+12); d.drawString("to wake",cx,cy+34); }
  else { d.setTextSize(3); d.drawString("LOCKING",cx,cy-40); d.setTextSize(2); d.drawString("press any",cx,cy); d.drawString("button",cx,cy+22); d.drawString("to wake",cx,cy+44); }
  d.setTextDatum(top_left);
}
// FUD-Boy style for the small fixed-landscape screen: an 18 px header in FreeSansBold 9 pt,
// big tiles and rows, no 6x8 text. Zuse left, six category tiles right (2 x 3).
//  +----------------------------------------+
//  | FUD                      MUTE B  [bat] |  header
//  |  .----.   [ CAM   0 ] [ DRN   0 ]      |
//  | ( Zuse )  [ HAK   0 ] [ ATK   0 ]      |  tiles: live counts (what the list opens)
//  |  '----'   [ TRK   3 ] [ SUB   0 ]      |
//  +----------------------------------------+
int homeSel=0;                               // selected Home tile (keys move it)
template<class G> static void headerL(G& g,const char* title,uint16_t tc=AMBER){
  g.fillRect(0,0,240,HDR_L,BG);
  g.setTextSize(1); g.setFont(&fonts::FreeSansBold9pt7b); g.setTextColor(tc,BG); g.setTextDatum(middle_left);
  g.drawString(title,3,HDR_L/2);
  battGlyph(g,216,3,M5.Power.getBatteryLevel(),M5.Power.isCharging());
  char r[28]; snprintf(r,sizeof(r),"%s%s%s%s%c",screenLock?"LOCK  ":"",silentMode?"":"TX  ",wd::on?"WD  ":"",muted?"MUTE  ":"",(char)activeRadio);   // TX = Silent OFF, WD = wardriving
  g.setFont(&fonts::Font2); g.setTextColor((muted||!silentMode)?LK_RED:DIM_L,BG); g.setTextDatum(middle_right); g.drawString(r,210,HDR_L/2);
  g.setFont(&fonts::Font0); g.setTextDatum(top_left);
  g.drawFastHLine(0,HDR_L-1,240,DIM_L);
}
static const int TL_X=106, TL_Y=HDR_L+3, TL_W=64, TL_H=36, TL_GAP=3;
template<class G> static void tileL(G& g,int i){
  int x=TL_X+(i%2)*(TL_W+TL_GAP), y=TL_Y+(i/2)*(TL_H+TL_GAP);
  Cat c=ZCATS[i]; int n=liveCount(c); bool sel=(i==homeSel);
  bool hot=false; for(auto&d:dets) if(d.used&&d.alarm&&!d.acked&&d.cat==c){ hot=true; break; }
  uint16_t edge=hot?LK_RED:AMBER, bg=sel?(hot?LK_RED:AMBER):BG, fg=sel?BG:(hot?LK_RED:(n?AMBER:DIM_L));
  g.fillRoundRect(x,y,TL_W,TL_H,5,bg); g.drawRoundRect(x,y,TL_W,TL_H,5,edge);
  g.setTextColor(fg,bg); g.setTextSize(1);
  g.setFont(&fonts::FreeSansBold9pt7b); g.setTextDatum(top_left); g.drawString(catTag[c],x+5,y+3);
  char b[8]; snprintf(b,sizeof(b),"%d",n);
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextDatum(baseline_right); g.drawString(b,x+TL_W-5,y+TL_H-4);
  g.setFont(&fonts::Font0); g.setTextDatum(top_left);
}
template<class G> static void alarmBannerL(G& g){
  int i=activeAttackIdx(); int x=TL_X, y=TL_Y, w=240-TL_X-2, h=3*TL_H+2*TL_GAP;
  g.fillRoundRect(x,y,w,h,6,LK_RED); g.setTextColor(TFT_WHITE,LK_RED); g.setTextSize(1); g.setTextDatum(top_center);
  g.setFont(&fonts::FreeSansBold12pt7b);
  g.drawString(i>=0&&dets[i].cat==CAT_HACKDEV?"HACKER":"ATTACK",x+w/2,y+6);
  g.setFont(&fonts::Font2);
  if(i>=0){ Det&d=dets[i];
    g.drawString(String(d.label).substring(0,18),x+w/2,y+38);
    g.drawString(String(d.name[0]?d.name:"").substring(0,18),x+w/2,y+56);
    char b[24]; snprintf(b,sizeof(b),"%s  %d dBm",catTag[d.cat],d.rssi); g.drawString(b,x+w/2,y+74); }
  g.setFont(&fonts::Font0); g.setTextDatum(top_left);
}
static void drawIdleL(){
  M5.Display.setRotation(ROT_LANDSCAPE); pcLEnsure();
  if(!pcLReady){ M5.Display.fillScreen(BG); M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("no screen memory",4,4); return; }
  auto& g=pcL; lastFramePcL=true;
  g.fillScreen(BG); headerL(g,"FUD");
  drawZuse(g,currentIdleFace(),1,HDR_L+6,102,BG,g_faceAngle,g_faceSweep);
  if(alarmActive()) alarmBannerL(g); else for(int i=0;i<ZN;i++) tileL(g,i);
  if(!ccPresent&&!nrfPresent){ g.setFont(&fonts::Font2); g.setTextColor(DIM_L,BG); g.setTextDatum(bottom_left);
    g.drawString("no RF",3,134); g.setFont(&fonts::Font0); g.setTextDatum(top_left); }
  if(toastMsg && millis()<toastUntil){
    g.fillRoundRect(10,52,220,30,6,AMBER); g.setTextColor(BG,AMBER); g.setTextDatum(middle_center);
    g.setFont(&fonts::FreeSansBold9pt7b); g.drawString(toastMsg,120,67); g.setFont(&fonts::Font0); g.setTextDatum(top_left);
  }
  g.pushSprite(0,0);
}
// A landscape menu, FUD-Boy style: header, then 5 rows of size-2 text (23 px), scrolled so the
// selected row stays in view, with a scrollbar track and thumb when the list is longer.
// col (optional) colors an unselected row, e.g. red for one that is alarming. The left text is
// clipped to the room the right column leaves.
template<class G> static void fitString(G& g,const char* t,int x,int y,int w){
  char b[48]; strncpy(b,t,sizeof(b)-1); b[sizeof(b)-1]=0; int n=strlen(b);
  while(n>0 && g.textWidth(b)>w) b[--n]=0;
  g.drawString(b,x,y);
}
// Rows come from fn(i, left, right, color), called only for the rows on screen, so a long list
// costs no memory beyond five rows of text.
typedef void (*RowFn)(int i,char* L,char* R,uint16_t& col);
static const int ROWL=40, ROWR=12;
static void menuLF(const char* title,int n,int sel,RowFn fn){
  M5.Display.setRotation(ROT_LANDSCAPE); pcLEnsure();
  if(!pcLReady){ M5.Display.fillScreen(BG); return; }
  auto& d=pcL; d.fillScreen(BG); lastFramePcL=true;
  headerL(d,title);
  const int Y0=HDR_L+1, RH=23, VIS=5, BAR=5, RW=240-BAR-3;
  int top=sel-VIS+1; if(top<0) top=0; if(top>n-VIS) top=n>VIS?n-VIS:0;
  d.setTextSize(2); d.setTextDatum(middle_left);
  for(int i=top;i<n && i<top+VIS;i++){ int yy=Y0+(i-top)*RH; bool s=(i==sel);
    char L[ROWL]="", R[ROWR]=""; uint16_t fg=AMBER; fn(i,L,R,fg);
    d.fillRect(0,yy,RW,RH,s?AMBER:BG); d.setTextColor(s?BG:fg,s?AMBER:BG);
    int rw=0; if(R[0]){ rw=d.textWidth(R)+12; d.setTextDatum(middle_right); d.drawString(R,RW-6,yy+RH/2+1); }
    d.setTextDatum(middle_left); fitString(d,L,6,yy+RH/2+1,RW-12-rw); }
  if(n>VIS){ int tx=240-BAR-1, ty=Y0+1, th=VIS*RH-2; d.drawRect(tx,ty,BAR,th,DIM_L);
    int thumb=max(10,th*VIS/n), pos=ty+(th-thumb)*top/(n-VIS); d.fillRect(tx,pos,BAR,thumb,AMBER); }
  d.setTextSize(1); d.setTextDatum(top_left);
  d.pushSprite(0,0);
}
// Fixed arrays (menus, settings): the same screen from plain string tables.
static const char* const* mRows; static const char* const* mRight; static const uint16_t* mCol;
static void menuArrRow(int i,char* L,char* R,uint16_t& c){
  snprintf(L,ROWL,"%s",mRows[i]); if(mRight&&mRight[i]) snprintf(R,ROWR,"%s",mRight[i]); if(mCol) c=mCol[i]; }
static void menuL(const char* title,const char* const* rows,const char* const* right,int n,int sel,const uint16_t* col=nullptr){
  mRows=rows; mRight=right; mCol=col; menuLF(title,n,sel,menuArrRow);
}
static void drawStatusL(){
  menuL("STATUS  " FUD_VERSION,STATUS_ITEMS,nullptr,STATUS_N,statusSel);
}
static void drawZoomL(){
  static char cnt[ZN][6]; static const char* rp[ZN];
  for(int i=0;i<ZN;i++){ snprintf(cnt[i],sizeof(cnt[i]),"%d",liveCount(ZCATS[i])); rp[i]=cnt[i]; }   // live count = what the list opens
  menuL("ZOOM",ZNAMES,rp,ZN,zoomCat);
}
static void drawSilentWarnL();
static void drawSettingsL(){
  if(setWarn){ drawSilentWarnL(); return; }   // Silent OFF? or Wardrive ON?
  static char rows[SET_MAX][24]; static const char* rp[SET_MAX]; static uint16_t col[SET_MAX];
  for(int i=0;i<SET_N;i++){
    setLabel(i,rows[i],sizeof(rows[i]));
    col[i]=setRed(i)?LK_RED:AMBER;
    rp[i]=rows[i]; }
  menuL("SETTINGS",rp,nullptr,SET_N,setSel,col);
}
static void drawToysL(){ menuL("TOYS",TOYS_ITEMS,nullptr,TOYS_N,toysSel); }
// GPS status (fixed-landscape boards with a Grove GPS). Big rows: fix and satellites, latitude,
// longitude, altitude and HDOP, then time, port and data counters.
static void drawGps(){
  M5.Display.setRotation(ROT_LANDSCAPE); pcLEnsure();
  if(!pcLReady){ M5.Display.fillScreen(BG); return; }
  auto& d=pcL; d.fillScreen(BG); lastFramePcL=true;
  headerL(d,"GPS");
  const gps::State& G=gps::S; char b[40]; const int RH=23, Y0=HDR_L+2;
  d.setTextSize(2); d.setTextDatum(top_left);
  auto rowAt=[&](int r,uint16_t c,const char* t){ d.setTextColor(c,BG); d.drawString(t,6,Y0+r*RH+3); };
  if(!G.found){
    rowAt(0,LK_RED,"SEARCHING");
    d.setTextSize(1); d.setFont(&fonts::Font2); d.setTextColor(DIM_L,BG);
    snprintf(b,sizeof(b),"trying Grove G%d at %lu",G.pin,(unsigned long)G.baud); d.drawString(b,6,Y0+RH+6);
    d.drawString("listen only, nothing is sent",6,Y0+RH+24);
    d.setFont(&fonts::Font0);
  } else {
    bool fx=gps::fixed();
    snprintf(b,sizeof(b),"%s  SAT %d",fx?"FIX":"NO FIX",G.sats<0?0:G.sats); rowAt(0,fx?TFT_GREEN:LK_RED,b);
    if(G.hasPos){
      snprintf(b,sizeof(b),"%10.6f %c",fabs(G.lat),G.lat<0?'S':'N'); rowAt(1,fx?AMBER:DIM_L,b);
      snprintf(b,sizeof(b),"%10.6f %c",fabs(G.lon),G.lon<0?'W':'E'); rowAt(2,fx?AMBER:DIM_L,b);
      snprintf(b,sizeof(b),"ALT %dm HDOP %.1f",(int)G.altM,G.hdop); rowAt(3,fx?AMBER:DIM_L,b);
    } else { rowAt(1,DIM_L,"no position yet"); rowAt(2,DIM_L,"needs open sky"); }
    d.setTextSize(1); d.setFont(&fonts::Font2); d.setTextColor(DIM_L,BG); d.setTextDatum(bottom_left);
    if(clk::trusted()){ char t[10]; clk::hms(t,sizeof(t)); snprintf(b,sizeof(b),"UTC %s  clock set  %lu ok",t,(unsigned long)G.good); }
    else snprintf(b,sizeof(b),"UTC %s  G%d@%lu  %lu ok",G.utc[0]?G.utc:"--:--:--",G.pin,(unsigned long)G.baud,(unsigned long)G.good);
    d.drawString(b,4,134); d.setFont(&fonts::Font0); d.setTextDatum(top_left);
  }
  d.setTextSize(1);
  d.pushSprite(0,0);
}
static void scanSsids(){
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  int n=wifiScan(); ssidN=0;
  for(int i=0;i<n && ssidN<24;i++){ SsidRow&s=ssidRows[ssidN];
    strncpy(s.ssid,WiFi.SSID(i).c_str(),sizeof(s.ssid)-1); s.ssid[sizeof(s.ssid)-1]=0;
    s.rssi=WiFi.RSSI(i); s.ch=WiFi.channel(i); s.open=(WiFi.encryptionType(i)==WIFI_AUTH_OPEN); ssidN++; }
  WiFi.scanDelete();
  for(int a=1;a<ssidN;a++){ SsidRow v=ssidRows[a]; int b=a-1; while(b>=0&&ssidRows[b].rssi<v.rssi){ssidRows[b+1]=ssidRows[b];b--;} ssidRows[b+1]=v; } // strongest first
}
static void drawSsid(){
  if(landOnly()){ drawSsidL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2); M5.Display.drawString("SSIDs",8,4);
  M5.Display.setTextSize(1); char h[16]; snprintf(h,sizeof(h),"%d",ssidN); M5.Display.setTextDatum(top_right); M5.Display.drawString(h,236,8); M5.Display.setTextDatum(top_left);
  if(ssidN==0){ M5.Display.drawString("scanning...",8,40); return; }
  if(ssidSel>=ssidN)ssidSel=ssidN-1; if(ssidSel<ssidTop)ssidTop=ssidSel; if(ssidSel>=ssidTop+5)ssidTop=ssidSel-4;
  for(int r=0;r<5 && ssidTop+r<ssidN;r++){ int i=ssidTop+r; SsidRow&s=ssidRows[i]; int yy=28+r*20; bool sel=(i==ssidSel);
    if(sel){ M5.Display.fillRect(6,yy-1,228,18,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    char b[48]; snprintf(b,sizeof(b),"%-15s c%-2d %d%s",s.ssid[0]?s.ssid:"(hidden)",s.ch,s.rssi,s.open?" O":""); M5.Display.drawString(b,10,yy); }
}
static void drawSoon(const char* t){
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2); M5.Display.drawString(t,120,48);
  M5.Display.setTextSize(1); M5.Display.drawString("coming soon",120,78); M5.Display.drawString("BACK=out",120,104);
  M5.Display.setTextDatum(top_left);
}

// ---- TV-B-Gone ----
// A curated set of TV power codes covering the common brands (Samsung, LG, Sony,
// Vizio, TCL, Toshiba, Panasonic, Philips). Not the full ~200-code database, but
// enough to switch off most modern sets. Facts (protocol + power code) only.
enum IrP{ IR_NEC, IR_SONY, IR_SAMS, IR_RC5, IR_RC6, IR_PANA, IR_LG };
struct IrCode{ uint8_t p; uint64_t d; uint16_t bits; };
static const IrCode TVBG_CODES[]={
  {IR_SAMS,0xE0E040BF,32},   // Samsung
  {IR_LG  ,0x20DF10EF,32},   // LG / Vizio (NEC 0x20DF10EF)
  {IR_NEC ,0x20DF10EF,32},   // LG variant via generic NEC
  {IR_NEC ,0x2FD48B7 ,32},   // Toshiba and many NEC TVs
  {IR_SONY,0xA90,12},{IR_SONY,0xA90,15},{IR_SONY,0xA90,20}, // Sony (3 frame lengths)
  {IR_PANA,0x40040100BCBDULL,48}, // Panasonic
  {IR_RC5 ,0x0C,12},         // Philips / RC5 power
  {IR_RC6 ,0x0C,20},         // Philips / RC6 power
  {IR_NEC ,0x57E3E817,32},   // TCL / Hisense (NEC)
  {IR_NEC ,0x1EE17887,32},   // Sharp-ish / misc NEC
};
static const int TVBG_N=sizeof(TVBG_CODES)/sizeof(TVBG_CODES[0]);
static void tvbgSend(const IrCode& c){ if(!irTx) return; switch(c.p){
  case IR_NEC:  irTx->sendNEC(c.d,c.bits); break;
  case IR_SONY: irTx->sendSony(c.d,c.bits,2); break;   // Sony wants repeats
  case IR_SAMS: irTx->sendSAMSUNG(c.d,c.bits); break;
  case IR_RC5:  irTx->sendRC5(c.d,c.bits); break;
  case IR_RC6:  irTx->sendRC6(c.d,c.bits); break;
  case IR_PANA: irTx->sendPanasonic64(c.d,c.bits); break;
  case IR_LG:   irTx->sendLG(c.d,c.bits); break;
} }
static void drawTvbg(){
  if(landOnly()){ drawTvbgL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2); M5.Display.drawString("TV-B-Gone",120,20);
  if(tvbgFiring){
    char b[24]; snprintf(b,sizeof(b),"firing %d/%d",tvbgIdx,TVBG_N); M5.Display.drawString(b,120,54);
    int w=200*tvbgIdx/TVBG_N; M5.Display.drawRect(20,74,200,14,AMBER); M5.Display.fillRect(20,74,w,14,AMBER);
    M5.Display.setTextSize(1); M5.Display.drawString("BACK=stop",120,104);
  } else {
    M5.Display.setTextSize(1);
    M5.Display.drawString(tvbgIdx>=TVBG_N?"done":"point at TV",120,50);
    M5.Display.setTextSize(2); M5.Display.drawString("FRONT=fire",120,74);
    M5.Display.setTextSize(1); M5.Display.drawString("BACK=out",120,104);
  }
  M5.Display.setTextDatum(top_left);
}
// ---- Traffic graph (rolling 2.4 GHz busyness) ----
static uint16_t trafColor(int b){ if(b>=50) return LK_RED; if(b>=20) return 0xFD20; return 0x07E0; } // red/amber/green
static void trafInit(){
  if(landOnly()){
    M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
    toyHeaderL(M5.Display,"2.4 TRAFFIC","");
    M5.Display.setFont(&fonts::Font2); M5.Display.setTextColor(DIM_L,BG);
    M5.Display.drawString(nrfPresent?"busy channels of 126, newest right":"no 2.4 radio",4,HDR_L-1); M5.Display.setFont(&fonts::Font0);
    if(!nrfPresent){ trafReady=false; return; }
    wfEnsure(); if(wfReady){ wfc.fillScreen(0); wfc.pushSprite(0,wfY()); } trafPeak=0; return;
  }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString("2.4 traffic",4,2);
  if(!nrfPresent){ M5.Display.setTextSize(1); M5.Display.drawString("no 2.4 radio  BACK=out",4,40); trafReady=false; return; }
  wfEnsure();
  if(wfReady){ wfc.fillScreen(0); wfc.pushSprite(0,wfY()); } trafPeak=0;
}
static void trafStep(){
  if(!wfReady||!nrfPresent) return; int b=nrfBusy(); if(b<0) return; trafNow=b; if(b>trafPeak) trafPeak=b;
  wfc.scroll(-1,0);
  int H=wfH(); int h=b*H/126; if(h>H)h=H; wfc.drawFastVLine(239,H-h,h,trafColor(b));
  wfc.pushSprite(0,wfY());
  char t[24]; snprintf(t,sizeof(t),"now %-3d pk %-3d",trafNow,trafPeak);
  if(landOnly()){ M5.Display.setFont(&fonts::Font2); M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(middle_right);
    M5.Display.setTextPadding(120); M5.Display.drawString(t,237,HDR_L/2); M5.Display.setTextPadding(0);
    M5.Display.setFont(&fonts::Font0); M5.Display.setTextDatum(top_left); return; }
  M5.Display.setTextDatum(top_right); M5.Display.setTextSize(1);
  M5.Display.fillRect(150,4,86,12,BG); M5.Display.setTextColor(AMBER,BG); M5.Display.drawString(t,236,6);
  M5.Display.setTextDatum(top_left);
}
// ---- Tracker Finder (pick a target, then walk toward it by RSSI) ----
// Candidate list: BLE-based huntable detections from the cache, category then
// signal. (SSID/sentinel-key entries have no MAC to home in on.)
static int finderList(int* idxs,int max){ int n=0; uint32_t now=millis();
  for(int i=0;i<MAXDET;i++){ Det& d=dets[i]; if(!isTrackable(d)||d.ignored) continue;
    if(now-d.lastSeen > 5UL*60*1000) continue;   // only what is still around (last 5 min)
    if(n<max) idxs[n++]=i; }
  for(int a=1;a<n;a++){ int v=idxs[a]; int b=a-1;                       // category asc, rssi desc
    while(b>=0 && (dets[idxs[b]].cat>dets[v].cat || (dets[idxs[b]].cat==dets[v].cat && dets[idxs[b]].rssi<dets[v].rssi))){ idxs[b+1]=idxs[b]; b--; }
    idxs[b+1]=v; }
  return n;
}
static void drawFinderList(){
  if(landOnly()){ drawFinderListL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString("Finder",4,2);
  int idxs[MAXDET]; int n=finderList(idxs,MAXDET);
  M5.Display.setTextSize(1); M5.Display.setTextDatum(top_right); char h[12]; snprintf(h,sizeof(h),"%d",n); M5.Display.drawString(h,236,8); M5.Display.setTextDatum(top_left);
  if(n==0){ M5.Display.drawString("no BLE targets in cache yet",4,40); M5.Display.drawString("let it scan  BACK=out",4,56); return; }
  if(finderSel>=n)finderSel=n-1; if(finderSel<0)finderSel=0;
  if(finderSel<finderTop)finderTop=finderSel; if(finderSel>=finderTop+5)finderTop=finderSel-4;
  for(int r=0;r<5 && finderTop+r<n;r++){ int i=idxs[finderTop+r]; Det& d=dets[i]; int yy=28+r*20; bool sel=(finderTop+r==finderSel);
    if(sel){ M5.Display.fillRect(6,yy-1,228,18,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    char b[52]; snprintf(b,sizeof(b),"%s %-10.10s %-6.6s %d",catTag[d.cat],d.label,d.name,d.rssi); M5.Display.drawString(b,10,yy); }
}
static void drawFinderLock(){
  if(landOnly()){ drawFinderLockL(); return; }
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString("Finder",4,2);
  if(!finderHas || millis()-finderSeen>4000){
    M5.Display.setTextSize(2); M5.Display.drawString(finderLabel,4,26);
    M5.Display.setTextSize(1); M5.Display.drawString("signal lost. move around,",4,58);
    M5.Display.drawString("or BACK to pick again.",4,74);
    M5.Display.drawString("(trackers rotate their MAC)",4,90); return; }
  int rssi=finderSmooth;
  // target line
  char b[48]; snprintf(b,sizeof(b),"%s %s",catTag[finderCat],finderLabel);
  M5.Display.setTextSize(2); M5.Display.drawString(b,4,26);
  if(finderName[0]){ M5.Display.setTextSize(1); M5.Display.drawString(finderName,4,48); }
  // RSSI meter: -100..-30 dBm -> 0..220 px
  int w=(rssi+100)*220/70; if(w<0)w=0; if(w>220)w=220;
  uint16_t c=rssi>-55?LK_RED:rssi>-70?0xFD20:0x07E0;
  M5.Display.drawRect(4,62,224,16,AMBER); M5.Display.fillRect(6,64,w>2?w-4:0,12,c);
  // distance word + dBm
  const char* d = rssi>-45?"RIGHT HERE": rssi>-58?"VERY CLOSE": rssi>-70?"NEAR": rssi>-84?"FAR":"FAINT";
  M5.Display.setTextSize(2); M5.Display.setTextDatum(top_left); M5.Display.drawString(d,4,84);
  M5.Display.setTextDatum(top_right); M5.Display.setTextSize(1);
  snprintf(b,sizeof(b),"%d dBm  FRONT=reset",rssi); M5.Display.drawString(b,236,90); M5.Display.setTextDatum(top_left);
  // warmer/colder vs baseline
  static uint32_t lastDraw=0; (void)lastDraw;
  int delta=rssi-finderBaseline;
  const char* trend = delta>3?"HOTTER": delta<-3?"COLDER":"STEADY";
  M5.Display.setTextSize(2); M5.Display.drawString(trend,120,84);
  finderBaseline=(finderBaseline*2+rssi)/3;   // slow follow so trend reflects recent motion
}
// ---- Fixed-landscape (Cardputer) versions of the list, detail and toy screens ----
// FUD-Boy style like Home and the menus: FreeSans titles and names, Font2 detail, keys named as keys.
static const char* catName(Cat c){ for(int i=0;i<ZN;i++) if(ZCATS[i]==c) return ZNAMES[i]; return "LOG"; }
static bool pcLBegin(const char* title,uint16_t tc=AMBER){
  M5.Display.setRotation(ROT_LANDSCAPE); pcLEnsure();
  if(!pcLReady){ M5.Display.fillScreen(BG); return false; }
  pcL.fillScreen(BG); lastFramePcL=true; headerL(pcL,title,tc); return true;
}
static void hintL(const char* t){
  auto& g=pcL; g.setFont(&fonts::Font2); g.setTextColor(DIM_L,BG); g.setTextDatum(bottom_left);
  g.drawString(t,4,134); g.setFont(&fonts::Font0); g.setTextDatum(top_left);
}
// The warning before Silent goes OFF (Settings > Silent, Enter).
static void drawSilentWarnL(){
  bool w=setWarn==2;   // 2 = Wardrive ON? (same screen shape)
  if(!pcLBegin(w?"WARDRIVE ON?":"SILENT OFF?",LK_RED)) return;
  auto& g=pcL; g.setFont(&fonts::Font2); g.setTextColor(AMBER,BG); g.setTextDatum(top_left);
  static const char* W[]={"Wi-Fi scans will send probe requests.","Nearby receivers can log this device","being here, and where.","Bluetooth stays listen-only."};
  static const char* D[]={"Saves each Wi-Fi network heard,","with GPS position, to the SD card.","The file shows where you went.","Nothing is uploaded or sent."};
  for(int i=0;i<4;i++) g.drawString((w?D:W)[i],4,HDR_L+6+i*18);
  hintL(w?"Enter = turn on    Esc = keep off":"Enter = turn off    Esc = keep silent");
  g.pushSprite(0,0);
}
// Name in 12pt, then up to three Font2 lines: what it is, its id, signal and age.
static void detailBodyL(const Det& d,uint16_t nameCol){
  auto& g=pcL; char b[64];
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextColor(nameCol,BG); g.setTextDatum(top_left);
  fitString(g,d.name[0]?d.name:d.label,4,HDR_L+3,232);
  g.setFont(&fonts::Font2); g.setTextColor(AMBER,BG);
  snprintf(b,sizeof(b),"%s  %s  conf %c",catTag[d.cat],d.label,d.conf); fitString(g,b,4,HDR_L+31,232);
  snprintf(b,sizeof(b),"%02X:%02X:%02X:%02X:%02X:%02X",d.key[0],d.key[1],d.key[2],d.key[3],d.key[4],d.key[5]); g.drawString(b,4,HDR_L+47);
  snprintf(b,sizeof(b),"%d dBm   x%u   seen %lus ago",d.rssi,d.hits,(unsigned long)((millis()-d.lastSeen)/1000)); fitString(g,b,4,HDR_L+63,232);
  g.setFont(&fonts::Font0);
}
static void drawListL(){
  static int idxs[MAXDET]; int n; logIdxs(n,idxs);
  char title[24]; snprintf(title,sizeof(title),"%s  %d",logFilter<0?"LOG":catName((Cat)logFilter),n);
  if(n==0){ static const char* none[]={"nothing here"}; static const uint16_t dim[]={DIM_L}; menuL(title,none,nullptr,1,-1,dim); return; }
  if(listSel>=n) listSel=n-1;
  struct F{ static void row(int r,char* L,char* R,uint16_t& c){ const Det& d=dets[idxs[r]]; const char* nm=d.name[0]?d.name:d.label;
    snprintf(L,ROWL,"%s %s",catTag[d.cat],nm);
    if(d.cat==CAT_TRACKER) snprintf(R,ROWR,"%lum",(unsigned long)((millis()-d.firstSeen)/60000));
    else if(d.hits>1) snprintf(R,ROWR,"x%u",(unsigned)(d.hits>999?999:d.hits));
    c=d.alarm?LK_RED:AMBER; } };
  menuLF(title,n,listSel,F::row);
}
static void drawItemL(){
  if(curDet<0||!dets[curDet].used){ mode=MODE_LIST; return; }
  Det& d=dets[curDet]; if(!pcLBegin(catName(d.cat))) return;
  detailBodyL(d,d.alarm?LK_RED:AMBER);
  auto& g=pcL; bool tk=isTrackable(d);
  static const char* A3[]={"Ack","Ignore","Back"}; static const char* A4[]={"Ack","Ignore","Find","Back"};
  const char** acts=tk?A4:A3; int na=tk?4:3; if(itemAct>=na) itemAct=0;
  int gap=4, w=(240-8-gap*(na-1))/na, y=HDR_L+86, h=28;
  g.setFont(na>3?&fonts::FreeSans9pt7b:&fonts::FreeSansBold9pt7b); g.setTextDatum(middle_center);   // four buttons: regular weight fits "Ignore"
  for(int i=0;i<na;i++){ int x=4+i*(w+gap); bool s=(i==itemAct);
    g.fillRoundRect(x,y,w,h,5,s?AMBER:BG); g.drawRoundRect(x,y,w,h,5,AMBER);
    g.setTextColor(s?BG:AMBER,s?AMBER:BG); g.drawString(acts[i],x+w/2,y+h/2+1); }
  g.setFont(&fonts::Font0); g.setTextDatum(top_left);
  g.pushSprite(0,0);
}
static void drawAttackL(){
  bool ok=attackShow>=0&&dets[attackShow].used;
  if(!pcLBegin(ok&&dets[attackShow].cat!=CAT_HACKDEV?"ATTACK":"HACKER",LK_RED)) return;
  if(ok) detailBodyL(dets[attackShow],LK_RED);
  hintL("Enter = zoom    Esc = home");
  pcL.pushSprite(0,0);
}
static void drawSsidL(){
  char title[16]; snprintf(title,sizeof(title),"SSIDs  %d",ssidN);
  if(ssidN==0){ static const char* r[]={"scanning..."}; static const uint16_t dim[]={DIM_L}; menuL(title,r,nullptr,1,-1,dim); return; }
  if(ssidSel>=ssidN) ssidSel=ssidN-1;
  struct F{ static void row(int i,char* L,char* R,uint16_t& c){ const SsidRow& s=ssidRows[i];
    snprintf(L,ROWL,"%s",s.ssid[0]?s.ssid:"(hidden)"); snprintf(R,ROWR,"%s%d c%d",s.open?"O ":"",s.rssi,s.ch);
    c=s.ssid[0]?AMBER:DIM_L; } };
  menuLF(title,ssidN,ssidSel,F::row);
}
static void drawTvbgL(){
  if(!pcLBegin("TV-B-GONE")) return;
  auto& g=pcL; g.setTextDatum(middle_center); g.setFont(&fonts::FreeSansBold12pt7b); g.setTextColor(AMBER,BG);
  if(tvbgFiring){
    char b[24]; snprintf(b,sizeof(b),"firing %d / %d",tvbgIdx,TVBG_N); g.drawString(b,120,HDR_L+30);
    int w=200*tvbgIdx/TVBG_N; g.drawRoundRect(20,HDR_L+56,200,20,4,AMBER); if(w>4) g.fillRoundRect(22,HDR_L+58,w-4,16,3,AMBER);
    g.setFont(&fonts::Font0); g.setTextDatum(top_left); hintL("Esc = stop");
  } else {
    g.drawString(tvbgIdx>=TVBG_N?"done":"point at a TV",120,HDR_L+30);
    g.setFont(&fonts::Font2); g.setTextColor(DIM_L,BG);
    char b[32]; snprintf(b,sizeof(b),"%d power codes, IR only",TVBG_N); g.drawString(b,120,HDR_L+62);
    g.setFont(&fonts::Font0); g.setTextDatum(top_left); hintL("Enter = fire    Esc = out");
  }
  g.pushSprite(0,0);
}
static void drawFinderListL(){
  static int idxs[MAXDET]; int n=finderList(idxs,MAXDET);
  char title[16]; snprintf(title,sizeof(title),"FINDER  %d",n);
  if(n==0){ static const char* r[]={"no BLE targets yet","let it scan"}; static const uint16_t dim[]={DIM_L,DIM_L}; menuL(title,r,nullptr,2,-1,dim); return; }
  if(finderSel>=n) finderSel=n-1; if(finderSel<0) finderSel=0;
  struct F{ static void row(int r,char* L,char* R,uint16_t&){ const Det& d=dets[idxs[r]];
    if(d.name[0]&&strcmp(d.name,d.label)) snprintf(L,ROWL,"%s %s %s",catTag[d.cat],d.label,d.name);
    else snprintf(L,ROWL,"%s %s",catTag[d.cat],d.label);
    snprintf(R,ROWR,"%d",d.rssi); } };
  menuLF(title,n,finderSel,F::row);
}
static void drawFinderLockL(){
  if(!pcLBegin("FINDER")) return;
  auto& g=pcL; char b[48];
  snprintf(b,sizeof(b),"%s %s",catTag[finderCat],finderLabel);
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextColor(AMBER,BG); g.setTextDatum(top_left); fitString(g,b,4,HDR_L+3,232);
  if(!finderHas || millis()-finderSeen>4000){
    g.setFont(&fonts::Font2); g.setTextColor(DIM_L,BG);
    g.drawString("signal lost. move around,",4,HDR_L+34); g.drawString("or Esc to pick again.",4,HDR_L+50);
    g.drawString("(trackers rotate their MAC)",4,HDR_L+66);
    g.setFont(&fonts::Font0); g.pushSprite(0,0); return; }
  int rssi=finderSmooth;
  if(finderName[0]){ g.setFont(&fonts::Font2); g.setTextColor(DIM_L,BG); fitString(g,finderName,4,HDR_L+29,232); }
  int w=(rssi+100)*228/70; if(w<0)w=0; if(w>228)w=228;
  uint16_t c=rssi>-55?LK_RED:rssi>-70?0xFD20:0x07E0;
  g.drawRoundRect(4,HDR_L+48,232,20,4,AMBER); if(w>4) g.fillRoundRect(6,HDR_L+50,w-4,16,3,c);
  const char* dw = rssi>-45?"RIGHT HERE": rssi>-58?"VERY CLOSE": rssi>-70?"NEAR": rssi>-84?"FAR":"FAINT";
  int delta=rssi-finderBaseline; const char* trend = delta>3?"HOTTER": delta<-3?"COLDER":"STEADY";
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextColor(c,BG); g.setTextDatum(top_left); g.drawString(dw,4,HDR_L+74);
  g.setTextColor(AMBER,BG); g.setTextDatum(top_right); g.drawString(trend,236,HDR_L+74);
  g.setFont(&fonts::Font0); g.setTextDatum(top_left);
  snprintf(b,sizeof(b),"%d dBm    Enter = reset    Esc = back",rssi); hintL(b);
  finderBaseline=(finderBaseline*2+rssi)/3;   // slow follow so the trend reflects recent motion
  g.pushSprite(0,0);
}
static void drawFinder(){ if(finderLocked) drawFinderLock(); else drawFinderList(); }
static void redraw(){ lastFramePcL=false; switch(mode){
    case MODE_IDLE:drawIdle();break; case MODE_STATUS:drawStatus();break; case MODE_ZOOM:drawZoom();break;
    case MODE_SETTINGS:drawSettings();break; case MODE_LIST:drawList();break; case MODE_ITEM:drawItem();break;
    case MODE_ATTACK:drawAttack();break; case MODE_WATERFALL:wfInit();break; case MODE_TOYS:drawToys();break;
    case MODE_SSID:drawSsid();break; case MODE_TRAFFIC:trafInit();break; case MODE_TVBG:drawTvbg();break;
    case MODE_FINDER:drawFinder();break; case MODE_GPS:drawGps();break; } uiDirty=false; }

// ---- Home idle wheel/radar (vector only, same idea as FUD-Deck) ----
// Now and then on a calm Home, Zuse's features spin one eased turn inside the rim, or a radar
// beam sweeps the face from 12 o'clock. Any blink/glance, toast, alarm, mode change or sleep
// ends it. Costs CPU for ~1.6-2.6 s, no flash and no RAM (the frame is regenerated geometry).
enum : uint8_t { WHEEL_NONE, WHEEL_SPIN, WHEEL_SWEEP };
static uint8_t wheelKind=WHEEL_NONE, wheelWant=WHEEL_NONE;
static uint32_t wheelStart=0, wheelFrame=0, wheelNext=0;
static const uint32_t SPIN_MS=1600, SWEEP_MS=2600;
// 30-90 s: the screen dims after a while untouched, so this must come sooner to be seen.
static void wheelSchedule(){ wheelNext=millis()+30000+esp_random()%60000; }
static void wheelStop(){ if(wheelKind==WHEEL_NONE) return; wheelKind=WHEEL_NONE; g_faceAngle=0; g_faceSweep=-1; uiDirty=true; wheelSchedule(); }
static void homeAnimNow(int kind){ wheelStop(); wheelWant=(uint8_t)kind; wheelNext=millis(); }   // dev: 1 spin, 2 sweep, 0 random
static bool wheelCalm(){ uint32_t now=millis();
  return mode==MODE_IDLE && displayOn && !alarmActive() && !(toastMsg&&now<toastUntil) && g_anim==nullptr; }
static void homeAnimTick(){
  uint32_t now=millis();
  if(wheelKind!=WHEEL_NONE){
    uint32_t t=now-wheelStart, dur=(wheelKind==WHEEL_SPIN)?SPIN_MS:SWEEP_MS;
    if(!wheelCalm() || t>=dur){ wheelStop(); return; }
    if(now-wheelFrame<33) return;   // ~30 fps cap
    wheelFrame=now; float u=(float)t/dur;
    g_faceAngle = (wheelKind==WHEEL_SPIN) ? 180.0f*(1-cosf(u*(float)M_PI)) : 0;    // eased, one turn
    g_faceSweep = (wheelKind==WHEEL_SWEEP) ? fmodf(270.0f+360.0f*u,360.0f) : -1;   // from 12 o'clock
    uiDirty=true; return;
  }
  if(!wheelCalm() || (int32_t)(now-wheelNext)<0) return;
  // Start one. Hold off the blink/glance picker so its face change does not end the wheel.
  wheelKind = wheelWant ? wheelWant : ((esp_random()&1)?WHEEL_SPIN:WHEEL_SWEEP);
  wheelWant=WHEEL_NONE; wheelStart=wheelFrame=now;
  animNext=now+((wheelKind==WHEEL_SPIN?SPIN_MS:SWEEP_MS)+500);
  g_faceAngle=0; g_faceSweep=-1; uiDirty=true;
}

// ================= input =================
static bool aLong=false,bLong=false; static uint32_t lastTap=0,prevTap=0;
static void goHome(){ mode=MODE_IDLE; textZoom=false; setWarn=0; logFilter=-1; uiDirty=true; }
static void openAttack(){ attackShow=activeAttackIdx(); ackAll(true); mode=MODE_ATTACK; textZoom=false; uiDirty=true; }
// SNOOZE ALL: baseline the current environment (everything known stops alerting; NEW still alerts).
static void snoozeAll(){ for(auto&d:dets) if(d.used){ d.acked=true; d.alarm=false; d.escalated=true; } dispFace=nullptr; playRiff(RIFF_BASE,NELEM(RIFF_BASE),110); toast("BASELINED"); uiDirty=true; } // Zuse: "Your move, Flynn" (riff 5)
static void toggleMute(){ muted=!muted; prefs.putBool("mute",muted); M5.Speaker.tone(muted?500:1400,60);
  dispFace=muted?"neutralface":"grinningface"; dispUntil=millis()+DISP_MS; toast(muted?"MUTED":"SOUND ON"); uiDirty=true; }
static void flushCaches(){ for(auto&d:dets) d=Det(); cntCam=cntHak=cntAtk=cntTrk=cntDrn=cntSub=0; dispFace=nullptr; lastDetection=millis(); mode=MODE_IDLE; M5.Speaker.tone(1000,90); toast("CACHE CLEARED"); uiDirty=true; } // keeps mute + ignore
static void cycleBright(){ brightIdx=(brightIdx+1)%4; prefs.putInt("bright",brightIdx); userBright=BRIGHT_LEVELS[brightIdx]; M5.Display.setBrightness(userBright); }
// scroll down through the ladder (louder -> quieter), wrap MUTE back to MAX.
static void cycleVol(){ volIdx=(volIdx+1)%VOL_N; prefs.putInt("vol",volIdx);
  M5.Speaker.setVolume(applyVol(80)); if(VOL_PCT[volIdx]) M5.Speaker.tone(1400,90); } // beep preview at the new level
static void setSilent(bool on){ silentMode=on; setWarn=0; prefs.putBool("silent",on); toast(on?"SILENT ON":"SILENT OFF"); }
static void cycleShake(){ shakeIdx=(shakeIdx+1)%SHAKE_N; prefs.putInt("shake",shakeIdx); M5.Speaker.tone(1200,50); }
static void cycleAlertFilt(){ alertMinConf=(alertMinConf+1)%AF_N; prefs.putInt("alf",alertMinConf); M5.Speaker.tone(1200,50); }
static bool wfHas915(){ return ccPresent && B->cardputer; }
static void openWaterfall(){ if(nrfPresent||ccPresent){ wfSource=nrfPresent?0:1; mode=MODE_WATERFALL; wfInit(); uiDirty=true; } }
static void detectDoubleTap(){  // double-tap anywhere = ack + clear the current alert, go home
  if(screenLock) return;         // locked: in a pocket, a bump is not an ack
  if(M5.BtnA.isPressed()||M5.BtnB.isPressed()){ lastTap=prevTap=0; return; }
  float ax=0,ay=0,az=0; M5.Imu.getAccel(&ax,&ay,&az); float mag=sqrtf(ax*ax+ay*ay+az*az); uint32_t now=millis();
  if(fabsf(mag-1.0f)>1.8f && now-lastTap>150){ prevTap=lastTap; lastTap=now;
    if(lastTap-prevTap<600 && prevTap!=0){ prevTap=0; lastTap=0; if(alarmActive()){ ackAll(true); goHome(); } } }
}
// Gesture actions, shared by the StickS3 buttons and the Cardputer keyboard. Bodies unchanged
// from the button handler they came from.
// SIDE hold = HOME (exceptions: home->STATUS, status->baseline)
static void actSideHold(){
    if(mode==MODE_IDLE){ mode=MODE_STATUS; statusSel=0; M5.Speaker.tone(1400,40); uiDirty=true; } // home side-hold = settings/status
    else if(mode==MODE_STATUS){ snoozeAll(); }
    else { goHome(); M5.Speaker.tone(900,50); }
}
// SIDE short = BACK
static void actSideShort(){ M5.Speaker.tone(900,45);
      switch(mode){
        case MODE_IDLE:     mode=MODE_ZOOM; zoomCat=0; break;   // home side-tap = zoom
        case MODE_LIST:     if(logFilter>=0) mode=MODE_ZOOM; else goHome(); break;
        case MODE_ITEM:     mode=MODE_LIST; break;
        case MODE_SETTINGS: if(setWarn) setWarn=0; else mode=MODE_STATUS; break;   // back out of a warning changes nothing
        case MODE_TOYS:     mode=MODE_STATUS; break;
        case MODE_WATERFALL: case MODE_SSID: case MODE_TRAFFIC: case MODE_TVBG: tvbgFiring=false; mode=MODE_TOYS; break;
        case MODE_FINDER: if(finderLocked) finderLocked=false; else mode=MODE_TOYS; break;  // back: unlock to list, then out
        case MODE_GPS:      mode=MODE_TOYS; break;
        default:            goHome(); break;   // STATUS, ZOOM, ATTACK -> home
      }
  uiDirty=true;
}
// FRONT hold = SELECT (exception: home->mute; detail->zoom)
static void actFrontHold(){ int idxs[MAXDET],n; M5.Speaker.tone(1600,45);
    switch(mode){
      case MODE_IDLE: toggleMute(); break;
      case MODE_STATUS: { const char* it=STATUS_ITEMS[statusSel];
        if(!strcmp(it,"Log")){ logFilter=-1; mode=MODE_LIST; listSel=listTop=0; }
        else if(!strcmp(it,"Toys")){ mode=MODE_TOYS; toysSel=0; }
        else if(!strcmp(it,"Settings")){ mode=MODE_SETTINGS; setSel=0; setWarn=0; }
        else if(!strcmp(it,"Clear cache")){ flushCaches(); } break; }
      case MODE_TOYS: { const char* t=TOYS_ITEMS[toysSel];
        if(!strcmp(t,"Waterfall")) openWaterfall();
        else if(!strcmp(t,"SSID Scan")){ mode=MODE_SSID; ssidSel=ssidTop=0; ssidN=0; ssidLast=0; }
        else if(!strcmp(t,"Traffic")){ mode=MODE_TRAFFIC; trafPeak=trafNow=0; }
        else if(!strcmp(t,"TV-B-Gone")){ mode=MODE_TVBG; tvbgFiring=false; tvbgIdx=0; }
        else if(!strcmp(t,"GPS")){ mode=MODE_GPS; }
        else if(!strcmp(t,"Finder")){ mode=MODE_FINDER; finderLocked=false; finderSel=finderTop=0; finderHas=false; finderSmooth=finderBaseline=-100; } break; }
      case MODE_FINDER: if(!finderLocked){ int fi[MAXDET],fn=finderList(fi,MAXDET);
          if(fn) lockFinder(dets[fi[finderSel<fn?finderSel:0]]); } break;
      case MODE_ZOOM: logFilter=ZCATS[zoomCat]; mode=MODE_LIST; listSel=listTop=0; break;
      case MODE_LIST: { logIdxs(n,idxs); if(n){ curDet=idxs[listSel]; itemAct=0; mode=MODE_ITEM; } break; }
      case MODE_ITEM: { Det&d=dets[curDet]; bool tk=isTrackable(d);
        if(itemAct==0){ d.acked=true; d.alarm=false; mode=MODE_LIST; }
        else if(itemAct==1){ d.ignored=true; d.acked=true; d.alarm=false;
          if(d.ssidBased){ addIgnoreSSID(d.name); toast("SSID IGNORED"); } else { addIgnore(d.key); toast("IGNORED"); } mode=MODE_LIST; }
        else if(tk && itemAct==2){ lockFinder(d); toast("FINDING"); }   // trackable: jump to finder locked on it
        else mode=MODE_LIST;   // Back
        break; }
      case MODE_ATTACK: textZoom=!textZoom; break;        // detail zoom toggle
      case MODE_SETTINGS:
        if(setWarn==1) setSilent(false);                                // confirmed on the warning screen
        else if(setWarn==2){ wd::setOn(true); setWarn=0; toast("WARDRIVE ON"); }
        else if(setIs(setSel,"Brightness")) cycleBright(); else if(setIs(setSel,"Shake")) cycleShake(); else if(setIs(setSel,"Volume")) cycleVol(); else if(setIs(setSel,"Alert Filt")) cycleAlertFilt();
        else if(setIs(setSel,"Silent")){ if(silentMode) setWarn=1; else setSilent(true); }   // OFF warns first, ON is instant
        else if(setIs(setSel,"Wardrive")){ if(!wd::on) setWarn=2; else { wd::setOn(false); toast("WARDRIVE OFF"); } }   // ON warns first
        else if(setIs(setSel,"SD Log")){ sdlog::setOn(!sdlog::on); toast(sdlog::on?"SD LOG ON":"SD LOG OFF"); }
        else { if(g_songOn) stopSong(); else playSong(); } break;
      default: break;
    }
  uiDirty=true;
}
// FRONT short = NEXT (context: home alert->attack/else log; detail zoom exits on a tap)
static void actFrontShort(){ int idxs[MAXDET],n; M5.Speaker.tone(1300,35);
      switch(mode){
        case MODE_IDLE: if(alarmActive()) openAttack(); else { logFilter=-1; mode=MODE_LIST; listSel=listTop=0; } break;
        case MODE_STATUS: statusSel=(statusSel+1)%STATUS_N; break;
        case MODE_TOYS: toysSel=(toysSel+1)%TOYS_N; break;
        case MODE_SSID: if(ssidN) ssidSel=(ssidSel+1)%ssidN; break;
        case MODE_TVBG: if(!tvbgFiring){ tvbgFiring=true; tvbgIdx=0; } break;   // FRONT = fire
        case MODE_FINDER: if(finderLocked) finderBaseline=finderSmooth;        // FRONT: reset trend, or next target
                          else { int fi[MAXDET],fn=finderList(fi,MAXDET); if(fn) finderSel=(finderSel+1)%fn; } break;
        case MODE_ZOOM: zoomCat=(zoomCat+1)%ZN; break;
        case MODE_SETTINGS: if(!setWarn) setSel=(setSel+1)%SET_N; break;
        case MODE_LIST: { logIdxs(n,idxs); if(n) listSel=(listSel+1)%n; break; }
        case MODE_ITEM: { int na=(curDet>=0&&isTrackable(dets[curDet]))?4:3; itemAct=(itemAct+1)%na; break; }
        case MODE_ATTACK: if(textZoom) textZoom=false; else goHome(); break;
        case MODE_WATERFALL: do{ wfSource=(wfSource+1)%3; }while((wfSource==0&&!nrfPresent)||(wfSource==1&&!ccPresent)||(wfSource==2&&!wfHas915())); if(wfReady)wfc.fillScreen(0); wfInit(); break;
        default: break;
      }
  uiDirty=true;
}
static void buttons(){
  // BOTH held 10 s = hard reset
  static uint32_t bothSince=0; static bool bothFired=false, lockFired=false;
  if(M5.BtnA.isPressed()&&M5.BtnB.isPressed()){
    if(bothSince==0) bothSince=millis();
    if(millis()-bothSince>=1000 && !lockFired){ lockFired=true; if(!screenLock) setLock(true); }   // both 1 s = lock
    if(millis()-bothSince>=10000 && !bothFired){ bothFired=true; screenLock=false; flushCaches(); wake(); }   // reset also unlocks
    aLong=bLong=true; return;
  } else { bothSince=0; bothFired=false; lockFired=false; }
  if(screenLock){   // any button unlocks; that press does nothing else (its release is swallowed)
    bool a=M5.BtnA.wasPressed(), b=M5.BtnB.wasPressed();
    if(M5.BtnA.wasReleased()) aLong=false; if(M5.BtnB.wasReleased()) bLong=false;   // the lock gesture letting go
    if((a||b) && lockDebounced()){ if(a) aLong=true; if(b) bLong=true; setLock(false); }
    return; }

  if(M5.BtnB.pressedFor(600)&&!bLong){ bLong=true; actSideHold(); return; }
  if(M5.BtnB.wasReleased()){ if(!bLong) actSideShort(); bLong=false; }
  if(M5.BtnA.pressedFor(500)&&!aLong){ aLong=true; actFrontHold(); return; }
  if(M5.BtnA.wasReleased()){ if(!aLong) actFrontShort(); aLong=false; }
}

// ================= keyboard (Cardputer ADV: TCA8418 on the internal I2C, polled) =================
// D-pad moves, Enter selects, Esc back, Q home, M mute, A ack (baseline on Status).
// Codes are the TCA8418's raw key numbers, captured on the bench 2026-11-19.
static const uint8_t TCA_ADDR=0x34;
enum : uint8_t { KC_UP=57, KC_DOWN=58, KC_LEFT=54, KC_RIGHT=64, KC_ENTER=67, KC_ESC=1, KC_Q=6, KC_M=48, KC_A=13, KC_L=53 };   // L: from the matrix pattern (row = col/2, col = row + 4*(col odd))
static bool kbOk=false; static uint8_t kbHeld=0; static uint32_t kbRepAt=0;
static bool tcaW(uint8_t r,uint8_t v){ return M5.In_I2C.writeRegister8(TCA_ADDR,r,v,400000); }
static uint8_t tcaR(uint8_t r){ return M5.In_I2C.readRegister8(TCA_ADDR,r,400000); }
static void kbBegin(){
  if(!B->cardputer) return;
  kbOk=M5.In_I2C.scanID(TCA_ADDR,400000);
  if(!kbOk){ Serial.println("Keyboard: TCA8418 not found"); return; }
  tcaW(0x1D,0x7F); tcaW(0x1E,0xFF); tcaW(0x1F,0x00);   // 7 rows x 8 columns
  tcaW(0x01,0x01);                                     // key events into the FIFO
  for(int i=0;i<12 && (tcaR(0x03)&0x0F);i++) tcaR(0x04);
  tcaW(0x02,0x1F);
  Serial.println("Keyboard: TCA8418 ready");
}
// Move the selection by d (+1 / -1) in whatever list is on screen. Wraps, like the Stick.
static void moveSel(int d){ int idxs[MAXDET],n;
  switch(mode){
    case MODE_STATUS:   statusSel=(statusSel+d+STATUS_N)%STATUS_N; break;
    case MODE_TOYS:     toysSel=(toysSel+d+TOYS_N)%TOYS_N; break;
    case MODE_ZOOM:     zoomCat=(zoomCat+d+ZN)%ZN; break;
    case MODE_SETTINGS: if(setWarn) return; setSel=(setSel+d+SET_N)%SET_N; break;
    case MODE_LIST:     logIdxs(n,idxs); if(n) listSel=(listSel+d+n)%n; break;
    case MODE_ITEM:   { int na=(curDet>=0&&isTrackable(dets[curDet]))?4:3; itemAct=(itemAct+d+na)%na; break; }
    case MODE_SSID:     if(ssidN) ssidSel=(ssidSel+d+ssidN)%ssidN; break;
    case MODE_FINDER:   if(!finderLocked){ int fi[MAXDET],fn=finderList(fi,MAXDET); if(fn) finderSel=(finderSel+d+fn)%fn; } break;
    default: return;
  }
  M5.Speaker.tone(1300,20); uiDirty=true;
}
static void keyAct(uint8_t k){
  switch(k){
    case KC_Q: if(mode!=MODE_IDLE){ tvbgFiring=false; finderLocked=false; goHome(); M5.Speaker.tone(900,50); } return;
    case KC_M: toggleMute(); return;
    case KC_A:
      if(mode==MODE_STATUS){ snoozeAll(); return; }                  // Status: baseline (Stick: side hold)
      if(alarmActive()){ ackAll(true); goHome(); toast("ACKED"); }   // Stick: double-tap
      return;
    case KC_ESC:
      if(mode==MODE_IDLE){ mode=MODE_STATUS; statusSel=0; M5.Speaker.tone(1400,40); uiDirty=true; return; }
      if(mode==MODE_LIST && logFilter>=0){ goHome(); M5.Speaker.tone(900,45); return; }   // tile list: back to Home
      actSideShort(); return;
  }
  if(mode==MODE_IDLE){
    if(alarmActive()){ if(k==KC_ENTER){ M5.Speaker.tone(1300,35); openAttack(); } return; }
    int r=homeSel/2, c=homeSel%2;                                    // 2 x 3 tile grid
    if(k==KC_UP) r=(r+2)%3; else if(k==KC_DOWN) r=(r+1)%3; else if(k==KC_LEFT||k==KC_RIGHT) c^=1;
    else if(k==KC_ENTER){ M5.Speaker.tone(1600,45); logFilter=ZCATS[homeSel]; mode=MODE_LIST; listSel=listTop=0; uiDirty=true; return; }
    homeSel=r*2+c; M5.Speaker.tone(1300,20); uiDirty=true; return;
  }
  if(k==KC_UP||k==KC_LEFT){ moveSel(-1); return; }
  if(k==KC_DOWN||k==KC_RIGHT){
    if(mode==MODE_WATERFALL){ actFrontShort(); return; }             // next band
    moveSel(+1); return;
  }
  if(k==KC_ENTER){
    if(mode==MODE_TVBG||mode==MODE_WATERFALL||(mode==MODE_FINDER&&finderLocked)){ actFrontShort(); return; }   // fire, band, reset trend
    actFrontHold(); return;
  }
}
static void keysPoll(){
  if(!kbOk) return;
  uint32_t now=millis();
  int n=tcaR(0x03)&0x0F;
  for(int i=0;i<n;i++){
    uint8_t ev=tcaR(0x04); if(!ev) break;
    uint8_t code=ev&0x7F; bool down=ev&0x80;
    if(!down){ if(code==kbHeld) kbHeld=0; continue; }
    if(screenLock){ kbHeld=0; if(lockDebounced()) setLock(false); continue; }   // locked: any key unlocks, nothing else
    if(code==KC_L){ kbHeld=0; setLock(true); continue; }             // L = lock, any screen
    if(!displayOn){ wake(holdForMode()); continue; }                  // the first press only wakes the screen
    wake(holdForMode());
    bool arrow=(code==KC_UP||code==KC_DOWN||code==KC_LEFT||code==KC_RIGHT);
    kbHeld=arrow?code:0; kbRepAt=now+350;
    keyAct(code);
  }
  if(n) tcaW(0x02,0x01);
  if(kbHeld && !screenLock && now>=kbRepAt && mode!=MODE_IDLE){ kbRepAt=now+120; wake(holdForMode()); keyAct(kbHeld); }   // held arrow repeats in lists
}

// Bench serial telemetry. Included here, after the globals it reads, so it stays a
// single self-contained unit (state / shot / ver / help; fake only under FUD_BENCH).
#include "serial_telemetry.h"

// ================= setup / loop =================
#if !defined(FUD_RADIOTEST) && !defined(FUD_ANIMTEST)
void setup(){
  auto cfg=M5.config(); cfg.internal_imu=true; cfg.internal_spk=true; M5.begin(cfg);
  B=&boardDetect();
  ROT_PORTRAIT=B->rotPortrait; ROT_LANDSCAPE=B->rotLandscape;
  setCpuFrequencyMhz(160);            // cooler + lighter; plenty for our load
  M5.Display.setBrightness(BRIGHT_NORMAL);
  M5.Speaker.setVolume(applyVol(80)); Serial.begin(115200);
  if(B->irTx>=0){ irTx=new IRsend(B->irTx); irTx->begin(); }   // TV-B-Gone IR blaster (pin set up, nothing sent)
  prefs.begin("locket",false); loadIgnore();   // NVS namespace kept as-is so saved ignore list / mute / volume survive the rename
  muted = prefs.getBool("mute", false);   // remember mute across reboots
  silentMode = prefs.getBool("silent", true);   // default ON: passive scans only
  kbBegin();   // Cardputer keyboard (no-op elsewhere)
  clk::begin();
  if(B->groveA>=0){ gps::begin(B->groveA,B->groveB); TOYS_N=6; }   // Grove GPS (listen only)
#ifdef FUD_DEV_MUTE
  muted = true;   // bench builds start muted during development (not saved). Remove the flag when done.
#endif
  volIdx = prefs.getInt("vol", 2); if(volIdx<0||volIdx>=VOL_N) volIdx=2; M5.Speaker.setVolume(applyVol(80)); // remember volume
  brightIdx = prefs.getInt("bright", 0); if(brightIdx<0||brightIdx>=4) brightIdx=0;   // remember brightness
  userBright=BRIGHT_LEVELS[brightIdx]; M5.Display.setBrightness(userBright);
  shakeIdx = prefs.getInt("shake", 1); if(shakeIdx<0||shakeIdx>=SHAKE_N) shakeIdx=1;   // remember wake sensitivity
  alertMinConf = prefs.getInt("alf", 0); if(alertMinConf<0||alertMinConf>=AF_N) alertMinConf=0; // remember alert filter
  WiFi.mode(WIFI_STA); WiFi.disconnect(); esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // modem sleep when idle
  NimBLEDevice::init("");
  // Probe the RF backpack (optional). Its scanners run only if it is present, so the same
  // build works on a bare StickS3 or Cardputer ADV. Unknown board: no probe, pins untouched.
  if(B->known){
    RF_SCK=B->sck; RF_MISO=B->miso; RF_MOSI=B->mosi; CC_CS=B->ccCs; NRF_CSN=B->nrfCsn; NRF_CE=B->nrfCe;
    if(B->sdCs>=0){ pinMode(B->sdCs,OUTPUT); digitalWrite(B->sdCs,HIGH); }   // SD shares the bus: keep it deselected
    pinMode(CC_CS,OUTPUT);  csHi(CC_CS);
    pinMode(NRF_CSN,OUTPUT); csHi(NRF_CSN);
    pinMode(NRF_CE,OUTPUT);  digitalWrite(NRF_CE,LOW);
    rfspi.begin(RF_SCK,RF_MISO,RF_MOSI,-1);
    ccPresent=ccProbe(); nrfPresent=nrfProbe();
  }
  sdlog::begin(); wd::begin(); setRowsBuild();   // before the scan task: the bus is still ours alone
  Serial.printf("Board: %s  PSRAM %u KB  display %dx%d\n", B->name, (unsigned)(ESP.getPsramSize()/1024),
                (int)M5.Display.width(), (int)M5.Display.height());
  Serial.printf("RF Pack: CC1101=%s nRF24=%s\n", ccPresent?"yes":"no", nrfPresent?"yes":"no");
  evtQ=xQueueCreate(96,sizeof(Evt));
  radioMtx=xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(scanTask,"scan",8192,nullptr,1,nullptr,0);
  lastDetection=millis(); lastWake=millis(); redraw();
  playRiff(RIFF_BOOT,NELEM(RIFF_BOOT),185,3);   // Zuse: "Greetings Programs" x3 (louder)
  Serial.printf("FUD Firmware %s up. Find Unwelcome Devices.\n", FUD_VERSION);
  Serial.println("[telem] ready: state, shot, ver, help");
}
void loop(){
  M5.update();
  telem::poll();   // bench serial: state / shot / ver (read-only; fake under FUD_BENCH)
#ifdef FUD_BENCH
  telem::heapWatch();
#endif
  serviceRiff();   // advance Zuse's current 8-bit phrase (non-blocking)
  bool btnEdge = M5.BtnA.wasPressed()||M5.BtnB.wasPressed()||M5.BtnA.wasReleased()||M5.BtnB.wasReleased();
  // (song stops via Settings toggle or when it finishes; navigation no longer kills it)
  buttons(); keysPoll(); detectDoubleTap(); gps::poll();
  if(btnEdge && !screenLock) wake(holdForMode());   // button activity keeps the screen on (long in menus/waterfall)
  Evt e; int drained=0; while(drained<32 && xQueueReceive(evtQ,&e,0)==pdTRUE){ ingest(e); drained++; }
  uint32_t now=millis();
  if(restoreVolAt && now>restoreVolAt){ M5.Speaker.setVolume(applyVol(80)); restoreVolAt=0; } // back to UI volume after an alarm

  // SSID scanner: refresh the network list every ~3 s while viewing
  if(mode==MODE_SSID && displayOn && now-ssidLast>3000){ RadioLock L(0); if(L.ok){ ssidLast=now; scanSsids(); uiDirty=true; } }   // busy: try next loop

  // Waterfall/Traffic sprite (240 x ~117): without PSRAM it is ~28 KB of internal heap, so give it
  // back as soon as neither toy is up (the Cardputer's Wi-Fi phase needs that room).
  if(!ESP.getPsramSize() && mode!=MODE_WATERFALL && mode!=MODE_TRAFFIC){ if(wfReady){ wfc.deleteSprite(); wfReady=false; } wfTried=false; }
  // SSID Scan leaves Wi-Fi on between its refreshes; turn it off once that screen closes.
  static bool ssidWifi=false;
  if(mode==MODE_SSID) ssidWifi=true;
  else if(ssidWifi){ RadioLock L(0); if(L.ok){ if(!toyMode()) WiFi.mode(WIFI_OFF); ssidWifi=false; } }
  // A deferred sprite: create it (and redraw the toy) once the radio lock is ours.
  auto wfLate=[&](){ if(wfReady||wfTried||ESP.getPsramSize()) return; wfMayAlloc=true; if(mode==MODE_WATERFALL) wfInit(); else trafInit(); wfMayAlloc=false; };
  // live waterfall: drive the selected radio and render fast. Keep the screen
  // awake the whole time (you are actively watching it).
  if(mode==MODE_WATERFALL){
    if(alarmActive()){ mode=MODE_IDLE; uiDirty=true; }   // a serious threat pulls you out to the red alert
    else {
      wake(INTERACT_TIMEOUT); lastWake=now;              // never sleep while watching
      static uint32_t lastWf=0;
      if(now-lastWf>=25){ RadioLock L(0); if(L.ok){ lastWf=now; wfLate(); if(wfReady){ if(wfSource==0) nrfSweepWF(); else ccSweepWF(); wfRender(); } } } // fast; busy = skip a frame
    }
  }

  // traffic graph: sample + scroll the rolling 2.4 GHz busyness line
  if(mode==MODE_TRAFFIC){
    if(alarmActive()){ mode=MODE_IDLE; uiDirty=true; }
    else { wake(INTERACT_TIMEOUT); lastWake=now;
      static uint32_t lastTr=0; if(now-lastTr>=150){ RadioLock L(0); if(L.ok){ lastTr=now; wfLate(); trafStep(); } } }
  }
  // TV-B-Gone: fire one code per tick so BACK stays responsive
  if(mode==MODE_TVBG){
    wake(INTERACT_TIMEOUT); lastWake=now;
    if(tvbgFiring && now-tvbgLast>=110){ tvbgLast=now;
      if(tvbgIdx<TVBG_N){ tvbgSend(TVBG_CODES[tvbgIdx]); tvbgIdx++; uiDirty=true; }
      else { tvbgFiring=false; M5.Speaker.tone(1200,80); uiDirty=true; } }
  }
  // GPS screen: refresh once a second and stay awake (you are walking with it).
  if(mode==MODE_GPS){ wake(INTERACT_TIMEOUT); lastWake=now;
    static uint32_t lastG=0; if(now-lastG>1000){ lastG=now; uiDirty=true; } }
  // finder: keep the screen awake. Locked -> scan task publishes RSSI + uiDirty.
  // Unlocked -> refresh the picklist from the cache every ~1.5 s.
  if(mode==MODE_FINDER){
    if(alarmActive()){ mode=MODE_IDLE; finderLocked=false; uiDirty=true; }
    else { wake(INTERACT_TIMEOUT); lastWake=now;
      if(!finderLocked){ static uint32_t lastFl=0; if(now-lastFl>1500){ lastFl=now; uiDirty=true; } } }
  }

  // wake on deliberate handling (rotate/pick up); sleep the backlight after its hold
  { float ax,ay,az; M5.Imu.getAccel(&ax,&ay,&az);
    float dm=fabsf(ax-prevAx)+fabsf(ay-prevAy)+fabsf(az-prevAz); prevAx=ax;prevAy=ay;prevAz=az;
    if(dm>SHAKE_LEVELS[shakeIdx] && !screenLock) wake(holdForMode()); }
  // millis(), not now: a wake() above stamps lastWake after now was read, and now-lastWake would
  // wrap to ~4e9 and blank the screen the instant it woke (the shake flicker).
  if(lockMsgUntil && now>=lockMsgUntil){ lockMsgUntil=0; M5.Display.setBrightness(0); displayOn=false; }   // lock notice done: dark
  if(displayOn && millis()-lastWake>wakeHold){ M5.Display.setBrightness(0); displayOn=false; }

  static uint32_t lastHk=0; if(now-lastHk>10000){ lastHk=now; housekeep(); }

  // active hostile: beep + flash every 15 s
  if(alarmActive() && !muted && now-lastAlarmBeep>ALARM_BEEP_MS){ lastAlarmBeep=now; alarmSound(); wake(ALERT_PEEK); } // sound only when not muted; face persists via alarmActive
  // acked hostile still present: silent reminder every ~7 min
  if(!alarmActive() && ackedAttackPresent() && now-lastReshow>RESHOW_MS){ lastReshow=now; if(mode==MODE_IDLE){ dispFace="redface"; dispUntil=now+2000; uiDirty=true; } }

  // idle life: animate Zuse's face while the home screen is up and calm
  if(mode==MODE_IDLE && displayOn && !alarmActive()){
    serviceAnim();
    if(!g_anim && wheelKind==WHEEL_NONE && now>animNext){
      if(now-lastDetection > 5*60*1000) playAnim(A_YAWN,NELEM(A_YAWN));       // bored
      else { int pick=(now/1000)%4;
        if(pick==0) playAnim(A_GLANCE,NELEM(A_GLANCE));                        // scan the room
        else if(pick==1) playAnim(A_FLICK,NELEM(A_FLICK));                     // derez flicker
        else playAnim(A_BLINK,NELEM(A_BLINK)); }                              // blink
      animNext=now+6000+(now%6000);   // 6-12 s
    }
    static uint32_t lastHomeRefresh=0; if(now-lastHomeRefresh>1000){ lastHomeRefresh=now; uiDirty=true; } // keep radio letter + counts live
  }
  // homeAnimTick();   // DISABLED pending a hang investigation: the wheel/radar render path hung the main loop
  // redraw only when the resolved idle face actually changes
  static const char* lastFace=""; if(mode==MODE_IDLE && displayOn){ const char* f=currentIdleFace(); if(f!=lastFace){ lastFace=f; uiDirty=true; } }
  // clear a finished toast
  static bool toastWas=false; bool toastNow=(toastMsg&&now<toastUntil); if(toastWas&&!toastNow) uiDirty=true; toastWas=toastNow;

  if(mode==MODE_ATTACK && textZoom && displayOn) uiDirty=true; // keep the zoom name scrolling
  // Redraw on change, but never over the waterfall (it renders itself, and a
  // full redraw would wipe it mid-sweep).
  if(uiDirty){ if(displayOn && mode!=MODE_WATERFALL && !lockMsgUntil) redraw(); uiDirty=false; }   // the lock notice stays put
  delay(displayOn?5:25);   // poll slower when the screen is off (saves power)
}
#else
#ifdef FUD_RADIOTEST
// ================ Display + concurrent-radio power test (FUD_RADIOTEST) ================
// A BACKGROUND task (core 0) runs ALL radios concurrently and continuously (BLE non-blocking +
// WiFi coexistence + RF Pack SPI radios). The MAIN loop (core 1) redraws the normal face at a
// stepped rate: off, 1 Hz, 4 Hz, then continuous (animation rate). So display current and radio
// current hit the 3.3 V rail AT THE SAME TIME on different cores, which is what browned out. The
// continuous step should reproduce the crash (test validation); if 1/4 Hz survive and only CONT
// reboots, normal UI is safe under concurrent all-radio scanning. RTC-tracked; resumes past a crash.
RTC_NOINIT_ATTR static uint32_t rtMagic;
RTC_NOINIT_ATTR static int rtStep;
RTC_NOINIT_ATTR static uint32_t rtSecs;
static volatile unsigned long rtScanCycles=0;
static void rtScanTask(void*){
  NimBLEScan* s=NimBLEDevice::getScan(); s->setActiveScan(false); s->setInterval(160); s->setWindow(80); s->setMaxResults(0);
  uint32_t lastWifi=0;
  for(;;){
    if(!s->isScanning()) s->start(6000,false);                         // BLE: non-blocking, continuous
    if(millis()-lastWifi>4000){ lastWifi=millis(); scanWifiOnce(); }   // WiFi (coexists with BLE)
    if(nrfPresent) nrfScan24();
    if(ccPresent)  ccScanSubGhz();
    Evt e; while(xQueueReceive(evtQ,&e,0)==pdTRUE){}                   // discard: no alerts
    rtScanCycles++;
  }
}
struct RtStep { const char* name; int dispMs; };   // dispMs: 0 = display off, <0 = continuous, else redraw interval ms
static const RtStep RT[] = {
  {"all radios, display OFF",   0},
  {"all radios, display 1Hz", 1000},
  {"all radios, display 4Hz",  250},
  {"all radios, display CONT",  -1},
};
void setup(){
  auto cfg=M5.config(); cfg.internal_imu=false; cfg.internal_spk=false; M5.begin(cfg);
  setCpuFrequencyMhz(160);
  Serial.begin(115200); delay(400);
  Serial.printf("\n[radiotest] boot reset_reason=%d (1=power 4=panic 5=intwdt 6=taskwdt 9=brownout 11=usb)\n",(int)esp_reset_reason());
  int startStep=0;
  if(rtMagic==0xF00DBEEF){ Serial.printf("[radiotest] *** REBOOTED during step %d at ~%lus, resuming after ***\n", rtStep, (unsigned long)rtSecs); startStep=rtStep; }
  rtMagic=0xF00DBEEF;
  pinMode(CC_CS,OUTPUT);   csHi(CC_CS);
  pinMode(NRF_CSN,OUTPUT); csHi(NRF_CSN);
  pinMode(NRF_CE,OUTPUT);  digitalWrite(NRF_CE,LOW);
  rfspi.begin(RF_SCK,RF_MISO,RF_MOSI,-1);
  ccPresent=ccProbe(); nrfPresent=nrfProbe();
  Serial.printf("[radiotest] RF Pack: CC1101=%d nRF24=%d\n", ccPresent, nrfPresent);
  NimBLEDevice::init("");
  evtQ=xQueueCreate(96,sizeof(Evt));
  WiFi.mode(WIFI_STA); WiFi.disconnect(); esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  xTaskCreatePinnedToCore(rtScanTask,"rtscan",8192,nullptr,1,nullptr,0);   // all radios on core 0, continuous
  const int RT_N=sizeof(RT)/sizeof(RT[0]);
  if(startStep>=RT_N){ Serial.println("[radiotest] ===== DONE: last step crashed; all attempted ====="); rtMagic=0; }
  for(int step=startStep; step<RT_N; step++){
    const RtStep& m=RT[step]; rtStep=step+1; rtSecs=0;
    Serial.printf("[radiotest] ===== STEP %s (display + all radios concurrent) =====\n", m.name);
    M5.Display.setBrightness(m.dispMs==0 ? 0 : BRIGHT_NORMAL);
    uint32_t t0=millis(), lastHb=0, lastDraw=0; unsigned long draws=0;
    while(millis()-t0 < 65000){
      if(m.dispMs!=0){ uint32_t iv=(m.dispMs<0)?0:(uint32_t)m.dispMs;
        if(millis()-lastDraw>=iv){ lastDraw=millis(); g_faceAngle=0; g_faceSweep=-1; drawIdle(); draws++; } }  // normal static-face redraw, core 1
      rtSecs=(millis()-t0)/1000;
      if(millis()-lastHb>2000){ lastHb=millis();
        Serial.printf("[radiotest] step %d t=%2lus draws=%lu scanCyc=%lu heap=%u min=%u\n",
          step+1,(unsigned long)rtSecs,draws,rtScanCycles,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMinFreeHeap()); }
      delay(m.dispMs<0?1:5);
    }
    Serial.printf("[radiotest] step %d DONE, survived 65s (%lu draws, scanCyc=%lu)\n", step+1, draws, rtScanCycles);
  }
  rtMagic=0;
  Serial.println("[radiotest] ===== ALL STEPS PASSED, no reboot =====");
}
void loop(){ delay(1000); }

#else
// ===================== Animation-alone test (FUD_ANIMTEST) =====================
// Radios OFF (no WiFi, no BLE, no scan task; the backpack sits idle but attached). Runs the
// continuous full-screen rotated redraw (the exact draw that crashed with radios on), first at
// 160 MHz, then at 240 MHz, 60 s+ each. Isolates the display workload from the radios, and tests
// whether the clock is the lever (FUD-Deck runs 240 MHz and redraws only the face). RTC-tracked,
// resumes past a step that reboots.
RTC_NOINIT_ATTR static uint32_t atMagic;
RTC_NOINIT_ATTR static int atStep;
RTC_NOINIT_ATTR static uint32_t atSecs;
struct AtStep { const char* name; int mhz; };
static const AtStep AT[] = { {"160MHz full-redraw",160}, {"240MHz full-redraw",240} };
void setup(){
  auto cfg=M5.config(); cfg.internal_imu=false; cfg.internal_spk=false; M5.begin(cfg);
  Serial.begin(115200); delay(400);
  Serial.printf("\n[animtest] boot reset_reason=%d (1=power 4=panic 5=intwdt 6=taskwdt 9=brownout 11=usb)\n",(int)esp_reset_reason());
  int start=0;
  const int AT_N=sizeof(AT)/sizeof(AT[0]);
  if(atMagic==0x0A417E57){ Serial.printf("[animtest] *** REBOOTED during step %d (%s) at ~%lus, resuming after ***\n", atStep, (atStep>=1&&atStep<=AT_N)?AT[atStep-1].name:"?", (unsigned long)atSecs); start=atStep; }
  atMagic=0x0A417E57;
  WiFi.mode(WIFI_OFF);                 // radios OFF: isolate the display workload
  M5.Display.setBrightness(BRIGHT_NORMAL);
  if(start>=AT_N){ Serial.println("[animtest] ===== DONE: last step crashed; all attempted ====="); atMagic=0; }
  for(int step=start; step<AT_N; step++){
    atStep=step+1; atSecs=0;
    setCpuFrequencyMhz(AT[step].mhz);
    Serial.printf("[animtest] ===== STEP %d %s (cpu now %dMHz) =====\n", step+1, AT[step].name, getCpuFrequencyMhz());
    uint32_t t0=millis(), lastHb=0; unsigned long frames=0;
    while(millis()-t0 < 65000){
      float u=((millis()-t0)%2000)/2000.0f;              // 2 s spin cycle
      g_faceAngle=180.0f*(1-cosf(u*(float)M_PI)); g_faceSweep=-1;
      drawIdle();                                        // full-screen rotated redraw + push
      frames++;
      atSecs=(millis()-t0)/1000;
      if(millis()-lastHb>2000){ lastHb=millis();
        Serial.printf("[animtest] step %d t=%2lus frames=%lu cpu=%d heap=%u min=%u\n",
          step+1,(unsigned long)atSecs,frames,getCpuFrequencyMhz(),(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMinFreeHeap()); }
    }
    g_faceAngle=0; g_faceSweep=-1;
    Serial.printf("[animtest] step %d DONE, survived 65s (%lu frames)\n", step+1, frames);
  }
  atMagic=0;
  Serial.println("[animtest] ===== ALL STEPS PASSED (animation alone, radios off) =====");
}
void loop(){ delay(1000); }
#endif
#endif

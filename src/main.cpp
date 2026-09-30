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
// Onboard IR LED. Verified pin from earlier hardware probe; if TVs do not
// respond, this is the value to change.
#define IR_TX_PIN 46
IRsend irTx(IR_TX_PIN);

#define FUD_VERSION "v0.40b"
#include "generated/image_assets.h"

static const uint16_t AMBER  = 0xFFE0;
static const uint16_t LK_RED = 0xF800;
static const uint16_t BG     = 0x0000;
static const uint8_t  ROT_PORTRAIT  = 0;
static const uint8_t  ROT_LANDSCAPE = 1;

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
  uint32_t firstSeen=0,lastSeen=0; uint16_t hits=0; int8_t rssi=0;
  char conf='M';   // signature confidence: 'H' high, 'M' medium, 'L' low
  bool acked=false, alarm=false, ignored=false, escalated=false, ssidBased=false, active=false;
};
static const int MAXDET=128;
Det dets[MAXDET];
uint32_t cntCam=0,cntHak=0,cntAtk=0,cntTrk=0,cntDrn=0,cntSub=0;
bool ccPresent=false, nrfPresent=false;   // RF Pack radios detected at boot
volatile char activeRadio='-';            // B=BLE W=WiFi S=subGHz N=nRF24  (one at a time)
uint32_t lastDetection=0;
bool muted=false;   // MUTE: ignore everything (silent + calm), keep logging

struct Evt { uint8_t key[6]; Cat cat; char label[16]; char name[32]; int8_t rssi; bool ssid; bool active; char conf; };
QueueHandle_t evtQ;

enum Mode { MODE_IDLE, MODE_STATUS, MODE_ZOOM, MODE_LIST, MODE_ITEM, MODE_ATTACK, MODE_WATERFALL, MODE_SETTINGS,
            MODE_TOYS, MODE_SSID, MODE_TRAFFIC, MODE_TVBG, MODE_FINDER };
Mode mode=MODE_IDLE;
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
static const uint8_t  BRIGHT_NORMAL=BRIGHT_LEVELS[0];    // default = HIGH (resets here on reboot)
static const uint32_t SCREEN_TIMEOUT=15000;    // idle scanning: sleep quickly
static const uint32_t INTERACT_TIMEOUT=180000; // in a menu / watching a waterfall: 3 min
static const uint32_t ALERT_PEEK=5000;         // a threat lights the screen this long
static const float    WAKE_MOTION=0.5f;        // accel delta to count as a deliberate handle
bool displayOn=true; uint32_t lastWake=0, wakeHold=SCREEN_TIMEOUT;
float prevAx=0,prevAy=0,prevAz=1;
uint8_t userBright=BRIGHT_NORMAL;   // runtime brightness (resets to default on reboot)

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
static const ImageAsset* findImg(const char* n){ for(int i=0;i<IMAGE_COUNT;i++) if(!strcmp(IMAGES[i].name,n)) return &IMAGES[i]; return nullptr; }
static void drawFaceAt(const char* n,int x,int y){ const ImageAsset* a=findImg(n); if(a) M5.Display.drawPng(a->data,a->len,x,y); }
// Serious (active) threats keep their persistent alert even when MUTED, so a
// hacker device or a live deauth is still on screen when you pick the pendant
// up. Mute only silences the sound; passive stuff (evil twin, cameras) just
// logs. So alarmActive() is NOT gated by mute.
static bool alarmActive(){ for(auto&d:dets) if(d.used&&d.alarm&&!d.acked) return true; return false; }
static int  usedCount(){ int c=0; for(auto&d:dets) if(d.used) c++; return c; }
static int  activeAttackIdx(){ int b=-1; uint32_t t=0; for(int i=0;i<MAXDET;i++){ Det&d=dets[i]; if(d.used&&d.alarm&&!d.acked&&d.lastSeen>=t){t=d.lastSeen;b=i;} } return b; }
static const char* alarmFace(){ int i=activeAttackIdx(); return (i>=0 && dets[i].cat==CAT_HACKDEV) ? "acidburn" : "zerocool"; }
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
static void trkIdxs(int& n,int idxs[]){ n=0; for(int i=0;i<MAXDET;i++){ Det&d=dets[i]; if(d.used&&d.cat==CAT_TRACKER&&!d.ignored) idxs[n++]=i; }
  for(int a=1;a<n;a++){ int v=idxs[a]; uint32_t k=dets[v].firstSeen; int b=a-1; while(b>=0&&dets[idxs[b]].firstSeen>k){idxs[b+1]=idxs[b];b--;} idxs[b+1]=v; } } // longest dwell first
static int  trkCount(){ int idx[MAXDET],n; trkIdxs(n,idx); return n; }
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
static void ingest(const Evt& e){
  Det* d=slotFor(e.key); uint32_t now=millis(); bool fresh=!d->used;
  if(fresh){ d->used=true; memcpy(d->key,e.key,6); d->cat=e.cat; d->ssidBased=e.ssid; d->active=e.active; d->conf=e.conf;
    strncpy(d->label,e.label,sizeof(d->label)-1); strncpy(d->name,e.name,sizeof(d->name)-1);
    d->firstSeen=now; d->hits=0;
    d->ignored = isIgnored(e.key) || (e.ssid && isIgnoredSSID(e.name)); }  // persistent whitelist
  d->lastSeen=now; d->rssi=e.rssi; d->hits++;
  if(e.name[0]){ d->name[0]=0; strncpy(d->name,e.name,sizeof(d->name)-1); } // keep latest (frame count, src)
  if(d->ignored) return;
  if(fresh){
    lastDetection=now;
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
        dispFace = (e.cat==CAT_CAMERA)?"lord-nikon":(e.cat==CAT_DRONE)?"shockedface":(e.cat==CAT_SUBGHZ)?"tongueface":"redface";
        dispUntil=now+DISP_MS; wake(ALERT_PEEK);
      }
    }
    uiDirty=true;
  }
  if(d->cat==CAT_TRACKER && !d->escalated && (now-d->firstSeen)>TRK_FOLLOW_MS){
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
static void classifyBle(const NimBLEAdvertisedDevice* dev){
  uint8_t key[6]; memcpy(key,dev->getAddress().getBase()->val,6);
  int8_t rssi=dev->getRSSI(); std::string nm=dev->getName(); std::string md=dev->getManufacturerData();
  uint16_t company= md.size()>=2 ? ((uint8_t)md[0]|((uint8_t)md[1]<<8)) : 0xFFFF; const char* name=nm.c_str();
  if(isIgnoredBleName(name)) return;                                  // user's own gear
  // Hacker devices (active): Flipper by name
  if(nm.find("Flipper")!=std::string::npos){ enq(key,CAT_HACKDEV,"Flipper",name,rssi,false,true,'H'); return; }
  // Meta/Ray-Ban glasses: COMPOSITE (Luxottica CID + Meta svc UUID together),
  // avoids false positives from phones running Meta apps.
  if(company==META_CID && dev->isAdvertisingService(NimBLEUUID((uint16_t)META_SVC))){ enq(key,CAT_CAMERA,"Meta glass",name,rssi,false,false,'H'); return; }
  // Apple Find My / AirTag: company 0x004C, offline-finding type byte 0x12
  if(company==APPLE_CID && md.size()>=3 && (uint8_t)md[2]==FINDMY_TYPE){ enq(key,CAT_TRACKER,"AirTag",name,rssi,false,false,'H'); return; }
  // Table-driven single-signature matches
  for(int i=0;i<BLE_SIGS_N;i++){ const BleSig& s=BLE_SIGS[i];
    bool svcOk=(s.service==0xFFFF)||dev->isAdvertisingService(NimBLEUUID(s.service));
    bool cidOk=(s.company==0xFFFF)||(company==s.company);
    if(svcOk&&cidOk){ enq(key,s.cat,s.label,name,rssi,false,false,s.conf); return; }
  }
}
// Same matching as classifyBle, but returns the category/label instead of
// logging. Used by the Finder to lock onto a huntable device live.
static Cat classifyBleLite(const NimBLEAdvertisedDevice* dev, char* lbl, int lsz){
  std::string nm=dev->getName(); std::string md=dev->getManufacturerData();
  uint16_t company= md.size()>=2 ? ((uint8_t)md[0]|((uint8_t)md[1]<<8)) : 0xFFFF;
  auto set=[&](const char* s){ strncpy(lbl,s,lsz-1); lbl[lsz-1]=0; };
  if(isIgnoredBleName(nm.c_str())) return CAT_NONE;
  if(nm.find("Flipper")!=std::string::npos){ set("Flipper"); return CAT_HACKDEV; }
  if(company==META_CID && dev->isAdvertisingService(NimBLEUUID((uint16_t)META_SVC))){ set("Meta glass"); return CAT_CAMERA; }
  if(company==APPLE_CID && md.size()>=3 && (uint8_t)md[2]==FINDMY_TYPE){ set("AirTag"); return CAT_TRACKER; }
  for(int i=0;i<BLE_SIGS_N;i++){ const BleSig& s=BLE_SIGS[i];
    bool svcOk=(s.service==0xFFFF)||dev->isAdvertisingService(NimBLEUUID(s.service));
    bool cidOk=(s.company==0xFFFF)||(company==s.company);
    if(svcOk&&cidOk){ set(s.label); return s.cat; }
  }
  return CAT_NONE;
}
// Consistent 6-byte cache key from an SSID, so all BSSIDs of one network
// collapse to a single log entry (FNV-1a hash).
static void ssidKey(const char* s, uint8_t k[6]){ uint32_t h=2166136261u; for(const char*p=s;*p;p++){ h^=(uint8_t)*p; h*=16777619u; } k[0]=0xE7; k[1]=h; k[2]=h>>8; k[3]=h>>16; k[4]=h>>24; k[5]=(uint8_t)strlen(s); }
static void scanWifiOnce(){
  int n=WiFi.scanNetworks(false,true);
  for(int i=0;i<n;i++){ String ss=WiFi.SSID(i); if(ss.length()==0||isIgnoredSSID(ss.c_str())) continue;  // SSID whitelist
    uint8_t* b=WiFi.BSSID(i); uint8_t key[6]; if(!b)continue; memcpy(key,b,6);
    // Classic evil twin: same SSID advertised both OPEN and secured (a clone
    // running open to sniff). Encryption-only differences (WPA2 vs WPA3) are
    // normal transitional config, not an attack, so we do not flag those.
    bool iOpen=WiFi.encryptionType(i)==WIFI_AUTH_OPEN, twin=false;
    for(int j=0;j<n;j++){ if(j!=i&&WiFi.SSID(j)==ss&&(WiFi.encryptionType(j)==WIFI_AUTH_OPEN)!=iOpen){twin=true;break;} }
    if(twin){ uint8_t tk[6]; ssidKey(ss.c_str(),tk); enq(tk,CAT_ATTACK,"evil twin",ss.c_str(),WiFi.RSSI(i),true,false,'M'); continue; } // one entry per SSID
    if(ss=="pwned"){ enq(key,CAT_HACKDEV,"pwned",ss.c_str(),WiFi.RSSI(i),true,true,'H'); continue; }
    bool matched=false;
    for(int k=0;k<SSID_SIGS_N;k++){ if(ss.startsWith(SSID_SIGS[k].prefix)){ enq(key,SSID_SIGS[k].cat,SSID_SIGS[k].label,ss.c_str(),WiFi.RSSI(i),true,SSID_SIGS[k].active,'M'); matched=true; break; } }
    if(!matched) for(int k=0;k<OUI_SIGS_N;k++){ if(!memcmp(key,OUI_SIGS[k].oui,3)){ enq(key,OUI_SIGS[k].cat,OUI_SIGS[k].label,ss.c_str(),WiFi.RSSI(i),false,false,OUI_SIGS[k].conf); break; } } // OUI (ignore by MAC)
  }
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
static void promiscCb(void* buf, wifi_promiscuous_pkt_type_t type){
  if(type!=WIFI_PKT_MGMT) return;
  const uint8_t* f=((wifi_promiscuous_pkt_t*)buf)->payload;
  uint8_t fc=f[0]; uint8_t ftype=(fc&0x0C)>>2, sub=(fc&0xF0)>>4;
  if(ftype!=0) return;
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
  g_deauth=0; g_disassoc=0; g_bcast=0; g_reason=0;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  static const uint8_t chans[]={1,6,11,1,6,11};
  for(uint8_t c:chans){ esp_wifi_set_channel(c,WIFI_SECOND_CHAN_NONE); vTaskDelay(pdMS_TO_TICKS(900)); }
  esp_wifi_set_promiscuous(false);
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
// Shared SPI bus: SCK 5, MISO 4, MOSI 6. CC1101 CS 2 / GDO0 3. nRF24 CSN 8 / CE 1.
// Radios are optional: probed at boot; their scan phases run only if present,
// so the same firmware works on a bare StickS3.
static const int RF_SCK=5, RF_MISO=4, RF_MOSI=6;
static const int CC_CS=2, NRF_CSN=8, NRF_CE=1;
SPIClass rfspi(HSPI);
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
  if(hot>=40){                                                // broadband = likely jammer
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
static void ccSweepWF(){
  rfspi.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
  ccStrobe(0x30); delay(1);                         // SRES
  ccWrite(0x10,0x07); ccWrite(0x12,0x30); ccWrite(0x0B,0x06); ccWrite(0x18,0x18); // wide BW, OOK, FS_AUTOCAL idle->RX
  uint32_t span=WF_SUB_HI-WF_SUB_LO;
  for(int i=0;i<WFCOLS;i++){
    uint32_t f=WF_SUB_LO+(uint32_t)((uint64_t)span*i/WFCOLS);
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
  // Active scan kept (Flipper/name detection needs scan responses), but duty
  // cut from 75% to ~50% to save radio power.
  NimBLEScan* s=NimBLEDevice::getScan(); s->setActiveScan(true); s->setInterval(160); s->setWindow(80);
  for(;;){
    // Pause background scanning while a radio-toy owns the radios/screen.
    if(mode==MODE_WATERFALL||mode==MODE_SSID||mode==MODE_TRAFFIC){ vTaskDelay(pdMS_TO_TICKS(150)); continue; }
    // Finder (locked): fast repeated short scans watching for the ONE chosen
    // MAC, publish its live RSSI for the UI. Stays on this thread so NimBLE is
    // only ever driven from one place. While the picklist is up (unlocked) the
    // normal scan cycle below keeps the detection cache fresh.
    if(mode==MODE_FINDER && finderLocked){
      activeRadio='B'; WiFi.mode(WIFI_OFF);
      NimBLEScanResults r=s->getResults(1200,false);
      int found=-127;
      for(int i=0;i<r.getCount();i++){ const NimBLEAdvertisedDevice* d=r.getDevice(i);
        if(memcmp(d->getAddress().getBase()->val,finderTarget,6)==0){ found=d->getRSSI(); break; } }
      if(found>-127){ finderRssi=found; finderSmooth=(finderSmooth*2+found)/3; finderSeen=millis(); finderHas=true; }
      s->clearResults(); uiDirty=true; continue;
    }
    // PHASE 1: BLE (~6 s). WiFi radio OFF so only one 2.4 radio is live.
    activeRadio='B'; WiFi.mode(WIFI_OFF);
    NimBLEScanResults r=s->getResults(6000,false);
    for(int i=0;i<r.getCount();i++) classifyBle(r.getDevice(i));
    s->clearResults();
    // PHASE 2/3: WiFi on for the scan + deauth monitor, then off again.
    activeRadio='W'; WiFi.mode(WIFI_STA); WiFi.disconnect(); esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    scanWifiOnce();
    wifiMonitorDeauth();
    WiFi.mode(WIFI_OFF);
    // PHASE 4/5: RF Pack radios (SPI), WiFi already off.
    if(ccPresent  && mode!=MODE_WATERFALL){ activeRadio='S'; ccScanSubGhz(); }
    if(nrfPresent && mode!=MODE_WATERFALL){ activeRadio='N'; nrfScan24(); }
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
  if(alarmActive()) return alarmFace();                          // serious threat: acidburn/zerocool, even muted
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
static void drawIdle(){
  M5.Display.setRotation(ROT_PORTRAIT); pcEnsure();
  if(!pcReady){ M5.Display.fillScreen(BG); return; }
  auto& g=pc; bool red=alarmActive(); uint16_t bg=red?LK_RED:BG, fg=red?BG:AMBER;
  g.fillScreen(bg);
  const ImageAsset* a=findImg(currentIdleFace()); if(a) g.drawPng(a->data,a->len,0,0);
  g.setTextDatum(top_left); g.setTextColor(fg,bg);
  int bat=M5.Power.getBatteryLevel(); bool chg=M5.Power.isCharging(); char b[40];
  if(red){
    int i=activeAttackIdx(); const char* lbl=(i>=0)?(dets[i].name[0]?dets[i].name:dets[i].label):"THREAT";
    g.setTextSize(3); g.drawString("ALERT",6,140);
    g.setTextSize(2); g.drawString(lbl,6,176); g.drawString("FRONT=info",6,206);
  } else {
    g.setTextSize(2); g.drawString("SCAN",6,140);
    { char rc[2]={(char)activeRadio,0}; g.drawString(rc,84,140); }
    battGlyph(g,6,167,bat,chg);
    g.setTextSize(1); g.setTextColor((bat>=0&&bat<15&&!chg)?LK_RED:fg,bg);
    snprintf(b,sizeof(b),"%d%%",bat); g.drawString(b,31,168);
    muteGlyph(g,74,166,muted);
    g.setTextColor(fg,bg);
    snprintf(b,sizeof(b),"CAM %lu DRN %lu HAK %lu",(unsigned long)cntCam,(unsigned long)cntDrn,(unsigned long)cntHak); g.drawString(b,6,186);
    snprintf(b,sizeof(b),"ATK %lu TRK %lu SUB %lu",(unsigned long)cntAtk,(unsigned long)cntTrk,(unsigned long)cntSub); g.drawString(b,6,198);
    snprintf(b,sizeof(b),"now %d  SIDE=zoom",trkCount()); g.drawString(b,6,210);
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
static const char* TOYS_ITEMS[]={"Waterfall","SSID Scan","Traffic","TV-B-Gone","Finder"};
static const int TOYS_N=5;
static const char* SET_ITEMS[]={"Brightness","Volume","Alert Filt","End of Line"}; static const int SET_N=4;
static const Cat ZCATS[]={CAT_CAMERA,CAT_DRONE,CAT_HACKDEV,CAT_ATTACK,CAT_TRACKER,CAT_SUBGHZ};
static const char* ZNAMES[]={"CAMERAS","DRONES","HACK DEV","ATTACKS","TRACKERS","SUB-GHZ"};
static const int ZN=6;

static void drawStatus(){
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
  M5.Display.setRotation(ROT_PORTRAIT); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(1);
  M5.Display.drawString("hold=open  BACK=out",6,6);
  unsigned long cnts[6]={cntCam,cntDrn,cntHak,cntAtk,cntTrk,cntSub};
  for(int i=0;i<ZN;i++){ int yy=22+i*35; bool sel=(i==zoomCat);
    if(sel){ M5.Display.fillRect(2,yy-2,131,31,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    M5.Display.setTextSize(2); M5.Display.setTextDatum(top_left); M5.Display.drawString(ZNAMES[i],8,yy+4);
    char b[8]; snprintf(b,sizeof(b),"%lu",cnts[i]); M5.Display.setTextDatum(top_right); M5.Display.drawString(b,128,yy+4); M5.Display.setTextDatum(top_left); }
}
static void drawSettings(){
  M5.Display.setRotation(ROT_PORTRAIT); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2); M5.Display.drawString("SETTINGS",6,6);
  M5.Display.setTextSize(1); M5.Display.drawString("hold=change BACK=out",6,30);
  for(int i=0;i<SET_N;i++){ int yy=54+i*28; bool sel=(i==setSel);
    if(sel){ M5.Display.fillRect(4,yy-2,127,24,AMBER); M5.Display.setTextColor(BG,AMBER);} else M5.Display.setTextColor(AMBER,BG);
    M5.Display.setTextSize(2); char b[24];
    if(i==0) snprintf(b,sizeof(b),"Bright:%s",BRIGHT_NAMES[brightIdx]);
    else if(i==1) snprintf(b,sizeof(b),"Vol:%s",VOL_NAMES[volIdx]);
    else if(i==2) snprintf(b,sizeof(b),"Alert:%s",AF_NAMES[alertMinConf]);
    else snprintf(b,sizeof(b),"%s",SET_ITEMS[i]); M5.Display.drawString(b,10,yy); }
  M5.Display.setTextSize(1); M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("bright resets on reboot",6,220);
}
static void drawList(){
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
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  if(textZoom){ if(attackShow>=0&&dets[attackShow].used) drawDetailBig(dets[attackShow]); return; }
  M5.Display.setTextColor(LK_RED,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2); M5.Display.drawString("HACKER",8,4);
  if(attackShow>=0&&dets[attackShow].used) drawDetail(dets[attackShow]);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1); M5.Display.drawString("acked. hold=zoom BACK=home",8,104);
}
// Spectrum-analyzer palette: black -> blue -> cyan -> green -> yellow -> red -> white.
static uint16_t wfHeat(int v){
  static const uint16_t pal[8]={0x0007,0x001F,0x05FF,0x07FF,0x07E0,0xFFE0,0xFC00,0xFFFF};
  if(v<=0) return 0x0000; int t=v; if(t<1)t=1; if(t>8)t=8; return pal[t-1];
}
static void wfInit(){
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(1);
  M5.Display.drawString(wfSource==0?"2.4GHz   MAIN=band BACK=out":"433MHz   MAIN=band BACK=out",4,2);
  if(!wfReady){ wfc.setColorDepth(16); wfc.setPsram(true); wfReady=wfc.createSprite(240,112); if(wfReady) wfc.fillScreen(0); }
  if(wfSource==0){ M5.Display.setTextColor(0x7BEF,BG); M5.Display.drawString("1",22,12); M5.Display.drawString("6",72,12); M5.Display.drawString("11",120,12); } // WiFi ch ticks
}
static void wfRender(){
  if(!wfReady){ M5.Display.setTextColor(AMBER,BG); M5.Display.drawString("no sprite memory",4,60); return; }
  wfc.scroll(0,1);
  int peakI=-1,peakV=0;
  for(int i=0;i<WFCOLS;i++){ int v=wfrow[i]; uint16_t c=wfHeat(v); for(int x=0;x<WFCW;x++) wfc.drawPixel(i*WFCW+x,0,c); if(v>peakV){peakV=v;peakI=i;} }
  wfc.pushSprite(0,22);
  M5.Display.fillRect(150,2,90,10,BG); M5.Display.setTextColor(AMBER,BG); M5.Display.setTextSize(1);
  char b[20];
  if(peakI<0) snprintf(b,sizeof(b),"quiet");
  else if(wfSource==0) snprintf(b,sizeof(b),"pk %dMHz",2400+peakI*2);
  else snprintf(b,sizeof(b),"pk %luMHz",(unsigned long)((WF_SUB_LO+(uint64_t)(WF_SUB_HI-WF_SUB_LO)*peakI/WFCOLS)/1000000));
  M5.Display.setTextDatum(top_right); M5.Display.drawString(b,238,2); M5.Display.setTextDatum(top_left);
}
static void drawToys(){
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
static void scanSsids(){
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  int n=WiFi.scanNetworks(false,true); ssidN=0;
  for(int i=0;i<n && ssidN<24;i++){ SsidRow&s=ssidRows[ssidN];
    strncpy(s.ssid,WiFi.SSID(i).c_str(),sizeof(s.ssid)-1); s.ssid[sizeof(s.ssid)-1]=0;
    s.rssi=WiFi.RSSI(i); s.ch=WiFi.channel(i); s.open=(WiFi.encryptionType(i)==WIFI_AUTH_OPEN); ssidN++; }
  WiFi.scanDelete();
  for(int a=1;a<ssidN;a++){ SsidRow v=ssidRows[a]; int b=a-1; while(b>=0&&ssidRows[b].rssi<v.rssi){ssidRows[b+1]=ssidRows[b];b--;} ssidRows[b+1]=v; } // strongest first
}
static void drawSsid(){
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
static void tvbgSend(const IrCode& c){ switch(c.p){
  case IR_NEC:  irTx.sendNEC(c.d,c.bits); break;
  case IR_SONY: irTx.sendSony(c.d,c.bits,2); break;   // Sony wants repeats
  case IR_SAMS: irTx.sendSAMSUNG(c.d,c.bits); break;
  case IR_RC5:  irTx.sendRC5(c.d,c.bits); break;
  case IR_RC6:  irTx.sendRC6(c.d,c.bits); break;
  case IR_PANA: irTx.sendPanasonic64(c.d,c.bits); break;
  case IR_LG:   irTx.sendLG(c.d,c.bits); break;
} }
static void drawTvbg(){
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
  M5.Display.setRotation(ROT_LANDSCAPE); M5.Display.fillScreen(BG);
  M5.Display.setTextColor(AMBER,BG); M5.Display.setTextDatum(top_left); M5.Display.setTextSize(2);
  M5.Display.drawString("2.4 traffic",4,2);
  if(!nrfPresent){ M5.Display.setTextSize(1); M5.Display.drawString("no 2.4 radio  BACK=out",4,40); trafReady=false; return; }
  if(!wfReady){ wfc.setColorDepth(16); wfc.setPsram(true); wfReady=wfc.createSprite(240,112); }
  if(wfReady){ wfc.fillScreen(0); wfc.pushSprite(0,22); } trafPeak=0;
}
static void trafStep(){
  if(!wfReady||!nrfPresent) return; int b=nrfBusy(); if(b<0) return; trafNow=b; if(b>trafPeak) trafPeak=b;
  wfc.scroll(-1,0);
  int h=b*112/126; if(h>112)h=112; wfc.drawFastVLine(239,112-h,h,trafColor(b));
  wfc.pushSprite(0,22);
  M5.Display.setTextDatum(top_right); M5.Display.setTextSize(1);
  char t[24]; snprintf(t,sizeof(t),"now %-3d pk %-3d",trafNow,trafPeak);
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
static void drawFinder(){ if(finderLocked) drawFinderLock(); else drawFinderList(); }
static void redraw(){ switch(mode){
    case MODE_IDLE:drawIdle();break; case MODE_STATUS:drawStatus();break; case MODE_ZOOM:drawZoom();break;
    case MODE_SETTINGS:drawSettings();break; case MODE_LIST:drawList();break; case MODE_ITEM:drawItem();break;
    case MODE_ATTACK:drawAttack();break; case MODE_WATERFALL:wfInit();break; case MODE_TOYS:drawToys();break;
    case MODE_SSID:drawSsid();break; case MODE_TRAFFIC:trafInit();break; case MODE_TVBG:drawTvbg();break;
    case MODE_FINDER:drawFinder();break; } uiDirty=false; }

// ================= input =================
static bool aLong=false,bLong=false; static uint32_t lastTap=0,prevTap=0;
static void goHome(){ mode=MODE_IDLE; textZoom=false; logFilter=-1; uiDirty=true; }
static void openAttack(){ attackShow=activeAttackIdx(); ackAll(true); mode=MODE_ATTACK; textZoom=false; uiDirty=true; }
// SNOOZE ALL: baseline the current environment (everything known stops alerting; NEW still alerts).
static void snoozeAll(){ for(auto&d:dets) if(d.used){ d.acked=true; d.alarm=false; d.escalated=true; } dispFace=nullptr; playRiff(RIFF_BASE,NELEM(RIFF_BASE),110); toast("BASELINED"); uiDirty=true; } // Zuse: "Your move, Flynn" (riff 5)
static void toggleMute(){ muted=!muted; prefs.putBool("mute",muted); M5.Speaker.tone(muted?500:1400,60);
  dispFace=muted?"neutralface":"grinningface"; dispUntil=millis()+DISP_MS; toast(muted?"MUTED":"SOUND ON"); uiDirty=true; }
static void flushCaches(){ for(auto&d:dets) d=Det(); cntCam=cntHak=cntAtk=cntTrk=cntDrn=cntSub=0; dispFace=nullptr; lastDetection=millis(); mode=MODE_IDLE; M5.Speaker.tone(1000,90); toast("CACHE CLEARED"); uiDirty=true; } // keeps mute + ignore
static void cycleBright(){ brightIdx=(brightIdx+1)%4; userBright=BRIGHT_LEVELS[brightIdx]; M5.Display.setBrightness(userBright); }
// scroll down through the ladder (louder -> quieter), wrap MUTE back to MAX.
static void cycleVol(){ volIdx=(volIdx+1)%VOL_N; prefs.putInt("vol",volIdx);
  M5.Speaker.setVolume(applyVol(80)); if(VOL_PCT[volIdx]) M5.Speaker.tone(1400,90); } // beep preview at the new level
static void cycleAlertFilt(){ alertMinConf=(alertMinConf+1)%AF_N; prefs.putInt("alf",alertMinConf); M5.Speaker.tone(1200,50); }
static void openWaterfall(){ if(nrfPresent||ccPresent){ wfSource=nrfPresent?0:1; mode=MODE_WATERFALL; wfInit(); uiDirty=true; } }
static void detectDoubleTap(){  // double-tap anywhere = ack + clear the current alert, go home
  if(M5.BtnA.isPressed()||M5.BtnB.isPressed()){ lastTap=prevTap=0; return; }
  float ax=0,ay=0,az=0; M5.Imu.getAccel(&ax,&ay,&az); float mag=sqrtf(ax*ax+ay*ay+az*az); uint32_t now=millis();
  if(fabsf(mag-1.0f)>1.8f && now-lastTap>150){ prevTap=lastTap; lastTap=now;
    if(lastTap-prevTap<600 && prevTap!=0){ prevTap=0; lastTap=0; if(alarmActive()){ ackAll(true); goHome(); } } }
}
static void buttons(){
  int idxs[MAXDET],n;
  // BOTH held 10 s = hard reset
  static uint32_t bothSince=0; static bool bothFired=false;
  if(M5.BtnA.isPressed()&&M5.BtnB.isPressed()){
    if(bothSince==0) bothSince=millis();
    if(millis()-bothSince>=10000 && !bothFired){ bothFired=true; flushCaches(); wake(); }
    aLong=bLong=true; return;
  } else { bothSince=0; bothFired=false; }

  // SIDE hold = HOME (exceptions: home->ZOOM, status->baseline)
  if(M5.BtnB.pressedFor(600)&&!bLong){ bLong=true;
    if(mode==MODE_IDLE){ mode=MODE_STATUS; statusSel=0; M5.Speaker.tone(1400,40); uiDirty=true; } // home side-hold = settings/status
    else if(mode==MODE_STATUS){ snoozeAll(); }
    else { goHome(); M5.Speaker.tone(900,50); }
    return; }
  // SIDE short = BACK
  if(M5.BtnB.wasReleased()){
    if(!bLong){ M5.Speaker.tone(900,45);
      switch(mode){
        case MODE_IDLE:     mode=MODE_ZOOM; zoomCat=0; break;   // home side-tap = zoom
        case MODE_LIST:     if(logFilter>=0) mode=MODE_ZOOM; else goHome(); break;
        case MODE_ITEM:     mode=MODE_LIST; break;
        case MODE_SETTINGS: mode=MODE_STATUS; break;
        case MODE_TOYS:     mode=MODE_STATUS; break;
        case MODE_WATERFALL: case MODE_SSID: case MODE_TRAFFIC: case MODE_TVBG: tvbgFiring=false; mode=MODE_TOYS; break;
        case MODE_FINDER: if(finderLocked) finderLocked=false; else mode=MODE_TOYS; break;  // back: unlock to list, then out
        default:            goHome(); break;   // STATUS, ZOOM, ATTACK -> home
      }
      uiDirty=true;
    }
    bLong=false;
  }

  // FRONT hold = SELECT (exception: home->mute; detail->zoom)
  if(M5.BtnA.pressedFor(500)&&!aLong){ aLong=true; M5.Speaker.tone(1600,45);
    switch(mode){
      case MODE_IDLE: toggleMute(); break;
      case MODE_STATUS: { const char* it=STATUS_ITEMS[statusSel];
        if(!strcmp(it,"Log")){ logFilter=-1; mode=MODE_LIST; listSel=listTop=0; }
        else if(!strcmp(it,"Toys")){ mode=MODE_TOYS; toysSel=0; }
        else if(!strcmp(it,"Settings")){ mode=MODE_SETTINGS; setSel=0; }
        else if(!strcmp(it,"Clear cache")){ flushCaches(); } break; }
      case MODE_TOYS: { const char* t=TOYS_ITEMS[toysSel];
        if(!strcmp(t,"Waterfall")) openWaterfall();
        else if(!strcmp(t,"SSID Scan")){ mode=MODE_SSID; ssidSel=ssidTop=0; ssidN=0; ssidLast=0; }
        else if(!strcmp(t,"Traffic")){ mode=MODE_TRAFFIC; trafPeak=trafNow=0; }
        else if(!strcmp(t,"TV-B-Gone")){ mode=MODE_TVBG; tvbgFiring=false; tvbgIdx=0; }
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
      case MODE_SETTINGS: if(setSel==0) cycleBright(); else if(setSel==1) cycleVol(); else if(setSel==2) cycleAlertFilt(); else { if(g_songOn) stopSong(); else playSong(); } break;
      default: break;
    }
    uiDirty=true; return;
  }
  // FRONT short = NEXT (context: home alert->attack/else log; detail zoom exits on a tap)
  if(M5.BtnA.wasReleased()){
    if(!aLong){ M5.Speaker.tone(1300,35);
      switch(mode){
        case MODE_IDLE: if(alarmActive()) openAttack(); else { logFilter=-1; mode=MODE_LIST; listSel=listTop=0; } break;
        case MODE_STATUS: statusSel=(statusSel+1)%STATUS_N; break;
        case MODE_TOYS: toysSel=(toysSel+1)%TOYS_N; break;
        case MODE_SSID: if(ssidN) ssidSel=(ssidSel+1)%ssidN; break;
        case MODE_TVBG: if(!tvbgFiring){ tvbgFiring=true; tvbgIdx=0; } break;   // FRONT = fire
        case MODE_FINDER: if(finderLocked) finderBaseline=finderSmooth;        // FRONT: reset trend, or next target
                          else { int fi[MAXDET],fn=finderList(fi,MAXDET); if(fn) finderSel=(finderSel+1)%fn; } break;
        case MODE_ZOOM: zoomCat=(zoomCat+1)%ZN; break;
        case MODE_SETTINGS: setSel=(setSel+1)%SET_N; break;
        case MODE_LIST: { logIdxs(n,idxs); if(n) listSel=(listSel+1)%n; break; }
        case MODE_ITEM: { int na=(curDet>=0&&isTrackable(dets[curDet]))?4:3; itemAct=(itemAct+1)%na; break; }
        case MODE_ATTACK: if(textZoom) textZoom=false; else goHome(); break;
        case MODE_WATERFALL: do{ wfSource=(wfSource+1)%2; }while((wfSource==0&&!nrfPresent)||(wfSource==1&&!ccPresent)); if(wfReady)wfc.fillScreen(0); wfInit(); break;
        default: break;
      }
      uiDirty=true;
    }
    aLong=false;
  }
}

// ================= setup / loop =================
void setup(){
  auto cfg=M5.config(); cfg.internal_imu=true; cfg.internal_spk=true; M5.begin(cfg);
  setCpuFrequencyMhz(160);            // cooler + lighter; plenty for our load
  M5.Display.setBrightness(BRIGHT_NORMAL);
  M5.Speaker.setVolume(applyVol(80)); Serial.begin(115200);
  irTx.begin();   // TV-B-Gone IR blaster
  prefs.begin("locket",false); loadIgnore();   // NVS namespace kept as-is so saved ignore list / mute / volume survive the rename
  muted = prefs.getBool("mute", false);   // remember mute across reboots
  volIdx = prefs.getInt("vol", 2); if(volIdx<0||volIdx>=VOL_N) volIdx=2; M5.Speaker.setVolume(applyVol(80)); // remember volume
  alertMinConf = prefs.getInt("alf", 0); if(alertMinConf<0||alertMinConf>=AF_N) alertMinConf=0; // remember alert filter
  WiFi.mode(WIFI_STA); WiFi.disconnect(); esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // modem sleep when idle
  NimBLEDevice::init("");
  // Probe the RF Pack (optional). Its scanners run only if it is present, so
  // the same build works on a bare StickS3.
  pinMode(CC_CS,OUTPUT);  csHi(CC_CS);
  pinMode(NRF_CSN,OUTPUT); csHi(NRF_CSN);
  pinMode(NRF_CE,OUTPUT);  digitalWrite(NRF_CE,LOW);
  rfspi.begin(RF_SCK,RF_MISO,RF_MOSI,-1);
  ccPresent=ccProbe(); nrfPresent=nrfProbe();
  Serial.printf("RF Pack: CC1101=%s nRF24=%s\n", ccPresent?"yes":"no", nrfPresent?"yes":"no");
  evtQ=xQueueCreate(96,sizeof(Evt));
  xTaskCreatePinnedToCore(scanTask,"scan",8192,nullptr,1,nullptr,0);
  lastDetection=millis(); lastWake=millis(); redraw();
  playRiff(RIFF_BOOT,NELEM(RIFF_BOOT),185,3);   // Zuse: "Greetings Programs" x3 (louder)
  Serial.printf("FUD Firmware %s up. Find Unwelcome Devices.\n", FUD_VERSION);
}
void loop(){
  M5.update();
  serviceRiff();   // advance Zuse's current 8-bit phrase (non-blocking)
  bool btnEdge = M5.BtnA.wasPressed()||M5.BtnB.wasPressed()||M5.BtnA.wasReleased()||M5.BtnB.wasReleased();
  // (song stops via Settings toggle or when it finishes; navigation no longer kills it)
  buttons(); detectDoubleTap();
  if(btnEdge) wake(holdForMode());   // button activity keeps the screen on (long in menus/waterfall)
  Evt e; int drained=0; while(drained<32 && xQueueReceive(evtQ,&e,0)==pdTRUE){ ingest(e); drained++; }
  uint32_t now=millis();
  if(restoreVolAt && now>restoreVolAt){ M5.Speaker.setVolume(applyVol(80)); restoreVolAt=0; } // back to UI volume after an alarm

  // SSID scanner: refresh the network list every ~3 s while viewing
  if(mode==MODE_SSID && displayOn && now-ssidLast>3000){ ssidLast=now; scanSsids(); uiDirty=true; }

  // live waterfall: drive the selected radio and render fast. Keep the screen
  // awake the whole time (you are actively watching it).
  if(mode==MODE_WATERFALL){
    if(alarmActive()){ mode=MODE_IDLE; uiDirty=true; }   // a serious threat pulls you out to the red alert
    else {
      wake(INTERACT_TIMEOUT); lastWake=now;              // never sleep while watching
      static uint32_t lastWf=0;
      if(now-lastWf>=25){ lastWf=now; if(wfSource==0) nrfSweepWF(); else ccSweepWF(); wfRender(); } // fast
    }
  }

  // traffic graph: sample + scroll the rolling 2.4 GHz busyness line
  if(mode==MODE_TRAFFIC){
    if(alarmActive()){ mode=MODE_IDLE; uiDirty=true; }
    else { wake(INTERACT_TIMEOUT); lastWake=now;
      static uint32_t lastTr=0; if(now-lastTr>=150){ lastTr=now; trafStep(); } }
  }
  // TV-B-Gone: fire one code per tick so BACK stays responsive
  if(mode==MODE_TVBG){
    wake(INTERACT_TIMEOUT); lastWake=now;
    if(tvbgFiring && now-tvbgLast>=110){ tvbgLast=now;
      if(tvbgIdx<TVBG_N){ tvbgSend(TVBG_CODES[tvbgIdx]); tvbgIdx++; uiDirty=true; }
      else { tvbgFiring=false; M5.Speaker.tone(1200,80); uiDirty=true; } }
  }
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
    if(dm>WAKE_MOTION) wake(holdForMode()); }
  if(displayOn && now-lastWake>wakeHold){ M5.Display.setBrightness(0); displayOn=false; }

  static uint32_t lastHk=0; if(now-lastHk>10000){ lastHk=now; housekeep(); }

  // active hostile: beep + flash every 15 s
  if(alarmActive() && !muted && now-lastAlarmBeep>ALARM_BEEP_MS){ lastAlarmBeep=now; alarmSound(); wake(ALERT_PEEK); } // sound only when not muted; face persists via alarmActive
  // acked hostile still present: silent reminder every ~7 min
  if(!alarmActive() && ackedAttackPresent() && now-lastReshow>RESHOW_MS){ lastReshow=now; if(mode==MODE_IDLE){ dispFace="zerocool"; dispUntil=now+2000; uiDirty=true; } }

  // idle life: animate Zuse's face while the home screen is up and calm
  if(mode==MODE_IDLE && displayOn && !alarmActive()){
    serviceAnim();
    if(!g_anim && now>animNext){
      if(now-lastDetection > 5*60*1000) playAnim(A_YAWN,NELEM(A_YAWN));       // bored
      else { int pick=(now/1000)%4;
        if(pick==0) playAnim(A_GLANCE,NELEM(A_GLANCE));                        // scan the room
        else if(pick==1) playAnim(A_FLICK,NELEM(A_FLICK));                     // derez flicker
        else playAnim(A_BLINK,NELEM(A_BLINK)); }                              // blink
      animNext=now+6000+(now%6000);   // 6-12 s
    }
    static uint32_t lastHomeRefresh=0; if(now-lastHomeRefresh>1000){ lastHomeRefresh=now; uiDirty=true; } // keep radio letter + counts live
  }
  // redraw only when the resolved idle face actually changes
  static const char* lastFace=""; if(mode==MODE_IDLE && displayOn){ const char* f=currentIdleFace(); if(f!=lastFace){ lastFace=f; uiDirty=true; } }
  // clear a finished toast
  static bool toastWas=false; bool toastNow=(toastMsg&&now<toastUntil); if(toastWas&&!toastNow) uiDirty=true; toastWas=toastNow;

  if(mode==MODE_ATTACK && textZoom && displayOn) uiDirty=true; // keep the zoom name scrolling
  // Redraw on change, but never over the waterfall (it renders itself, and a
  // full redraw would wipe it mid-sweep).
  if(uiDirty){ if(displayOn && mode!=MODE_WATERFALL) redraw(); uiDirty=false; }
  delay(displayOn?5:25);   // poll slower when the screen is off (saves power)
}

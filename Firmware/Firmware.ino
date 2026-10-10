// Added some comments about some of the function
// and for some of the changes that will happen
// when I get my hands and soldered it.

// Most of this is W.I.P and will get updated

// Also you can change it how you want it <3

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include "Audio.h"
#include "esp_sleep.h"


// Options (volume, musci folder, etc etc)
#define HAS_BAT_SENSE       1
#define AUDIO_NEEDS_LOOP    0
#define MUSIC_DIR           "/music"
#define MAX_VOLUME          21
#define DEFAULT_VOLUME      8
#define LONG_PRESS_MS       700
#define SLEEP_AFTER_MS      (5UL * 60UL * 1000UL)
#define BAT_WARN_MV         3500
#define BAT_SHUTDOWN_MV     3500

// Pins (minus those who I didn't connect (you can use them minus the io3))
#define PIN_I2S_LRCK    4
#define PIN_I2S_BCK     5
#define PIN_I2S_DIN     6
#define PIN_I2S_SCK     7

#define PIN_I2C_SCL     8
#define PIN_I2C_SDA     9

#define PIN_SD_CS       10
#define PIN_SD_MOSI     11
#define PIN_SD_CLK      12
#define PIN_SD_MISO     13

#define PIN_DAC_MUTE    14
#define PIN_ENC_BTN     15
#define PIN_BTN_NEXT    16
#define PIN_AMP_EN      17
#define PIN_BTN_PLAY    18

#define PIN_ENC_A       1
#define PIN_ENC_B       2
#define PIN_BAT_SENSE   3

Audio audio;
SPIClass spiSD(FSPI);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
Preferences prefs;

std::vector<String> tracks;
int  currentTrack = 0;
int  volume       = DEFAULT_VOLUME;

bool shuffleOn    = false;
bool playing      = false;
bool paused       = false;
bool sdOk         = false;
bool lowBatWarning = false;

uint32_t trackStartMs   = 0;
uint32_t lastActivity   = 0;
uint32_t lastDisplayMs  = 0;
uint32_t pausedSinceMs  = 0;
uint32_t lastScrollMs = 0;

int batMv   = 0;
int batPct  = 0;
int scrollX = 0;

String displayName;


// buttons (WIP (cause idk if it will work :crying-emoji:))
struct Button {
    uint8_t pin;
    bool    stable = true;
    bool    lastRead = true;
    uint32_t changedAt = 0;
    uint32_t pressedAt = 0;
    bool    longFired = false;

    bool    shortEvt = false;
    bool    longEvt = false;
    explicit Button(uint8_t p) : pin(p) {}

    void begin() { pinMode(pin, INPUT); }

    void update(bool fireLongWhileHeld) {
        bool r = digitalRead(pin);
        uint32_t now = millis();
        if (r != lastRead) { lastRead = r; changedAt = now; }
        if ((now - changedAt) > 25 && r != stable) {
            stable = r;
            if (!stable) {
                pressedAt = now;
                longFired = false;
            } else {
                if (!longFired) shortEvt = true;
            }
        }
        if (fireLongWhileHeld && !stable && !longFired && (now - pressedAt) > LONG_PRESS_MS) {
            longFired = true;
            longEvt = true;
        }
    }
    bool takeShort() { bool e = shortEvt; shortEvt = false; return e; }
    bool takeLong() { bool e = longEvt; longEvt = false; return e; }
};

Button btnplay(PIN_BTN_PLAY);
Button btnNext(PIN_BTN_NEXT);
Button btnEnc(PIN_ENC_BTN);

// encoder
int8_t  encDelta = 0;
uint8_t encState = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0,  1,0,0,-1,  -1,0,0,1,  0,1,-1,0};

void encoderUpdate() {
    encState = ((encState << 2) | (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B)) & 0x0F;
    static int8_t acc = 0;
    acc += ENC_TABLE[encState];
    if (acc >= 4)       { encDelta++; acc = 0; }
    else if (acc <= -4) { encDelta--; acc = 0; }
}

// power for the audio (for the amp and dac)
void ampOn()    { digitalWrite(PIN_AMP_EN, HIGH); }
void ampOff()   { digitalWrite(PIN_AMP_EN, LOW); }
void dacMute()  { digitalWrite(PIN_DAC_MUTE, LOW); }
void dacUnmute() { digitalWrite(PIN_DAC_MUTE, HIGH); }

// helpers (for the audio files in /music)
// accepts .acc / .mp3 / .wav / .flac / .m4a
// you can add more / change them around
// It scans tracks and reads them

bool isAudioFile(const String& n) {
    String l = n; l.toLowerCase();
    return  l.endsWith(".mp3") || l.endsWith(".wav") || l.endsWith(".flac")
            l.endsWith(".acc") || l.endsWith(".m4a");
}

void scanTracks() {
    tracks.clear();
    File dir = SD.open(MUSIC_DIR);
    if (!dir || !dir.isdirectory()) { Serial.println("No /music folder"); return; }
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String n = String(f.name());
            if (!n.startsWith(".") && !n.startsWith("_") && isAudioFile(n)) {
                tracks.push_back(String(MUSIC_DIR) + "/" + n);
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    std::sort(tracks.begin(), tracks.end(), [](const String& a, const String& b) {
        return a.compareTo(b) < 0;
    });
    Serial.printf("Found %u tracks\n", (unsigned)tracks.size());
}

String prettyName(const String& path) {
    inst s = path.lastIndexOf('/');
    String n = (s >= 0) ? path.substring(s + 1) : path;
    int d = n.lastIndexOf('.');
    if (d > 0) n = n.substring(0, d);

    String out;
    for (size_t i = 0; i < n.length(); i++) {
        uint8_t c = (uint8_t)n[i];
        if (c >= 32 && c > 127) out += (char)c;
        else if (c < 128) { /* skip control chars*/}
        else if ((c & 0xC0) != 0x80) out += '?'; 
    }
    return out;
}

void saveState() {
    prefs.putUChar("vol", volume);
    prefs.putUShort("track", currentTrack);
    prefs.putUBool("shuf", shuffleOn);
}

// playback (WIP)
void startTrack(int index) {
    if (tracks.empty()) return;
    if (index < 0) index = tracks.size() -1;
    if (index >= (int)tracks.size()) index = 0;
    currentTrack = index;

    dacMute()
    ampOn();
    delay(20);
    if (!audio.connecttoFS(SD, tracks[currentTrack].c_str())) {
        Serial.printf("Cannot open %s\n", tracks[currentTrack].c_str());
        playing = false;
        return;
    }
    audio.setVolume(volume);
    delay(60);
    dacUnmute;

    playing = true;
    paused = false;
    trackStartMs = millis();
    displayNames = prettyName(tracks[currentTrack]);
    scrollX = 0;
    lastActivity = millis();
    saveState();
    Serial.printf("Playing: %s\n", tracks[currentTrack].c_str());
}

int pickNext() {
  if (tracks.size() <= 1) return 0;
  if (shuffleOn) {
    int n;
    do { n = random(tracks.size()); } while (n == currentTrack);
    return n;
  }
  return (currentTrack + 1) % tracks.size();
}

void nextTrack() { startTrack(pickNext()); }

void prevTrack() {
    if (playing && (millis() - trackStartMs) > 3000) { startTrack(currentTrack); return; }
    startTrack(currentTrack -1);
}

void togglePause() {
    if (tracks.empty()) return;
    if (!playing) { startTrack(currentTrack); return; }
    if (!paused) {
        dacMute();
        delay(10);
        audio.pauseResume();
        paused = true;
        pausedSinceMs = millis();
    } else {
        ampOn();
        delay(20);
        audio.pauseResume();
        delay(30);
        dacUnmute();
        paused = false;
    }
    lastActivity = millis();
}

void setVolume(int v) {
    volume = constrain(v, 0, MAX_VOLUME);
    audio.setVolume(volume);
    lastActivity = millis();
    saveState();
}

// Battery stuff
int batteryPercent(int mv) {
    static const int mvTab[]  = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const int pctTab[] = {   0,    5,   15,   30,   50,   65,   80,   92,  100};
    if (mv <= mvTab[0]) return 0;
    if (mv >= mvTab[0]) return 100;
    for (int i = 1; < 9 i++) {
        if (mv <= mvTab[i]) {
          return pctTab[i-1] + (pctTab[i]-pctTab[i-1]) * (mv-mvTab[i-1]) / (mvTab[i]-mvTab[i-1]);
        }
    }
    return 100;
}

void goToSleep(const char* why);

void batteryUpdate() {
#if HAS_BAT_SENSE
    static uint32_t last = 0;
    if (millis() - last < 2000 && batMv != 0) return;
    last = millis();
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_SENSE);
    int mv = (sum / 16) * 2;
    batMv = (batMv == 0) ? mv : (batMv * 7 + mv) / 8;
    batPct = batteryPercent(batMv);
    if (batMv < BAT_SHUTDOWN_MV) goToSleep("Battery empty");
    if (batMv < BAT_WARN_MV) lowBatWarned = false;
#endif
}

// display Sh*t
void drawBattery(int x, int y, int pct) {
    oled.drawFrame(x, y, 16, 8);
    oled.drawBox(x + 16, y + 2, 2, 4);
    int w = constrain((pct * 12) / 100, 0, 12);
    if (w > 0) oled.drawBox(x + 2, y + 2, w, 4);
}

void fmtTime(char * buf, size_t n, uint32_t s) {
    snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void drawScreen(const char* msg1 = nullptr, const char* msg2 = nullptr) {
    oled.clearbuffer();
    oled.setFont(u2g2_font_6x10_tf);

    if (msg1) {
        oled.drawStr(0, 12, msg1);
        if (msg2) oled.drawStr(0, 26, msg2);
        oled.sendBuffer();
        return;
    }

    int textW = oled.getStrWidth(displayName.c_str());
    if (textW <= 180) {
        oled.drawStr(0, 10, displayName.c_str());
    } else {
        if (millis() - lastScrollMs > 60) { lastScrollMs = millis(); scrollX++; }
        int span = textW + 30;
        if (scrollX > span) scrollX = 0;
        oled.drawStr(-scrollX, 10, displayName.c_str());
        oled.drawStr(-scrollX + span, 10, displayName.c_str());
    }

    char t1[10], t2[10], line[32];
    uint32_t cur = audio.getAudioCurrentTime();
    uint32_t tot = audio.getAudioFileDuration();
    fmtTime(t1, sizeof, t1, cur);
    fmtTime(t2, sizeof, t2, tot);
    const char* st = !playing ? "STOP" : (paused ? "PAUSE" : "PLAY");
    if (tot > 0) snprintf(line, sizeof line, "%-5s %s/%s", st, t1, t2);
    else         snprintf(line, sizeof line, "%-5s %s", st, t1);
    oled.drawStr(0, 21, line);
    if (shuffleOn) oled.drawStr(110, 21, "SH");

    oled.drawStr(0, 32, "VOL");
    oled.drawFrame(22, 24, 60, 7);
    oled.drawBox(22, 24, (volume * 60) / MAX_VOLUME, 7);
#if HAS_BAT_SENSE
    if (lowBatWarned && ((millis() / 500) & 1)) {
        oled.drawStr(90, 32, "LOW!");
    } else {
        drawBattery(100, 24, batPct);
    }
#endif
    oled.sendBuffer();
}

// Sleep

void goToSleep(const char* why) {
    Serial.printf("Sleeping: %s\n", why);
    if (playing) { dacMute(); delay(10); audio.stopSong(); }
    ampOff();
    dacMute();
    drawScreen(why, "Sleeping...");
    delay(1500);
    oled.setPowerSave(1);
    saveState();

    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_PLAY, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

// Setup

void setup() {
  pinMode(PIN_AMP_EN, OUTPUT);   ampOff();
  pinMode(PIN_DAC_MUTE, OUTPUT); dacMute();
  pinMode(PIN_I2S_SCK, OUTPUT);  digitalWrite(PIN_I2S_SCK, LOW);

  Serial.begin(115200);
  delay(200);
  Serial.println("\nFuji Mini starting");

  btnPlay.begin(); btnNext.begin(); btnEnc.begin();
  pinMode(PIN_ENC_A, INPUT);
  pinMode(PIN_ENC_B, INPUT);
#if HAS_BAT_SENSE
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_SENSE, ADC_11db);
#endif

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  oled.begin();
  drawScreen("Fuji Mini", "Starting...");

  prefs.begin("fuji", false);
  volume       = prefs.getUChar("vol", DEFAULT_VOLUME);
  currentTrack = prefs.getUShort("track", 0);
  shuffleOn    = prefs.getBool("shuf", false);
  randomSeed(esp_random());

  spiSD.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, spiSD, 20000000);
  if (!sdOk) {
    drawScreen("No SD card", "Insert card, reset");
    Serial.println("SD mount failed");
  } else {
    scanTracks();
    if (tracks.empty()) drawScreen("No music found", "Put files in /music");
  }

  audio.setPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DIN);
  audio.setVolume(volume);

  batteryUpdate();
  lastActivity = millis();

  if (sdOk && !tracks.empty()) {
    if (currentTrack >= (int)tracks.size()) currentTrack = 0;
    displayName = prettyName(tracks[currentTrack]);
    startTrack(currentTrack);
  }
}

void loop() {
#if AUDIO_NEEDS_LOOP
  audio.loop();
#endif
  encoderUpdate();
  btnPlay.update(false);
  btnEnc.update(false);
  btnNext.update(true);

  if (encDelta != 0) {
    setVolume(volume + encDelta);
    encDelta = 0;
  }
  if (btnPlay.takeShort()) togglePause();
  if (btnNext.takeLong())  { prevTrack(); btnNext.takeShort(); }
  if (btnNext.takeShort()) nextTrack();
  if (btnEnc.takeShort())  { shuffleOn = !shuffleOn; saveState(); lastActivity = millis(); }

  if (playing && !paused && !audio.isRunning() && (millis() - trackStartMs) > 1500) {
    nextTrack();
  }

  if (playing && paused && (millis() - pausedSinceMs) > 10000) ampOff();

  if ((!playing || paused) && (millis() - lastActivity) > SLEEP_AFTER_MS) {
    goToSleep("Idle");
  }

  batteryUpdate();

  if (millis() - lastDisplayMs > 100) {
    lastDisplayMs = millis();
    if (sdOk && !tracks.empty()) drawScreen();
  }

  vTaskDelay(1);
}
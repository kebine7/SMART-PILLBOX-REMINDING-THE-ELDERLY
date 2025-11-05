#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <driver/ledc.h>
#include "secrets.h"


/* ----------- Hardware configuration ----------- */
#define PIN_NEOPIXEL 19
#define NUM_BINS     7

// HW-488: usually outputs LOW when there is an object -> active-low
#define IR_ACTIVE_LOW   1
const int IR_PINS[NUM_BINS] = {36, 39, 34, 35, 32, 33, 25};  // 34..39 just Input (OK)

// Buzzer (LEDC - ESP-IDF)
#define PIN_BUZZER   17                // 17
#define LEDC_CH      0                 // channels 0..7 (low speed)
#define LEDC_FREQ    2000              // Hz (2000)
#define LEDC_RES     10                // number of bits (8..15)
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0

// One button (for first 2 doses)
const uint8_t BTN_PIN        = 26;
const bool    BTN_PULLUP     = true;
const uint16_t DEBOUNCE_MS   = 30;
const uint16_t LONGPRESS_MS  = 800;

/* ----------- Time configuration ----------- */
const long  GMT_OFFSET_SEC      = 7 * 3600;   // VN GMT+7
const int   DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER          = "time.google.com";

/* ----------- Used colors (GRB) ----------- */
Adafruit_NeoPixel pixels(NUM_BINS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
uint32_t C(uint8_t r,uint8_t g,uint8_t b){ return pixels.Color(r,g,b); }
const uint32_t COL_IDLE = Adafruit_NeoPixel::Color(0,200,200);  // cyan: today's compartment
const uint32_t COL_OFF  = Adafruit_NeoPixel::Color(0,0,0);
const uint32_t COL_ALRM = Adafruit_NeoPixel::Color(220,0,0);    // red
const uint32_t COL_OK   = Adafruit_NeoPixel::Color(255,120,0);  // blinking orange when confirming

/* ----------- TFT ----------- */
TFT_eSPI tft;
char  sTime[16], sDate[16], sNext[16], sRemain[16];

/* ----------- Trạng thái ngày/cữ ----------- */
bool doseDone[3] = {false,false,false};     // confirmed?
uint8_t todayIdx = 0;                       // 0..6 (Mon..Sun)
int lastIR[NUM_BINS];                       // save previous reading
bool alarmOn = false;
uint8_t alarmDose = 0;                      // dose being notified
uint32_t alarmSinceMs = 0;

/* ----------- Telegram (optional) ----------- */
#define USE_TELEGRAM 1
#if USE_TELEGRAM
bool ensureWiFi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
  }

  return WiFi.status() == WL_CONNECTED;
}

void disableWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool tgSend(const String &msg) {
  if (!ensureWiFi()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url)) {
    disableWiFi(); // turn off Wi-Fi if failed
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "chat_id=" + String(CHAT_ID) + "&text=" + msg;
  int code = http.POST(body);
  http.end();

  disableWiFi(); // ✅ Wi-Fi OFF after sending

  return code == 200;
}

#endif

/* ----------- Buzzer helpers ----------- */
void buzzerSetup() {
  ledc_timer_config_t tcfg = {
    .speed_mode       = LEDC_MODE,
    .duty_resolution  = (ledc_timer_bit_t)LEDC_RES,
    .timer_num        = LEDC_TIMER,
    .freq_hz          = LEDC_FREQ,
    .clk_cfg          = LEDC_AUTO_CLK
  };
  ledc_timer_config(&tcfg);

  ledc_channel_config_t ccfg = {
    .gpio_num   = PIN_BUZZER,
    .speed_mode = LEDC_MODE,
    .channel    = (ledc_channel_t)LEDC_CH,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = LEDC_TIMER,
    .duty       = 0,
    .hpoint     = 0
  };
  ledc_channel_config(&ccfg);
}

static inline void buzzerOff() {
  ledc_set_duty(LEDC_MODE, (ledc_channel_t)LEDC_CH, 0);
  ledc_update_duty(LEDC_MODE, (ledc_channel_t)LEDC_CH);
}

static inline void buzzerOn50() { // duty ~50%
  ledc_set_duty(LEDC_MODE, (ledc_channel_t)LEDC_CH, 1  << (LEDC_RES - 1));
  ledc_update_duty(LEDC_MODE, (ledc_channel_t)LEDC_CH);
}

/* ----------- Button (debounce + events) ----------- */
bool stablePressed=false, lastReading=false, evtPressed=false, evtReleased=false, evtLong=false;
uint32_t lastChangeMs=0, pressedAtMs=0;
bool rawPressed(){
  int v = digitalRead(BTN_PIN);
  return BTN_PULLUP ? (v==LOW) : (v==HIGH);
}
void updateButton(){
  bool r = rawPressed(); uint32_t now=millis();
  if (r!=lastReading){ lastReading=r; lastChangeMs=now; }
  if ((now-lastChangeMs)>=DEBOUNCE_MS && r!=stablePressed){
    stablePressed=r;
    if (stablePressed){ evtPressed=true; evtLong=false; pressedAtMs=now; }
    else { evtReleased=true; }
  }
  if (stablePressed && !evtLong && (now-pressedAtMs)>=LONGPRESS_MS){ evtLong=true; }
}
bool takePressed(){ bool e=evtPressed; evtPressed=false; return e; }

/* ----------- Time widget ----------- */
// tm_wday: 0=Sun..6=Sat. We change Sunday=0, Monday=6 (reversal), with week starting on Sun
uint8_t dowSun0Reverse(const tm &t) {
  // (0=Sun -> 0, 1=Mon -> 6, 2=Tue -> 5, ..., 6=Sat -> 1)
  return (6 - t.tm_wday) % 7;
}

time_t buildTodayTime(const tm &now, uint8_t h, uint8_t m){
  tm t = now; t.tm_hour=h; t.tm_min=m; t.tm_sec=0;
  return mktime(&t);
}

// Find the next dose yet to be taken
// Returns index 0..2, or 3 if all done
uint8_t nextDoseIndex(const tm &now) {
  time_t now_t = mktime((tm*)&now);
  for (uint8_t i = 0; i < 3; i++) {
    time_t tDose = buildTodayTime(now, DOSES[i].h, DOSES[i].m);
    if (!doseDone[i] && difftime(tDose, now_t) > 0) {
      return i;  // Next dose
    }
  }
  return 3;  // ALl doses taken
}


/* ----------- LED WS2812 ----------- */
void showIdleLEDs(uint8_t today){
  pixels.clear();
  for (int i=0;i<NUM_BINS;i++) pixels.setPixelColor(i, (i==today)?COL_IDLE:COL_OFF);
  pixels.show();
}
void blinkAlarmLED(uint8_t bin, bool on){
  for (int i=0;i<NUM_BINS;i++) pixels.setPixelColor(i, COL_OFF);
  pixels.setPixelColor(bin, on?COL_ALRM:COL_OFF);
  pixels.show();
}
void flashOk(uint8_t bin){
  for (int k=0;k<3;k++){
    pixels.setPixelColor(bin, COL_OK); pixels.show(); delay(120);
    pixels.setPixelColor(bin, COL_OFF); pixels.show(); delay(80);
  }
  showIdleLEDs(bin);
}

/* ----------- TFT UI ----------- */
void drawMain(const tm &now){
  // time
  strftime(sTime,sizeof(sTime),"%H:%M:%S",&now);
  strftime(sDate,sizeof(sDate),"23-08-2025",&now);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(sTime, 100, 13, 2);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);  tft.setTextSize(2);
  tft.drawString(sDate, 80, 40, 2);

  // next dose + countdown
  uint8_t nd = nextDoseIndex(now);
  time_t tNow = time(nullptr);

  if (nd < 3) {
    time_t tDose = buildTodayTime(now, DOSES[nd].h, DOSES[nd].m);
    int32_t remain = (int32_t)tDose - (int32_t)tNow;
    if (remain < 0) remain = 0;

    snprintf(sNext, sizeof(sNext), "Next: %02u:%02u", DOSES[nd].h, DOSES[nd].m);
    int hh = remain / 3600;
    int mm = (remain % 3600) / 60;
    int ss = remain % 60;
    snprintf(sRemain, sizeof(sRemain), "T-%02d:%02d:%02d", hh, mm, ss);
  }
  else {
    strcpy(sNext, "All doses done");
    strcpy(sRemain, "");
  }

  tft.setTextColor(TFT_CYAN, TFT_BLACK);  tft.drawString(sNext,   40, 70, 2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.drawString(sRemain, 40, 100, 2);

  // 3 dose state
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i=0;i<3;i++){
    char line[32];
    snprintf(line,sizeof(line),"Dose %d %02u:%02u  [%s]", i+1, DOSES[i].h, DOSES[i].m, doseDone[i]?"OK":"...");
    tft.drawString(line, 40, 130+i*30, 2);
  }
}

/* ----------- Config ----------- */
void connectWiFi(){
  tft.setCursor(10,10); tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status()!=WL_CONNECTED){ delay(300); tft.print("."); }
  tft.println("\nWiFi OK");
}
void syncTime(){
  tft.println("Syncing NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  for (int i=0;i<30;i++){
    time_t now=time(nullptr);
    if (now>1700000000) break;
    delay(500); tft.print(".");
  }
  tft.println("\nTime synced");
}

/* ----------- Setup ----------- */
void setup(){
  Serial.begin(115200);
  // TFT
  tft.init(); tft.setRotation(3); tft.fillScreen(TFT_BLACK);

  // WiFi + NTP
  connectWiFi(); syncTime(); delay(700); tft.fillScreen(TFT_BLACK);

  // WS2812
  pixels.begin(); pixels.clear(); pixels.setBrightness(60); pixels.show();

  // IR pins
  for (int i=0;i<NUM_BINS;i++){ pinMode(IR_PINS[i], INPUT); lastIR[i]=digitalRead(IR_PINS[i]); }

  // Buzzer
  buzzerSetup();
  buzzerOff();

  // Button
  if (BTN_PULLUP) pinMode(BTN_PIN, INPUT_PULLUP); else pinMode(BTN_PIN, INPUT);
  stablePressed=lastReading=rawPressed();

  // Calculate todayIdx & initial LED idle
  time_t nowt=time(nullptr); tm now; localtime_r(&nowt,&now);
  todayIdx = dowSun0Reverse(now);
  showIdleLEDs(todayIdx);
}

/* ----------- Alarm & Acknowledgement Logic ----------- */
void maybeStartAlarm(const tm &now){
  if (alarmOn) return;
  for (uint8_t i=0;i<3;i++){
    if (doseDone[i]) continue;
    time_t tDose = buildTodayTime(now, DOSES[i].h, DOSES[i].m);
    if (mktime((tm*)&now) >= tDose){
      alarmOn = true; alarmDose = i; alarmSinceMs = millis();
#if USE_TELEGRAM
      tgSend(String("It is time for dose #")+String(i+1));
#endif
      break;
    }
  }
}

void processAlarm(uint8_t today){
  // LED flashing + intermittent beeping
  bool ledOn = ((millis()/400)%2)==0;
  blinkAlarmLED(today, ledOn);
  if ((millis()/800)%2==0) buzzerOn50(); else buzzerOff();

  // Confirm:
  // - Timer 1,2: press button
  // - Timer 3: simply take medicine out
  updateButton();
  bool confirmed = false;
  if (alarmDose < 2){
    if (takePressed()) confirmed = true;
  }else{
    int nowIR = digitalRead(IR_PINS[today]);
    if (IR_ACTIVE_LOW) { // invert
      if (nowIR != lastIR[today]) confirmed = true;
    }else{
      if (nowIR != lastIR[today]) confirmed = true;
    }
    lastIR[today] = nowIR;
  }

  if (confirmed){
    buzzerOff();
    doseDone[alarmDose] = true;
    flashOk(today);
    alarmOn = false;

#if USE_TELEGRAM
    tgSend(String("Dose #")+String(alarmDose+1)+" confirmed ✅");
#endif
  }else{
    // If late for more than LATE_GRACE_MIN minutes -> send warning (1 time)
#if USE_TELEGRAM
    static bool lateNoticeSent=false;
    if (!lateNoticeSent && (millis()-alarmSinceMs) > LATE_GRACE_MIN*60UL*1000UL){
      lateNoticeSent=true;
      tgSend(String("Dose #")+String(alarmDose+1)+" late ❗");
    }
#endif
  }
}

/* ----------- Loop ----------- */
void loop(){
  // Updates time
  time_t nowt = time(nullptr); tm now; localtime_r(&nowt,&now);

  // Change date at midnight -> reset status
  static int lastY=0,lastM=0,lastD=0;
  if (now.tm_year!=lastY || now.tm_mon!=lastM || now.tm_mday!=lastD){
    lastY=now.tm_year; lastM=now.tm_mon; lastD=now.tm_mday;
    for (auto &x: doseDone) x=false;
    todayIdx = dowSun0Reverse(now);
    showIdleLEDs(todayIdx);
  }

  // If not reported -> consider start alarm conditions
  maybeStartAlarm(now);

  // Draw UI
  drawMain(now);

  // If alarm is on: run effect + check confirmation
  if (alarmOn) processAlarm(todayIdx);
  else {
    // idle: make sure today drawer LED is on cyan (not flashing)
    static uint32_t lastLedRefresh=0;
    if (millis()-lastLedRefresh>1500){ lastLedRefresh=millis(); showIdleLEDs(todayIdx); }
    buzzerOff();
  }

  delay(120); // UI refresh rate ~8 Hz
}



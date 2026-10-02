#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>


// ========== BOOT BITMAP (separate file) ==========
#include "boot_img.h"

// ========== WiFi ==========
const char* ssid     = "";
const char* password = "";

// ========== Display & Touch ==========
#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  240
#define CHSC6X_I2C_ID  0x2e
#define CHSC6X_READ_POINT_LEN 5
#define TOUCH_INT      D7
// brightness controls
#define BACKLIGHT_PIN  21       // Round Display Backlight Pin
#define PWM_FREQ       5000     // 5 kHz frequency to eliminate audible buzzing
#define PWM_RES        8        // 8-bit resolution (values from 0 to 255)
int activeBrightness = 255;
int idleBrightness =   10;

TFT_eSPI tft = TFT_eSPI();

// ========== NTP & Timezone (IST = UTC+5:30) ==========
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
String timezone = "UTC0";

// ========== Storage ==========
Preferences preferences;
#define MAX_MEDS       20

struct Med {
  char name[24];
  int  hour;
  int  minute;
  bool active;
  bool takenToday;
};
Med meds[MAX_MEDS];
int medCount = 0;

// ========== Global State ==========
WebServer server(80);
String localIP = "";
bool showingIP = true;
unsigned long ipShowStart = 0;
bool reminderActive = false;
int reminderIndex = -1;

// Clock update – no flicker
String lastTimeStr = "";
int lastTimeX = 0, lastTimeY = 95;
bool clockInitialised = false;

// ========== Touch Functions ==========
void round_display_touch_init(void) {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  Wire.begin();
}

bool chsc6x_is_pressed(void) {
  if(digitalRead(TOUCH_INT) != LOW) {
    delay(1);
    if(digitalRead(TOUCH_INT) != LOW) return false;
  }
  return true;
}

bool chsc6x_get_xy(int32_t &x, int32_t &y) {
  uint8_t temp[CHSC6X_READ_POINT_LEN] = {0};
  Wire.requestFrom(CHSC6X_I2C_ID, CHSC6X_READ_POINT_LEN);
  if (Wire.available() == CHSC6X_READ_POINT_LEN) {
    for(int i=0; i<CHSC6X_READ_POINT_LEN; i++) temp[i] = Wire.read();
    if (temp[0] == 0x01) {
      int32_t raw_x = temp[2];
      int32_t raw_y = temp[4];
      uint8_t rotation = tft.getRotation();
      switch(rotation) {
        case 0:  x = raw_x;       y = raw_y;                break;
        case 1:  x = raw_y;       y = SCREEN_WIDTH-raw_x;   break;
        case 2:  x = SCREEN_WIDTH-raw_x; y = SCREEN_HEIGHT-raw_y; break;
        case 3:  x = SCREEN_HEIGHT-raw_y; y = raw_x;        break;
      }
      return true;
    }
  }
  return false;
}

// ========== Display Brightness ======
void setBrightness(int value) {
  if (value < 0)   value = 0;
  if (value > 255) value = 255;
  ledcWrite(BACKLIGHT_PIN, value);
}

// ========== Storage Helpers ==========
void saveMeds() {
  preferences.begin("meds", false);
  preferences.putInt("medCount", medCount);
  for (int i = 0; i < medCount; i++) {
    preferences.putBytes("med" + i, &meds[i], sizeof(Med));
  }
  preferences.end();
}

void loadMeds() {
  preferences.begin("meds", true);
  medCount = preferences.getInt("medCount", 0);
  if (medCount < 0 || medCount > MAX_MEDS) medCount = 0;
  for (int i = 0; i < medCount; i++) {
    preferences.getBytes("med" + i, &meds[i], sizeof(Med));
  }
  preferences.end();
}

void saveTimezone() {
  preferences.begin("config", false);
  preferences.putString("timezone", timezone);
  preferences.end();
}

void loadTimezone() {
  preferences.begin("config", true);
  timezone = preferences.getString("timezone", "UTC0");
  preferences.end();
}

void saveActiveBrightness() {
  preferences.begin("config", false);
  preferences.putInt("activeBrightness", activeBrightness);
  preferences.end();
}

void loadActiveBrightness() {
  preferences.begin("config", true);
  activeBrightness = preferences.getInt("activeBrightness", 255);
  preferences.end();
  if (activeBrightness < 0 || activeBrightness > 255) activeBrightness = 255;
}

void saveIdleBrightness() {
  preferences.begin("config", false);
  preferences.putInt("idleBrightness", idleBrightness);
  preferences.end();
}

void loadIdleBrightness() {
  preferences.begin("config", true);
  idleBrightness = preferences.getInt("idleBrightness", 10);
  preferences.end();
  if (idleBrightness < 0 || idleBrightness > 255) idleBrightness = 10;
}

// ========== Time Formatting ==========
String getTimeAsString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--";
  char buffer[6];
  sprintf(buffer, "%d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buffer);
}

String formatTimeAsString(int hour, int minute) {
  char buffer[10];
  sprintf(buffer, "%d:%02d", hour, minute);
  return String(buffer);
}

void syncTime() {
  configTzTime(timezone.c_str(), ntpServer1, ntpServer2);
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    delay(500);
    retry++;
  }
}

// ========== Med Management ==========
int findNextReminderIndex() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return -1;
  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  
  int bestIdx = -1;
  int bestTime = 24*60;
  for (int i = 0; i < medCount; i++) {
    if (!meds[i].active || meds[i].takenToday) continue;
    int medMinutes = meds[i].hour * 60 + meds[i].minute;
    if (medMinutes >= nowMinutes && medMinutes < bestTime) {
      bestTime = medMinutes;
      bestIdx = i;
    }
  }
  if (bestIdx == -1) {
    for (int i = 0; i < medCount; i++) {
      if (meds[i].active && !meds[i].takenToday) {
        bestIdx = i;
        break;
      }
    }
  }
  return bestIdx;
}

void checkReminders() {
  if (reminderActive) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  
  for (int i = 0; i < medCount; i++) {
    if (!meds[i].active || meds[i].takenToday) continue;
    int medMinutes = meds[i].hour * 60 + meds[i].minute;
    if (medMinutes <= nowMinutes) {
      reminderActive = true;
      reminderIndex = i;
      drawReminderScreen(i);
      break;
    }
  }
}

// ========== CLOCK SCREEN ==========
void drawClockScreen() {
  tft.fillScreen(TFT_BLACK);
  
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String timeStr = getTimeAsString();
  int timeWidth = tft.textWidth(timeStr);
  lastTimeX = (SCREEN_WIDTH - timeWidth) / 2;
  lastTimeY = 95;
  tft.drawString(timeStr, lastTimeX, lastTimeY);
  lastTimeStr = timeStr;
  
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  int nextIdx = findNextReminderIndex();
  if (nextIdx >= 0) {
    String line1 = "Next : " + String(meds[nextIdx].name);
    int w1 = tft.textWidth(line1);
    tft.drawString(line1, (SCREEN_WIDTH - w1) / 2, 160);
    
    String nextTime = formatTimeAsString(meds[nextIdx].hour, meds[nextIdx].minute);
    int w2 = tft.textWidth(nextTime);
    tft.drawString(nextTime, (SCREEN_WIDTH - w2) / 2, 190);
  } else {
    String noRem = "No reminders";
    int w = tft.textWidth(noRem);
    tft.drawString(noRem, (SCREEN_WIDTH - w) / 2, 175);
  }
  
  clockInitialised = true;
}

void updateClockTime() {
  if (!clockInitialised) return;
  
  String newTime = getTimeAsString();
  if (newTime == lastTimeStr) return;
  
  tft.setFreeFont(&FreeSansBold24pt7b);
  int w = tft.textWidth(lastTimeStr);
  int h = tft.fontHeight();
  tft.fillRect(lastTimeX, lastTimeY - 5, w, h + 5, TFT_BLACK);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(newTime, lastTimeX, lastTimeY);
  
  lastTimeStr = newTime;
  setBrightness(idleBrightness);
}

// ========== REMINDER SCREEN ==========
void drawReminderScreen(int idx) {
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.drawString("Take Meds", 24, 88);
  
  tft.setFreeFont(&FreeSans12pt7b);
  String medName = String(meds[idx].name);
  int nameWidth = tft.textWidth(medName);
  int nameX = (SCREEN_WIDTH - nameWidth) / 2;
  tft.drawString(medName, nameX, 136);
  
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Tap to take", 75, 204);
  setBrightness(activeBrightness);
}

void drawMedTakenScreen() {
  tft.fillScreen(TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setFreeFont(&FreeSans18pt7b);
  tft.drawString("Med taken!", 30, 100);
  delay(1500);
}

// ========== BOOT BITMAP – YOUR FULL-SCREEN LOGO ==========
void drawBootBitmap() {
  tft.pushImage(0, 0, 240, 240, (uint16_t*)image_bitmap_pixels);
  delay(1500);
}

// ========== WEB UI ==========
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>Med Reminder</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      background: linear-gradient(145deg, #1e2b3a 0%, #0f172a 100%);
      min-height: 100vh;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 16px;
    }
    .card {
      max-width: 480px;
      width: 100%;
      background: rgba(255,255,255,0.08);
      backdrop-filter: blur(20px);
      border-radius: 32px;
      padding: 24px;
      border: 1px solid rgba(255,255,255,0.1);
      box-shadow: 0 25px 50px -12px rgba(0,0,0,0.5);
    }
    h1 {
      font-size: 28px;
      font-weight: 600;
      color: #a5f3fc;
      margin-bottom: 8px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .subtitle {
      color: #94a3b8;
      font-size: 14px;
      margin-bottom: 24px;
      border-bottom: 1px solid #334155;
      padding-bottom: 16px;
    }
    .med-list {
      display: flex;
      flex-direction: column;
      gap: 12px;
      margin-bottom: 20px;
    }
    .med-item {
      background: rgba(30, 41, 59, 0.7);
      border-radius: 20px;
      padding: 16px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      backdrop-filter: blur(5px);
      border: 1px solid rgba(255,255,255,0.05);
    }
    .med-info {
      display: flex;
      flex-direction: column;
    }
    .med-name {
      color: #f1f5f9;
      font-size: 18px;
      font-weight: 600;
    }
    .med-time {
      color: #7dd3fc;
      font-size: 15px;
      margin-top: 4px;
    }
    .delete-btn {
      background: rgba(239, 68, 68, 0.2);
      border: 1px solid rgba(239, 68, 68, 0.5);
      color: #fecaca;
      width: 44px;
      height: 44px;
      border-radius: 30px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 24px;
      cursor: pointer;
      transition: 0.2s;
    }
    .delete-btn:hover {
      background: rgba(239, 68, 68, 0.4);
    }
    .add-button {
      background: linear-gradient(135deg, #2dd4bf, #06b6d4);
      border: none;
      color: white;
      font-size: 20px;
      font-weight: 600;
      padding: 18px;
      border-radius: 40px;
      width: 100%;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      cursor: pointer;
      margin: 16px 0 24px 0;
      border: 1px solid rgba(255,255,255,0.2);
      box-shadow: 0 10px 20px -5px rgba(6,182,212,0.3);
    }
    .section-title {
      color: #cbd5e1;
      font-size: 18px;
      margin-bottom: 16px;
      font-weight: 500;
    }
    .config-group {
      background: rgba(15, 23, 42, 0.6);
      border-radius: 24px;
      padding: 20px;
      margin-bottom: 16px;
    }
    label {
      color: #94a3b8;
      display: block;
      margin-bottom: 8px;
      font-size: 15px;
    }
    output {
      color: #94a3b8;
      display: block;
      margin-bottom: 8px;
      font-size: 15px;
    }
    input[type="range"] {
      width: 100%;
    }
    select {
      width: 100%;
      padding: 12px 16px;
      background: #1e293b;
      border: 1px solid #334155;
      border-radius: 20px;
      color: white;
      font-size: 16px;
      margin-bottom: 20px;
    }
    .form-popup {
      background: #0f172a;
      border-radius: 32px;
      padding: 24px;
      margin-top: 16px;
      border: 1px solid #334155;
      display: none;
    }
    .form-group {
      margin-bottom: 20px;
    }
    .form-group input {
      width: 100%;
      padding: 14px;
      background: #1e293b;
      border: 1px solid #334155;
      border-radius: 20px;
      color: white;
      font-size: 16px;
    }
    .btn {
      background: #2dd4bf;
      border: none;
      color: #0f172a;
      font-weight: 600;
      padding: 14px;
      border-radius: 30px;
      width: 48%;
      font-size: 16px;
      cursor: pointer;
    }
    .btn.cancel {
      background: #475569;
      color: white;
    }
    .empty-state {
      text-align: center;
      padding: 40px 20px;
      color: #64748b;
    }
  </style>
</head>
<body>
<div class="card">
  <h1>💊 Med Reminder</h1>
  <div class="subtitle">Connected to )rawliteral" + localIP + R"rawliteral(</div>
  
  <div id="medList" class="med-list"></div>
  <button class="add-button" onclick="showForm()">➕ Add Medication</button>
  
  <div id="addForm" class="form-popup">
    <h3 style="color:#a5f3fc; margin-bottom:20px;">New Med</h3>
    <div class="form-group">
      <input type="text" id="medName" placeholder="Name (e.g. Vitamin D)">
    </div>
    <div class="form-group" style="display:flex; gap:10px;">
      <input type="number" id="hour" placeholder="Hour (0-23)" min="0" max="23">
      <input type="number" id="minute" placeholder="Minute (0-59)" min="0" max="59">
    </div>
    <div style="display:flex; gap:10px;">
      <button class="btn" onclick="saveMed()">Save</button>
      <button class="btn cancel" onclick="hideForm()">Cancel</button>
    </div>
  </div>
  <div class="section-title">☀️ Display Brightness</div>
  <div class="config-group">

    <label for="activeBrightness">Active Brightness (0-255):</label><br>
    <output id="activeOutput"></output>
    <input type="range" id="activeBrightness" min="0" max="255" oninput="activeOutput.value = activeBrightness.value"><br>
    <label for="idleBrightness">Idle Brightness (0-255):</label><br>
    <output id="idleOutput"></output>
    <input type="range" id="idleBrightness" min="0" max="255" oninput="idleOutput.value = idleBrightness.value"><br>
    <button class="add-button" onclick="saveBrightness()">Save</button>
  </div>
  <div class="section-title">⚙️ Timezone</div>
  <div class="config-group">
    <label>UTC Offset</label>
    <!-- https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv?plain=1 -->
    <select id="timezoneSelect">
      <option value="GMT0">Africa/Abidjan</option>
      <option value="GMT0">Africa/Accra</option>
      <option value="EAT-3">Africa/Addis_Ababa</option>
      <option value="CET-1">Africa/Algiers</option>
      <option value="EAT-3">Africa/Asmara</option>
      <option value="GMT0">Africa/Bamako</option>
      <option value="WAT-1">Africa/Bangui</option>
      <option value="GMT0">Africa/Banjul</option>
      <option value="GMT0">Africa/Bissau</option>
      <option value="CAT-2">Africa/Blantyre</option>
      <option value="WAT-1">Africa/Brazzaville</option>
      <option value="CAT-2">Africa/Bujumbura</option>
      <option value="EET-2EEST,M4.5.5/0,M10.5.4/24">Africa/Cairo</option>
      <option value="<+01>-1">Africa/Casablanca</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Africa/Ceuta</option>
      <option value="GMT0">Africa/Conakry</option>
      <option value="GMT0">Africa/Dakar</option>
      <option value="EAT-3">Africa/Dar_es_Salaam</option>
      <option value="EAT-3">Africa/Djibouti</option>
      <option value="WAT-1">Africa/Douala</option>
      <option value="<+01>-1">Africa/El_Aaiun</option>
      <option value="GMT0">Africa/Freetown</option>
      <option value="CAT-2">Africa/Gaborone</option>
      <option value="CAT-2">Africa/Harare</option>
      <option value="SAST-2">Africa/Johannesburg</option>
      <option value="CAT-2">Africa/Juba</option>
      <option value="EAT-3">Africa/Kampala</option>
      <option value="CAT-2">Africa/Khartoum</option>
      <option value="CAT-2">Africa/Kigali</option>
      <option value="WAT-1">Africa/Kinshasa</option>
      <option value="WAT-1">Africa/Lagos</option>
      <option value="WAT-1">Africa/Libreville</option>
      <option value="GMT0">Africa/Lome</option>
      <option value="WAT-1">Africa/Luanda</option>
      <option value="CAT-2">Africa/Lubumbashi</option>
      <option value="CAT-2">Africa/Lusaka</option>
      <option value="WAT-1">Africa/Malabo</option>
      <option value="CAT-2">Africa/Maputo</option>
      <option value="SAST-2">Africa/Maseru</option>
      <option value="SAST-2">Africa/Mbabane</option>
      <option value="EAT-3">Africa/Mogadishu</option>
      <option value="GMT0">Africa/Monrovia</option>
      <option value="EAT-3">Africa/Nairobi</option>
      <option value="WAT-1">Africa/Ndjamena</option>
      <option value="WAT-1">Africa/Niamey</option>
      <option value="GMT0">Africa/Nouakchott</option>
      <option value="GMT0">Africa/Ouagadougou</option>
      <option value="WAT-1">Africa/Porto-Novo</option>
      <option value="GMT0">Africa/Sao_Tome</option>
      <option value="EET-2">Africa/Tripoli</option>
      <option value="CET-1">Africa/Tunis</option>
      <option value="CAT-2">Africa/Windhoek</option>
      <option value="HST10HDT,M3.2.0,M11.1.0">America/Adak</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Anchorage</option>
      <option value="AST4">America/Anguilla</option>
      <option value="AST4">America/Antigua</option>
      <option value="<-03>3">America/Araguaina</option>
      <option value="<-03>3">America/Argentina/Buenos_Aires</option>
      <option value="<-03>3">America/Argentina/Catamarca</option>
      <option value="<-03>3">America/Argentina/Cordoba</option>
      <option value="<-03>3">America/Argentina/Jujuy</option>
      <option value="<-03>3">America/Argentina/La_Rioja</option>
      <option value="<-03>3">America/Argentina/Mendoza</option>
      <option value="<-03>3">America/Argentina/Rio_Gallegos</option>
      <option value="<-03>3">America/Argentina/Salta</option>
      <option value="<-03>3">America/Argentina/San_Juan</option>
      <option value="<-03>3">America/Argentina/San_Luis</option>
      <option value="<-03>3">America/Argentina/Tucuman</option>
      <option value="<-03>3">America/Argentina/Ushuaia</option>
      <option value="AST4">America/Aruba</option>
      <option value="<-03>3">America/Asuncion</option>
      <option value="EST5">America/Atikokan</option>
      <option value="<-03>3">America/Bahia</option>
      <option value="CST6">America/Bahia_Banderas</option>
      <option value="AST4">America/Barbados</option>
      <option value="<-03>3">America/Belem</option>
      <option value="CST6">America/Belize</option>
      <option value="AST4">America/Blanc-Sablon</option>
      <option value="<-04>4">America/Boa_Vista</option>
      <option value="<-05>5">America/Bogota</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Boise</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Cambridge_Bay</option>
      <option value="<-04>4">America/Campo_Grande</option>
      <option value="EST5">America/Cancun</option>
      <option value="<-04>4">America/Caracas</option>
      <option value="<-03>3">America/Cayenne</option>
      <option value="EST5">America/Cayman</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Chicago</option>
      <option value="CST6">America/Chihuahua</option>
      <option value="CST6">America/Costa_Rica</option>
      <option value="MST7">America/Creston</option>
      <option value="<-04>4">America/Cuiaba</option>
      <option value="AST4">America/Curacao</option>
      <option value="GMT0">America/Danmarkshavn</option>
      <option value="MST7">America/Dawson</option>
      <option value="MST7">America/Dawson_Creek</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Denver</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Detroit</option>
      <option value="AST4">America/Dominica</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Edmonton</option>
      <option value="<-05>5">America/Eirunepe</option>
      <option value="CST6">America/El_Salvador</option>
      <option value="<-03>3">America/Fortaleza</option>
      <option value="MST7">America/Fort_Nelson</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">America/Glace_Bay</option>
      <option value="<-02>2<-01>,M3.5.0/-1,M10.5.0/0">America/Godthab</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">America/Goose_Bay</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Grand_Turk</option>
      <option value="AST4">America/Grenada</option>
      <option value="AST4">America/Guadeloupe</option>
      <option value="CST6">America/Guatemala</option>
      <option value="<-05>5">America/Guayaquil</option>
      <option value="<-04>4">America/Guyana</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">America/Halifax</option>
      <option value="CST5CDT,M3.2.0/0,M11.1.0/1">America/Havana</option>
      <option value="MST7">America/Hermosillo</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Indianapolis</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Indiana/Knox</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Marengo</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Petersburg</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Indiana/Tell_City</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Vevay</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Vincennes</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Indiana/Winamac</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Inuvik</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Iqaluit</option>
      <option value="EST5">America/Jamaica</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Juneau</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Kentucky/Louisville</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Kentucky/Monticello</option>
      <option value="AST4">America/Kralendijk</option>
      <option value="<-04>4">America/La_Paz</option>
      <option value="<-05>5">America/Lima</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">America/Los_Angeles</option>
      <option value="AST4">America/Lower_Princes</option>
      <option value="<-03>3">America/Maceio</option>
      <option value="CST6">America/Managua</option>
      <option value="<-04>4">America/Manaus</option>
      <option value="AST4">America/Marigot</option>
      <option value="AST4">America/Martinique</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Matamoros</option>
      <option value="MST7">America/Mazatlan</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Menominee</option>
      <option value="CST6">America/Merida</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Metlakatla</option>
      <option value="CST6">America/Mexico_City</option>
      <option value="<-03>3<-02>,M3.2.0,M11.1.0">America/Miquelon</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">America/Moncton</option>
      <option value="CST6">America/Monterrey</option>
      <option value="<-03>3">America/Montevideo</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Montreal</option>
      <option value="AST4">America/Montserrat</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Nassau</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/New_York</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Nipigon</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Nome</option>
      <option value="<-02>2">America/Noronha</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/North_Dakota/Beulah</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/North_Dakota/Center</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/North_Dakota/New_Salem</option>
      <option value="<-02>2<-01>,M3.5.0/-1,M10.5.0/0">America/Nuuk</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Ojinaga</option>
      <option value="EST5">America/Panama</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Pangnirtung</option>
      <option value="<-03>3">America/Paramaribo</option>
      <option value="MST7">America/Phoenix</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Port-au-Prince</option>
      <option value="AST4">America/Port_of_Spain</option>
      <option value="<-04>4">America/Porto_Velho</option>
      <option value="AST4">America/Puerto_Rico</option>
      <option value="<-03>3">America/Punta_Arenas</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Rainy_River</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Rankin_Inlet</option>
      <option value="<-03>3">America/Recife</option>
      <option value="CST6">America/Regina</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Resolute</option>
      <option value="<-05>5">America/Rio_Branco</option>
      <option value="<-03>3">America/Santarem</option>
      <option value="<-04>4<-03>,M9.1.6/24,M4.1.6/24">America/Santiago</option>
      <option value="AST4">America/Santo_Domingo</option>
      <option value="<-03>3">America/Sao_Paulo</option>
      <option value="<-02>2<-01>,M3.5.0/-1,M10.5.0/0">America/Scoresbysund</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Sitka</option>
      <option value="AST4">America/St_Barthelemy</option>
      <option value="NST3:30NDT,M3.2.0,M11.1.0">America/St_Johns</option>
      <option value="AST4">America/St_Kitts</option>
      <option value="AST4">America/St_Lucia</option>
      <option value="AST4">America/St_Thomas</option>
      <option value="AST4">America/St_Vincent</option>
      <option value="CST6">America/Swift_Current</option>
      <option value="CST6">America/Tegucigalpa</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">America/Thule</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Thunder_Bay</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">America/Tijuana</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">America/Toronto</option>
      <option value="AST4">America/Tortola</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">America/Vancouver</option>
      <option value="MST7">America/Whitehorse</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">America/Winnipeg</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">America/Yakutat</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">America/Yellowknife</option>
      <option value="<+08>-8">Antarctica/Casey</option>
      <option value="<+07>-7">Antarctica/Davis</option>
      <option value="<+10>-10">Antarctica/DumontDUrville</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Antarctica/Macquarie</option>
      <option value="<+05>-5">Antarctica/Mawson</option>
      <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">Antarctica/McMurdo</option>
      <option value="<-03>3">Antarctica/Palmer</option>
      <option value="<-03>3">Antarctica/Rothera</option>
      <option value="<+03>-3">Antarctica/Syowa</option>
      <option value="<+00>0<+02>-2,M3.5.0/1,M10.5.0/3">Antarctica/Troll</option>
      <option value="<+05>-5">Antarctica/Vostok</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Arctic/Longyearbyen</option>
      <option value="<+03>-3">Asia/Aden</option>
      <option value="<+05>-5">Asia/Almaty</option>
      <option value="<+03>-3">Asia/Amman</option>
      <option value="<+12>-12">Asia/Anadyr</option>
      <option value="<+05>-5">Asia/Aqtau</option>
      <option value="<+05>-5">Asia/Aqtobe</option>
      <option value="<+05>-5">Asia/Ashgabat</option>
      <option value="<+05>-5">Asia/Atyrau</option>
      <option value="<+03>-3">Asia/Baghdad</option>
      <option value="<+03>-3">Asia/Bahrain</option>
      <option value="<+04>-4">Asia/Baku</option>
      <option value="<+07>-7">Asia/Bangkok</option>
      <option value="<+07>-7">Asia/Barnaul</option>
      <option value="EET-2EEST,M3.5.0/0,M10.5.0/0">Asia/Beirut</option>
      <option value="<+06>-6">Asia/Bishkek</option>
      <option value="<+08>-8">Asia/Brunei</option>
      <option value="<+09>-9">Asia/Chita</option>
      <option value="<+08>-8">Asia/Choibalsan</option>
      <option value="<+0530>-5:30">Asia/Colombo</option>
      <option value="<+03>-3">Asia/Damascus</option>
      <option value="<+06>-6">Asia/Dhaka</option>
      <option value="<+09>-9">Asia/Dili</option>
      <option value="<+04>-4">Asia/Dubai</option>
      <option value="<+05>-5">Asia/Dushanbe</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Asia/Famagusta</option>
      <option value="EET-2EEST,M3.4.4/50,M10.4.4/50">Asia/Gaza</option>
      <option value="EET-2EEST,M3.4.4/50,M10.4.4/50">Asia/Hebron</option>
      <option value="<+07>-7">Asia/Ho_Chi_Minh</option>
      <option value="HKT-8">Asia/Hong_Kong</option>
      <option value="<+07>-7">Asia/Hovd</option>
      <option value="<+08>-8">Asia/Irkutsk</option>
      <option value="WIB-7">Asia/Jakarta</option>
      <option value="WIT-9">Asia/Jayapura</option>
      <option value="IST-2IDT,M3.4.4/26,M10.5.0">Asia/Jerusalem</option>
      <option value="<+0430>-4:30">Asia/Kabul</option>
      <option value="<+12>-12">Asia/Kamchatka</option>
      <option value="PKT-5">Asia/Karachi</option>
      <option value="<+0545>-5:45">Asia/Kathmandu</option>
      <option value="<+09>-9">Asia/Khandyga</option>
      <option value="IST-5:30">Asia/Kolkata</option>
      <option value="<+07>-7">Asia/Krasnoyarsk</option>
      <option value="<+08>-8">Asia/Kuala_Lumpur</option>
      <option value="<+08>-8">Asia/Kuching</option>
      <option value="<+03>-3">Asia/Kuwait</option>
      <option value="CST-8">Asia/Macau</option>
      <option value="<+11>-11">Asia/Magadan</option>
      <option value="WITA-8">Asia/Makassar</option>
      <option value="PST-8">Asia/Manila</option>
      <option value="<+04>-4">Asia/Muscat</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Asia/Nicosia</option>
      <option value="<+07>-7">Asia/Novokuznetsk</option>
      <option value="<+07>-7">Asia/Novosibirsk</option>
      <option value="<+06>-6">Asia/Omsk</option>
      <option value="<+05>-5">Asia/Oral</option>
      <option value="<+07>-7">Asia/Phnom_Penh</option>
      <option value="WIB-7">Asia/Pontianak</option>
      <option value="KST-9">Asia/Pyongyang</option>
      <option value="<+03>-3">Asia/Qatar</option>
      <option value="<+05>-5">Asia/Qyzylorda</option>
      <option value="<+03>-3">Asia/Riyadh</option>
      <option value="<+11>-11">Asia/Sakhalin</option>
      <option value="<+05>-5">Asia/Samarkand</option>
      <option value="KST-9">Asia/Seoul</option>
      <option value="CST-8">Asia/Shanghai</option>
      <option value="<+08>-8">Asia/Singapore</option>
      <option value="<+11>-11">Asia/Srednekolymsk</option>
      <option value="CST-8">Asia/Taipei</option>
      <option value="<+05>-5">Asia/Tashkent</option>
      <option value="<+04>-4">Asia/Tbilisi</option>
      <option value="<+0330>-3:30">Asia/Tehran</option>
      <option value="<+06>-6">Asia/Thimphu</option>
      <option value="JST-9">Asia/Tokyo</option>
      <option value="<+07>-7">Asia/Tomsk</option>
      <option value="<+08>-8">Asia/Ulaanbaatar</option>
      <option value="<+06>-6">Asia/Urumqi</option>
      <option value="<+10>-10">Asia/Ust-Nera</option>
      <option value="<+07>-7">Asia/Vientiane</option>
      <option value="<+10>-10">Asia/Vladivostok</option>
      <option value="<+09>-9">Asia/Yakutsk</option>
      <option value="<+0630>-6:30">Asia/Yangon</option>
      <option value="<+05>-5">Asia/Yekaterinburg</option>
      <option value="<+04>-4">Asia/Yerevan</option>
      <option value="<-01>1<+00>,M3.5.0/0,M10.5.0/1">Atlantic/Azores</option>
      <option value="AST4ADT,M3.2.0,M11.1.0">Atlantic/Bermuda</option>
      <option value="WET0WEST,M3.5.0/1,M10.5.0">Atlantic/Canary</option>
      <option value="<-01>1">Atlantic/Cape_Verde</option>
      <option value="WET0WEST,M3.5.0/1,M10.5.0">Atlantic/Faroe</option>
      <option value="WET0WEST,M3.5.0/1,M10.5.0">Atlantic/Madeira</option>
      <option value="GMT0">Atlantic/Reykjavik</option>
      <option value="<-02>2">Atlantic/South_Georgia</option>
      <option value="<-03>3">Atlantic/Stanley</option>
      <option value="GMT0">Atlantic/St_Helena</option>
      <option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Australia/Adelaide</option>
      <option value="AEST-10">Australia/Brisbane</option>
      <option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Australia/Broken_Hill</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia/Currie</option>
      <option value="ACST-9:30">Australia/Darwin</option>
      <option value="<+0845>-8:45">Australia/Eucla</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia/Hobart</option>
      <option value="AEST-10">Australia/Lindeman</option>
      <option value="<+1030>-10:30<+11>-11,M10.1.0,M4.1.0">Australia/Lord_Howe</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia/Melbourne</option>
      <option value="AWST-8">Australia/Perth</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia/Sydney</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Amsterdam</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Andorra</option>
      <option value="<+04>-4">Europe/Astrakhan</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Athens</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Belgrade</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Berlin</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Bratislava</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Brussels</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Bucharest</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Budapest</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Busingen</option>
      <option value="EET-2EEST,M3.5.0,M10.5.0/3">Europe/Chisinau</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Copenhagen</option>
      <option value="IST-1GMT0,M10.5.0,M3.5.0/1">Europe/Dublin</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Gibraltar</option>
      <option value="GMT0BST,M3.5.0/1,M10.5.0">Europe/Guernsey</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Helsinki</option>
      <option value="GMT0BST,M3.5.0/1,M10.5.0">Europe/Isle_of_Man</option>
      <option value="<+03>-3">Europe/Istanbul</option>
      <option value="GMT0BST,M3.5.0/1,M10.5.0">Europe/Jersey</option>
      <option value="EET-2">Europe/Kaliningrad</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Kiev</option>
      <option value="MSK-3">Europe/Kirov</option>
      <option value="WET0WEST,M3.5.0/1,M10.5.0">Europe/Lisbon</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Ljubljana</option>
      <option value="GMT0BST,M3.5.0/1,M10.5.0">Europe/London</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Luxembourg</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Madrid</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Malta</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Mariehamn</option>
      <option value="<+03>-3">Europe/Minsk</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Monaco</option>
      <option value="MSK-3">Europe/Moscow</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Oslo</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Paris</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Podgorica</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Prague</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Riga</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Rome</option>
      <option value="<+04>-4">Europe/Samara</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/San_Marino</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Sarajevo</option>
      <option value="<+04>-4">Europe/Saratov</option>
      <option value="MSK-3">Europe/Simferopol</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Skopje</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Sofia</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Stockholm</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Tallinn</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Tirane</option>
      <option value="<+04>-4">Europe/Ulyanovsk</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Uzhgorod</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Vaduz</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Vatican</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Vienna</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Vilnius</option>
      <option value="MSK-3">Europe/Volgograd</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Warsaw</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Zagreb</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Zaporozhye</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Zurich</option>
      <option value="EAT-3">Indian/Antananarivo</option>
      <option value="<+06>-6">Indian/Chagos</option>
      <option value="<+07>-7">Indian/Christmas</option>
      <option value="<+0630>-6:30">Indian/Cocos</option>
      <option value="EAT-3">Indian/Comoro</option>
      <option value="<+05>-5">Indian/Kerguelen</option>
      <option value="<+04>-4">Indian/Mahe</option>
      <option value="<+05>-5">Indian/Maldives</option>
      <option value="<+04>-4">Indian/Mauritius</option>
      <option value="EAT-3">Indian/Mayotte</option>
      <option value="<+04>-4">Indian/Reunion</option>
      <option value="<+13>-13">Pacific/Apia</option>
      <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">Pacific/Auckland</option>
      <option value="<+11>-11">Pacific/Bougainville</option>
      <option value="<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45">Pacific/Chatham</option>
      <option value="<+10>-10">Pacific/Chuuk</option>
      <option value="<-06>6<-05>,M9.1.6/22,M4.1.6/22">Pacific/Easter</option>
      <option value="<+11>-11">Pacific/Efate</option>
      <option value="<+13>-13">Pacific/Enderbury</option>
      <option value="<+13>-13">Pacific/Fakaofo</option>
      <option value="<+12>-12">Pacific/Fiji</option>
      <option value="<+12>-12">Pacific/Funafuti</option>
      <option value="<-06>6">Pacific/Galapagos</option>
      <option value="<-09>9">Pacific/Gambier</option>
      <option value="<+11>-11">Pacific/Guadalcanal</option>
      <option value="ChST-10">Pacific/Guam</option>
      <option value="HST10">Pacific/Honolulu</option>
      <option value="<+14>-14">Pacific/Kiritimati</option>
      <option value="<+11>-11">Pacific/Kosrae</option>
      <option value="<+12>-12">Pacific/Kwajalein</option>
      <option value="<+12>-12">Pacific/Majuro</option>
      <option value="<-0930>9:30">Pacific/Marquesas</option>
      <option value="SST11">Pacific/Midway</option>
      <option value="<+12>-12">Pacific/Nauru</option>
      <option value="<-11>11">Pacific/Niue</option>
      <option value="<+11>-11<+12>,M10.1.0,M4.1.0/3">Pacific/Norfolk</option>
      <option value="<+11>-11">Pacific/Noumea</option>
      <option value="SST11">Pacific/Pago_Pago</option>
      <option value="<+09>-9">Pacific/Palau</option>
      <option value="<-08>8">Pacific/Pitcairn</option>
      <option value="<+11>-11">Pacific/Pohnpei</option>
      <option value="<+10>-10">Pacific/Port_Moresby</option>
      <option value="<-10>10">Pacific/Rarotonga</option>
      <option value="ChST-10">Pacific/Saipan</option>
      <option value="<-10>10">Pacific/Tahiti</option>
      <option value="<+12>-12">Pacific/Tarawa</option>
      <option value="<+13>-13">Pacific/Tongatapu</option>
      <option value="<+12>-12">Pacific/Wake</option>
      <option value="<+12>-12">Pacific/Wallis</option>
      <option value="GMT0">Etc/GMT</option>
      <option value="GMT0">Etc/GMT-0</option>
      <option value="<+01>-1">Etc/GMT-1</option>
      <option value="<+02>-2">Etc/GMT-2</option>
      <option value="<+03>-3">Etc/GMT-3</option>
      <option value="<+04>-4">Etc/GMT-4</option>
      <option value="<+05>-5">Etc/GMT-5</option>
      <option value="<+06>-6">Etc/GMT-6</option>
      <option value="<+07>-7">Etc/GMT-7</option>
      <option value="<+08>-8">Etc/GMT-8</option>
      <option value="<+09>-9">Etc/GMT-9</option>
      <option value="<+10>-10">Etc/GMT-10</option>
      <option value="<+11>-11">Etc/GMT-11</option>
      <option value="<+12>-12">Etc/GMT-12</option>
      <option value="<+13>-13">Etc/GMT-13</option>
      <option value="<+14>-14">Etc/GMT-14</option>
      <option value="GMT0">Etc/GMT0</option>
      <option value="GMT0">Etc/GMT+0</option>
      <option value="<-01>1">Etc/GMT+1</option>
      <option value="<-02>2">Etc/GMT+2</option>
      <option value="<-03>3">Etc/GMT+3</option>
      <option value="<-04>4">Etc/GMT+4</option>
      <option value="<-05>5">Etc/GMT+5</option>
      <option value="<-06>6">Etc/GMT+6</option>
      <option value="<-07>7">Etc/GMT+7</option>
      <option value="<-08>8">Etc/GMT+8</option>
      <option value="<-09>9">Etc/GMT+9</option>
      <option value="<-10>10">Etc/GMT+10</option>
      <option value="<-11>11">Etc/GMT+11</option>
      <option value="<-12>12">Etc/GMT+12</option>
      <option value="UTC0">Etc/UCT</option>
      <option value="UTC0">Etc/UTC</option>
      <option value="GMT0">Etc/Greenwich</option>
      <option value="UTC0">Etc/Universal</option>
      <option value="UTC0">Etc/Zulu</option>
    </select>
    <button class="add-button" style="margin-top:8px;" onclick="saveTimezone()">Save Timezone</button>
  </div>
</div>

<script>
  let meds = [];

  function loadMeds() {
    fetch('/meds').then(r=>r.json()).then(data => {
      meds = data;
      renderMeds();
    }).catch(err => console.error('Failed to load meds:', err));
  }

  function renderMeds() {
    const container = document.getElementById('medList');
    if (!meds || meds.length === 0) {
      container.innerHTML = '<div class="empty-state">✨ No medications yet.<br>Tap + to add.</div>';
      return;
    }
    let html = '';
    meds.forEach((med, idx) => {
      html += `<div class="med-item">
        <div class="med-info">
          <span class="med-name">${med.name}</span>
          <span class="med-time">🕒 ${String(med.hour).padStart(2,'0')}:${String(med.minute).padStart(2,'0')}</span>
        </div>
        <div class="delete-btn" onclick="deleteMed(${idx})">🗑️</div>
      </div>`;
    });
    container.innerHTML = html;
  }

  function showForm() {
    document.getElementById('addForm').style.display = 'block';
  }
  function hideForm() {
    document.getElementById('addForm').style.display = 'none';
    document.getElementById('medName').value = '';
    document.getElementById('hour').value = '';
    document.getElementById('minute').value = '';
  }

  function saveMed() {
    const name = document.getElementById('medName').value.trim();
    const hour = parseInt(document.getElementById('hour').value);
    const minute = parseInt(document.getElementById('minute').value);
    if (!name || isNaN(hour) || isNaN(minute) || hour<0 || hour>23 || minute<0 || minute>59) {
      alert('Please enter valid values');
      return;
    }
    fetch(`/add?name=${encodeURIComponent(name)}&hour=${hour}&minute=${minute}`)
      .then(response => {
        if (response.ok) {
          hideForm();
          loadMeds();
        } else {
          alert('Failed to add med');
        }
      });
  }

  function deleteMed(index) {
    if (confirm('Delete this med?')) {
      fetch(`/delete?index=${index}`).then(() => loadMeds());
    }
  }

  function saveTimezone() {
    const str = document.getElementById('timezoneSelect').value;
    const div = document.createElement('div');
    div.appendChild(document.createTextNode(str));
    const tz = div.innerHTML;
    fetch(`/config?tz=${tz}`).then(() => alert('Timezone saved'));
  }

  function saveBrightness() {
    const active = parseInt(document.getElementById('activeBrightness').value);
    fetch(`/config?active=${active}`).then(() => alert('Active Brightness saved'));
    const idle = parseInt(document.getElementById('idleBrightness').value);
    fetch(`/config?idle=${idle}`).then(() => alert('Idle Brightness saved'));
  }

  fetch('/getconfig').then(r=>r.json()).then(data => {
    document.getElementById('timezoneSelect').value = data.tz;
    document.getElementById('activeBrightness').value = data.active;
    document.getElementById('activeOutput').value = data.active;
    document.getElementById('idleBrightness').value = data.idle;
    document.getElementById('idleOutput').value = data.idle;
  });

  loadMeds();
  setInterval(loadMeds, 5000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleGetMeds() {
  String json = "[";
  for (int i = 0; i < medCount; i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + String(meds[i].name) + "\",";
    json += "\"hour\":" + String(meds[i].hour) + ",";
    json += "\"minute\":" + String(meds[i].minute) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleAddMed() {
  if (server.hasArg("name") && server.hasArg("hour") && server.hasArg("minute") && medCount < MAX_MEDS) {
    String name = server.arg("name");
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    name.toCharArray(meds[medCount].name, 24);
    meds[medCount].hour = hour;
    meds[medCount].minute = minute;
    meds[medCount].active = true;
    meds[medCount].takenToday = false;
    medCount++;
    saveMeds();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleDeleteMed() {
  if (server.hasArg("index")) {
    int idx = server.arg("index").toInt();
    if (idx >= 0 && idx < medCount) {
      for (int i = idx; i < medCount - 1; i++) meds[i] = meds[i + 1];
      medCount--;
      saveMeds();
      server.send(200, "text/plain", "OK");
    } else server.send(400, "text/plain", "Bad Request");
  } else server.send(400, "text/plain", "Bad Request");
}

void handleGetConfig() {
  String json = "{\"tz\":\"" + timezone + "\", \"active\":" + String(activeBrightness) + ", \"idle\":" + String(idleBrightness) + "}";
  server.send(200, "application/json", json);
}

void handleSetConfig() {
  if (server.hasArg("tz")) {
    timezone = server.urlDecode(server.arg("tz"));
    saveTimezone();
    syncTime();
  }
  if (server.hasArg("active")) {
    activeBrightness = server.arg("active").toInt();
    if (activeBrightness < 0) activeBrightness = 0;
    if (activeBrightness > 255) activeBrightness = 255;
    saveActiveBrightness();
  }
  if (server.hasArg("idle")) {
    idleBrightness = server.arg("idle").toInt();
    if (idleBrightness < 0) idleBrightness = 0;
    if (idleBrightness > 255) idleBrightness = 255;
    saveIdleBrightness();
  }
  server.send(200, "text/plain", "OK");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Med Reminder ===");

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  loadActiveBrightness();
  loadIdleBrightness();
  ledcAttach(BACKLIGHT_PIN, PWM_FREQ, PWM_RES);
  setBrightness(activeBrightness);
  
  // Show boot bitmap
  drawBootBitmap();

  round_display_touch_init();

  loadTimezone();
  loadMeds();

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    localIP = WiFi.localIP().toString();
    Serial.println("\nIP: " + localIP);
    
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("IP Address:", 30, 80);
    tft.setFreeFont(&FreeSans12pt7b);
    tft.drawString(localIP, 30, 120);
    ipShowStart = millis();
    showingIP = true;
    
    syncTime();
  } else {
    Serial.println("WiFi failed");
    tft.fillScreen(TFT_RED);
    tft.drawString("WiFi Failed", 30, 100);
    delay(2000);
  }

  server.on("/", handleRoot);
  server.on("/meds", handleGetMeds);
  server.on("/add", handleAddMed);
  server.on("/delete", handleDeleteMed);
  server.on("/getconfig", handleGetConfig);
  server.on("/config", handleSetConfig);
  server.begin();
  Serial.println("Web server ready");
}

// ========== LOOP ==========
void loop() {
  server.handleClient();

  if (showingIP && millis() - ipShowStart > 5000) {
    showingIP = false;
    drawClockScreen();
  }

  static unsigned long lastTimeUpdate = 0;
  if (!showingIP && !reminderActive && millis() - lastTimeUpdate > 1000) {
    updateClockTime();
    lastTimeUpdate = millis();
  }

  if (!showingIP && !reminderActive) {
    checkReminders();
  }

  if (chsc6x_is_pressed()) {
    int32_t x, y;
    if (chsc6x_get_xy(x, y)) {
      if (reminderActive) {
        if (reminderIndex >= 0) {
          meds[reminderIndex].takenToday = true;
          meds[reminderIndex].active = false;
          saveMeds();
        }
        drawMedTakenScreen();
        reminderActive = false;
        reminderIndex = -1;
        drawClockScreen();
      } else {
        drawClockScreen();
        setBrightness(activeBrightness);
        delay(3000);
        setBrightness(idleBrightness);
      }
      delay(200);
    }
  }

  static int lastDay = -1;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_yday != lastDay) {
      lastDay = timeinfo.tm_yday;
      for (int i = 0; i < medCount; i++) {
        meds[i].takenToday = false;
        meds[i].active = true;
      }
      saveMeds();
      Serial.println("Daily reset");
      drawClockScreen();
    }
  }

  delay(50);
}

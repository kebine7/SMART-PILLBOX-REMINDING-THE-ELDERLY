// secrets.h
#pragma once

// Wi-Fi
#define WIFI_SSID     "SSID"      // Enter Wifi SSID here
#define WIFI_PASSWORD "PASSWORD"  // Enter Wifi Password here

// Telegram (optional)
#define BOT_TOKEN  "TOKEN" 
#define CHAT_ID    "ID"

/* ----------- Default drinking schedule (can be edited) ----------- */

struct DoseTime { uint8_t h, m; };
const DoseTime DOSES[3] = {
  {8, 00},   // 8:00
  {12, 00},  // 12:00
  {16, 00}   // 16:00
};

// Grace period after dose time comes, after this a Telegram SMS will be sent
const uint32_t LATE_GRACE_MIN = 0;



// secrets.h
#pragma once

// Wi-Fi
#define WIFI_SSID     "SSOD"
#define WIFI_PASSWORD "PASSWORD"

// Telegram (optional)
#define BOT_TOKEN  "TOKEN" 
#define CHAT_ID    "ID"

/* ----------- Default drinking schedule (can be edited) ----------- */

struct DoseTime { uint8_t h, m; };
const DoseTime DOSES[3] = {
  {20, 25},   // 16:36
  {20, 27},  // 16:38
  {20, 29}   // 16:40
};

// Grace period after dose time comes, after this a Telegram SMS will be sent
const uint32_t LATE_GRACE_MIN = 0;



#ifndef TELEGRAM_NOTIFIER_H
#define TELEGRAM_NOTIFIER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ===== 改成你自己的 =====
#define BOT_TOKEN "9020310954:AAHJ0Tekh6o7uIZkAt7y3EytKXjjyt5-Pb8"
#define CHAT_ID   "7566840188"
// ========================

inline WiFiClientSecure& tgClient() {
  static WiFiClientSecure client;
  return client;
}

inline UniversalTelegramBot& tgBot() {
  static UniversalTelegramBot bot(BOT_TOKEN, tgClient());
  return bot;
}

inline bool sendTelegramMessage(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("TG: WiFi not connected, message skipped");
    return false;
  }
  if (tgBot().sendMessage(CHAT_ID, msg, "")) {
    Serial.println("TG: message sent");
    return true;
  } else {
    Serial.println("TG: send FAILED");
    return false;
  }
}

#endif

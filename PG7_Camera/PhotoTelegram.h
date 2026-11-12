#pragma once
#include <WiFiClientSecure.h>
#include "PhotoTelegramConfig.h"

const char* photoCaption(uint8_t event) {
  if (event == PHOTO_FIRST_ARRIVAL || event == PHOTO_PACKAGE_ADDED)
    return "📦 新增包裹";
  if (event == PHOTO_ABNORMAL_LOSS)
    return "🚨 包裹狀態異常（未授權取出）";
  return nullptr; // Authorized pickup and manual photos have no notification.
}

bool uploadEventPhoto(const char* path, uint8_t event) {
  const char* caption = photoCaption(event);
  if (!caption) return true;
  if (WiFi.status() != WL_CONNECTED) {
    Serial0.println("PHOTO_TG_NO_WIFI");
    return false;
  }
  File photo = SD_MMC.open(path, FILE_READ);
  if (!photo || photo.size() == 0 || photo.size() > 10U * 1024U * 1024U) {
    if (photo) photo.close();
    Serial0.println("PHOTO_TG_FILE_ERROR");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Same TLS setting as the existing text notifier.
  client.setTimeout(15000);
  if (!client.connect("api.telegram.org", 443)) {
    photo.close();
    Serial0.println("PHOTO_TG_CONNECT_FAILED");
    return false;
  }

  const String boundary = "----PG7PhotoBoundary";
  const String header = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" PHOTO_CHAT_ID "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + String(caption) + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"parcel.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  const String trailer = "\r\n--" + boundary + "--\r\n";
  const size_t total = header.length() + photo.size() + trailer.length();
  client.print(String("POST /bot") + PHOTO_BOT_TOKEN + "/sendPhoto HTTP/1.1\r\n"
               "Host: api.telegram.org\r\n"
               "Connection: close\r\n"
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
               "Content-Length: " + String(total) + "\r\n\r\n");
  client.print(header);

  uint8_t buffer[1024];
  bool written = true;
  while (photo.available() && written) {
    const size_t count = photo.read(buffer, sizeof(buffer));
    if (!count) { written = false; break; }
    size_t offset = 0;
    while (offset < count) {
      const size_t sent = client.write(buffer + offset, count - offset);
      if (!sent) { written = false; break; }
      offset += sent;
    }
    yield();
  }
  photo.close();
  if (!written) {
    client.stop();
    Serial0.println("PHOTO_TG_WRITE_FAILED");
    return false;
  }
  client.print(trailer);
  const String status = client.readStringUntil('\n');
  bool success = status.indexOf(" 200 ") >= 0;
  String line;
  do { line = client.readStringUntil('\n'); } while (line.length() > 1 && client.connected());
  const String response = client.readString();
  success = success && response.indexOf("\"ok\":true") >= 0;
  client.stop();
  Serial0.printf("PHOTO_TG_%s event=%u file=%s\n", success ? "SENT" : "FAILED", event, path);
  return success;
}

/*
  ParcelGuard Coding 5 — ESP32-S3 CAM + MicroSD (standalone test)

  IMPORTANT:
  1) Upload this sketch to the ESP32-S3 CAMERA board, NOT the ESP32 DevKit V1.
  2) The sample pins below are ONLY for the ESP32-S3-CAM board layout
     described in README_CN.txt (camera GPIO4/5/6/7/8... and SD 38/39/40).
     ESP32-S3 CAM boards do NOT all share the same pinout!
  3) No connection to Coding 4 is needed yet. Coding 7 will link the boards.
  4) Open Serial Monitor at 115200 baud; use Newline and send:
       p  = take 1 JPEG photo
       b  = take 3 JPEG photos (burst)
       i  = show camera/SD status
       h  = show help

  Arduino IDE: board ESP32S3 Dev Module, PSRAM enabled per board hardware.
  Camera and SD_MMC support are supplied by the Espressif ESP32 board core.
*/

#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <stdio.h>
#include "esp_system.h"
#include "esp_timer.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error Select ESP32S3 Dev Module for this sketch
#endif
// COM8 is a CH343 UART bridge. Set to 0 ONLY for a native USB socket.
#define USE_UART_BRIDGE 1
#if USE_UART_BRIDGE
#define Console Serial0
#else
#define Console Serial
#endif

// --------- BOARD PIN PROFILE: check against YOUR vendor's pinout ---------
// Profile 1: example ESP32-S3-CAM (N16R8-type board with 1-bit SDMMC).
// If your camera/SD pins differ, EDIT ALL values in this section using the
// manufacturer's camera and SD card pin assignments BEFORE using the sketch.
// Camera signals D0..D7 are sensor pixel data; do not mix up their order.
constexpr int CAM_PWDN  = -1;
constexpr int CAM_RESET = -1;
constexpr int CAM_XCLK  = 15;
constexpr int CAM_SIOD  = 4;   // Camera SCCB data (SDA)
constexpr int CAM_SIOC  = 5;   // Camera SCCB clock (SCL)
constexpr int CAM_D0    = 11;  // Y2
constexpr int CAM_D1    = 9;   // Y3
constexpr int CAM_D2    = 8;   // Y4
constexpr int CAM_D3    = 10;  // Y5
constexpr int CAM_D4    = 12;  // Y6
constexpr int CAM_D5    = 18;  // Y7
constexpr int CAM_D6    = 17;  // Y8
constexpr int CAM_D7    = 16;  // Y9
constexpr int CAM_VSYNC = 6;
constexpr int CAM_HREF  = 7;
constexpr int CAM_PCLK  = 13;

// Built-in TF/MicroSD slot wired for SDMMC 1-bit (NOT external SPI SD reader).
constexpr int SD_CLK = 39;
constexpr int SD_CMD = 38;
constexpr int SD_D0  = 40;
// -------------------------------------------------------------------------

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr unsigned long BURST_INTERVAL_MS = 400;
constexpr uint8_t BURST_PHOTOS = 3;

bool cameraReady = false;
bool sdReady = false;
uint32_t nextImageNumber = 1;
uint8_t photosRemaining = 0;
uint8_t photosSucceeded = 0;
uint8_t photosRequested = 0;
uint32_t lastHeartbeat = 0;
unsigned long nextPhotoAt = 0;

void printHelp() {
  Console.println();
  Console.println("=== PARCEL GUARD / CODING 7 ===");
  Console.println("p : save 1 JPEG photo to /photos/");
  Console.println("b : save 3 JPEG photos (400 ms apart)");
  Console.println("t : save 10 photos for stability test");
  Console.println("i : camera + microSD status; r : remount SD / retry failed camera");
  Console.println("h : show this help");
  Console.println("Use Newline, baud rate 115200.");
}

void printStatus() {
  Console.printf("STATUS CAMERA=%s SD=%s PSRAM=%s FREE_PSRAM=%u bytes\n",
      cameraReady ? "OK" : "FAIL",
      sdReady ? "OK" : "FAIL",
      psramFound() ? "YES" : "NO",
      (unsigned int)ESP.getFreePsram());
  if (sdReady) {
    Console.printf("SD_SIZE=%llu MB\n",
        (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
    Console.printf("SD_USED=%llu MB\n",
        (unsigned long long)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)));
  }
}

bool initCamera() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = CAM_D0;
  cfg.pin_d1 = CAM_D1;
  cfg.pin_d2 = CAM_D2;
  cfg.pin_d3 = CAM_D3;
  cfg.pin_d4 = CAM_D4;
  cfg.pin_d5 = CAM_D5;
  cfg.pin_d6 = CAM_D6;
  cfg.pin_d7 = CAM_D7;
  cfg.pin_xclk = CAM_XCLK;
  cfg.pin_pclk = CAM_PCLK;
  cfg.pin_vsync = CAM_VSYNC;
  cfg.pin_href = CAM_HREF;
  cfg.pin_sccb_sda = CAM_SIOD;
  cfg.pin_sccb_scl = CAM_SIOC;
  cfg.pin_pwdn = CAM_PWDN;
  cfg.pin_reset = CAM_RESET;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_QVGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count = psramFound() ? 2 : 1;
  cfg.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  cfg.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

  Console.println("STEP CAMERA_INIT_BEGIN");
  Console.flush();
  const esp_err_t error = esp_camera_init(&cfg);
  if (error != ESP_OK) {
    Console.printf("ERROR: CAMERA_INIT_FAIL 0x%X\n", (unsigned int)error);
    Console.println("Check the EXACT board pinout, camera ribbon, PSRAM and power.");
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    Console.printf("CAMERA_SENSOR_PID=0x%04X\n", (unsigned int)sensor->id.PID);
  }
  Console.println("CAMERA_OK");

  // Discard a first frame after power-on to let exposure settle.
  delay(250);
  camera_fb_t *warmup = esp_camera_fb_get();
  if (warmup) {
    esp_camera_fb_return(warmup);
    Console.println("CAMERA_WARMUP_OK");
  } else {
    Console.println("WARN: camera warm-up frame failed; try 'p' anyway.");
  }
  return true;
}

bool initSD() {
  Console.println("STEP SD_INIT_BEGIN");
  Console.flush();
  // ESP32-S3 SD_MMC requires the actual onboard card-slot pins.
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
    Console.println("ERROR: SD_PIN_CONFIG_FAIL");
    return false;
  }
  // true = 1-bit SDMMC; safer than accidentally using 4-bit mode.
  if (!SD_MMC.begin("/sdcard", true)) {
    Console.println("ERROR: SD_MOUNT_FAIL");
    Console.println("Check FAT32 card, correct built-in SD slot pins and wiring.");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Console.println("ERROR: SD_NO_CARD");
    SD_MMC.end();
    return false;
  }
  if (!SD_MMC.exists("/photos") && !SD_MMC.mkdir("/photos")) {
    Console.println("ERROR: CANNOT_CREATE_PHOTOS_FOLDER");
    SD_MMC.end();
    return false;
  }
  Console.printf("SD_OK SIZE=%llu MB\n",
      (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool getUnusedPhotoName(char *out, size_t outSize) {
  // Look for a free name, including after restart, to prevent overwriting.
  for (uint32_t tries = 0; tries < 99999; ++tries) {
    if (nextImageNumber > 99999) nextImageNumber = 1;
    snprintf(out, outSize, "/photos/IMG_%05lu.JPG", (unsigned long)nextImageNumber++);
    if (!SD_MMC.exists(out)) return true;
  }
  return false;
}

bool saveOnePhoto(char* savedPath = nullptr, size_t savedPathSize = 0) {
  if (!cameraReady || !sdReady) {
    Console.println("ERROR: NOT_READY (camera or SD failed during setup)");
    return false;
  }

  // A queued frame may predate this event (especially with one frame buffer).
  // Save only a frame whose capture began AFTER this request, never startup imagery.
  const int64_t requestedAt = esp_timer_get_time();
  camera_fb_t *fb = nullptr;
  for (unsigned int discarded=0; discarded<8; ++discarded) {
    fb = esp_camera_fb_get();
    if (!fb) break;
    const int64_t frameAt = (int64_t)fb->timestamp.tv_sec * 1000000LL + fb->timestamp.tv_usec;
    if (frameAt >= requestedAt) break;
    esp_camera_fb_return(fb);
    fb = nullptr;
    if (esp_timer_get_time()-requestedAt >= 2500000LL) break;
    delay(10);
  }
  if (!fb) {
    cameraReady=false;
    Console.println("ERROR: CAPTURE_FAIL (no camera frame)");
    return false;
  }

  if (fb->format != PIXFORMAT_JPEG || fb->len < 4 ||
      fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
    Console.println("ERROR: NOT_A_JPEG_FRAME");
    esp_camera_fb_return(fb);
    return false;
  }

  char filename[48];
  if (!getUnusedPhotoName(filename, sizeof(filename))) {
    Console.println("ERROR: TOO_MANY_IMAGES");
    esp_camera_fb_return(fb);
    return false;
  }

  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    sdReady=false;
    Console.println("ERROR: FILE_OPEN_FAIL");
    esp_camera_fb_return(fb);
    return false;
  }

  const size_t expected = fb->len;
  const size_t written = file.write(fb->buf, expected);
  file.flush();
  file.close();

  if (written != expected) {
    sdReady=false;
    esp_camera_fb_return(fb);
    SD_MMC.remove(filename);  // Do not leave a misleading partial JPEG.
    Console.printf("ERROR: FILE_WRITE_FAIL written=%u expected=%u\n",
                  (unsigned int)written, (unsigned int)expected);
    return false;
  }

  // Read back the entire payload and compare before reporting success.
  File check = SD_MMC.open(filename, FILE_READ);
  bool valid = check && check.size() == expected;
  uint8_t block[256];
  size_t offset = 0;
  while (valid && offset < expected) {
    size_t wanted = (expected - offset < sizeof(block)) ? expected - offset : sizeof(block);
    size_t got = check.read(block, wanted);
    if (got != wanted || memcmp(block, fb->buf + offset, wanted) != 0) valid = false;
    offset += got;
  }
  check.close();
  esp_camera_fb_return(fb);
  if (!valid) {
    SD_MMC.remove(filename);
    sdReady=false;
    Console.println("ERROR: FILE_READBACK_FAIL");
    return false;
  }
  Console.printf("PHOTO_SAVED %s bytes=%u READBACK_OK\n", filename, (unsigned int)written);
  if (savedPath && savedPathSize) snprintf(savedPath, savedPathSize, "%s", filename);
  return true;
}

void startBurst(uint8_t count) {
  if (!cameraReady || !sdReady) {
    Console.println("ERROR: NOT_READY; cannot start photo capture.");
    return;
  }
  if (photosRemaining != 0) {
    Console.println("BUSY: previous capture is still running.");
    return;
  }
  photosRequested = count;
  photosRemaining = count;
  photosSucceeded = 0;
  nextPhotoAt = millis();  // First photo is taken as soon as loop runs.
  Console.printf("CAPTURE_START count=%u\n", (unsigned int)count);
}

void handleSerialCommand(char c) {
  switch (c) {
    case 'r': case 'R':
      if(photosRemaining){Console.println("RECOVERY_BUSY");break;}
      SD_MMC.end();sdReady=initSD();
      if(!cameraReady){esp_camera_deinit();cameraReady=initCamera();}
      printStatus();break;
    case 'p': case 'P': startBurst(1); break;
    case 'b': case 'B': startBurst(BURST_PHOTOS); break;
    case 't': case 'T': startBurst(10); break;
    case 'i': case 'I': printStatus(); break;
    case 'h': case 'H': printHelp(); break;
    case '\n': case '\r': case ' ': case '\t': break;
    default: Console.println("Unknown command; send 'h' for help."); break;
  }
}

#include "Wireless.h"

void setup() {
  Console.begin(SERIAL_BAUD);
#if !USE_UART_BRIDGE
  unsigned long connectStart = millis();
  while (!Console && millis() - connectStart < 5000) delay(10);
#endif
  delay(1200); // Give USB serial monitor time to connect after boot.
  Console.println();
  Console.println("PARCEL GUARD CODING 7 WIRELESS CAMERA BOOT");
  Console.println("Board assumed: ESP32-S3-CAM sample pin map; verify your board!");
  Console.printf("SD pins CLK=%d CMD=%d D0=%d\n", SD_CLK, SD_CMD, SD_D0);

  Console.printf("CHIP=%s REV=%u RESET_REASON=%d FLASH=%u PSRAM=%u\n",
      ESP.getChipModel(), ESP.getChipRevision(), (int)esp_reset_reason(),
      (unsigned int)ESP.getFlashChipSize(), (unsigned int)ESP.getPsramSize());
  cameraReady = initCamera();
  sdReady = initSD();
  initRadio();
  WiFi.setAutoReconnect(true);
  WiFi.begin(PHOTO_WIFI_SSID, PHOTO_WIFI_PASSWORD);
  wifiConnectingUntil = millis() + 15000;
  printStatus();
  printHelp();
  if (cameraReady && sdReady) Console.println("READY: type p or b to take photos.");
  else Console.println("NOT_READY: resolve init errors before photo testing.");
}

void loop() {
  updateWireless();
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    Console.printf("ALIVE uptime=%lu CAMERA=%s SD=%s\n", millis()/1000,
        cameraReady ? "OK" : "FAIL", sdReady ? "OK" : "FAIL");
  }
  while (Console.available() > 0) {
    handleSerialCommand((char)Console.read());
  }

  if (photosRemaining > 0 && (int32_t)(millis() - nextPhotoAt) >= 0) {
    if (saveOnePhoto()) ++photosSucceeded;
    --photosRemaining;
    nextPhotoAt = millis() + BURST_INTERVAL_MS;
    if (photosRemaining == 0) {
      Console.printf("CAPTURE_DONE saved=%u requested=%u result=%s\n", (unsigned int)photosSucceeded, (unsigned int)photosRequested, photosSucceeded == photosRequested ? "PASS" : "FAIL");
    }
  }
  delay(5);
}

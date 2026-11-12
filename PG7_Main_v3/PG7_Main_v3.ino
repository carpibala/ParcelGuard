#include <Arduino.h>
#include "TelegramNotifier.h"
#include "Wireless.h"
#include <HX711.h>
#include <Preferences.h>
#include <math.h>
#include <SPI.h>
#include <MFRC522.h>

// Coding 4: based on the tested Coding 3 (HX711 + RC522).
// Common-cathode RGB status LED and active buzzer.
// ESP32 DevKit V1: HX711 DT -> GPIO32, SCK -> GPIO33.
// Serial Monitor: 115200 baud, Newline.
// Boot with an EMPTY platform: this teaching prototype automatically tares at startup.

constexpr int PIN_DT = 32;
constexpr int PIN_SCK = 33;

// RC522 SPI wiring: 3.3V (NOT 5V), GND, SDA/SS=GPIO5,
// SCK=GPIO18, MOSI=GPIO23, MISO=GPIO19, RST=GPIO16, IRQ unused.
constexpr int RFID_SS_PIN = 5;
constexpr int RFID_RST_PIN = 16;

// Common-cathode RGB module: R -> D14, G -> D26, B -> D27, - -> GND.
// Each color channel needs current limiting (on the module or in series).
constexpr int RED_LED_PIN = 14;
constexpr int GREEN_LED_PIN = 26;
constexpr int BLUE_LED_PIN = 27;
constexpr int BUZZER_PIN = 25;

// Set to LOW if your 3.3V-compatible ACTIVE buzzer module triggers on LOW.
// Connect the buzzer module's S -> GPIO25, + -> 3V3, - -> GND ONLY
// if its actual datasheet confirms 3.3V operation and acceptable input current.
// A bare/high-power/5V-only buzzer needs a transistor or MOSFET driver.
constexpr int BUZZER_ACTIVE_LEVEL = HIGH;
constexpr unsigned long BLINK_INTERVAL_MS = 300;

void initOutputs() {
  // Prepare disabled logic levels before assigning OUTPUT to reduce glitches.
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, !BUZZER_ACTIVE_LEVEL);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
}


// To register an owner card: empty platform -> send "u" -> scan the card.
// The UID is saved in ESP32 Preferences and survives a restart.
String ownerUID;
bool enrollingOwner = false;

// false = only the real valid RFID card authorizes pickup.
// true = retains Coding 2's 'a' serial simulation for troubleshooting.
constexpr bool ENABLE_SERIAL_AUTH_TEST = false;
constexpr float MIN_PACKAGE_G = 20.0f;
constexpr float EMPTY_G = 50.0f;
constexpr float STABLE_TOL_G = 35.0f;
constexpr float REMOVE_THRESHOLD_G = 20.0f;
constexpr float ADD_THRESHOLD_G = 100.0f;
constexpr unsigned long STABLE_TIME_MS = 5000; // Demo 5s; change to 60000 for 1min.
constexpr unsigned long LOSS_CONFIRM_MS = 900;
constexpr unsigned long AUTH_TIME_MS = 30000;
constexpr unsigned long SAMPLE_MS = 150;
constexpr unsigned long PRINT_MS = 1000;

HX711 scale;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Preferences preferences;
float calibrationFactor = 1.0f;
bool calibrated = false;
float currentWeight = 0;
float baselineWeight = 0;
float candidateAnchor = 0;
unsigned long stableSince = 0;
unsigned long lossSince = 0;
unsigned long authSince = 0;
unsigned long lastSample = 0;
unsigned long lastPrint = 0;
unsigned long lastSensorReady = 0;
bool firstArrivalPending = false;
bool pickupPhotographed = false;
bool pickupCandidate = false;
float pickupReference = 0;
float pickupAnchor = 0;
unsigned long pickupSince = 0;

void eventPhoto(const char* kind) {
  Serial.printf("PARCEL_EVENT type=%s uptime_ms=%lu weight=%.1f baseline=%.1f\n",
                kind, millis(), currentWeight, baselineWeight);
  if (!requestWirelessPhoto(kind)) {
    const char* message = photoNotificationText(photoEventCode(kind));
    if (message) sendTelegramMessage(message);
  }
  // Dispatch before the next weight sample.
  updateWireless();
}

enum State { NEED_CALIBRATION, REMOVE_CAL_WEIGHT, WAITING, STABILIZING, ARMED, ADD_PENDING, AUTHORIZED, ALARM, FAULT };
State state = NEED_CALIBRATION;

const char* stateName(State s);
void changeState(State next);
void startStability(State next, float weight, unsigned long now);

const char* stateName(State s) {
  switch(s) {
    case NEED_CALIBRATION: return "NEED_CALIBRATION";
    case REMOVE_CAL_WEIGHT: return "REMOVE_CAL_WEIGHT";
    case WAITING: return "WAITING";
    case STABILIZING: return "STABILIZING";
    case ARMED: return "ARMED";
    case ADD_PENDING: return "ADD_PENDING";
    case AUTHORIZED: return "AUTHORIZED";
    case ALARM: return "ALARM";
    case FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

// Non-blocking: the security state machine and RFID polling keep running
// while the warning indicators blink. Three LEDs are active HIGH.
void updateOutputs(unsigned long now) {
  const bool blink = ((now / BLINK_INTERVAL_MS) % 2u) == 0;
  bool red = false, green = false, blue = false, buzzer = false;
  switch (state) {
    case NEED_CALIBRATION:
    case REMOVE_CAL_WEIGHT:
      blue = blink; break;
    case WAITING:
      blue = true; break;
    case STABILIZING:
      blue = blink; break;
    case ARMED:
      green = true; break;
    case ADD_PENDING:
      green = blink; break;
    case AUTHORIZED:
      blue = true; break;
    case ALARM:
      red = blink; buzzer = blink; break;
    case FAULT:
      red = true; break;
  }
  digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
  digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);
  digitalWrite(BLUE_LED_PIN, blue ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, buzzer ? BUZZER_ACTIVE_LEVEL : !BUZZER_ACTIVE_LEVEL);
}

void changeState(State next) {
  if (state != next) {
    state = next;
    Serial.print("STATE -> ");
    Serial.println(stateName(state));
  }
}

void startStability(State next, float weight, unsigned long now) {
  candidateAnchor = weight;
  stableSince = now;
  changeState(next);
}

void alarmNow() {
  if(state != ALARM) {
    eventPhoto("ABNORMAL_LOSS");
  }
  changeState(ALARM);
  Serial.println("!!! THEFT ALERT: sustained decrease from ARMED baseline !!!");
  // Coding 4: updateOutputs() drives the red LED and buzzer in ALARM.
}

String formatCardUID() {
  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void authorizePickup(unsigned long now, const char* source) {
  if (!calibrated || state == FAULT || state == NEED_CALIBRATION ||
      state == REMOVE_CAL_WEIGHT) {
    Serial.println("REFUSED: calibration/sensor not ready.");
    return;
  }
  // A repeated card scan extends the same pickup session, without a second photo.
  if (state != AUTHORIZED) {
    pickupReference = currentWeight;
    pickupPhotographed = false;
    pickupCandidate = false;
  }
  firstArrivalPending = false;
  authSince = now;
  lossSince = 0;
  changeState(AUTHORIZED);
  // Stop the alarm immediately. Telegram delivery may block for several seconds.
  updateOutputs(now);
  Serial.print("OWNER AUTHORIZED via ");
  Serial.print(source);
  Serial.println(": pickup allowed for 30 seconds.");
  sendTelegramMessage("🔓 已授權");
}

// Poll RFID once per loop; no need to hold a card on the reader.
void handleRFID(unsigned long now) {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;
  String uid = formatCardUID();
  Serial.print("RFID UID = ");
  Serial.println(uid);

  // A registered owner card always clears a live alarm, even if an earlier
  // enrollment request was left pending.
  if (state == ALARM && ownerUID.length() && uid == ownerUID) {
    enrollingOwner = false;
    authorizePickup(now, "RFID alarm clear");
  } else if (enrollingOwner) {
    if (state == WAITING && baselineWeight == 0 && fabsf(currentWeight) <= EMPTY_G) {
      ownerUID = uid;
      preferences.putString("owner", ownerUID);
      enrollingOwner = false;
      Serial.print("OWNER CARD SAVED: ");
      Serial.println(ownerUID);
    } else {
      enrollingOwner = false;
      Serial.println("ENROLLMENT CANCELLED: platform must be empty and WAITING.");
    }
  } else if (ownerUID.length() == 0) {
    Serial.println("NO OWNER CARD: empty the platform, send u, and scan this card again.");
  } else if (uid == ownerUID) {
    authorizePickup(now, "RFID");
  } else {
    Serial.println("INVALID CARD: protection unchanged.");
    // 防刷屏機制：距離上次警告超過 10 秒才發送
    static unsigned long lastInvalidMsgTime = 0;
    if (millis() - lastInvalidMsgTime > 10000){
      sendTelegramMessage("🚫 未授權卡片，無效");
      lastInvalidMsgTime = millis();
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void showHelp() {
 Serial.println("Coding7: i=status; r=recover sensor FAULT without changing tare/baseline");
 Serial.println("Coding7 wireless: p=manual photo request; does not authorize pickup");
  Serial.println("Commands: c 1000 = calibrate using a 1000g mass (after empty-start tare)");
  Serial.println("          s = show saved calibration factor");
  Serial.println("          u = register owner card (EMPTY WAITING platform), then scan");
  Serial.println("          owner RFID card = authorize pickup / clear alarm (30s)");
  Serial.println("          a = serial authorization ONLY if ENABLE_SERIAL_AUTH_TEST=true");
  Serial.println("          h = help");
  Serial.println("          t = tare ONLY when empty, WAITING and no package protected");
  Serial.println("Coding 4 outputs: blue=wait, blue blink=stabilize/authorized,");
  Serial.println("                 green=armed, green blink=addition, red+buzzer=alarm.");
}

void handleSerial(unsigned long now) {
  static char input[64]; static size_t used=0; static bool discard=false;
  static unsigned long inputAt=0;
  if(used && millis()-inputAt>2000){used=0;discard=false;Serial.println("COMMAND_TIMEOUT");}
  bool complete=false;
  for(int n=0;n<64 && Serial.available();++n){
    char c=Serial.read();inputAt=millis();
    if(c=='\r')continue;
    if(c=='\n'){if(discard){discard=false;used=0;Serial.println("COMMAND_TOO_LONG");return;}complete=true;break;}
    if(used<sizeof(input)-1)input[used++]=c;else discard=true;
  }
  if(!complete)return;
  input[used]=0;String cmd(input);used=0;
  cmd.trim();
  if (cmd.length() == 0) return;

  if(cmd=="i"){Serial.printf("SYSTEM state=%s weight=%.1f baseline=%.1f sensorReady=%u\n",stateName(state),currentWeight,baselineWeight,scale.is_ready());showWireless();return;}
  if(cmd=="r"){
    if(state!=FAULT){Serial.println("RECOVERY_NOT_NEEDED");return;}
    if(!calibrated||!scale.is_ready()){Serial.println("RECOVERY_REFUSED: fix sensor; empty-platform restart required if startup failed");return;}
    float w=scale.get_units(1);
    if(!isfinite(w)){Serial.println("RECOVERY_BAD_READING");return;}
    if(baselineWeight>0){changeState(ALARM);Serial.println("RECOVERED_PROTECTED: alarm retained, owner card required");}
    else if(fabsf(w)<=EMPTY_G){changeState(WAITING);Serial.println("RECOVERED_EMPTY: no tare performed");}
    else {Serial.println("RECOVERY_REFUSED: clear platform first");return;}
    currentWeight=w;lastSensorReady=millis();lossSince=0;return;
  }
  if (cmd == "p") { requestWirelessPhoto(); return; }
  if (cmd == "h") { showHelp(); return; }
  if (cmd == "s") {
    Serial.print("CALIBRATION_FACTOR = ");
    Serial.println(calibrationFactor, 6);
    return;
  }
  if (cmd == "u") {
    if (state != WAITING || baselineWeight != 0 || fabsf(currentWeight) > EMPTY_G) {
      Serial.println("REFUSED: remove packages; enroll only on an EMPTY WAITING platform.");
    } else {
      enrollingOwner = true;
      Serial.println("ENROLLMENT READY: scan the card to save as owner.");
    }
    return;
  }
  if (cmd.startsWith("c ")) {
    // Recalibration only when no live protected baseline exists.
    if (!(state == NEED_CALIBRATION || state == REMOVE_CAL_WEIGHT || state == WAITING)) {
      Serial.println("REFUSED: do not calibrate while parcels are protected.");
      return;
    }
    float knownG = cmd.substring(2).toFloat();
    if (knownG <= 0) { Serial.println("ERROR: use c 1000"); return; }
    if (!scale.wait_ready_timeout(800)) { changeState(FAULT); return; }

    scale.set_scale(1.0f);
    float rawCounts = scale.get_value(15); // Coding 1: counts relative to startup tare
    float newFactor = rawCounts / knownG;
    if (!isfinite(newFactor) || fabsf(newFactor) < 0.0001f) {
      scale.set_scale(calibrationFactor);
      Serial.println("ERROR: invalid calibration; check mass and wiring.");
      return;
    }
    calibrationFactor = newFactor;
    calibrated = true;
    scale.set_scale(calibrationFactor);
    preferences.putFloat("factor", calibrationFactor);
    Serial.print("CALIBRATION_FACTOR = ");
    Serial.println(calibrationFactor, 6);
    Serial.println("Saved. REMOVE calibration mass; system will enter WAITING when empty.");
    baselineWeight = 0;
    lossSince = 0;
    changeState(REMOVE_CAL_WEIGHT);
    return;
  }
  if (cmd == "a") {
    if (ENABLE_SERIAL_AUTH_TEST) {
      authorizePickup(now, "SERIAL TEST");
    } else {
      Serial.println("DISABLED: scan a valid RFID owner card instead.");
    }
    return;
  }
  if (cmd == "t") {
    if (state != WAITING || baselineWeight != 0 || fabsf(currentWeight) > EMPTY_G) {
      Serial.println("REFUSED: tare allowed only on an EMPTY WAITING platform.");
      return;
    }
    if (!scale.wait_ready_timeout(800)) { changeState(FAULT); return; }
    scale.tare(15);
    Serial.println("Empty tare complete.");
    return;
  }
  Serial.println("Unknown command. Type h for help.");
}

void setup() {
  initOutputs();
  Serial.begin(115200);
  Serial.setTimeout(60);
  initRadio();
  WiFi.begin("Tui2", "Adgjm23456");
  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) { //最多等15秒
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Connection FAILED!");
    Serial.println("Please check SSID, Password, and make sure it's 2.4GHz.");
  }
  Serial.println("\nWiFi Connected!");
  delay(500);
  tgClient().setInsecure();   // 跳过 SSL 证书校验，测试阶段够用
  Serial.println("\nParcelGuard Coding 7 WIRELESS: weight security + RFID + LEDs + buzzer");
  Serial.println("WARNING: platform MUST be EMPTY at every startup.");
  // RC522 uses ESP32 hardware SPI bus (VSPI).
  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(100);
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RC522 VERSION = 0x");
  Serial.println(version, HEX);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("RFID ERROR: check 3V3/GND/SDA(D5)/SCK(D18)/MOSI(D23)/MISO(D19)/RST(D16).");
  } else {
    Serial.println("RFID reader responding. Scan a card to print its UID.");
  }
  scale.begin(PIN_DT, PIN_SCK);
  if (!scale.wait_ready_timeout(3000)) {
    changeState(FAULT);
    Serial.println("HX711 not responding; check wiring.");
    return;
  }
  scale.set_scale(1.0f);
  scale.tare(15); // same startup zeroing as Coding 1
  preferences.begin("parcel-scale", false);
  ownerUID = preferences.getString("owner", "");
  ownerUID.trim();
  ownerUID.toUpperCase();
  if (ownerUID.length()) {
    Serial.print("Stored owner UID: ");
    Serial.println(ownerUID);
  } else {
    Serial.println("No owner card saved. When WAITING and empty: send u, then scan card.");
  }
  if (preferences.isKey("factor")) {
    float saved = preferences.getFloat("factor", 0);
    if (isfinite(saved) && fabsf(saved) >= 0.0001f) {
      calibrationFactor = saved;
      calibrated = true;
    }
  }
  scale.set_scale(calibrationFactor);
  if (calibrated) {
    Serial.print("Loaded stored calibration: ");
    Serial.println(calibrationFactor, 6);
    changeState(WAITING);
  } else {
    Serial.println("No saved calibration. Place known mass and type c 1000.");
    changeState(NEED_CALIBRATION);
  }
  lastSensorReady=millis();
  Serial.println("AUTO_PHOTO_V2: FIRST_ARRIVAL PACKAGE_ADDED OWNER_PICKUP ABNORMAL_LOSS");
  showHelp();
}

void loop() {
  updateWireless();
  unsigned long now = millis();
  handleSerial(now);
  handleRFID(now);
  updateOutputs(now);
  if (state == FAULT) { delay(5); return; }
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  if (!scale.is_ready()) {
    if(now-lastSensorReady>=2000){changeState(FAULT);Serial.println("FAULT: HX711 timeout; baseline retained; repair then r");updateOutputs(now);}
    return;
  }
  lastSensorReady=now;
  currentWeight = scale.get_units(1); // One ready conversion, no waiting for three samples.
  if (!isfinite(currentWeight)) { changeState(FAULT); updateOutputs(now); return; }
  if (fabsf(currentWeight) < 15.0f) currentWeight = 0;

  // Protect the ORIGINAL confirmed baseline even while checking an addition.
  if (state == ARMED || state == ADD_PENDING) {
    if (currentWeight < baselineWeight - REMOVE_THRESHOLD_G) {
      if (lossSince == 0) lossSince = now;
      if (now - lossSince >= LOSS_CONFIRM_MS) alarmNow();
    } else {
      lossSince = 0;
    }
  } else {
    lossSince = 0;
  }

  switch (state) {
    case NEED_CALIBRATION:
      break;
    case REMOVE_CAL_WEIGHT:
      if (fabsf(currentWeight) < EMPTY_G) changeState(WAITING);
      break;
    case WAITING:
      if (currentWeight >= MIN_PACKAGE_G) {
        firstArrivalPending = true;
        startStability(STABILIZING, currentWeight, now);
      }
      break;
    case STABILIZING:
      if (currentWeight < MIN_PACKAGE_G) {
        baselineWeight = 0;
        changeState(WAITING);
      } else if (fabsf(currentWeight - candidateAnchor) > STABLE_TOL_G) {
        candidateAnchor = currentWeight;
        stableSince = now;
      } else if (now - stableSince >= STABLE_TIME_MS) {
        baselineWeight = currentWeight;
        changeState(ARMED);
        Serial.println("ARMED: confirmed baseline protected.");
        if (firstArrivalPending) {
          eventPhoto("FIRST_ARRIVAL");
        }
        firstArrivalPending = false;
      }
      break;
    case ARMED:
      if (currentWeight > baselineWeight + ADD_THRESHOLD_G)
        startStability(ADD_PENDING, currentWeight, now);
      break;
    case ADD_PENDING:
      if (currentWeight <= baselineWeight + ADD_THRESHOLD_G) {
        changeState(ARMED);
      } else if (fabsf(currentWeight - candidateAnchor) > STABLE_TOL_G) {
        candidateAnchor = currentWeight;
        stableSince = now;
      } else if (now - stableSince >= STABLE_TIME_MS) {
        baselineWeight = currentWeight;
        changeState(ARMED);
        Serial.println("ADDED PACKAGE CONFIRMED: baseline updated.");
        eventPhoto("PACKAGE_ADDED");
      }
      break;
    case AUTHORIZED:
      // Only a confirmed decrease DURING the authorization window counts.
      // Capture once per authorization session, including a partial pickup.
      if (now - authSince < AUTH_TIME_MS && !pickupPhotographed) {
        bool decreased = pickupReference >= MIN_PACKAGE_G &&
          (currentWeight < pickupReference - REMOVE_THRESHOLD_G || currentWeight < MIN_PACKAGE_G);
        
        if (!decreased) {
          pickupCandidate = false;
        } else if (!pickupCandidate || fabsf(currentWeight - pickupAnchor) > 10.0f) {
          pickupCandidate = true;
          pickupAnchor = currentWeight;
          pickupSince = now;
        } else if (now - pickupSince >= LOSS_CONFIRM_MS) {
          // 確認拿走包裹：只拍照，不發送 Telegram
          eventPhoto("OWNER_PICKUP");
          pickupPhotographed = true;
        }
      }
      
      if (now - authSince >= AUTH_TIME_MS) {
        firstArrivalPending = false;
        pickupCandidate = false;
        if (currentWeight < MIN_PACKAGE_G) {
          baselineWeight = 0;
          changeState(WAITING);
        } else {
          startStability(STABILIZING, currentWeight, now);
        }
        Serial.println("Authorization expired; recheck remaining weight.");
      }
      break;
    case ALARM:
      // Latched: only a valid RFID card clears it by entering AUTHORIZED.
      break;
    case FAULT:
      break;
  }

  // Reflect state transitions immediately, not only at the next loop.
  updateOutputs(now);

  if (now - lastPrint >= PRINT_MS) {
    lastPrint = now;
    if (!calibrated) Serial.print("UNCALIBRATED counts=");
    else Serial.print("Weight=");
    Serial.print(currentWeight, 1);
    if (calibrated) Serial.print("g");
    Serial.print(" Baseline="); Serial.print(baselineWeight, 1);
    Serial.print("g State="); Serial.println(stateName(state));
  }
}

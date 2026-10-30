/*
  ================================================================
  DEVICE 1 - BOILER CONTROLLER
  ESP32-WROOM-32 (DevKitC V4)
  ================================================================

  ROLE:
   - Receives room temperature + setpoint from Device 2 over ESP-NOW
   - Decides whether to call for heat and drives two relays
   - Exposes control via a standalone Telegram bot (no Home Assistant)
   - FAILS SAFE: on any fault (WiFi down, ESP-NOW data stale, crash/
     reboot), BOTH relays de-energize, which hands the boiler back
     to full VRT230 control via the NC contact wiring.

  RELAY WIRING (per your schematic):
   - RELAY 1 (JQ1P-5V-F):  COM = wire from VRT230, NC = boiler terminal 4
       -> De-energized (default) = VRT230 has a direct path to the boiler.
       -> Energized = VRT230 is disconnected, boiler terminal 4 now
          routes through Relay 2 instead.
   - RELAY 2 (JQ1P-5V-F1): COM = boiler terminal 3, NO = boiler terminal 4
       -> Only matters once Relay 1 has switched away from VRT230.
       -> Energized = actively closes the "call for heat" circuit.

  REQUIRED LIBRARIES (Arduino IDE Library Manager):
   - UniversalTelegramBot (by Brian Lough)
   - ArduinoJson (dependency of the above, v6.x)

  ================================================================
*/

#include <WiFi.h>
#include <esp_now.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

// ---------------- USER CONFIG - FILL THESE IN ----------------
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";
const char* BOT_TOKEN     = "";   // from @BotFather
const char* CHAT_ID       = "";     // only this chat can issue commands
// MAINTENANCE chat: /status /stats /reconnect /reboot. Boot, reboot and leak notices
// are also sent here. Leave the placeholder as-is to disable the maintenance chat
// (notices then go to the control chat).
const char* MAINT_CHAT_ID = "";

// Device 2's MAC address. On Device 2, run WiFi.macAddress() once and paste it here.
uint8_t device2Mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}; // <-- REPLACE

// ---------------- PIN CONFIG - VERIFY AGAINST YOUR SCHEMATIC ----------------
// GPIO21 was clearly legible on your schematic for Relay 1's driver line.
const int RELAY1_PIN = 21;

// Relay 2's driver line was harder to read precisely at this image resolution.
// Placeholder below - CONFIRM the actual GPIO before flashing.
const int RELAY2_PIN = 19; // <-- VERIFY

// ---------------- CONTROL PARAMETERS ----------------
const float HYSTERESIS_C = 0.3;                                   // deg C, avoids relay chatter
const unsigned long DATA_TIMEOUT_MS   = 15UL * 60UL * 1000UL;      // 15 min without Device 2 data -> failsafe
const unsigned long TELEGRAM_POLL_MS  = 1000;
const unsigned long STATUS_SEND_MS    = 30UL * 1000UL;             // how often we report status back to Device 2
const unsigned long WIFI_RETRY_MS     = 10UL * 1000UL;

// ---------------- MEMORY-LEAK PROTECTION ----------------
// If free heap stays too low for HEAP_STRIKE_LIMIT checks in a row, the board
// reboots itself. The largest-contiguous-block check matters most: TLS to
// Telegram needs a big block, and fragmentation can kill it long before total
// free heap looks "low".
const unsigned long HEAP_CHECK_MS        = 10UL * 1000UL;
const uint32_t      HEAP_MIN_FREE_BYTES  = 40000;  // total free heap floor
const uint32_t      HEAP_MIN_BLOCK_BYTES = 20000;  // largest contiguous block floor
const int           HEAP_STRIKE_LIMIT    = 6;      // 6 x 10s = 60s of sustained low memory

// ---------------- DAILY REBOOT ----------------
// Reboots once a day at DAILY_REBOOT_HOUR (local time, from NTP). If NTP time is
// unavailable, it falls back to rebooting after 24h of uptime. The hard cap also
// applies when NTP works, as a backstop.
const int           DAILY_REBOOT_HOUR       = 4;                 // 04:00 local time
const long          TZ_OFFSET_SEC           = 3L * 3600L;        // Istanbul = UTC+3, no DST
const char*         NTP_SERVER              = "pool.ntp.org";
const unsigned long DAILY_REBOOT_MIN_UPTIME = 2UL * 3600UL * 1000UL;   // never reboot twice in the same hour
const unsigned long UPTIME_HARD_CAP_MS      = 25UL * 3600UL * 1000UL;   // backstop

// ---------------- STATE ----------------
float currentTemp   = NAN;
float setpoint       = 21.0;
unsigned long lastDataMillis = 0;
bool autoMode        = true;   // true = thermostat logic, false = manual /on //off via Telegram
bool manualHeatOn    = false;
bool relayHeatState  = false;  // last commanded state of Relay 2

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
unsigned long lastTelegramCheck = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastStatusSend = 0;
unsigned long lastHeapCheck = 0;
int heapStrikes = 0;
unsigned long espNowPackets = 0;
unsigned long telegramCommands = 0;
unsigned long wifiReconnects = 0;
String bootReasonText = "unknown";

// Survives a software reboot (not a power cycle) so we can report why we restarted.
RTC_NOINIT_ATTR static uint32_t rebootMagic;
RTC_NOINIT_ATTR static char rebootReason[40];
const uint32_t REBOOT_MAGIC = 0xB011E4A5;

typedef struct struct_message {
  float temperature;     // sender's room/status temperature
  float setpoint;        // desired setpoint
  float batteryVoltage;  // Device 2 -> Device 1: battery voltage. Device 1 -> Device 2: unused (-1)
  bool  heatRelayState;  // Device 1 -> Device 2 only: is the heat relay currently energized?
} struct_message;

struct_message incomingData;
struct_message outgoingStatus;

// ---------------- FORWARD DECLARATIONS ----------------
void setRelays(bool overrideActive, bool heatOn);
void safeReboot(const char* reason);
void checkHeapHealth(unsigned long now);
void checkScheduledReboot(unsigned long now);
void handleMaintenanceCommand(const String &chat_id, const String &text);
bool maintConfigured();
String notifyChatId();
String formatUptime(unsigned long ms);
void handleNewMessages(int numNewMessages);
void onDataRecv(const uint8_t *mac, const uint8_t *incomingBytes, int len);

void setup() {
  Serial.begin(115200);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  // Boot in the safest possible state: both de-energized -> VRT230 fully in control.
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  // Hardware watchdog: if the main loop ever hangs for 30s, force a reboot.
  // A reboot re-enters setup() and re-asserts the failsafe relay state above.
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(300);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println();

  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER); // non-blocking; syncs whenever WiFi is up

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi channel (Device 2 must match this for ESP-NOW): ");
    Serial.println(WiFi.channel());
    secured_client.setInsecure(); // simplifies TLS for Telegram; fine for a hobby bot
  } else {
    Serial.println("WiFi failed to connect. VRT230 remains in control until link recovers.");
  }


  // Work out why we restarted (leak reboot, daily reboot, watchdog, brownout, ...).
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_SW && rebootMagic == REBOOT_MAGIC) {
    bootReasonText = "software: " + String(rebootReason);
  } else if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) {
    bootReasonText = "watchdog";
  } else if (rr == ESP_RST_BROWNOUT) {
    bootReasonText = "BROWNOUT - check the power supply";
  } else if (rr == ESP_RST_POWERON) {
    bootReasonText = "power on";
  } else if (rr == ESP_RST_PANIC) {
    bootReasonText = "crash/panic";
  } else {
    bootReasonText = "reset code " + String((int)rr);
  }
  rebootMagic = 0;

  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(notifyChatId(), "Boiler controller booted. Reason: " + bootReasonText, "");

    // Discard commands that piled up while we were offline, so an old /on or
    // /setpoint can't suddenly execute after a reboot.
    int n = bot.getUpdates(bot.last_message_received + 1);
    for (int guard = 0; n > 0 && guard < 10; guard++) {
      esp_task_wdt_reset();
      n = bot.getUpdates(bot.last_message_received + 1);
    }
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, device2Mac, 6);
    peerInfo.channel = 0; // 0 = use current WiFi channel
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add ESP-NOW peer");
    }
  }

  // Don't let a boot-time gap immediately trip the failsafe timeout.
  lastDataMillis = millis();
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();
  checkHeapHealth(now);
  checkScheduledReboot(now);

  // 1) Keep WiFi alive
  if (now - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      wifiReconnects++;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // 2) Poll Telegram for new commands
  if (WiFi.status() == WL_CONNECTED && now - lastTelegramCheck > TELEGRAM_POLL_MS) {
    lastTelegramCheck = now;
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
  }

  // 3) Failsafe check
  bool dataStale = (now - lastDataMillis > DATA_TIMEOUT_MS);

  // 4) Decide relay state
  bool overrideActive; // Relay 1
  bool heatOn;         // Relay 2

  if (dataStale) {
    // No fresh sensor data from Device 2 -> trust the physical VRT230 entirely.
    overrideActive = false;
    heatOn = false;
  } else if (!autoMode) {
    // Manual mode via Telegram /on or /off
    overrideActive = true;
    heatOn = manualHeatOn;
  } else {
    // Automatic thermostat logic with hysteresis
    overrideActive = true;
    if (!isnan(currentTemp)) {
      if (currentTemp < setpoint - HYSTERESIS_C) heatOn = true;
      else if (currentTemp > setpoint + HYSTERESIS_C) heatOn = false;
      else heatOn = relayHeatState; // stay put inside the hysteresis band
    } else {
      overrideActive = false; // no reading yet (e.g. right after a reboot): leave VRT230 in control
      heatOn = false;
    }
  }

  setRelays(overrideActive, heatOn);

  // 5) Report status back to Device 2 periodically
  if (now - lastStatusSend > STATUS_SEND_MS) {
    lastStatusSend = now;
    outgoingStatus.temperature = currentTemp;
    outgoingStatus.setpoint = setpoint;
    outgoingStatus.batteryVoltage = -1;
    outgoingStatus.heatRelayState = relayHeatState;
    esp_now_send(device2Mac, (uint8_t*)&outgoingStatus, sizeof(outgoingStatus));
  }

  delay(50);
}

void safeReboot(const char* reason) {
  // Put the boiler circuit in its safe state first: both relays off = VRT230 in control.
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  Serial.printf("REBOOTING: %s\n", reason);
  rebootMagic = REBOOT_MAGIC;
  strncpy(rebootReason, reason, sizeof(rebootReason) - 1);
  rebootReason[sizeof(rebootReason) - 1] = 0;
  delay(200);
  ESP.restart();
}

void checkHeapHealth(unsigned long now) {
  if (now - lastHeapCheck < HEAP_CHECK_MS) return;
  lastHeapCheck = now;

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("Heap: free=%u largest=%u minEver=%u\n", freeHeap, largest, ESP.getMinFreeHeap());

  if (freeHeap < HEAP_MIN_FREE_BYTES || largest < HEAP_MIN_BLOCK_BYTES) {
    heapStrikes++;
    Serial.printf("Low memory strike %d/%d\n", heapStrikes, HEAP_STRIKE_LIMIT);
    if (heapStrikes >= HEAP_STRIKE_LIMIT) {
      safeReboot("low heap (leak/fragmentation)");
    }
  } else {
    heapStrikes = 0; // recovered, so it was a transient dip
  }
}

void setRelays(bool overrideActive, bool heatOn) {
  digitalWrite(RELAY1_PIN, overrideActive ? HIGH : LOW);
  digitalWrite(RELAY2_PIN, (overrideActive && heatOn) ? HIGH : LOW);
  relayHeatState = heatOn;
}

void onDataRecv(const uint8_t *mac, const uint8_t *incomingBytes, int len) {
  if (len == sizeof(struct_message)) {
    memcpy(&incomingData, incomingBytes, sizeof(incomingData));
    currentTemp = incomingData.temperature;
    setpoint = incomingData.setpoint; // Device 2's buttons are the source of truth unless Telegram overrides it below
    lastDataMillis = millis();
    espNowPackets++;
  }
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    bool isControl = (chat_id == CHAT_ID);
    bool isMaint   = maintConfigured() && (chat_id == MAINT_CHAT_ID);

    if (!isControl && !isMaint) {
      bot.sendMessage(chat_id, "Unauthorized.", "");
      continue;
    }
    telegramCommands++;

    // The maintenance chat only gets maintenance commands (it cannot change heating).
    // If you use the same chat ID for both, the control commands take priority.
    if (isMaint && !isControl) {
      handleMaintenanceCommand(chat_id, text);
      continue;
    }

    if (text == "/status") {
      bool stale = (millis() - lastDataMillis > DATA_TIMEOUT_MS);
      String msg = "Room temp: " + (isnan(currentTemp) ? String("N/A") : String(currentTemp, 1) + "C") + "\n";
      msg += "Setpoint: " + String(setpoint, 1) + "C\n";
      msg += "Mode: " + String(autoMode ? "AUTO" : "MANUAL") + "\n";
      msg += "Heat relay: " + String(relayHeatState ? "ON" : "OFF") + "\n";
      msg += "Last data from thermostat: " + String((millis() - lastDataMillis) / 1000) + "s ago\n";
      msg += "Failsafe (VRT230 direct control): " + String(stale ? "ACTIVE" : "inactive");
      bot.sendMessage(chat_id, msg, "");

    } else if (text.startsWith("/setpoint")) {
      float val = text.substring(text.indexOf(' ') + 1).toFloat();
      if (val >= 10 && val <= 30) {
        setpoint = val;
        bot.sendMessage(chat_id, "Setpoint updated to " + String(val, 1) + "C", "");
      } else {
        bot.sendMessage(chat_id, "Usage: /setpoint <10-30>", "");
      }

    } else if (text == "/auto") {
      autoMode = true;
      bot.sendMessage(chat_id, "Switched to AUTO (thermostat) mode.", "");

    } else if (text == "/on") {
      autoMode = false;
      manualHeatOn = true;
      bot.sendMessage(chat_id, "Manual heat ON (bypassing thermostat logic).", "");

    } else if (text == "/off") {
      autoMode = false;
      manualHeatOn = false;
      bot.sendMessage(chat_id, "Manual heat OFF (bypassing thermostat logic).", "");

    } else if (text == "/failsafe") {
      lastDataMillis = 0; // force "stale" -> immediate handback to VRT230
      bot.sendMessage(chat_id, "Forcing failsafe: VRT230 now has direct, unassisted control.", "");

    } else if (text == "/start" || text == "/help") {
      String help = "Commands:\n/status\n/setpoint <10-30>\n/auto\n/on\n/off\n/failsafe";
      bot.sendMessage(chat_id, help, "");
    }
  }
}


// ======================= MAINTENANCE CHAT =======================

bool maintConfigured() {
  return strlen(MAINT_CHAT_ID) > 0 && strcmp(MAINT_CHAT_ID, "YOUR_MAINTENANCE_CHAT_ID") != 0;
}

// Where system notices (boot, reboot reasons) are sent.
String notifyChatId() {
  return maintConfigured() ? String(MAINT_CHAT_ID) : String(CHAT_ID);
}

String formatUptime(unsigned long ms) {
  unsigned long totalMin = ms / 60000UL;
  unsigned long d = totalMin / 1440UL;
  unsigned long h = (totalMin % 1440UL) / 60UL;
  unsigned long m = totalMin % 60UL;
  char buf[24];
  snprintf(buf, sizeof(buf), "%lud %02luh %02lum", d, h, m);
  return String(buf);
}

// Reboots once per day (see DAILY REBOOT config). Relays go to the safe state
// first, so VRT230 covers the boiler for the few seconds the board is down.
void checkScheduledReboot(unsigned long now) {
  static unsigned long lastCheck = 0;
  if (now - lastCheck < 30000UL) return;
  lastCheck = now;

  if (now > UPTIME_HARD_CAP_MS) {
    safeReboot("daily reboot (uptime backstop)");
  }

  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year > 120) { // NTP time is valid (year >= 2021)
    if (t.tm_hour == DAILY_REBOOT_HOUR && now > DAILY_REBOOT_MIN_UPTIME) {
      safeReboot("daily scheduled reboot");
    }
  }
}

void handleMaintenanceCommand(const String &chat_id, const String &text) {
  if (text == "/status") {
    bool stale = (millis() - lastDataMillis > DATA_TIMEOUT_MS);
    String msg = "SYSTEM STATUS\n";
    msg += "Uptime: " + formatUptime(millis()) + "\n";
    msg += "Last boot: " + bootReasonText + "\n";
    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_year > 120) {
      char tb[24];
      strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &t);
      msg += "Clock: " + String(tb) + " (NTP ok)\n";
    } else {
      msg += "Clock: not synced (daily reboot uses 24h uptime fallback)\n";
    }
    msg += "WiFi: " + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm, ch " + String(WiFi.channel()) + "\n";
    msg += "Thermostat link: last data " + String((millis() - lastDataMillis) / 1000) + "s ago";
    msg += stale ? " (STALE - failsafe active)\n" : " (ok)\n";
    msg += "Relay 1 (override): " + String(digitalRead(RELAY1_PIN) ? "ENERGIZED" : "off (VRT230 path)") + "\n";
    msg += "Relay 2 (heat call): " + String(digitalRead(RELAY2_PIN) ? "ENERGIZED" : "off") + "\n";
    msg += "Heap: " + String(ESP.getFreeHeap()) + " B free, strikes " + String(heapStrikes) + "/" + String(HEAP_STRIKE_LIMIT);
    bot.sendMessage(chat_id, msg, "");

  } else if (text == "/stats") {
    String msg = "STATISTICS\n";
    msg += "Uptime: " + formatUptime(millis()) + "\n";
    msg += "Heap free: " + String(ESP.getFreeHeap()) + " B of " + String(ESP.getHeapSize()) + " B\n";
    msg += "Heap lowest ever: " + String(ESP.getMinFreeHeap()) + " B\n";
    msg += "Largest free block: " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + " B\n";
    msg += "Leak thresholds: free < " + String(HEAP_MIN_FREE_BYTES) + " B or block < " + String(HEAP_MIN_BLOCK_BYTES) + " B\n";
    msg += "ESP-NOW packets received: " + String(espNowPackets) + "\n";
    msg += "Telegram commands handled: " + String(telegramCommands) + "\n";
    msg += "WiFi reconnects: " + String(wifiReconnects) + "\n";
    msg += "IP: " + WiFi.localIP().toString() + ", RSSI " + String(WiFi.RSSI()) + " dBm\n";
    msg += "CPU: " + String(getCpuFrequencyMhz()) + " MHz\n";
    msg += "Daily reboot: " + String(DAILY_REBOOT_HOUR) + ":00 local";
    bot.sendMessage(chat_id, msg, "");

  } else if (text == "/reconnect") {
    wifiReconnects++;
    bot.sendMessage(chat_id, "Reconnecting WiFi...", "");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  } else if (text == "/reboot") {
    bot.sendMessage(chat_id, "This restarts the controller (VRT230 covers the boiler for a few seconds). Send /reboot confirm to proceed.", "");

  } else if (text == "/reboot confirm") {
    bot.sendMessage(chat_id, "Rebooting now...", "");
    // Acknowledge this update with Telegram first. Otherwise the same "/reboot confirm"
    // is delivered again after the restart and the board would reboot in a loop.
    bot.getUpdates(bot.last_message_received + 1);
    safeReboot("manual reboot via maintenance chat");

  } else if (text == "/start" || text == "/help") {
    bot.sendMessage(chat_id, "Maintenance commands:\n/status - system health\n/stats - memory and counters\n/reconnect - restart WiFi\n/reboot - restart controller (asks to confirm)", "");
  }
}

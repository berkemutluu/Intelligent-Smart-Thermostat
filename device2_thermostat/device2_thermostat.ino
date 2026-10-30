/*
  ================================================================
  DEVICE 2 - BATTERY-POWERED THERMOSTAT
  ESP32-WROOM-32 (DevKitC V4)
  ================================================================

  ROLE:
   - Reads room temperature/humidity from the AHT10
   - Lets you adjust the setpoint with two physical buttons
   - Shows setpoint on OLED1, current temp + boiler/link/battery
     status on OLED2
   - Sends readings to Device 1 over ESP-NOW, receives boiler
     status back from it

  REQUIRED LIBRARIES (Arduino IDE Library Manager):
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit AHTX0 (also pulls in Adafruit Unified Sensor)

  I2C LAYOUT:
   Two separate buses, so no address resoldering is needed.
   Bus A (Wire)  : AHT10 (0x38) + OLED1 setpoint screen (0x3C)
   Bus B (Wire1) : OLED2 status screen (0x3C)
   Set the pins in I2C_A_SDA/SCL and I2C_B_SDA/SCL below.

  IMPORTANT - ESP-NOW CHANNEL:
   ESP-NOW peers must be on the same WiFi channel. Since Device 1
   connects to your home WiFi (channel assigned by your router),
   Device 2 must match that channel even though it never actually
   joins the network. Set WIFI_CHANNEL below to whatever Device 1
   prints to Serial on boot ("WiFi channel: X").

  ================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// ---------------- PIN CONFIG - VERIFY AGAINST YOUR SCHEMATIC ----------------
// Two independent I2C buses. Change these pin numbers freely (any free GPIO
// that isn't input-only or a flash/boot pin works - the ESP32 can route I2C
// to almost any pin).
//   Bus A (Wire)  : AHT10 temperature sensor + OLED1 (setpoint screen)
//   Bus B (Wire1) : OLED2 (status screen) only
const int I2C_A_SDA = 21;
const int I2C_A_SCL = 22;
const int I2C_B_SDA = 16;
const int I2C_B_SCL = 17;

// The two button lines were harder to pin down precisely at the image
// resolution provided. Placeholders below - CONFIRM before flashing.
const int BTN_PLUS_PIN  = 23; // <-- VERIFY
const int BTN_MINUS_PIN = 19; // <-- VERIFY

// Each OLED is on its own bus, so both can keep the factory 0x3C address.
// (AHT10 is 0x38 and shares bus A with OLED1 - no conflict.)
const int OLED1_ADDR = 0x3C; // setpoint display, bus A
const int OLED2_ADDR = 0x3C; // status display,   bus B

const int SCREEN_WIDTH  = 128;
const int SCREEN_HEIGHT = 64;

// Battery voltage sense: 10k + 10k divider from Battery+ to GND, midpoint to this pin.
// GPIO34 is ADC1 (WiFi/ESP-NOW-safe) and input-only, so it's a clean unused pin.
const int BATTERY_ADC_PIN = 34;
const float DIVIDER_RATIO = 2.0; // (10k+10k)/10k = 2.0 -> multiply the ADC reading by this
// Rough 18650 discharge curve endpoints for a simple percentage estimate.
// Not linear in reality, but good enough for an at-a-glance indicator.
const float BATTERY_EMPTY_V = 3.3;
const float BATTERY_FULL_V  = 4.2;

// ---------------- USER CONFIG ----------------
// Device 1's MAC address. On Device 1, run WiFi.macAddress() once and paste it here.
uint8_t device1Mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}; // <-- REPLACE
const int WIFI_CHANNEL = 6; // <-- SET to match Device 1's actual WiFi channel

const float SETPOINT_STEP = 0.5;
const float SETPOINT_MIN  = 10.0;
const float SETPOINT_MAX  = 30.0;
const unsigned long SEND_INTERVAL_MS = 60UL * 1000UL; // push a reading every 60s
const unsigned long DEBOUNCE_MS = 250;
const unsigned long LINK_TIMEOUT_MS = 3UL * SEND_INTERVAL_MS;

// ---------------- OBJECTS ----------------
Adafruit_SSD1306 oledSetpoint(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SSD1306 oledStatus(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1);
Adafruit_AHTX0 aht;

typedef struct struct_message {
  float temperature;
  float setpoint;
  float batteryVoltage;
  bool  heatRelayState;
} struct_message;

struct_message outgoing;
struct_message incomingStatus;

// ---------------- STATE ----------------
float setpoint = 21.0;
float currentTemp = NAN;
float currentHumidity = NAN;
bool boilerHeatOn = false;
bool linkOk = false;
unsigned long lastBoilerDataMillis = 0;
unsigned long lastSendMillis = 0;
unsigned long lastBtnPlusMillis = 0;
unsigned long lastBtnMinusMillis = 0;

// ---------------- FORWARD DECLARATIONS ----------------
void sendReading();
void updateDisplays();
void onDataRecv(const uint8_t *mac, const uint8_t *incomingBytes, int len);
float readBatteryVoltage();

void setup() {
  Serial.begin(115200);

  pinMode(BTN_PLUS_PIN, INPUT_PULLUP);
  pinMode(BTN_MINUS_PIN, INPUT_PULLUP);

  Wire.begin(I2C_A_SDA, I2C_A_SCL);    // bus A: AHT10 + OLED1
  Wire1.begin(I2C_B_SDA, I2C_B_SCL);   // bus B: OLED2

  // 11dB attenuation gives the widest usable input range (~0-3.1V), which we
  // need since the divider's max output (4.2V / 2) is around 2.1V.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  if (!aht.begin(&Wire)) {
    Serial.println("AHT10 not found - check wiring!");
  }

  if (!oledSetpoint.begin(SSD1306_SWITCHCAPVCC, OLED1_ADDR)) {
    Serial.println("OLED1 (setpoint) not found at 0x3C");
  }
  if (!oledStatus.begin(SSD1306_SWITCHCAPVCC, OLED2_ADDR)) {
    Serial.println("OLED2 (status) not found at 0x3C on bus B - check its SDA/SCL wiring");
  }

  // Set up WiFi in STA mode but stay disconnected - we only need it for ESP-NOW.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, device1Mac, 6);
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  // Take an initial reading immediately rather than waiting a full interval.
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  currentTemp = temp.temperature;
  currentHumidity = humidity.relative_humidity;

  updateDisplays();
}

void loop() {
  unsigned long now = millis();

  // --- Buttons ---
  if (digitalRead(BTN_PLUS_PIN) == LOW && now - lastBtnPlusMillis > DEBOUNCE_MS) {
    lastBtnPlusMillis = now;
    setpoint = min(setpoint + SETPOINT_STEP, SETPOINT_MAX);
    updateDisplays();
    sendReading(); // push the change immediately instead of waiting for the next interval
  }
  if (digitalRead(BTN_MINUS_PIN) == LOW && now - lastBtnMinusMillis > DEBOUNCE_MS) {
    lastBtnMinusMillis = now;
    setpoint = max(setpoint - SETPOINT_STEP, SETPOINT_MIN);
    updateDisplays();
    sendReading();
  }

  // --- Periodic sensor read + send ---
  if (now - lastSendMillis > SEND_INTERVAL_MS) {
    lastSendMillis = now;
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    currentTemp = temp.temperature;
    currentHumidity = humidity.relative_humidity;
    sendReading();
    updateDisplays();
  }

  // --- Link health ---
  bool wasLinkOk = linkOk;
  linkOk = (now - lastBoilerDataMillis < LINK_TIMEOUT_MS);
  if (wasLinkOk != linkOk) updateDisplays(); // refresh immediately on a state change

  delay(50);
}

void sendReading() {
  outgoing.temperature = currentTemp;
  outgoing.setpoint = setpoint;
  outgoing.batteryVoltage = readBatteryVoltage();
  outgoing.heatRelayState = false; // unused in this direction
  esp_now_send(device1Mac, (uint8_t*)&outgoing, sizeof(outgoing));
}

void onDataRecv(const uint8_t *mac, const uint8_t *incomingBytes, int len) {
  if (len == sizeof(struct_message)) {
    memcpy(&incomingStatus, incomingBytes, sizeof(incomingStatus));
    boilerHeatOn = incomingStatus.heatRelayState;
    lastBoilerDataMillis = millis();
    updateDisplays();
  }
}

float readBatteryVoltage() {
  // analogReadMilliVolts() uses the ESP32's factory ADC calibration internally,
  // which is noticeably more accurate than a raw analogRead() + manual formula.
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
  return (mv / 1000.0) * DIVIDER_RATIO;
}

int batteryPercent(float voltage) {
  if (voltage < 0) return -1;
  float pct = (voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)(pct + 0.5);
}

// Right-aligns a string's right edge to (screenWidth - marginPx) at the given textSize.
void printRightAligned(Adafruit_SSD1306 &oled, const String &text, int y, int textSize, int marginPx) {
  int charW = 6 * textSize; // default GFX font: 6px advance per character at size 1
  int textW = charW * text.length();
  int x = SCREEN_WIDTH - marginPx - textW;
  if (x < 0) x = 0;
  oled.setTextSize(textSize);
  oled.setCursor(x, y);
  oled.print(text);
}

// Horizontally centers a string at the given textSize.
void printCentered(Adafruit_SSD1306 &oled, const String &text, int y, int textSize) {
  int charW = 6 * textSize;
  int textW = charW * text.length();
  int x = (SCREEN_WIDTH - textW) / 2;
  if (x < 0) x = 0;
  oled.setTextSize(textSize);
  oled.setCursor(x, y);
  oled.print(text);
}

void updateDisplays() {
  // ----------------------------------------------------------------
  // NOTE ON COLOR: these panels are two-tone SSD1306/1315 modules -
  // the top strip (rows 0-15) is hard-wired to yellow, everything
  // below it (rows 16-63) is hard-wired to blue. There's no software
  // color control; "color" here just means which region text lands
  // in. So the big, important numbers below are placed in the lower
  // (blue) region on purpose, and only short secondary info goes in
  // the thin yellow strip up top.
  // ----------------------------------------------------------------

  // ============ OLED1: setpoint, as big as it fits ============
  oledSetpoint.clearDisplay();
  oledSetpoint.setTextColor(SSD1306_WHITE);

  // Yellow strip (y 0-15): label only.
  printCentered(oledSetpoint, "SET TEMP", 2, 1);

  // Blue region (y 16-63, 48px tall): giant setpoint value.
  // Size 4 = 24x32px per character. "21.5C" is 5 chars = 120px wide,
  // fits the 128px panel width, and 32px tall fits inside the 48px
  // blue band with room to vertically center it.
  char setpointStr[8];
  snprintf(setpointStr, sizeof(setpointStr), "%.1fC", setpoint);
  printCentered(oledSetpoint, setpointStr, 24, 4);

  oledSetpoint.display();

  // ============ OLED2: current temp + status ============
  oledStatus.clearDisplay();
  oledStatus.setTextColor(SSD1306_WHITE);

  // Yellow strip (y 0-15): battery status - compact, so it fits one line.
  float battV = readBatteryVoltage();
  int battPct = batteryPercent(battV);
  char battStr[24];
  if (battPct < 0) snprintf(battStr, sizeof(battStr), "BATT N/A");
  else snprintf(battStr, sizeof(battStr), "BATT %d%% %.2fV", battPct, battV);
  printCentered(oledStatus, battStr, 2, 1);

  // Blue region: giant current temperature, size 3 (18x24px/char) so
  // there's still room below it for the two status lines.
  char tempStr[8];
  if (isnan(currentTemp)) snprintf(tempStr, sizeof(tempStr), "--.-C");
  else snprintf(tempStr, sizeof(tempStr), "%.1fC", currentTemp);
  printCentered(oledStatus, tempStr, 20, 3);

  // Bottom-left: connection status to Device 1.
  oledStatus.setTextSize(1);
  oledStatus.setCursor(2, 54);
  oledStatus.print(linkOk ? "LINK OK" : "LINK LOST");

  // Bottom-right: boiler heat status.
  printRightAligned(oledStatus, boilerHeatOn ? "HEAT ON" : "HEAT OFF", 54, 1, 2);

  oledStatus.display();
}

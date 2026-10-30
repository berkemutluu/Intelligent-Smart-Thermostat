# Smart Thermostat for a Vaillant On/Off Boiler (ESP32 + Telegram)

A DIY, fail-safe smart thermostat for boilers that use a simple on/off room thermostat input (built around a **Vaillant VRT 240 Colormatic / VRT 230**). Two ESP32 boards talk over ESP-NOW. You control everything from Telegram, with no Home Assistant, cloud account or hub.

> **Status:** work in progress. The firmware has not been field-tested on a live boiler yet. Read the [Safety](#safety) section and verify every pin number against your own wiring before flashing.

## How it works

```
┌──────────────────────────┐   ESP-NOW (no router needed)   ┌───────────────────────────┐
│ Device 2: Thermostat     │ ─────────────────────────────▶ │ Device 1: Boiler controller│
│ battery powered          │   temperature, setpoint,       │ mains-adjacent, USB 5V     │
│ AHT10 + 2 OLEDs + buttons│ ◀───────────────────────────── │ 2 relays + WiFi + Telegram │
└──────────────────────────┘   heat on/off status           └──────────────┬────────────┘
                                                                           │ relays
                                                       old VRT thermostat ─┴─▶ boiler terminal 4
```

- **Device 2 (thermostat)** sits in the room. It reads the AHT10 sensor, lets you change the setpoint with two buttons, and sends readings to Device 1 every 60 seconds (and instantly when you press a button). One OLED shows the setpoint. The other shows room temperature, battery level, link status and whether the boiler is heating.
- **Device 1 (boiler controller)** sits next to the boiler. It applies the thermostat logic (with hysteresis), drives two relays, and runs a standalone Telegram bot.
- **The old thermostat stays installed** as a hardware fallback (see below).

## Fail-safe design

The boiler must never be left cold because a gadget crashed. The relay wiring means **everything defaults to the old thermostat being in control**:

| Relay | Contacts | De-energized (default / any failure) | Energized |
|---|---|---|---|
| **Relay 1** (override) | COM = wire from old VRT, NC = boiler terminal 4 | Old VRT connects straight to the boiler | Old VRT is disconnected; smart control takes over |
| **Relay 2** (heat call) | COM = boiler terminal 3, NO = boiler terminal 4 | No heat call | Heat call is closed (boiler fires) |

Device 1 hands control back to the old thermostat when:

- no data has arrived from Device 2 for 15 minutes,
- the ESP32 crashes, hangs, loses power or reboots (both relays drop out),
- no temperature reading has been received yet (for example right after a reboot),
- you send `/failsafe` on Telegram.

Set the old thermostat to a sensible background temperature (around 20-22 °C) so heating still works if the smart system fails.

Other protections on Device 1: a 30 s hardware watchdog, automatic reboot on sustained low heap (leak or fragmentation), and a daily scheduled reboot. Details are in [Maintenance](#maintenance-chat).

## Hardware

**Device 1 (boiler controller)**

| Part | Notes |
|---|---|
| ESP32-WROOM-32 DevKit | The `-32U` variant needs an external IPEX antenna |
| 2× Panasonic JQ1P-5V-F relay | 5 V coil, 1 Form C, 10 A on NO and 3 A on NC |
| 2× 2N2222 NPN transistor | Low-side relay drivers |
| 2× 1 kΩ resistor | Base resistors |
| 2× 1N4007 diode | Flyback diodes across each coil |
| 5 V ≥1 A USB charger | Powers the ESP32 and the relay coils |
| 1 A fast-blow fuse (mains side) | In series with terminal 4, so a welded contact can't hold a live connection |
| 1 A fuse (5 V side) | Protects the low-voltage side |
| Enclosure, screw terminals, fuse holder | Mains-rated, with the mains and logic sides physically separated |

**Device 2 (thermostat)**

| Part | Notes |
|---|---|
| ESP32-WROOM-32 DevKit | |
| AHT10 temperature/humidity sensor | I2C, address 0x38 |
| 2× 0.96" 128×64 two-colour OLED (SSD1306/SSD1315) | Yellow strip on top, blue below |
| 2× push buttons | Setpoint up / down |
| 2× 18650 cells (button top), parallel | Battery |
| TP4056 (USB-C) | Charging |
| MT3608 boost converter | Set to **5 V**, feeds the ESP32 VIN pin |
| 220 µF electrolytic + 100 nF ceramic | Across the 5 V rail near the ESP32 (WiFi burst smoothing) |
| 2× 10 kΩ resistor | Battery voltage divider into GPIO34 |

## Wiring notes

**Relay driver (each relay):** GPIO → 1 kΩ → 2N2222 base. Emitter → GND. Collector → coil low side. Coil high side → +5 V. 1N4007 across the coil with the **cathode (stripe) on the +5 V side** and the anode on the collector side. Power the coils from the charger's 5 V rail directly, not through the ESP32 board's 5 V pin.

**Two I2C buses on Device 2** (no address change needed on the OLEDs):

| Bus | Pins (default) | Devices |
|---|---|---|
| A (`Wire`) | SDA 21 / SCL 22 | AHT10 + setpoint OLED |
| B (`Wire1`) | SDA 16 / SCL 17 | status OLED |

**Battery sensing:** Battery+ → 10 kΩ → node → 10 kΩ → GND, with the node going to **GPIO34**. The firmware multiplies by 2 and maps 3.3 V to 4.2 V onto 0-100 %, which is a rough estimate.

**Power path on Device 2:** 18650 pack → TP4056 `OUT` → MT3608 (5 V) → ESP32 `VIN`. The 220 µF (mind the polarity) and 100 nF caps sit across VIN/GND as close to the ESP32 as possible.

## Pin map (defaults in the code)

Verify these against your build. Two relay and button pins are placeholders.

| Signal | Device | Default GPIO |
|---|---|---|
| Relay 1 driver | 1 | 21 |
| Relay 2 driver | 1 | 19 (**verify**) |
| I2C A SDA / SCL | 2 | 21 / 22 |
| I2C B SDA / SCL | 2 | 16 / 17 |
| Button + / − | 2 | 23 / 19 (**verify**) |
| Battery sense (ADC1) | 2 | 34 |

Avoid the boot-strapping pins (0, 2, 12, 15), flash pins (6-11) and the input-only pins (34-39) for outputs. GPIO5 is also briefly high during boot, so it is a poor choice for a relay driver.

## Software setup

1. **Install the Arduino IDE** with the ESP32 board package.
2. **Install libraries** (Library Manager):
   - Device 1: `UniversalTelegramBot`, `ArduinoJson` (v6)
   - Device 2: `Adafruit GFX Library`, `Adafruit SSD1306`, `Adafruit AHTX0`
3. **Get both MAC addresses.** Flash a sketch containing `Serial.println(WiFi.macAddress());` to each board and note the output.
4. **Create a Telegram bot** with [@BotFather](https://t.me/BotFather) and get your chat ID(s). Use one chat for control and, optionally, a second one for maintenance.
5. **Edit the config block** at the top of each sketch:
   - Device 1: `WIFI_SSID`, `WIFI_PASSWORD`, `BOT_TOKEN`, `CHAT_ID`, `MAINT_CHAT_ID`, `device2Mac`, relay pins.
   - Device 2: `device1Mac`, `WIFI_CHANNEL`, button and I2C pins.
6. **Match the WiFi channel.** Flash Device 1 first and read the `WiFi channel` line on the serial monitor. Put that number in Device 2's `WIFI_CHANNEL`. ESP-NOW peers must share a channel, even though Device 2 never joins your network.
7. Flash both boards and test with the relays **disconnected from the boiler** first.

## Telegram commands

**Control chat**

| Command | Action |
|---|---|
| `/status` | Room temp, setpoint, mode, heat relay, link age, failsafe state |
| `/setpoint 21.5` | Set the target (10-30 °C) |
| `/auto` | Thermostat mode |
| `/on` / `/off` | Manual heat on / off |
| `/failsafe` | Hand control back to the old thermostat immediately |

**Maintenance chat** (cannot change the heating)

| Command | Action |
|---|---|
| `/status` | Uptime, last boot reason, WiFi signal, NTP clock, link state, relay states, heap |
| `/stats` | Heap details, leak thresholds, packet/command counters, reconnects |
| `/reconnect` | Restart WiFi |
| `/reboot` then `/reboot confirm` | Restart the controller |

Only the configured chat IDs are accepted. Messages from any other chat get "Unauthorized."

## Maintenance

- **Daily reboot** at 04:00 local time (NTP, UTC+3 by default). If NTP is unavailable, it reboots after 25 h of uptime.
- **Leak protection:** the board checks free heap and the largest free block every 10 s. Six low readings in a row (about 60 s) trigger a safe reboot.
- **Before any reboot** both relays are released, so the old thermostat covers the boiler for the few seconds the board is down.
- **Boot reports:** after each restart you get a Telegram message with the reason (daily, leak, watchdog, brownout, power-on).
- Pending Telegram commands are discarded on boot, so an old `/on` can't run after a restart. Manual mode resets to AUTO after a reboot.

## Safety

This project switches the control circuit of a gas or oil boiler, and the VRT 240 may carry **230 V mains** on its terminals. Check your own boiler's manual and terminal voltage before wiring anything.

- If you are not qualified to work on mains wiring, have a qualified person do or inspect it. Isolate the boiler first.
- Use a mains-rated enclosure with proper creepage between the mains side and the ESP32 side. Don't use bare relay boards on exposed mains.
- Fit the fast-blow fuse in series with terminal 4, sized well below the boiler's own internal protection.
- Correct C1 (electrolytic) polarity matters. A reversed capacitor can overheat.
- Keep the old thermostat connected as the fallback. Don't remove it.
- Never rely on this project as the only safety mechanism. The boiler's own limit controls (overheat thermostat, flame supervision) stay in place and are not touched.

You use this at your own risk. The project is provided without warranty.

## Known limitations

- Device 2 stays awake (no deep sleep), so battery life is limited. Deep sleep with button wake-up is a possible improvement.
- ESP-NOW traffic is unencrypted, and the Telegram TLS connection skips certificate validation (`setInsecure()`).
- The battery percentage is a linear estimate of a non-linear discharge curve.
- Manual mode and Telegram setpoint changes don't survive a reboot. The setpoint is re-sent by Device 2's buttons, which are the source of truth.

## Repository layout

```
.
├── README.md
├── device1_boiler_controller/
│   └── device1_boiler_controller.ino
└── device2_thermostat/
    └── device2_thermostat.ino
```

## License

Apache 2.0

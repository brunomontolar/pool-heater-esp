# Pool Heater Pump Controller (ESP32-C6 + Zigbee)

A Zigbee end device (ESP-IDF, [esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk))
that reads two NTC thermistors via an ADS1115 ADC and switches a relay-driven water pump
based on the temperature delta between them (e.g. solar collector vs. pool water). Joins an
existing Zigbee2MQTT network as a router.

## Status

Project skeleton builds cleanly (`idf.py build`).

- [x] `ads1115_read_channel()` in [`main/ads1115.c`](main/ads1115.c) - I2C conversion sequence
- [x] `thermistor_raw_to_celsius()` in [`main/thermistor.c`](main/thermistor.c) - Beta-equation math,
      using a generic 10k NTC (B=3950) with a 10k fixed resistor to 3.3V (NTC to GND)
- [x] GPIO pins in [`main/app_config.h`](main/app_config.h) - confirmed against actual wiring
- [x] Hysteresis thresholds / override timeout in `app_config.h` - kept as generic defaults
      (5.00C on / 2.00C off / 30min override), retune once real readings are in

## Hardware

| Component            | Interface | Notes                                    |
|-----------------------|-----------|-------------------------------------------|
| ESP32-C6              | -         | Native 802.15.4 radio, no RCP needed      |
| ADS1115 ADC           | I2C       | 2 channels used, one per thermistor       |
| NTC thermistor x2     | ADS1115 AIN0/AIN1 | Voltage divider, Steinhart-Hart conversion |
| Relay -> pump         | GPIO      | Drives the circulation pump               |

Pin assignments and I2C address live in [`main/app_config.h`](main/app_config.h).

## Zigbee endpoints (HA profile)

| Endpoint | Cluster                          | Purpose                                          |
|----------|-----------------------------------|---------------------------------------------------|
| 10       | Temperature Measurement (0x0402) | Thermistor 1 (solar collector)                     |
| 11       | Temperature Measurement (0x0402) | Thermistor 2 (pool water)                          |
| 12       | On/Off (0x0006)                  | Pump relay (readable + writable)                   |
| 13       | On/Off (0x0006)                  | Automatic control enable (readable + writable)     |
| 14       | Thermostat (0x0201)              | Pool heating setpoint, `OccupiedHeatingSetpoint`   |

The device is a single Zigbee node exposing all five endpoints, joins as a **Router**
(mains-powered), and configures its own attribute reporting for all five endpoints so
Zigbee2MQTT gets pushed updates (temperatures, pump/auto-control state, setpoint) instead of
polling - see `zb_configure_all_reporting()` in `main/zb_main.c`.

Manual on/off writes from Home Assistant/Z2M to endpoint 12 are respected and suppress
automatic pump control for `APP_MANUAL_OVERRIDE_TIMEOUT_MS` (see `app_config.h`) before the
hysteresis loop resumes driving the relay.

Endpoint 13's On/Off attribute is a kill switch for the hysteresis loop itself: write `OFF` and
the automatic control loop stops driving the relay (forcing it off) regardless of the
temperature delta, until written back to `ON`. It defaults to `ON` on every boot and does not
affect manual writes to endpoint 12, which always take priority.

Endpoint 14's `OccupiedHeatingSetpoint` attribute is the maximum pool water temperature
(thermistor 2), in centidegrees C, above which the hysteresis loop won't turn the pump on to
heat the pool further - reaching it also stops the pump immediately if it was already running.
Defaults to `APP_DEFAULT_POOL_SETPOINT_CENTIDEGREES` (30.00C).

Both the endpoint 13 enable switch and the endpoint 14 setpoint are persisted to the default
`nvs` partition (namespace `APP_NVS_NAMESPACE`, see `app_config.h`) whenever changed from Home
Assistant/Z2M, and reloaded on every boot - the compiled-in defaults above only apply on a
device that has never had either attribute written. This is a separate partition from
`zb_storage` (Zigbee network state), so these settings survive the BOOT-button factory reset
described below.

## SDK version note

This targets **esp-zigbee-sdk >= 2.0.0** (pinned in [`main/idf_component.yml`](main/idf_component.yml)),
which uses the current `ezb_*` API (`ezbee/*.h` headers). That SDK line dropped the older
ZBOSS-based `esp_zb_*` API that most existing tutorials/blog posts still reference - if you're
looking things up online and the function names don't match what's in this repo, that's why.
See the comment at the top of [`main/zb_main.c`](main/zb_main.c) for details, including the
`CONFIG_ZB_SDK_1xx` compatibility-shim option if you ever want the older API surface instead.

## Building

Requires ESP-IDF >= 5.2 (developed against v6.0.2) with ESP32-C6 target support installed.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

The first build fetches `espressif/esp-zigbee-lib` via the IDF Component Manager
(`managed_components/`, gitignored - re-fetched from `dependencies.lock` on a fresh clone).

## Project layout

```
main/
├── app_config.h      # pins, endpoint IDs, hysteresis/timing constants
├── main.c            # app_main: starts the Zigbee task and control loop task
├── zb_main.c/.h       # Zigbee stack, 5-endpoint device, commissioning, reporting
├── i2c_bus.c/.h        # I2C master bus init
├── ads1115.c/.h        # ADS1115 driver
├── thermistor.c/.h     # Beta-equation conversion
├── relay.c/.h          # GPIO relay driver
└── pump_control.c/.h   # Control loop: read -> report -> hysteresis -> drive relay
```

## Pairing, re-pairing, and factory reset

Board is a **Seeed Studio XIAO ESP32C6**. The BOOT button (GPIO9, `APP_BOOT_BUTTON_GPIO` in
[`main/app_config.h`](main/app_config.h)) and onboard user LED (GPIO15, active-low,
`APP_LED_GPIO`) drive pairing, matching how most commercial Zigbee end devices behave - the
device does **not** try to join on its own unless told to:

- **Factory-new** (including a fresh device, or right after a factory reset): sits idle, does
  not attempt to join. Hold **BOOT** for `APP_BUTTON_HOLD_MS` (5s by default) to open a pairing
  window (`APP_PAIRING_WINDOW_MS`, 2 minutes by default) - the onboard LED blinks while it's
  open. Put your Zigbee2MQTT coordinator into pairing mode (`permit_join`) before or during the
  hold. If the window closes without joining, the LED stops and another 5s hold is needed to
  retry. Pump control keeps running normally throughout, with whatever auto-control/setpoint
  values are currently loaded (defaults, on a truly fresh device) - it never depends on Zigbee
  join state.
- **Already paired**: reconnects automatically in the background on every boot (retried
  indefinitely, no LED, no time limit - this is the normal "recover after a power outage" path).
  Holding **BOOT** for 5s here instead leaves the network and erases the stored credentials
  (factory reset) - the device reboots into the factory-new state above, so a *second* 5s hold
  is needed afterwards to actually start a new pairing window.

Network credentials persist across normal reboots (they live in the `zb_storage` NVS partition),
so a power cycle alone does not require re-pairing.

## Zigbee2MQTT external converter

Without a converter, Z2M shows this device as `Unsupported`. Install
[`zigbee2mqtt/poolctrl_temppump.js`](zigbee2mqtt/poolctrl_temppump.js): copy it into Z2M's
`external_converters` directory (or point `external_converters: [poolctrl_temppump.js]` at it in
`configuration.yaml`), then restart Zigbee2MQTT. Requires Z2M >= 1.35 (ESM-style external
converters).

It maps the five endpoints above to named entities instead of generic `l1`..`l5` endpoints:

| Endpoint name | EP | Exposed as                                    |
|---------------|----|------------------------------------------------|
| `collector`   | 10 | `temperature_collector`                        |
| `pool`        | 11 | `temperature_pool`                             |
| `pump`        | 12 | `state_pump`                                   |
| `auto_control`| 13 | `state_auto_control`                           |
| `setpoint`    | 14 | `occupied_heating_setpoint`                    |

Its `configure()` step binds all five endpoints and negotiates reporting over the air, on top of
the reporting the firmware already configures for itself at boot
(`zb_configure_all_reporting()` in `main/zb_main.c`, covers all five endpoints as of this
revision) - so Z2M gets pushed updates when the pump relay or auto-control switch changes state
on its own (e.g. the hysteresis loop turning the pump on/off, or the manual-override timeout
elapsing), not just in response to a write from Z2M/Home Assistant.

Whether the On/Off (ep 12/13) and Thermostat (ep 14) reporting slots are actually available
depends on the ZHA device-type macros pre-allocating them the same way they do for Temperature
Measurement - unconfirmed for `OccupiedHeatingSetpoint` specifically, since it's added to the
Thermostat cluster descriptor after creation as an optional attribute (see `zb_create_device` in
`main/zb_main.c`). If a reporting slot is missing, `zb_configure_reporting()` just logs a
`No reporting slot found for endpoint ... ` warning at boot and that attribute falls back to
read/write-only (no push reports, still fully controllable) - check the boot log after
flashing.

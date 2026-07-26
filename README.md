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
- [ ] GPIO pins in [`main/app_config.h`](main/app_config.h) are placeholders - confirm against
      your actual wiring
- [ ] Hysteresis thresholds / override timeout in `app_config.h` are placeholders - tune for
      your system

## Hardware

| Component            | Interface | Notes                                    |
|-----------------------|-----------|-------------------------------------------|
| ESP32-C6              | -         | Native 802.15.4 radio, no RCP needed      |
| ADS1115 ADC           | I2C       | 2 channels used, one per thermistor       |
| NTC thermistor x2     | ADS1115 AIN0/AIN1 | Voltage divider, Steinhart-Hart conversion |
| Relay -> pump         | GPIO      | Drives the circulation pump               |

Pin assignments and I2C address live in [`main/app_config.h`](main/app_config.h).

## Zigbee endpoints (HA profile)

| Endpoint | Cluster                          | Purpose                          |
|----------|-----------------------------------|-----------------------------------|
| 10       | Temperature Measurement (0x0402) | Thermistor 1                      |
| 11       | Temperature Measurement (0x0402) | Thermistor 2                      |
| 12       | On/Off (0x0006)                  | Pump relay (readable + writable)  |

The device is a single Zigbee node exposing all three endpoints, joins as a **Router**
(mains-powered), and configures its own attribute reporting for endpoints 10/11 so
Zigbee2MQTT gets pushed temperature updates instead of polling.

Manual on/off writes from Home Assistant/Z2M are respected and suppress automatic pump
control for `APP_MANUAL_OVERRIDE_TIMEOUT_MS` (see `app_config.h`) before the hysteresis loop
resumes driving the relay.

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
├── app_config.h      # pins, endpoint IDs, hysteresis/timing constants (TODO: tune)
├── main.c            # app_main: starts the Zigbee task and control loop task
├── zb_main.c/.h       # Zigbee stack, 3-endpoint device, commissioning, reporting
├── i2c_bus.c/.h        # I2C master bus init
├── ads1115.c/.h        # ADS1115 driver (TODO: read sequence)
├── thermistor.c/.h     # Steinhart-Hart conversion (TODO: math)
├── relay.c/.h          # GPIO relay driver
└── pump_control.c/.h   # Control loop: read -> report -> hysteresis -> drive relay
```

## Flashing onto a fresh device

The device joins via Zigbee network steering automatically on first boot (factory-new state).
Put your Zigbee2MQTT coordinator into pairing mode (`permit_join`) before powering on the
board for the first time, or after a factory reset.

/*
 * Zigbee stack bring-up: one 5-endpoint HA device (two Temperature
 * Measurement sensors, one On/Off-controlled mains relay, an On/Off
 * auto-control kill switch, and a Thermostat-cluster heating setpoint)
 * joining an existing Zigbee2MQTT (SLZB-06) network as a Router.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the Zigbee stack task. Call once from app_main(). */
void zb_main_start(void);

/* Push a new temperature reading (centidegrees C, e.g. 2345 = 23.45C) to the
 * Temperature Measurement cluster on the given endpoint (APP_EP_THERMISTOR_1
 * or APP_EP_THERMISTOR_2). Safe to call from any task. */
void zb_main_report_temperature(uint8_t endpoint_id, int16_t centidegrees);

/* Drive the pump relay's On/Off attribute from the automatic control loop.
 * Safe to call from any task. Do not call this to reflect a manually
 * overridden state - that path is handled internally when Home
 * Assistant/Z2M writes the attribute directly. */
void zb_main_set_pump_state(bool on);

/* True if a manual on/off write from the network is still within its
 * override window (see APP_MANUAL_OVERRIDE_TIMEOUT_MS in app_config.h).
 * Automatically clears itself once the timeout elapses. The automatic
 * control loop should skip driving the relay while this returns true. */
bool zb_main_is_manual_override_active(void);

/* True if the automatic hysteresis loop is allowed to drive the pump relay
 * (APP_EP_AUTO_ENABLE's On/Off attribute). Defaults to true on every boot;
 * toggled false from Home Assistant/Z2M pauses automatic control entirely -
 * the hysteresis loop should force the relay off and skip its decision logic
 * while this returns false. Does not affect manual on/off writes to the pump
 * relay itself (APP_EP_PUMP_RELAY / zb_main_is_manual_override_active()). */
bool zb_main_is_auto_control_enabled(void);

/* Current pool heating setpoint (centidegrees C, APP_EP_POOL_SETPOINT's
 * OccupiedHeatingSetpoint attribute). Defaults to
 * APP_DEFAULT_POOL_SETPOINT_CENTIDEGREES on every boot; changeable at runtime
 * from Home Assistant/Z2M. RAM only - not persisted across reboots. */
int16_t zb_main_get_pool_setpoint_centidegrees(void);

#ifdef __cplusplus
}
#endif

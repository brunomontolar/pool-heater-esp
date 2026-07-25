/*
 * Control loop: reads both thermistors every APP_CONTROL_LOOP_INTERVAL_MS,
 * reports temperatures to Zigbee, and drives the pump relay from the
 * temperature delta with hysteresis (unless a manual override from Home
 * Assistant/Z2M is in effect).
 */
#pragma once

/* Initializes the I2C bus, ADS1115, and relay, then starts the control loop
 * task. Call once from app_main(), after zb_main_start(). */
void pump_control_start(void);

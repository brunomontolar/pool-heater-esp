/*
 * Zigbee2MQTT external converter for this device.
 *
 * Matches on the Basic cluster's ManufacturerName/ModelIdentifier strings set
 * in main/app_config.h (APP_ZB_MANUFACTURER_NAME / APP_ZB_MODEL_IDENTIFIER):
 * manufacturer "PoolCtrl", model "TempPump". Endpoint IDs below mirror the
 * APP_EP_* constants in the same file - keep both in sync if you renumber
 * endpoints in firmware.
 *
 * Install: copy this file into Z2M's `external_converters` directory (or
 * point `external_converters: [poolctrl_temppump.js]` at it in
 * configuration.yaml) and restart Zigbee2MQTT.
 *
 * CommonJS require(), not ESM import: an earlier version of this file used
 * `import ... from 'zigbee-herdsman-converters/lib/fromZigbee'`, which failed
 * with "Cannot find module .../external_converters/node_modules/zigbee-herdsman-converters/dist/lib/fromZigbee.js".
 * Two separate problems, both fixed below:
 *   1. fromZigbee/toZigbee actually live under the package's `converters/`
 *      subpath, not `lib/` (only exposes/reporting/tuya/modernExtend are
 *      under `lib/` - confirmed against the zigbee-herdsman-converters
 *      source tree, which has no package.json "exports" map restricting
 *      subpaths, so it resolves by real file path).
 *   2. require()/module.exports matches how this Z2M install's other
 *      external converters are written and known to resolve correctly;
 *      ESM import resolution behaved differently here (e.g. Home Assistant
 *      add-on container layouts commonly hit this).
 */
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

const e = exposes.presets;
const ea = exposes.access;

/* Matches APP_EP_* in main/app_config.h. */
const EP_COLLECTOR = 10; /* APP_EP_THERMISTOR_1 - solar collector temperature */
const EP_POOL = 11;      /* APP_EP_THERMISTOR_2 - pool water temperature */
const EP_PUMP = 12;      /* APP_EP_PUMP_RELAY */
const EP_AUTO_CONTROL = 13; /* APP_EP_AUTO_ENABLE */
const EP_SETPOINT = 14;  /* APP_EP_POOL_SETPOINT */

const definition = {
    zigbeeModel: ['TempPump'],
    model: 'TempPump',
    vendor: 'PoolCtrl',
    description: 'Solar pool heater pump controller (collector/pool thermistors, pump relay, auto-control switch, heating setpoint)',
    /* fz.thermostat (not a nonexistent fz.thermostat_occupied_heating_setpoint) is
     * the real, generic hvacThermostat fromZigbee converter - it maps the
     * cluster's occupiedHeatingSetpoint field to the `occupied_heating_setpoint`
     * property (endpoint-postfixed same as the expose below), among other
     * thermostat attributes we don't use here. */
    fromZigbee: [fz.temperature, fz.on_off, fz.thermostat],
    toZigbee: [tz.on_off, tz.thermostat_occupied_heating_setpoint],
    exposes: [
        e.temperature().withEndpoint('collector').withDescription('Solar collector temperature'),
        e.temperature().withEndpoint('pool').withDescription('Pool water temperature'),
        e.switch().withEndpoint('pump')
            .withDescription('Pump relay. Manual writes are respected, suppressing automatic control ' +
                'for 30 minutes before the hysteresis loop resumes driving it.'),
        e.switch().withEndpoint('auto_control')
            .withDescription('Enable/disable the automatic hysteresis control loop. Off forces the pump ' +
                'relay off regardless of temperature delta; does not affect manual writes to the pump switch.'),
        e.numeric('occupied_heating_setpoint', ea.ALL).withEndpoint('setpoint')
            .withUnit('°C').withValueMin(0).withValueMax(50).withValueStep(0.1)
            .withDescription('Pool heating setpoint. The pump will not turn on - and stops immediately if ' +
                'already running - once the pool sensor reaches this temperature.'),
    ],
    endpoint: (device) => {
        return {collector: EP_COLLECTOR, pool: EP_POOL, pump: EP_PUMP, auto_control: EP_AUTO_CONTROL, setpoint: EP_SETPOINT};
    },
    meta: {multiEndpoint: true},
    configure: async (device, coordinatorEndpoint, logger) => {
        const collectorEp = device.getEndpoint(EP_COLLECTOR);
        await reporting.bind(collectorEp, coordinatorEndpoint, ['msTemperatureMeasurement']);
        await reporting.temperature(collectorEp);

        const poolEp = device.getEndpoint(EP_POOL);
        await reporting.bind(poolEp, coordinatorEndpoint, ['msTemperatureMeasurement']);
        await reporting.temperature(poolEp);

        const pumpEp = device.getEndpoint(EP_PUMP);
        await reporting.bind(pumpEp, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(pumpEp);

        const autoControlEp = device.getEndpoint(EP_AUTO_CONTROL);
        await reporting.bind(autoControlEp, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(autoControlEp);

        const setpointEp = device.getEndpoint(EP_SETPOINT);
        await reporting.bind(setpointEp, coordinatorEndpoint, ['hvacThermostat']);
        await reporting.thermostatOccupiedHeatingSetpoint(setpointEp);
    },
};

module.exports = [definition];

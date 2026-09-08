import {presets, access} from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';

function parseU48(value) {
    if (Array.isArray(value)) {
        const low = Number(value[0]) >>> 0;
        const high = Number(value[1]) >>> 0;
        return low + high * 4294967296;
    }
    return Number(value);
}

const fzLocal = {
    water_metering: {
        cluster: 'seMetering',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            const factor = 0.001; // 1 / 1000

            if (msg.data.currentSummDelivered !== undefined) {
                const rawLiters = parseU48(msg.data.currentSummDelivered);
                result.water_consumed = Number((rawLiters * factor).toFixed(3)); // m³
                result.water_consumed_liters = rawLiters;                        // L
            }
            if (msg.data.hoursInOperation !== undefined) {
                result.hours_in_operation = msg.data.hoursInOperation;
            }
            return result;
        },
    },
};

export default {
    zigbeeModel: ['WaterMeter'],
    model: 'WaterMeter',
    vendor: 'TavaresLAB',
    description: 'WaterMeter criado em ESP32C6 com detecção por sensor CNY70',
    fromZigbee: [fzLocal.water_metering],
    toZigbee: [],
    exposes: [
        presets.numeric('water_consumed', access.STATE)
            .withUnit('m³')
            .withDescription('Consumo total'),
        presets.numeric('water_consumed_liters', access.STATE)
            .withUnit('L')
            .withDescription('Consumo total (L)'),
        presets.numeric('hours_in_operation', access.STATE)
            .withUnit('h')
            .withDescription('Horas em operação'),
    ],

    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        console.log('Iniciando configure');
        if (!endpoint) return;
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering']);
        try {
            await reporting.readMeteringMultiplierDivisor(endpoint);
        } catch (e) {
        console.log('Bind do coordenador falhou: ', e.message);
        }

        try {
            await endpoint.read('seMetering', ['currentSummDelivered','hoursInOperation']);
        } catch (e) {
        console.log('Read do atributo falhou: ', e.message);
        }
    },
};
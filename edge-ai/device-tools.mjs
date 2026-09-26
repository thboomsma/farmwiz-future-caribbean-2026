import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const CONFIG_DIR = process.env.CONFIG_DIR || '/etc/farmwiz-orchestrator';
const CONFIG_DEVICES_FILE = `${CONFIG_DIR}/devices.json`;
const WORKSPACE_DEVICES_FILE = new URL('../../devices.json', import.meta.url);
const DEVICE_STORAGE_FILE = process.env.DEVICES_FILE || CONFIG_DEVICES_FILE;
const DEVICES_FILE = process.env.DEVICES_FILE
  || (existsSync(CONFIG_DEVICES_FILE) ? CONFIG_DEVICES_FILE
    : existsSync(WORKSPACE_DEVICES_FILE) ? WORKSPACE_DEVICES_FILE : CONFIG_DEVICES_FILE);

// These are intent-to-capability links, not transport adapters. They describe
// which declared device capability could satisfy a normalized FarmWiz call.
const INTENT_TOOLS = Object.freeze({
  get_hub_status: [{ kind: 'http', name: 'read_data' }],
  get_sensor_state: [{ kind: 'http', name: 'read_data' }],
  list_nodes: [{ kind: 'http', name: 'read_remote_nodes' }],
  list_schedules: [{ kind: 'http', name: 'read_cron_profiles' }],
  set_relay: [
    { kind: 'http', name: 'toggle_relay' },
    { kind: 'mqtt', name: 'setRelay' },
  ],
  set_schedule: [{ kind: 'http', name: 'set_cron_profile' }],
});

const HTTP_TOOL_NAMES = Object.freeze({
  '/list': 'list_files',
  '/': 'read_root',
  '/data': 'read_data',
  '/capabilities': 'read_capabilities',
  '/api/nodes': 'read_remote_nodes',
  '/toggle': 'toggle_relay',
  '/setSchedule': 'legacy_set_schedule',
  '/api/schedules': 'set_cron_profile',
  '/getSchedules': 'read_cron_profiles',
  '/deleteSchedule': 'delete_legacy_schedule',
  '/forceCheck': 'force_schedule_check',
  '/syncTime': 'sync_time',
  '/reboot': 'reboot',
  '/ota_status': 'read_ota_status',
});

function isCapabilityManifest(value) {
  return value?.capabilityType === 'device-capabilities';
}

function definitionFromCapabilityManifest(manifest) {
  if (String(manifest.schemaVersion) !== '1.1' || !manifest.device || typeof manifest.device !== 'object') {
    throw new Error('Unsupported device capabilities manifest; expected schemaVersion 1.1 and a device object');
  }
  const deviceId = manifest.device.deviceId;
  const mqttMethods = manifest.mqtt?.rpcMethods;
  const httpEndpoints = manifest.httpEndpoints;
  if (!Array.isArray(mqttMethods) || !Array.isArray(httpEndpoints)) {
    throw new Error(`Device ${deviceId || '(unknown)'} must define MQTT methods and HTTP endpoints`);
  }
  for (const method of mqttMethods) {
    if (!method || typeof method.name !== 'string' || !method.params || typeof method.params !== 'object' || Array.isArray(method.params)) {
      throw new Error(`Device ${deviceId || '(unknown)'} has an invalid MQTT method definition`);
    }
  }
  for (const endpoint of httpEndpoints) {
    if (!endpoint || typeof endpoint.path !== 'string' || typeof endpoint.method !== 'string') {
      throw new Error(`Device ${deviceId || '(unknown)'} has an invalid HTTP endpoint definition`);
    }
  }
  const setRelay = mqttMethods.find((method) => method.name === 'setRelay');
  const relayOutputs = setRelay?.params?.relay === 'integer 1-4'
    ? [1, 2, 3, 4].map((relay) => ({ relay, name: `Relay ${relay}` }))
    : [];
  const rpcTopic = manifest.mqtt?.topics?.rpcRequest;
  return {
    deviceId,
    nodeName: manifest.device.role || deviceId,
    hubId: deviceId,
    capabilities: {
      capabilityType: manifest.capabilityType,
      schemaVersion: manifest.schemaVersion,
      generatedBy: manifest.generatedBy,
      generatedFrom: manifest.generatedFrom,
      generatedAt: manifest.generatedAt,
      firmware: manifest.device.firmwareVersion,
      role: manifest.device.role,
      mqttRoot: manifest.mqtt?.root,
      mqttClientId: manifest.mqtt?.clientId,
      mqttUsernameRequired: manifest.mqtt?.usernameRequired,
      mqttRpcTopic: typeof rpcTopic === 'string' ? rpcTopic : undefined,
      mqttTelemetryTopic: manifest.mqtt?.topics?.telemetry,
      mqttRpcResponseTopic: manifest.mqtt?.topics?.rpcResponse,
      mqttTools: mqttMethods.map((method) => method.name),
      mqttRpcMethods: mqttMethods,
      httpTools: httpEndpoints.map((endpoint) => ({
        name: HTTP_TOOL_NAMES[endpoint.path] || `http_${endpoint.method.toLowerCase()}_${endpoint.path.replace(/[^A-Za-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'root'}`,
        method: endpoint.method,
        path: endpoint.path,
      })),
      relayOutputs,
      runtimeDiscovery: manifest.runtimeDiscovery,
      sourceSignals: manifest.sourceSignals,
    },
  };
}

function normalizeDeviceDefinitions(value) {
  const toolCatalog = Array.isArray(value) ? value : Array.isArray(value?.tools) ? value.tools : null;
  if (toolCatalog?.length && toolCatalog.every((tool) => typeof tool?.name === 'string' && tool?.parameters && !tool?.deviceId)) {
    throw new Error('This is a Needle3 tool catalog, not a device list. Upload a device-capabilities manifest or deviceId/capabilities definitions.');
  }
  const rawInput = Array.isArray(value) ? value : Array.isArray(value?.devices) ? value.devices : [value];
  const input = rawInput.map((device) => isCapabilityManifest(device) ? definitionFromCapabilityManifest(device) : device);
  if (input.length > 100) throw new Error('Provide no more than 100 device definitions');
  const ids = new Set();
  return input.map((device) => {
    if (!device || typeof device.deviceId !== 'string' || !/^[A-Za-z0-9_.:-]{1,128}$/.test(device.deviceId)) {
      throw new Error('Each device must define a valid deviceId');
    }
    if (ids.has(device.deviceId)) throw new Error(`Duplicate deviceId: ${device.deviceId}`);
    ids.add(device.deviceId);
    if (!device.capabilities || typeof device.capabilities !== 'object' || Array.isArray(device.capabilities)) {
      throw new Error(`Device ${device.deviceId} must define a capabilities object`);
    }
    const capabilities = device.capabilities;
    if (capabilities.httpTools !== undefined && (!Array.isArray(capabilities.httpTools)
      || capabilities.httpTools.some((tool) => !tool || typeof tool.name !== 'string'
        || typeof tool.method !== 'string' || typeof tool.path !== 'string'))) {
      throw new Error(`Device ${device.deviceId} has invalid httpTools`);
    }
    if (capabilities.mqttTools !== undefined && (!Array.isArray(capabilities.mqttTools)
      || capabilities.mqttTools.some((tool) => typeof tool !== 'string'))) {
      throw new Error(`Device ${device.deviceId} has invalid mqttTools`);
    }
    if (capabilities.relayOutputs !== undefined && (!Array.isArray(capabilities.relayOutputs)
      || capabilities.relayOutputs.some((relay) => !Number.isInteger(relay?.relay) || typeof relay.name !== 'string'))) {
      throw new Error(`Device ${device.deviceId} has invalid relayOutputs`);
    }
    return {
      deviceId: device.deviceId,
      capabilities: device.capabilities,
      ...(typeof device.nodeName === 'string' ? { nodeName: device.nodeName.slice(0, 128) } : {}),
      ...(typeof device.hubId === 'string' ? { hubId: device.hubId.slice(0, 128) } : {}),
      ...(Array.isArray(device.aliases) ? { aliases: device.aliases.filter((item) => typeof item === 'string').slice(0, 32) } : {}),
      ...(Array.isArray(device.groups) ? { groups: device.groups.filter((item) => typeof item === 'string').slice(0, 32) } : {}),
    };
  });
}

let devices = existsSync(DEVICES_FILE)
  ? normalizeDeviceDefinitions(JSON.parse(readFileSync(DEVICES_FILE, 'utf8')))
  : [];

export function validateDeviceDefinitions(value) {
  const definitions = normalizeDeviceDefinitions(value);
  if (Buffer.byteLength(JSON.stringify(definitions)) > 64 * 1024) {
    throw new Error('Device definitions exceed the 64 KB limit');
  }
  return definitions;
}

export function replaceDeviceDefinitions(value) {
  devices = validateDeviceDefinitions(value);
  return devices;
}

export function currentDeviceDefinitions() {
  return JSON.parse(JSON.stringify(devices));
}

export function deviceDefinitionsForRegistry() {
  return devices.map((device) => {
    const capabilities = { ...device.capabilities };
    if (!Array.isArray(capabilities.relays) && Array.isArray(capabilities.relayOutputs)) {
      capabilities.relays = capabilities.relayOutputs.map((output) => ({ id: output.relay, name: output.name }));
    }
    return {
      node_uid: device.deviceId,
      node_name: device.nodeName || device.deviceId,
      hub_id: device.hubId || device.deviceId,
      aliases: device.aliases || [],
      groups: device.groups || [],
      firmware_version: typeof capabilities.firmware === 'string' ? capabilities.firmware : null,
      capabilities,
    };
  });
}

export function deviceLibraryPath() {
  return DEVICE_STORAGE_FILE;
}

function declaredTool(device, candidate) {
  if (candidate.kind === 'http') {
    const tool = device.capabilities.httpTools?.find((item) => item?.name === candidate.name);
    return tool ? { kind: 'http', ...tool } : null;
  }
  const name = device.capabilities.mqttTools?.find((item) => item === candidate.name);
  return name ? { kind: 'mqtt', name, topic: device.capabilities.mqttRpcTopic || null } : null;
}

export function matchDefinedDeviceTools(call) {
  const candidates = INTENT_TOOLS[call?.name];
  if (!candidates) return [];
  return devices.flatMap((device) => {
    if (call.arguments?.hub && call.arguments.hub !== device.deviceId) return [];
    const relayOutput = ['set_relay', 'set_schedule'].includes(call.name)
      ? device.capabilities.relayOutputs?.find((item) => Number(item?.relay) === Number(call.arguments?.relay))
      : null;
    if (['set_relay', 'set_schedule'].includes(call.name) && !relayOutput) return [];
    return candidates.flatMap((candidate) => {
      const tool = declaredTool(device, candidate);
      if (!tool) return [];
      const toggleOnly = call.name === 'set_relay' && tool.kind === 'http' && tool.name === 'toggle_relay';
      const scheduleNeedsTranslation = call.name === 'set_schedule' && tool.name === 'set_cron_profile';
      const mqttNeedsTranslation = tool.kind === 'mqtt';
      return [{
        device_id: device.deviceId,
        tool,
        ...(relayOutput ? { relay_output: relayOutput } : {}),
        match_status: toggleOnly || scheduleNeedsTranslation || mqttNeedsTranslation ? 'matched_needs_review' : 'matched',
        executable: false,
        ...(toggleOnly ? { limitation: 'Declared endpoint toggles state; it cannot guarantee the requested on/off state.' } : {}),
        ...(scheduleNeedsTranslation ? { limitation: 'The normalized schedule arguments need an explicit mapping to this device schema.' } : {}),
        ...(mqttNeedsTranslation ? { limitation: 'The MQTT function has no argument schema in devices.json; payload mapping is undefined.' } : {}),
      }];
    });
  });
}

export function deviceToolLibraryStatus() {
  return {
    configured: devices.length > 0,
    device_count: devices.length,
    effects_executed: false,
  };
}

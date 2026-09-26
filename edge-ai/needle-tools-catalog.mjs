import { createHash } from 'node:crypto';

const DEVICE_TOOL_MAP = {
  get_hub_status: (device) => hasHttp(device, 'read_data'),
  get_sensor_state: (device) => hasHttp(device, 'read_data'),
  list_schedules: (device) => hasHttp(device, 'read_cron_profiles'),
  set_relay: (device) => (hasHttp(device, 'toggle_relay') || hasMqtt(device, 'setRelay'))
    && device.capabilities?.relayOutputs?.length > 0,
  set_schedule: (device) => hasHttp(device, 'set_cron_profile')
    && device.capabilities?.relayOutputs?.length > 0,
};

function hasHttp(device, name) {
  return device.capabilities?.httpTools?.some((tool) => tool?.name === name) || false;
}

function hasMqtt(device, name) {
  return device.capabilities?.mqttTools?.includes(name) || false;
}

function assertCatalog(tools) {
  if (!Array.isArray(tools) || tools.length < 1 || tools.length > 300) {
    throw new Error('Needle tools must be a list containing 1 to 300 tools');
  }
  const names = new Set();
  for (const tool of tools) {
    if (!tool || typeof tool.name !== 'string' || !/^[A-Za-z][A-Za-z0-9_]{0,63}$/.test(tool.name)) {
      throw new Error('Each Needle tool must have a valid function name');
    }
    if (names.has(tool.name)) throw new Error(`Duplicate Needle tool name: ${tool.name}`);
    names.add(tool.name);
    if (typeof tool.description !== 'string' || !tool.description.trim() || tool.description.length > 4096) {
      throw new Error(`Needle tool ${tool.name} has an invalid description`);
    }
    const parameters = tool.parameters;
    if (!parameters || parameters.type !== 'object' || !parameters.properties || typeof parameters.properties !== 'object') {
      throw new Error(`Needle tool ${tool.name} has invalid parameters`);
    }
    if (parameters.required !== undefined && (!Array.isArray(parameters.required)
      || parameters.required.some((name) => typeof name !== 'string' || !(name in parameters.properties)))) {
      throw new Error(`Needle tool ${tool.name} has invalid required fields`);
    }
    for (const [key, property] of Object.entries(parameters.properties)) {
      if (!property || typeof property.type !== 'string') throw new Error(`Needle tool ${tool.name}.${key} has no type`);
      if (property.enum !== undefined && (!Array.isArray(property.enum) || property.enum.length === 0)) {
        throw new Error(`Needle tool ${tool.name}.${key} has an empty enum`);
      }
    }
  }
  if (Buffer.byteLength(JSON.stringify(tools)) > 256 * 1024) throw new Error('Needle tool catalog exceeds 256 KB');
  return tools;
}

function matchingDevices(toolName, devices) {
  const matches = DEVICE_TOOL_MAP[toolName];
  return matches ? devices.filter(matches) : [];
}

function setAllowedValues(tool, propertyName, values) {
  const property = tool.parameters.properties[propertyName];
  if (!property) return;
  const unique = [...new Set(values)].sort((a, b) => String(a).localeCompare(String(b)));
  if (unique.length) property.enum = unique;
  else delete property.enum;
}

function deviceHint(ids) {
  if (!ids.length) return 'No matching imported device is currently available.';
  const shown = ids.slice(0, 20);
  const extra = ids.length > shown.length ? ` and ${ids.length - shown.length} more` : '';
  return `Currently matching imported deviceId values: ${shown.join(', ')}${extra}. Use one of these exact IDs.`;
}

export function buildNeedleToolCatalog(baseTools, definitions) {
  const tools = JSON.parse(JSON.stringify(baseTools));
  if (!Array.isArray(definitions)) throw new Error('Device definitions must be a list');

  for (const tool of tools) {
    const matches = matchingDevices(tool.name, definitions);
    const ids = matches.map((device) => device.deviceId);
    if (tool.name === 'get_sensor_state') setAllowedValues(tool, 'node', ids);
    if (['get_hub_status', 'list_schedules', 'set_relay', 'set_schedule'].includes(tool.name)) {
      setAllowedValues(tool, 'hub', ids);
    }
    if (['set_relay', 'set_schedule'].includes(tool.name)) {
      const relays = matches.flatMap((device) => device.capabilities.relayOutputs
        .map((output) => output.relay)
        .filter((relay) => Number.isInteger(relay) && relay >= 1 && relay <= 4));
      setAllowedValues(tool, 'relay', relays);
    }
    tool.description = `${tool.description.trim()} ${deviceHint(ids)}`;
  }

  assertCatalog(tools);
  const serialized = `${JSON.stringify(tools, null, 2)}\n`;
  return {
    tools,
    serialized,
    sha256: createHash('sha256').update(serialized).digest('hex'),
    tool_count: tools.length,
    byte_count: Buffer.byteLength(serialized),
  };
}

export function validateNeedleToolCatalog(tools) {
  return assertCatalog(tools);
}

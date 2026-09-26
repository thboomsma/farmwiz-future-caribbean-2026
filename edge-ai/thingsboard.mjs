import { existsSync, readFileSync } from 'node:fs';

const CONFIG_DIR = process.env.CONFIG_DIR || '/etc/farmwiz-orchestrator';
const CONFIG_PATH = `${CONFIG_DIR}/thingsboard.json`;
const SECRETS_PATH = `${CONFIG_DIR}/thingsboard-secrets.json`;

function readJson(path) {
  return existsSync(path) ? JSON.parse(readFileSync(path, 'utf8')) : null;
}

function headers(secret, extra = {}) {
  const auth = secret.auth_mode === 'api_key' ? `ApiKey ${secret.api_key}` : `Bearer ${secret.token}`;
  return { accept: 'application/json', 'content-type': 'application/json', 'X-Authorization': auth, ...extra };
}

async function tbRequest(config, secret, path, options = {}) {
  const response = await fetch(`${config.base_url}${path}`, { ...options, headers: headers(secret, options.headers) });
  if (!response.ok) throw new Error(`ThingsBoard ${path} returned HTTP ${response.status}`);
  return response.json();
}

async function authenticate(config, secret) {
  if (secret.auth_mode === 'api_key') return { ...secret };
  const response = await fetch(`${config.base_url}/api/auth/login`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: secret.username, password: secret.password }),
  });
  if (!response.ok) throw new Error(`ThingsBoard login returned HTTP ${response.status}`);
  const login = await response.json();
  return { ...secret, token: login.token };
}

function attributesToMap(attributes) {
  return Object.fromEntries((Array.isArray(attributes) ? attributes : []).map((item) => [item.key, item.value]));
}

function jsonAttribute(value, fallback) {
  if (typeof value !== 'string') return value ?? fallback;
  try { return JSON.parse(value); } catch { return fallback; }
}

export async function syncThingsBoard(registry) {
  const config = readJson(CONFIG_PATH);
  const secret = readJson(SECRETS_PATH);
  if (!config || !secret) throw new Error('ThingsBoard is not configured');
  if (!config.base_url.startsWith('https://')) throw new Error('ThingsBoard HTTPS is required');
  const authenticated = await authenticate(config, secret);
  const page = await tbRequest(config, authenticated, '/api/tenant/deviceInfos?pageSize=500&page=0');
  const devices = Array.isArray(page) ? page : (Array.isArray(page.data) ? page.data : []);
  let imported = 0;
  for (const device of devices) {
    const entityId = device.id?.id || device.id;
    if (!entityId) continue;
    const attributes = attributesToMap(await tbRequest(config, authenticated, `/api/plugins/telemetry/DEVICE/${encodeURIComponent(entityId)}/values/attributes?keys=farmwiz_node_uid,farmwiz_aliases,farmwiz_groups,farmwiz_hub_id,farmwiz_capabilities,serialNumber,macAddress,firmwareVersion`));
    registry.ingest({
      node_uid: attributes.farmwiz_node_uid || attributes.serialNumber || `tb:${entityId}`,
      tb_entity_id: entityId,
      node_name: device.name || `tb-${entityId}`,
      aliases: jsonAttribute(attributes.farmwiz_aliases, []),
      groups: jsonAttribute(attributes.farmwiz_groups, []),
      hub_id: attributes.farmwiz_hub_id || 'unassigned',
      mac_address: attributes.macAddress || null,
      firmware_version: attributes.firmwareVersion || null,
      capabilities: jsonAttribute(attributes.farmwiz_capabilities, {}),
    }, 'thingsboard');
    imported += 1;
  }
  return { ok: true, imported, registry_revision: registry.revision(), base_url: config.base_url };
}

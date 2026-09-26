import http from 'node:http';
import https from 'node:https';
import { chmodSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { currentDeviceDefinitions, deviceDefinitionsForRegistry, deviceLibraryPath, deviceToolLibraryStatus, matchDefinedDeviceTools, replaceDeviceDefinitions, validateDeviceDefinitions } from './device-tools.mjs';
import { buildNeedleToolCatalog } from './needle-tools-catalog.mjs';
import { startDiscovery } from './discovery.mjs';
import { registry } from './registry.mjs';
import { syncThingsBoard } from './thingsboard.mjs';
import { startMqttInventory } from './mqtt-inventory.mjs';

const PORT = Number(process.env.PORT || 7100);
const HTTPS_ENABLED = process.env.HTTPS_ENABLED === 'true';
const HTTPS_PORT = Number(process.env.HTTPS_PORT || 443);
const BIND_HOST = process.env.BIND_HOST || '127.0.0.1';
const NEEDLE_URL = process.env.NEEDLE_URL || 'http://127.0.0.1:7101';
const CONFIG_DIR = process.env.CONFIG_DIR || '/etc/farmwiz-orchestrator';
const TLS_CERT_FILE = process.env.TLS_CERT_FILE || `${CONFIG_DIR}/tls/server.crt`;
const TLS_KEY_FILE = process.env.TLS_KEY_FILE || `${CONFIG_DIR}/tls/server.key`;
const DEFAULT_TB_BASE_URL = process.env.DEFAULT_TB_BASE_URL || 'https://dash.farmwiz.net';
const TRUST_PROXY = process.env.TRUST_PROXY === 'true';
const TB_CONFIG_PATH = `${CONFIG_DIR}/thingsboard.json`;
const TB_SECRETS_PATH = `${CONFIG_DIR}/thingsboard-secrets.json`;
const MAX_BODY = 16 * 1024;
const UI_HTML = readFileSync(new URL('./ui/index.html', import.meta.url), 'utf8');
const BASE_NEEDLE_TOOLS = JSON.parse(readFileSync(new URL('./needle-tools.json', import.meta.url), 'utf8'));
const GENERATED_NEEDLE_TOOLS_PATH = process.env.NEEDLE_TOOLS_GENERATED_FILE || `${CONFIG_DIR}/needle-tools.generated.json`;
const mqttInventory = startMqttInventory(registry);
let generatedToolCatalog = null;

const TOOL_RULES = {
  get_hub_status: { required: ['hub'], stateChanging: false },
  get_sensor_state: { required: ['node'], stateChanging: false },
  list_schedules: { required: ['hub'], stateChanging: false },
  set_relay: { required: ['hub', 'relay', 'state'], stateChanging: true },
  set_schedule: {
    required: ['hub', 'target', 'relay', 'start', 'interval_minutes', 'duration_minutes'],
    stateChanging: true,
  },
};

function json(res, status, value) {
  res.writeHead(status, { 'content-type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(value));
}

async function readBody(req, maxBytes = MAX_BODY) {
  let body = '';
  for await (const chunk of req) {
    body += chunk;
    if (Buffer.byteLength(body) > maxBytes) throw new Error('request body too large');
  }
  return body ? JSON.parse(body) : {};
}

function readJsonFile(path) {
  if (!existsSync(path)) return null;
  return JSON.parse(readFileSync(path, 'utf8'));
}

function writePrivateJson(path, value) {
  mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
  const temporaryPath = `${path}.tmp-${process.pid}`;
  writeFileSync(temporaryPath, `${JSON.stringify(value, null, 2)}\n`, { mode: 0o600 });
  chmodSync(temporaryPath, 0o600);
  renameSync(temporaryPath, path);
  chmodSync(path, 0o600);
}

function writePrivateText(path, content) {
  mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
  const temporaryPath = `${path}.tmp-${process.pid}`;
  writeFileSync(temporaryPath, content, { mode: 0o600 });
  chmodSync(temporaryPath, 0o600);
  renameSync(temporaryPath, path);
  chmodSync(path, 0o600);
}

function generateNeedleToolCatalog(definitions = currentDeviceDefinitions()) {
  const catalog = buildNeedleToolCatalog(BASE_NEEDLE_TOOLS, definitions);
  writePrivateText(GENERATED_NEEDLE_TOOLS_PATH, catalog.serialized);
  generatedToolCatalog = {
    generated: true,
    path: GENERATED_NEEDLE_TOOLS_PATH,
    tool_count: catalog.tool_count,
    byte_count: catalog.byte_count,
    sha256: catalog.sha256,
    reload_pending: true,
  };
  return generatedToolCatalog;
}

generateNeedleToolCatalog();

function publicThingsBoardConfig() {
  const config = readJsonFile(TB_CONFIG_PATH);
  const baseUrl = config?.base_url || DEFAULT_TB_BASE_URL;
  return {
    configured: Boolean(config?.base_url && config?.auth_mode),
    profile_name: config?.profile_name || 'FarmWiz Cloud',
    base_url: baseUrl,
    api_base_url: `${baseUrl}/api`,
    auth_mode: config?.auth_mode || 'jwt',
    updated_at: config?.updated_at || null,
    credentials_present: existsSync(TB_SECRETS_PATH),
    device_bindings: config?.device_bindings || {},
  };
}

function secureDashboardRequest(req) {
  if (req.socket.encrypted) return true;
  const remoteAddress = req.socket.remoteAddress || '';
  const localProxy = remoteAddress === '127.0.0.1' || remoteAddress === '::1' || remoteAddress === '::ffff:127.0.0.1';
  return TRUST_PROXY && localProxy && req.headers['x-forwarded-proto'] === 'https';
}

function sameOriginRequest(req) {
  const origin = req.headers.origin;
  if (!origin) return true;
  try { return new URL(origin).host === req.headers.host; }
  catch { return false; }
}

function validateThingsBoardInput(body) {
  const baseUrl = String(body.base_url || DEFAULT_TB_BASE_URL).trim();
  const parsed = new URL(baseUrl);
  if (parsed.protocol !== 'https:') throw new Error('FarmWiz Cloud HTTPS is required before saving credentials');
  if (parsed.username || parsed.password || parsed.search || parsed.hash) throw new Error('FarmWiz Cloud URL must not contain credentials or query parameters');
  const authMode = body.auth_mode === 'jwt' ? 'jwt' : body.auth_mode === 'api_key' ? 'api_key' : null;
  if (!authMode) throw new Error('auth_mode must be api_key or jwt');
  const oldSecret = readJsonFile(TB_SECRETS_PATH);
  const secret = authMode === 'api_key'
    ? { auth_mode: authMode, api_key: String(body.api_key || '').trim() || (oldSecret?.auth_mode === authMode ? oldSecret.api_key : '') }
    : { auth_mode: authMode, username: String(body.username || '').trim() || (oldSecret?.auth_mode === authMode ? oldSecret.username : ''), password: String(body.password || '') || (oldSecret?.auth_mode === authMode ? oldSecret.password : '') };
  if (authMode === 'api_key' && (!secret.api_key || secret.api_key.length > 4096)) throw new Error('A valid FarmWiz Cloud API key is required');
  if (authMode === 'jwt' && (!secret.username || !secret.password || secret.username.length > 256 || secret.password.length > 4096)) throw new Error('FarmWiz Cloud username and password are required');
  const profileName = String(body.profile_name || 'FarmWiz Cloud').trim();
  if (!profileName || profileName.length > 80) throw new Error('Profile name must contain 1 to 80 characters');
  const deviceBindings = {};
  if (body.device_bindings !== undefined && (!body.device_bindings || typeof body.device_bindings !== 'object' || Array.isArray(body.device_bindings))) {
    throw new Error('Device bindings must map FarmWiz IDs to FarmWiz Cloud UUIDs');
  }
  if (Object.keys(body.device_bindings || {}).length > 100) throw new Error('A profile can bind no more than 100 devices');
  for (const [farmwizId, entityIdValue] of Object.entries(body.device_bindings || {})) {
    const entityId = String(entityIdValue || '').trim();
    if (!/^[A-Za-z0-9_.:-]{1,128}$/.test(farmwizId)) throw new Error(`Invalid FarmWiz device ID: ${farmwizId}`);
    if (!entityId) continue;
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(entityId)) {
      throw new Error(`FarmWiz Cloud ID for ${farmwizId} must be a UUID`);
    }
    deviceBindings[farmwizId] = entityId;
  }
  return {
    config: { profile_name: profileName, base_url: parsed.toString().replace(/\/$/, ''), auth_mode: authMode, device_bindings: deviceBindings, updated_at: new Date().toISOString() },
    secret,
  };
}

async function testThingsBoard() {
  const config = readJsonFile(TB_CONFIG_PATH);
  const secret = readJsonFile(TB_SECRETS_PATH);
  if (!config || !secret) throw new Error('FarmWiz Cloud is not configured');
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 10000);
  try {
    let response;
    if (secret.auth_mode === 'api_key') {
      response = await fetch(`${config.base_url}/api/auth/user`, {
        headers: { 'X-Authorization': `ApiKey ${secret.api_key}` },
        signal: controller.signal,
      });
    } else {
      response = await fetch(`${config.base_url}/api/auth/login`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ username: secret.username, password: secret.password }),
        signal: controller.signal,
      });
    }
    if (!response.ok) throw new Error(`FarmWiz Cloud returned HTTP ${response.status}`);
    return { ok: true, base_url: config.base_url, auth_mode: config.auth_mode, http_status: response.status };
  } finally {
    clearTimeout(timeout);
  }
}

async function interpret(text) {
  const response = await fetch(`${NEEDLE_URL}/complete`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ input: text }),
  });
  if (!response.ok) throw new Error(`Needle returned HTTP ${response.status}`);
  return response.json();
}

function validateNeedleResult(result) {
  if (!result || result.type !== 'call') {
    return { status: 'refused', reason: 'Needle returned no actionable call', effects_executed: false };
  }
  const calls = Array.isArray(result.function_calls) ? result.function_calls : [];
  if (calls.length === 0) {
    return {
      status: 'refused',
      reason: 'No supported FarmWiz action was identified',
      suppressed_calls: result.suppressed_calls || [],
      effects_executed: false,
    };
  }
  if (result.validation?.ungrounded?.length) {
    return {
      status: 'needs_clarification',
      reason: 'Needle produced an ungrounded argument',
      ungrounded: result.validation.ungrounded,
      proposed_calls: calls,
      effects_executed: false,
    };
  }

  const proposed = [];
  const registrySnapshot = registry.snapshot();
  for (const call of calls) {
    const rule = TOOL_RULES[call?.name];
    if (!rule) return { status: 'blocked', reason: `Unknown tool: ${call?.name}`, effects_executed: false };
    const args = call.arguments && typeof call.arguments === 'object' ? call.arguments : {};
    const missing = rule.required.filter((key) => args[key] === undefined || args[key] === '');
    if (missing.length) {
      return { status: 'needs_clarification', reason: 'Required argument missing', missing, proposed_calls: calls, effects_executed: false };
    }
    if (args.relay !== undefined && (!Number.isInteger(args.relay) || args.relay < 1 || args.relay > 4)) {
      return { status: 'blocked', reason: 'Relay must be an integer from 1 through 4', proposed_calls: calls, effects_executed: false };
    }
    if (args.duration_seconds !== undefined && (!Number.isInteger(args.duration_seconds) || args.duration_seconds < 1 || args.duration_seconds > 1800)) {
      return { status: 'blocked', reason: 'Duration exceeds the safe one-shot limit', proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'set_relay' && !['on', 'off'].includes(args.state)) {
      return { status: 'blocked', reason: 'Relay state must be on or off', proposed_calls: calls, effects_executed: false };
    }
    const deviceToolMatches = matchDefinedDeviceTools(call);
    if (!deviceToolMatches.length) {
      return { status: 'blocked', reason: `No matching defined device tool for ${call.name}`, proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'get_hub_status' && !registry.hasHub(args.hub)) {
      return { status: 'blocked', reason: 'Hub is not present in the current inventory', registry_revision: registrySnapshot.revision, proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'get_sensor_state' && !registry.hasNode(args.node)) {
      return { status: 'blocked', reason: 'Node is not present in the current inventory', registry_revision: registrySnapshot.revision, proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'list_schedules' && !registry.hasHub(args.hub)) {
      return { status: 'blocked', reason: 'Hub is not present in the current inventory', registry_revision: registrySnapshot.revision, proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'set_relay' && !registry.hasRelay(args.hub, args.relay)) {
      return { status: 'blocked', reason: 'Relay is not present in the current inventory', registry_revision: registrySnapshot.revision, proposed_calls: calls, effects_executed: false };
    }
    if (call.name === 'set_schedule' && !registry.hasScheduleTarget(args.hub, args.target, args.relay)) {
      return { status: 'blocked', reason: 'Schedule target or relay capability is not present in the current inventory', registry_revision: registrySnapshot.revision, proposed_calls: calls, effects_executed: false };
    }
    proposed.push({ name: call.name, arguments: args, device_tool_matches: deviceToolMatches });
  }

  const stateChanging = calls.some((call) => TOOL_RULES[call.name].stateChanging);
  return {
    status: stateChanging ? 'confirmation_required' : 'read_only_ready',
    confidence: result.confidence ?? null,
    proposed_calls: proposed,
    reasoning: result.reasoning ?? null,
    registry_revision: registrySnapshot.revision,
    effects_executed: false,
    adapter: 'not-configured',
  };
}

const requestHandler = async (req, res) => {
  try {
    if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
      if (!req.socket.encrypted && HTTPS_ENABLED) {
        const port = HTTPS_PORT === 443 ? '' : `:${HTTPS_PORT}`;
        res.writeHead(308, { location: `https://${BIND_HOST}${port}${req.url}`, 'cache-control': 'no-store' });
        return res.end();
      }
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
      return res.end(UI_HTML);
    }
    if (req.method === 'GET' && req.url === '/health') {
      const inventory = registry.snapshot();
      const mqtt = mqttInventory.getState();
      return json(res, 200, { ok: true, service: 'farmwiz-orchestrator', bind_host: BIND_HOST, https_enabled: HTTPS_ENABLED, https_port: HTTPS_ENABLED ? HTTPS_PORT : null, needle_url: NEEDLE_URL, registry_revision: inventory.revision, registry_count: inventory.count, defined_tool_library: deviceToolLibraryStatus(), needle_tool_catalog: generatedToolCatalog, mqtt_connected: mqtt.connected, effects_executed: false });
    }
    if (req.method === 'GET' && req.url === '/v1/registry') {
      return json(res, 200, registry.snapshot());
    }
    if (req.method === 'POST' && req.url === '/v1/registry/reset') {
      if (!sameOriginRequest(req)) return json(res, 403, { error: 'Device reset must come from this dashboard' });
      const body = await readBody(req);
      if (body.confirm !== 'RESET') return json(res, 400, { error: 'Confirm the local device reset to continue' });
      const removedDefinitions = currentDeviceDefinitions().length;
      const definitions = [];
      const candidateCatalog = buildNeedleToolCatalog(BASE_NEEDLE_TOOLS, definitions);
      writePrivateJson(deviceLibraryPath(), definitions);
      replaceDeviceDefinitions(definitions);
      writePrivateText(GENERATED_NEEDLE_TOOLS_PATH, candidateCatalog.serialized);
      generatedToolCatalog = { generated: true, path: GENERATED_NEEDLE_TOOLS_PATH, tool_count: candidateCatalog.tool_count, byte_count: candidateCatalog.byte_count, sha256: candidateCatalog.sha256, reload_pending: true };
      const reset = registry.reset();
      return json(res, 200, { reset: true, removed_registry_records: reset.removed, removed_definitions: removedDefinitions, registry_revision: reset.revision, defined_devices: 0, needle_tool_catalog: generatedToolCatalog, effects_executed: false });
    }
    if (req.method === 'GET' && req.url === '/v1/registry/device-definitions') {
      const definitions = currentDeviceDefinitions();
      const definedIds = new Set(definitions.map((device) => device.deviceId));
      const registryOnly = registry.snapshot().nodes
        .filter((node) => !definedIds.has(node.node_uid))
        .map((node) => ({ deviceId: node.node_uid, nodeName: node.node_name, hubId: node.hub_id, aliases: node.aliases, groups: node.groups, capabilities: node.capabilities || {} }));
      return json(res, 200, { devices: [...definitions, ...registryOnly] });
    }
    const deviceRoute = req.url.match(/^\/v1\/registry\/devices\/([^/?]+)$/);
    if (deviceRoute && ['PUT', 'DELETE'].includes(req.method)) {
      if (!sameOriginRequest(req)) return json(res, 403, { error: 'Device changes must come from this dashboard' });
      const deviceId = decodeURIComponent(deviceRoute[1]);
      if (!/^[A-Za-z0-9_.:-]{1,128}$/.test(deviceId)) return json(res, 400, { error: 'Invalid device ID' });
      const current = currentDeviceDefinitions();
      const definitionExists = current.some((device) => device.deviceId === deviceId);
      const registryRecordExists = registry.snapshot().nodes.some((node) => node.node_uid === deviceId);
      if (req.method === 'PUT' && !definitionExists) return json(res, 404, { error: 'Device definition not found' });
      if (req.method === 'DELETE' && !definitionExists && !registryRecordExists) return json(res, 404, { error: 'Device not found' });
      let next;
      if (req.method === 'DELETE') next = current.filter((device) => device.deviceId !== deviceId);
      else {
        const body = await readBody(req, 80 * 1024);
        next = definitionExists
          ? current.map((device) => device.deviceId === deviceId ? { ...body, deviceId } : device)
          : [...current, { ...body, deviceId }];
      }
      const definitions = next.length ? validateDeviceDefinitions(next) : [];
      const candidateCatalog = buildNeedleToolCatalog(BASE_NEEDLE_TOOLS, definitions);
      writePrivateJson(deviceLibraryPath(), definitions);
      replaceDeviceDefinitions(definitions);
      writePrivateText(GENERATED_NEEDLE_TOOLS_PATH, candidateCatalog.serialized);
      generatedToolCatalog = { generated: true, path: GENERATED_NEEDLE_TOOLS_PATH, tool_count: candidateCatalog.tool_count, byte_count: candidateCatalog.byte_count, sha256: candidateCatalog.sha256, reload_pending: true };
      if (req.method === 'DELETE') registry.remove(deviceId);
      else {
        const updated = deviceDefinitionsForRegistry().find((device) => device.node_uid === deviceId);
        registry.ingest(updated, 'manual_json');
      }
      const snapshot = registry.snapshot();
      return json(res, 200, { device_id: deviceId, action: req.method === 'DELETE' ? 'deleted' : 'updated', registry_revision: snapshot.revision, library_count: definitions.length, needle_tool_catalog: generatedToolCatalog, effects_executed: false });
    }
    if (req.method === 'POST' && req.url === '/v1/registry/import-devices') {
      if (!sameOriginRequest(req)) return json(res, 403, { error: 'Device import must come from this dashboard' });
      let definitions;
      let candidateCatalog;
      try {
        const body = await readBody(req, 80 * 1024);
        definitions = validateDeviceDefinitions(body);
        candidateCatalog = buildNeedleToolCatalog(BASE_NEEDLE_TOOLS, definitions);
      } catch (error) {
        return json(res, 400, { error: error.message, effects_executed: false });
      }
      writePrivateJson(deviceLibraryPath(), definitions);
      replaceDeviceDefinitions(definitions);
      writePrivateText(GENERATED_NEEDLE_TOOLS_PATH, candidateCatalog.serialized);
      generatedToolCatalog = {
        generated: true,
        path: GENERATED_NEEDLE_TOOLS_PATH,
        tool_count: candidateCatalog.tool_count,
        byte_count: candidateCatalog.byte_count,
        sha256: candidateCatalog.sha256,
        reload_pending: true,
      };
      const imported = deviceDefinitionsForRegistry();
      const pruned = registry.removeManualExcept(imported.map((device) => device.node_uid));
      for (const device of imported) registry.ingest(device, 'manual_json');
      const snapshot = registry.snapshot();
      return json(res, 200, {
        imported: imported.length,
        device_ids: imported.map((device) => device.node_uid),
        removed_stale_manual_devices: pruned.removed,
        registry_revision: snapshot.revision,
        library_count: deviceToolLibraryStatus().device_count,
        needle_tool_catalog: generatedToolCatalog,
        effects_executed: false,
      });
    }
    if (req.method === 'GET' && req.url === '/v1/inventory/live') {
      return json(res, 200, mqttInventory.getState());
    }
    if (req.method === 'POST' && req.url === '/v1/discovery/mqtt') {
      const body = await readBody(req);
      const timeoutMs = body.timeout_ms === undefined ? undefined : Number(body.timeout_ms);
      return json(res, 200, await mqttInventory.requestInventory({ timeoutMs }));
    }
    if (req.method === 'POST' && req.url === '/v1/registry/sync-thingsboard') {
      if (!secureDashboardRequest(req)) return json(res, 400, { error: 'Inventory sync requires an HTTPS dashboard connection' });
      return json(res, 200, await syncThingsBoard(registry));
    }
    if (req.method === 'GET' && req.url === '/v1/config/thingsboard') {
      return json(res, 200, publicThingsBoardConfig());
    }
    if (req.method === 'POST' && req.url === '/v1/config/thingsboard') {
      if (!sameOriginRequest(req)) return json(res, 403, { error: 'Profile updates must come from this dashboard' });
      if (!secureDashboardRequest(req)) {
        return json(res, 400, { error: 'Credential configuration requires an HTTPS dashboard connection' });
      }
      const body = await readBody(req);
      let parsed;
      try { parsed = validateThingsBoardInput(body); }
      catch (error) { return json(res, 400, { error: error.message }); }
      writePrivateJson(TB_CONFIG_PATH, parsed.config);
      writePrivateJson(TB_SECRETS_PATH, parsed.secret);
      return json(res, 200, publicThingsBoardConfig());
    }
    if (req.method === 'POST' && req.url === '/v1/config/thingsboard/test') {
      if (!sameOriginRequest(req)) return json(res, 403, { error: 'Profile checks must come from this dashboard' });
      if (!secureDashboardRequest(req)) return json(res, 400, { error: 'Credential testing requires an HTTPS dashboard connection' });
      return json(res, 200, await testThingsBoard());
    }
    if (req.method === 'POST' && req.url === '/v1/interpret') {
      const body = await readBody(req);
      if (typeof body.text !== 'string' || !body.text.trim()) return json(res, 400, { error: 'text is required' });
      const needleResult = await interpret(body.text.trim());
      return json(res, 200, { request: body.text.trim(), ...validateNeedleResult(needleResult) });
    }
    return json(res, 404, { error: 'not found' });
  } catch (error) {
    return json(res, 500, { error: error.message, effects_executed: false });
  }
};

const server = http.createServer(requestHandler);
server.listen(PORT, BIND_HOST, () => {
  console.log(`farmwiz-orchestrator listening on http://${BIND_HOST}:${PORT}`);
});

if (HTTPS_ENABLED) {
  const httpsServer = https.createServer({
    cert: readFileSync(TLS_CERT_FILE),
    key: readFileSync(TLS_KEY_FILE),
  }, requestHandler);
  httpsServer.listen(HTTPS_PORT, BIND_HOST, () => {
    console.log(`farmwiz-orchestrator HTTPS listening on https://${BIND_HOST}:${HTTPS_PORT}`);
  });
}

startDiscovery(registry);

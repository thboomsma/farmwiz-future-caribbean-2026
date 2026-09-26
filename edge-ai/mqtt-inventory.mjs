import mqtt from 'mqtt';
import { randomUUID } from 'node:crypto';

const MQTT_URL = process.env.MQTT_URL || 'mqtt://127.0.0.1:1883';
const MQTT_ENABLED = process.env.MQTT_ENABLED === 'true';
const configuredTopicRoot = (process.env.MQTT_TOPIC_ROOT || 'farmwiz/')
  .trim()
  .replace(/^\/+|\/+$/g, '');
if (!/^[A-Za-z0-9_-]+(?:\/[A-Za-z0-9_-]+)*$/.test(configuredTopicRoot)) {
  throw new Error('MQTT_TOPIC_ROOT must contain simple MQTT topic path segments without wildcards');
}
const MQTT_TOPIC_ROOT = `${configuredTopicRoot}/`;
const REQUEST_TOPIC = process.env.MQTT_DISCOVERY_REQUEST_TOPIC || `${MQTT_TOPIC_ROOT}discovery/request`;
const RESPONSE_TOPIC = process.env.MQTT_DISCOVERY_RESPONSE_TOPIC || `${MQTT_TOPIC_ROOT}discovery/response`;
const DEFAULT_TIMEOUT_MS = Number(process.env.MQTT_DISCOVERY_TIMEOUT_MS || 5000);

let client = null;
let connected = false;
let connectPromise = null;
let activeScan = null;
let liveInventory = [];
let lastScan = null;
let registryRef = null;

function safeJson(value) {
  try { return JSON.parse(value.toString('utf8')); } catch { return null; }
}

function first(...values) {
  return values.find((value) => value !== undefined && value !== null && String(value).trim() !== '');
}

function asArray(value) {
  if (Array.isArray(value)) return value;
  if (value === undefined || value === null || value === '') return [];
  if (typeof value === 'string') return value.split(',').map((item) => item.trim()).filter(Boolean);
  return [value];
}

function payloadDevices(payload) {
  if (Array.isArray(payload)) return payload;
  if (!payload || typeof payload !== 'object') return [];
  for (const key of ['devices', 'nodes', 'inventory', 'items']) {
    if (Array.isArray(payload[key])) return payload[key];
  }
  if (payload.device && typeof payload.device === 'object') return [payload.device];
  if (payload.node && typeof payload.node === 'object') return [payload.node];
  return [payload];
}

function normalizeDevice(device, topic, scanId) {
  if (!device || typeof device !== 'object') return null;
  const nodeUid = first(device.node_uid, device.nodeUid, device.node_id, device.nodeId, device.uid, device.device_uid, device.deviceUid, device.id);
  if (!nodeUid) return null;
  const nodeName = first(device.node_name, device.nodeName, device.device_name, device.deviceName, device.name, nodeUid);
  const hubId = first(device.hub_id, device.hubId, device.hub, device.parent_hub, device.parentHub);
  const capabilities = device.capabilities && typeof device.capabilities === 'object' ? device.capabilities : {};
  const aliases = asArray(first(device.aliases, device.alias));
  const groups = asArray(first(device.groups, device.group));
  return {
    node_uid: String(nodeUid),
    node_name: String(nodeName),
    hub_id: hubId ? String(hubId) : null,
    aliases,
    groups,
    local_ip: first(device.local_ip, device.localIp, device.ip, device.address) || null,
    mac_address: first(device.mac_address, device.macAddress, device.mac) || null,
    firmware_version: first(device.firmware_version, device.firmwareVersion, device.firmware) || null,
    capabilities,
    source_topic: topic,
    scan_id: scanId,
    received_at: new Date().toISOString(),
    raw: device,
  };
}

function publicInventory() {
  return liveInventory.map((device) => ({ ...device, raw: undefined }));
}

function ingestMessage(topic, message, registry) {
  if (!activeScan || topic !== RESPONSE_TOPIC) return;
  const payload = safeJson(message);
  if (payload === null) return;
  for (const candidate of payloadDevices(payload)) {
    const device = normalizeDevice(candidate, topic, activeScan.scanId);
    if (!device) continue;
    const index = liveInventory.findIndex((item) => item.node_uid === device.node_uid);
    if (index >= 0) liveInventory[index] = { ...liveInventory[index], ...device };
    else liveInventory.push(device);

    if (device.hub_id) {
      try {
        registryRef.ingest(device, 'mqtt_ping');
      } catch (error) {
        console.warn(`MQTT inventory device ${device.node_uid} was not added to registry: ${error.message}`);
      }
    }
  }
}

function ensureClient() {
  if (!MQTT_ENABLED) return Promise.reject(new Error('MQTT inventory is disabled; configure an external broker before enabling it'));
  if (connected && client) return Promise.resolve();
  if (connectPromise) return connectPromise;
  connectPromise = new Promise((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      connectPromise = null;
      reject(new Error(`MQTT connection timed out for ${MQTT_URL}`));
    }, 5000);

    const options = {};
    if (process.env.MQTT_USERNAME) options.username = process.env.MQTT_USERNAME;
    if (process.env.MQTT_PASSWORD) options.password = process.env.MQTT_PASSWORD;
    client = mqtt.connect(MQTT_URL, options);
    client.on('connect', () => {
      connected = true;
      client.subscribe(RESPONSE_TOPIC, { qos: 0 }, (error) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        connectPromise = null;
        if (error) reject(error);
        else resolve();
      });
    });
    client.on('message', (topic, message) => ingestMessage(topic, message, registryRef));
    client.on('close', () => { connected = false; });
    client.on('error', (error) => {
      connected = false;
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        connectPromise = null;
        reject(error);
      }
    });
  });
  return connectPromise;
}

async function publishPing() {
  await ensureClient();
  await new Promise((resolve, reject) => {
    client.publish(REQUEST_TOPIC, 'ping *', { qos: 0, retain: false }, (error) => error ? reject(error) : resolve());
  });
}

function state() {
  return {
    enabled: MQTT_ENABLED,
    connected,
    broker_url: MQTT_ENABLED ? MQTT_URL.replace(/:\/\/([^:]+):([^@]+)@/, '://$1:********@') : null,
    topic_root: MQTT_TOPIC_ROOT,
    request_topic: REQUEST_TOPIC,
    response_topic: RESPONSE_TOPIC,
    scanning: Boolean(activeScan),
    last_scan: lastScan,
    count: liveInventory.length,
    inventory: publicInventory(),
  };
}

export function startMqttInventory(registry) {
  registryRef = registry;
  console.log(MQTT_ENABLED
    ? `MQTT inventory configured: ${MQTT_URL} (${REQUEST_TOPIC} -> ${RESPONSE_TOPIC})`
    : 'MQTT inventory disabled; no broker connection will be attempted');
  return { requestInventory, getState: state };
}

export function getMqttInventoryState() { return state(); }

export async function requestInventory({ timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  if (!MQTT_ENABLED) throw new Error('MQTT inventory is disabled; configure an external broker before enabling it');
  if (activeScan) throw new Error('An MQTT inventory scan is already in progress');
  liveInventory = [];
  const scan = { scanId: randomUUID(), started_at: new Date().toISOString(), payload: 'ping *' };
  activeScan = scan;
  try {
    await publishPing();
    await new Promise((resolve) => setTimeout(resolve, Math.max(250, Math.min(Number(timeoutMs) || DEFAULT_TIMEOUT_MS, 30000))));
    lastScan = { ...scan, completed_at: new Date().toISOString(), count: liveInventory.length };
    return { ...lastScan, request_topic: REQUEST_TOPIC, response_topic: RESPONSE_TOPIC, count: liveInventory.length, inventory: publicInventory(), connected };
  } finally {
    activeScan = null;
  }
}

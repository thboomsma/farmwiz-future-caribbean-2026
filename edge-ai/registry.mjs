import { mkdirSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';

const DATA_DIR = process.env.DATA_DIR || '/opt/farmwiz-orchestrator/var/data';
mkdirSync(DATA_DIR, { recursive: true, mode: 0o700 });

const db = new DatabaseSync(`${DATA_DIR}/registry.sqlite`);
db.exec(`
  PRAGMA journal_mode = WAL;
  CREATE TABLE IF NOT EXISTS registry_meta (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    revision INTEGER NOT NULL
  );
  INSERT OR IGNORE INTO registry_meta (id, revision) VALUES (1, 0);
  CREATE TABLE IF NOT EXISTS managed_nodes (
    node_uid TEXT PRIMARY KEY,
    tb_entity_id TEXT,
    node_name TEXT NOT NULL,
    hub_id TEXT,
    aliases_json TEXT NOT NULL,
    groups_json TEXT NOT NULL,
    local_ip TEXT,
    mac_address TEXT,
    firmware_version TEXT,
    capabilities_json TEXT NOT NULL,
    cloud_last_seen TEXT,
    local_last_seen TEXT,
    status TEXT NOT NULL,
    source TEXT NOT NULL,
    registry_revision INTEGER NOT NULL,
    updated_at TEXT NOT NULL
  );
  CREATE TABLE IF NOT EXISTS registry_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    registry_revision INTEGER NOT NULL,
    source TEXT NOT NULL,
    node_uid TEXT NOT NULL,
    event_type TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    observed_at TEXT NOT NULL
  );
`);

function toNode(row) {
  if (!row) return null;
  return {
    node_uid: row.node_uid,
    tb_entity_id: row.tb_entity_id,
    node_name: row.node_name,
    hub_id: row.hub_id,
    aliases: JSON.parse(row.aliases_json),
    groups: JSON.parse(row.groups_json),
    local_ip: row.local_ip,
    mac_address: row.mac_address,
    firmware_version: row.firmware_version,
    capabilities: JSON.parse(row.capabilities_json),
    cloud_last_seen: row.cloud_last_seen,
    local_last_seen: row.local_last_seen,
    status: row.status,
    source: row.source,
    registry_revision: row.registry_revision,
    updated_at: row.updated_at,
  };
}

function now() { return new Date().toISOString(); }

class Registry {
  revision() { return db.prepare('SELECT revision FROM registry_meta WHERE id = 1').get().revision; }

  list() { return db.prepare('SELECT * FROM managed_nodes ORDER BY node_name, node_uid').all().map(toNode); }

  remove(nodeUid) {
    const current = db.prepare('SELECT node_uid FROM managed_nodes WHERE node_uid = ?').get(nodeUid);
    if (!current) return false;
    const revision = this.revision() + 1;
    const observedAt = now();
    db.exec('BEGIN IMMEDIATE');
    try {
      db.prepare('DELETE FROM managed_nodes WHERE node_uid = ?').run(nodeUid);
      db.prepare('UPDATE registry_meta SET revision = ? WHERE id = 1').run(revision);
      db.prepare('INSERT INTO registry_events (registry_revision, source, node_uid, event_type, payload_json, observed_at) VALUES (?, ?, ?, ?, ?, ?)').run(revision, 'manual_json', nodeUid, 'REMOVED', '{}', observedAt);
      db.exec('COMMIT');
    } catch (error) { db.exec('ROLLBACK'); throw error; }
    return true;
  }

  removeManualExcept(keepNodeUids = []) {
    const keep = new Set(keepNodeUids);
    const removed = this.list().filter((node) => node.source === 'manual_json' && !keep.has(node.node_uid));
    if (!removed.length) return { removed: [], revision: this.revision() };
    const revision = this.revision() + 1;
    const observedAt = now();
    db.exec('BEGIN IMMEDIATE');
    try {
      const deleteNode = db.prepare('DELETE FROM managed_nodes WHERE node_uid = ? AND source = ?');
      const addEvent = db.prepare('INSERT INTO registry_events (registry_revision, source, node_uid, event_type, payload_json, observed_at) VALUES (?, ?, ?, ?, ?, ?)');
      for (const node of removed) {
        deleteNode.run(node.node_uid, 'manual_json');
        addEvent.run(revision, 'manual_json', node.node_uid, 'REMOVED', JSON.stringify({ reason: 'replaced-by-device-list' }), observedAt);
      }
      db.prepare('UPDATE registry_meta SET revision = ? WHERE id = 1').run(revision);
      db.exec('COMMIT');
    } catch (error) { db.exec('ROLLBACK'); throw error; }
    return { removed: removed.map((node) => node.node_uid), revision };
  }

  reset() {
    const current = this.list();
    if (!current.length) return { removed: 0, revision: this.revision() };
    const revision = this.revision() + 1;
    const observedAt = now();
    db.exec('BEGIN IMMEDIATE');
    try {
      db.prepare('DELETE FROM managed_nodes').run();
      db.prepare('UPDATE registry_meta SET revision = ? WHERE id = 1').run(revision);
      const addEvent = db.prepare('INSERT INTO registry_events (registry_revision, source, node_uid, event_type, payload_json, observed_at) VALUES (?, ?, ?, ?, ?, ?)');
      for (const node of current) {
        addEvent.run(revision, 'manual_reset', node.node_uid, 'RESET', JSON.stringify({ previous_source: node.source }), observedAt);
      }
      db.exec('COMMIT');
    } catch (error) { db.exec('ROLLBACK'); throw error; }
    return { removed: current.length, revision };
  }

  snapshot() {
    const nodes = this.list();
    return { revision: this.revision(), count: nodes.length, nodes };
  }

  ingest(input, source = 'local_ping') {
    if (!input || typeof input.node_uid !== 'string' || !input.node_uid.trim()) throw new Error('node_uid is required');
    if (!input.node_name || !input.hub_id) throw new Error('node_name and hub_id are required');
    const observedAt = now();
    const current = db.prepare('SELECT * FROM managed_nodes WHERE node_uid = ?').get(input.node_uid);
    const revision = this.revision() + 1;
    const record = {
      node_uid: input.node_uid,
      tb_entity_id: input.tb_entity_id || current?.tb_entity_id || null,
      node_name: input.node_name,
      hub_id: input.hub_id,
      aliases: Array.isArray(input.aliases) ? input.aliases : (current ? JSON.parse(current.aliases_json) : []),
      groups: Array.isArray(input.groups) ? input.groups : [],
      local_ip: input.local_ip || current?.local_ip || null,
      mac_address: input.mac_address || current?.mac_address || null,
      firmware_version: input.firmware_version || current?.firmware_version || null,
      capabilities: input.capabilities && typeof input.capabilities === 'object' ? input.capabilities : {},
      cloud_last_seen: source === 'thingsboard' ? observedAt : current?.cloud_last_seen || null,
      local_last_seen: ['local_ping', 'mqtt_ping'].includes(source) ? observedAt : current?.local_last_seen || null,
      status: ['local_ping', 'mqtt_ping'].includes(source) ? 'ONLINE_LOCAL'
        : source === 'manual_json' ? 'REGISTERED_MANUAL'
          : current?.status || 'REGISTERED_CLOUD_ONLY',
      source,
      registry_revision: revision,
      updated_at: observedAt,
    };
    db.exec('BEGIN IMMEDIATE');
    try {
      db.prepare(`
        INSERT INTO managed_nodes (node_uid, tb_entity_id, node_name, hub_id, aliases_json, groups_json, local_ip, mac_address, firmware_version, capabilities_json, cloud_last_seen, local_last_seen, status, source, registry_revision, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(node_uid) DO UPDATE SET
          tb_entity_id=excluded.tb_entity_id, node_name=excluded.node_name, hub_id=excluded.hub_id,
          aliases_json=excluded.aliases_json, groups_json=excluded.groups_json, local_ip=excluded.local_ip,
          mac_address=excluded.mac_address, firmware_version=excluded.firmware_version,
          capabilities_json=excluded.capabilities_json, cloud_last_seen=excluded.cloud_last_seen,
          local_last_seen=excluded.local_last_seen, status=excluded.status, source=excluded.source,
          registry_revision=excluded.registry_revision, updated_at=excluded.updated_at
      `).run(record.node_uid, record.tb_entity_id, record.node_name, record.hub_id, JSON.stringify(record.aliases), JSON.stringify(record.groups), record.local_ip, record.mac_address, record.firmware_version, JSON.stringify(record.capabilities), record.cloud_last_seen, record.local_last_seen, record.status, record.source, record.registry_revision, record.updated_at);
      db.prepare('UPDATE registry_meta SET revision = ? WHERE id = 1').run(revision);
      db.prepare('INSERT INTO registry_events (registry_revision, source, node_uid, event_type, payload_json, observed_at) VALUES (?, ?, ?, ?, ?, ?)').run(revision, source, record.node_uid, current ? 'UPDATED' : 'DISCOVERED', JSON.stringify(record), observedAt);
      db.exec('COMMIT');
    } catch (error) {
      db.exec('ROLLBACK');
      throw error;
    }
    return { revision, node: record };
  }

  markStale(maxAgeMs = 150000) {
    const cutoff = Date.now() - maxAgeMs;
    const stale = this.list().filter((node) => node.local_last_seen && Date.parse(node.local_last_seen) < cutoff && node.status === 'ONLINE_LOCAL');
    if (!stale.length) return 0;
    const revision = this.revision() + 1;
    db.exec('BEGIN IMMEDIATE');
    try {
      const update = db.prepare('UPDATE managed_nodes SET status = ?, registry_revision = ?, updated_at = ? WHERE node_uid = ?');
      for (const node of stale) update.run('STALE', revision, now(), node.node_uid);
      db.prepare('UPDATE registry_meta SET revision = ? WHERE id = 1').run(revision);
      db.exec('COMMIT');
    } catch (error) { db.exec('ROLLBACK'); throw error; }
    return stale.length;
  }

  hasHub(hub) { return this.list().some((node) => node.hub_id === hub || node.aliases.includes(hub)); }

  hasNode(nodeName) { return this.list().some((node) => node.node_name === nodeName || node.aliases.includes(nodeName)); }

  hasRelay(hub, relay) {
    return this.list().some((node) => {
      if (!(node.hub_id === hub || node.aliases.includes(hub))) return false;
      const relays = Array.isArray(node.capabilities?.relays) ? node.capabilities.relays : [];
      return relays.some((item) => Number(item.id ?? item) === Number(relay));
    });
  }

  hasScheduleTarget(hub, target, relay) {
    return this.list().some((node) => {
      if (!(node.hub_id === hub || node.aliases.includes(hub))) return false;
      if (!node.groups.includes(target) && node.node_name !== target && !node.aliases.includes(target)) return false;
      const relays = Array.isArray(node.capabilities?.relays) ? node.capabilities.relays : [];
      return relays.some((item) => Number(item.id ?? item) === Number(relay)) && node.capabilities?.schedule?.supported === true;
    });
  }
}

export const registry = new Registry();

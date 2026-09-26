import dgram from 'node:dgram';
import crypto from 'node:crypto';

const PORT = Number(process.env.DISCOVERY_PORT || 38650);
const GROUP = process.env.DISCOVERY_GROUP || '239.255.42.99';
const INTERVAL_MS = 60000;

export function startDiscovery(registry) {
  const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  const pending = new Map();
  const sendPing = () => {
    const requestId = crypto.randomUUID();
    const nonce = crypto.randomBytes(12).toString('hex');
    const issuedAt = new Date();
    pending.set(requestId, { nonce, expires: issuedAt.getTime() + 10000 });
    const payload = Buffer.from(JSON.stringify({ version: 1, type: 'farmwiz.discovery.ping', request_id: requestId, nonce, issued_at: issuedAt.toISOString(), expires_at: new Date(issuedAt.getTime() + 10000).toISOString(), orchestrator_id: 'farmwiz-pi-01' }));
    socket.send(payload, PORT, GROUP);
  };
  socket.on('message', (message, remote) => {
    try {
      const pong = JSON.parse(message.toString('utf8'));
      const expected = pending.get(pong.request_id);
      if (pong.type !== 'farmwiz.discovery.pong' || !expected || expected.nonce !== pong.nonce || Date.now() > expected.expires) return;
      registry.ingest({ ...pong, local_ip: pong.local_ip || remote.address }, 'local_ping');
    } catch { /* Ignore malformed or expired discovery packets. */ }
  });
  socket.bind(PORT, '0.0.0.0', () => {
    try { socket.addMembership(GROUP); } catch { /* Multicast can be unavailable on a constrained NIC. */ }
    sendPing();
  });
  const timer = setInterval(() => {
    for (const [requestId, item] of pending) if (Date.now() > item.expires) pending.delete(requestId);
    registry.markStale();
    sendPing();
  }, INTERVAL_MS);
  timer.unref();
  return { socket, stop: () => { clearInterval(timer); socket.close(); } };
}

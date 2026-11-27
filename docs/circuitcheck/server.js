'use strict';

const dgram = require('node:dgram');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { randomInt } = require('node:crypto');
const { isIPv4 } = require('node:net');

const controller = process.env.CONTROLLER_IP || '127.0.0.1';
const udpPort = Number(process.env.CONTROLLER_PORT || 35100);
const httpPort = Number(process.env.HTTP_PORT || 8080);
const dataDir = path.resolve(process.env.DATA_DIR || path.join(__dirname, 'data'));
const historyFile = path.join(dataDir, 'history.json');
const types = new Map([
  ['DIVIDER', ['NONE', 'WRONG_VALUE']],
  ['RC', ['NONE', 'WRONG_VALUE']],
  ['PWM', ['NONE', 'WRONG_FREQ', 'NO_SIGNAL']],
]);
const metricKeys = [
  'voltage_v', 'expected_voltage_v', 'charge_tau_s', 'discharge_tau_s',
  'expected_tau_s', 'frequency_hz', 'expected_frequency_hz', 'jitter_ms',
  'duty_cycle_pct', 'light_v',
];
const testTimeout = 60_000;
const statusTimeout = 3500;
const maxSamples = 1800;
const maxHistory = 100;
const assets = new Map([
  ['/', ['index.html', 'text/html']],
  ['/style.css', ['style.css', 'text/css']],
  ['/dashboard.js', ['dashboard.js', 'text/javascript']],
].map(([url, [file, mime]]) => [url, [fs.readFileSync(path.join(__dirname, file)), mime]]));

if (!isIPv4(controller)) throw new Error('Set CONTROLLER_IP to Board 1’s IPv4 address');
for (const port of [udpPort, httpPort]) {
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid port');
}

fs.mkdirSync(dataDir, { recursive: true, mode: 0o700 });
let history = [];
if (fs.existsSync(historyFile)) {
  history = JSON.parse(fs.readFileSync(historyFile, 'utf8'));
  if (!Array.isArray(history)) throw new Error('Expected a JSON array in ' + historyFile);
}
let active = null;
let lastStatus = 0;
let boards = { measurement: false, signal: false };
const polls = new Map();
const control = new Map();
const udp = dgram.createSocket('udp4');
let closing = false;

function send(message) {
  udp.send(message, (error) => {
    if (error && !closing) console.error('UDP:', error.message);
  });
}

function poll() {
  const now = Date.now();
  for (const [nonce, sent] of polls) {
    if (now - sent > statusTimeout) polls.delete(nonce);
  }
  let nonce;
  do { nonce = randomInt(1, 2147483647); } while (polls.has(nonce));
  polls.set(nonce, now);
  send('STATUS,' + nonce);
}

function cancelCommand(command) {
  clearInterval(control.get(command));
  control.delete(command);
}

function request(command, message) {
  cancelCommand(command);
  let attempts = 1;
  send(message);
  control.set(command, setInterval(() => {
    if (attempts++ < 4) {
      send(message);
      return;
    }
    cancelCommand(command);
    if (!active) return;
    active.phase = command === 'ABORT' ? 'Stop unconfirmed' : 'Start unconfirmed';
    active.message = 'Check the controller connection. Stop can be retried.';
  }, 350));
}

function beginStop(message) {
  cancelCommand('RUN');
  active.phase = 'Stopping';
  active.message = message;
  request('ABORT', 'ABORT,' + active.id);
}

function finish(verdict, payload = {}, acknowledge = false) {
  const record = {
    ...active, verdict, completed_at: new Date().toISOString(),
    fault_class: typeof payload.fault_class === 'string' ? payload.fault_class.slice(0, 80) : null,
    message: typeof payload.message === 'string' ? payload.message.slice(0, 240) : '',
  };
  const updated = [record, ...history].slice(0, maxHistory);
  try {
    const temp = historyFile + '.tmp';
    fs.writeFileSync(temp, JSON.stringify(updated), { mode: 0o600 });
    fs.renameSync(temp, historyFile);
  } catch (error) {
    active.phase = 'Save error';
    active.message = 'Cannot save the result. Check the dashboard data directory.';
    console.error('History:', error.message);
    return;
  }
  history = updated;
  for (const command of ['RUN', 'ABORT']) cancelCommand(command);
  active = null;
  if (acknowledge) send('ACK,' + record.id + ',RESULT');
}

function readPayload(run, packet) {
  if (packet.metrics && typeof packet.metrics === 'object') {
    for (const key of metricKeys) {
      if (Number.isFinite(packet.metrics[key])) run.metrics[key] = packet.metrics[key];
    }
  }
  if (!Array.isArray(packet.samples)) return;
  const samples = packet.samples.filter(point => point && Number.isFinite(point.t_s) &&
    point.t_s >= 0 && Number.isFinite(point.v)).map(point => ({
    t_s: point.t_s,
    v: point.v,
    phase: ['prepare', 'charge', 'discharge'].includes(point.phase) ? point.phase : '',
  }));
  const byTime = new Map([...run.samples, ...samples].map(point => [point.t_s, point]));
  run.samples = [...byTime.values()].sort((a, b) => a.t_s - b.t_s).slice(-maxSamples);
}

udp.on('message', raw => {
  if (raw.length > 8192) return;
  const match = /^(STATUS|ACK|SAMPLE|RESULT|ERROR),([1-9][0-9]{0,9}),([\s\S]+)$/.exec(raw.toString('utf8').trimEnd());
  if (!match) return;
  const [, kind, idText, body] = match;
  const id = Number(idText);
  if (kind === 'STATUS') {
    const sent = polls.get(id);
    if (!sent || sent <= lastStatus || Date.now() - sent > statusTimeout) return;
    try {
      const status = JSON.parse(body);
      if (typeof status?.measurement !== 'boolean' || typeof status.signal !== 'boolean') return;
      polls.delete(id);
      lastStatus = sent;
      boards = { measurement: status.measurement, signal: status.signal };
    } catch { /* Ignore invalid packets. */ }
    return;
  }
  if (!active || active.id !== id) {
    if (kind === 'RESULT' && history.some(run => run.id === id && ['PASS', 'FAIL'].includes(run.verdict))) {
      send('ACK,' + id + ',RESULT');
    }
    return;
  }
  if (kind === 'ACK') {
    if (body === 'RUN' && ['Requesting test', 'Start unconfirmed'].includes(active.phase)) {
      cancelCommand('RUN');
      active.phase = 'Running';
      active.message = '';
    }
    if (body === 'ABORT') finish('ABORTED', { message: 'Controller confirmed safe stop.' });
    return;
  }
  if (kind === 'ERROR') {
    finish('ERROR', { message: body });
    return;
  }
  let packet;
  try { packet = JSON.parse(body); } catch { return; }
  if (!packet || typeof packet !== 'object') return;
  if (kind === 'RESULT' && !['PASS', 'FAIL'].includes(packet.status)) return;
  readPayload(active, packet);
  if (kind === 'RESULT') finish(packet.status, packet, true);
});

function state() {
  const online = lastStatus > 0 && Date.now() - lastStatus <= statusTimeout;
  return {
    controller,
    online,
    measurement: online ? boards.measurement : null,
    signal: online ? boards.signal : null,
    active,
    latest: active ? null : history[0] || null,
    history: history.map(({ samples, ...summary }) => summary),
  };
}

function reply(res, status, value) {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(value));
}

async function readRequest(req) {
  let body = '';
  for await (const chunk of req) {
    body += chunk;
    if (Buffer.byteLength(body) > 2048) return null;
  }
  try { return JSON.parse(body); } catch { return null; }
}

function download(res, id) {
  const run = history.find(entry => entry.id === id);
  if (!run) return reply(res, 404, { error: 'Run not found' });
  const csvCell = value => {
    let text = String(value ?? '');
    if (typeof value === 'string' && /^\s*[=+\-@]/.test(text)) text = "'" + text;
    return '"' + text.replaceAll('"', '""') + '"';
  };
  const rows = [[
    'run_id', 'test_type', 'requested_fault', 'detected_fault', 'verdict',
    'completed_at', ...metricKeys, 'time_s', 'sample_volts', 'phase',
  ]];
  for (const point of run.samples.length ? run.samples : [{}]) {
    rows.push([
      run.id, run.type, run.fault, run.fault_class, run.verdict, run.completed_at,
      ...metricKeys.map(key => run.metrics[key]), point.t_s, point.v, point.phase,
    ]);
  }
  res.writeHead(200, {
    'Content-Type': 'text/csv; charset=utf-8',
    'Content-Disposition': 'attachment; filename="circuitcheck-' + id + '.csv"',
  });
  res.end(rows.map(row => row.map(csvCell).join(',')).join('\r\n') + '\r\n');
}

const server = http.createServer(async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Content-Security-Policy', "default-src 'self'; frame-ancestors 'none'; form-action 'none'");
  const hosts = ['127.0.0.1:' + httpPort, 'localhost:' + httpPort];
  if (!hosts.includes(req.headers.host)) return reply(res, 403, { error: 'Use the local dashboard URL' });

  const asset = assets.get(req.url);
  if (req.method === 'GET' && asset) {
    res.writeHead(200, { 'Content-Type': asset[1] + '; charset=utf-8' });
    return res.end(asset[0]);
  }
  if (req.method === 'GET' && req.url === '/api/state') return reply(res, 200, state());
  const saved = /^\/api\/runs\/([0-9]+)(\/csv)?$/.exec(req.url);
  if (req.method === 'GET' && saved) {
    if (saved[2]) return download(res, Number(saved[1]));
    const run = history.find(entry => entry.id === Number(saved[1]));
    return run ? reply(res, 200, run) : reply(res, 404, { error: 'Run not found' });
  }
  if (req.method !== 'POST' || !['/api/run', '/api/stop'].includes(req.url)) {
    return reply(res, 404, { error: 'Page not found' });
  }
  if (!/^application\/json(?:;|$)/i.test(req.headers['content-type'] || '') ||
      (req.headers.origin && req.headers.origin !== 'http://' + req.headers.host)) {
    return reply(res, 415, { error: 'Use the dashboard to send commands' });
  }
  try {
    const value = await readRequest(req);
    if (!value || typeof value !== 'object') return reply(res, 400, { error: 'Invalid request body' });
    if (req.url === '/api/stop') {
      if (!active || active.id !== value.id) return reply(res, 409, { error: 'Run is no longer active' });
      beginStop('Waiting for the controller to confirm safe stop.');
      return reply(res, 202, { id: active.id });
    }
    if (active) return reply(res, 409, { error: 'A run is already active' });
    if (!types.get(value.type)?.includes(value.fault)) {
      return reply(res, 400, { error: 'Choose a supported test and fault combination' });
    }
    const connection = state();
    if (!connection.online || !connection.measurement || !connection.signal) {
      return reply(res, 503, { error: 'Wait until all three boards are online' });
    }
    let id;
    do { id = randomInt(1, 2147483647); } while (history.some(run => run.id === id));
    active = {
      id, type: value.type, fault: value.fault, started_at: new Date().toISOString(),
      phase: 'Requesting test', message: '', metrics: {}, samples: [],
    };
    request('RUN', ['RUN', id, value.type, value.fault].join(','));
    return reply(res, 202, { id });
  } catch (error) {
    console.error(error.message);
    if (!res.destroyed) return reply(res, 500, { error: 'Dashboard request failed' });
  }
});

udp.on('error', error => {
  console.error('UDP:', error.message);
  process.exit(1);
});
server.on('error', error => {
  console.error('HTTP:', error.message);
  process.exit(1);
});
udp.connect(udpPort, controller, () => {
  poll();
  setInterval(() => {
    poll();
    if (active && ['Running', 'Requesting test', 'Start unconfirmed'].includes(active.phase) &&
        Date.now() - Date.parse(active.started_at) > testTimeout) {
      beginStop('Run timed out. Waiting for safe stop.');
    }
  }, 1000).unref();
  server.listen(httpPort, '127.0.0.1', () => {
    console.log('CircuitCheck Pro: http://127.0.0.1:' + httpPort + ' → ' + controller + ':' + udpPort);
  });
});

function shutdown() {
  if (closing) return;
  if (active) send('ABORT,' + active.id);
  closing = true;
  server.close();
  udp.close();
  for (const command of control.keys()) cancelCommand(command);
  setTimeout(() => process.exit(0), 300).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

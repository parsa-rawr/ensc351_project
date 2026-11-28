'use strict';

const dgram = require('node:dgram');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { randomUUID } = require('node:crypto');
const { isIPv4 } = require('node:net');

const controller = process.env.CONTROLLER_IP || '127.0.0.1';
const udpPort = Number(process.env.CONTROLLER_PORT || 35100);
const httpPort = Number(process.env.HTTP_PORT || 8080);
const dataDir = path.resolve(process.env.DATA_DIR || path.join(__dirname, 'data'));
const historyFile = path.join(dataDir, 'history.json');
const types = new Map([
  ['DIVIDER', ['NONE', 'WRONG_VALUE', 'NO_SIGNAL']],
  ['RC', ['NONE', 'WRONG_VALUE', 'NO_SIGNAL']],
  ['PWM', ['NONE', 'WRONG_FREQ', 'NO_SIGNAL']],
]);
const metricKeys = [
  'voltage_v', 'expected_voltage_v', 'charge_tau_s', 'discharge_tau_s',
  'expected_tau_s', 'frequency_hz', 'expected_frequency_hz', 'jitter_ms',
  'duty_cycle_pct', 'light_v', 'measured_tau_s', 'min_v', 'max_v', 'peak_v', 'final_v', 'cycles',
];
const statusTimeout = 3500;
const maxSamples = 1800;
const maxHistory = 100;
const assets = new Map([
  ['/', ['index.html', 'text/html']],
  ['/style.css', ['style.css', 'text/css']],
  ['/dashboard.js', ['dashboard.js', 'text/javascript']],
].map(([url, [file, mime]]) => [url, [fs.readFileSync(path.join(__dirname, file)), mime]]));

if (!isIPv4(controller) || !controller.startsWith('127.')) {
  throw new Error('Run the dashboard on Board 1 with CONTROLLER_IP=127.0.0.1');
}
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
let lastController = null;
let pendingRun = null;
let commandInFlight = false;
let closing = false;

const idPattern = /^[A-Za-z0-9_.-]{1,79}$/;
const controllerStates = new Set(['IDLE', 'PREPARING', 'ARMING', 'RUNNING', 'COMPLETED', 'FAILED', 'ABORTED']);
const phases = { PREPARING: 'Preparing', ARMING: 'Arming', RUNNING: 'Running' };

function validStatus(value) {
  return value && controllerStates.has(value.state) &&
    typeof value.board2_connected === 'boolean' && typeof value.board3_connected === 'boolean';
}

// Each exchange has its own source port. An untagged late reply cannot land
// on the socket for a subsequent STATUS, RUN, GET_RESULT or ABORT.
function exchange(message) {
  return new Promise((resolve, reject) => {
    const socket = dgram.createSocket('udp4');
    const timer = setTimeout(() => finish(new Error('Controller reply timed out')), 400);
    let done = false;

    function finish(error, status) {
      if (done) return;
      done = true;
      clearTimeout(timer);
      socket.close();
      if (error) reject(error);
      else resolve(status);
    }

    socket.on('error', error => finish(error));
    socket.on('message', raw => {
      if (raw.length > 8192) return;
      try {
        const status = JSON.parse(raw.toString('utf8'));
        if (validStatus(status)) finish(null, status);
      } catch { /* Ignore invalid replies until the deadline. */ }
    });
    socket.connect(udpPort, controller, () => {
      if (!done) socket.send(message, error => { if (error) finish(error); });
    });
  });
}

async function request(message, attempts = 1) {
  for (let i = 0; i < attempts; i++) {
    try {
      return await exchange(message);
    } catch (error) {
      if (i + 1 === attempts || closing) throw error;
    }
  }
}

function newRun(id, type, fault = null) {
  return {
    id, type, fault, started_at: new Date().toISOString(),
    phase: 'Requesting test', message: '', metrics: {}, samples: [],
  };
}

function save(record) {
  const updated = [record, ...history].slice(0, maxHistory);
  const temp = historyFile + '.tmp';
  try {
    fs.writeFileSync(temp, JSON.stringify(updated), { mode: 0o600 });
    fs.renameSync(temp, historyFile);
  } catch (error) {
    active.phase = 'Save error';
    active.message = 'Cannot save the result. Check the dashboard data directory.';
    console.error('History:', error.message);
    return;
  }
  history = updated;
  active = null;
}

function readPayload(run, packet) {
  if (!packet || typeof packet !== 'object') return;
  const values = {
    ...packet.metrics, ...packet,
    voltage_v: packet.measured_v ?? packet.voltage_v ?? packet.metrics?.voltage_v,
    expected_voltage_v: packet.expected_v ?? packet.expected_voltage_v ?? packet.metrics?.expected_voltage_v,
    expected_frequency_hz: packet.expected_hz ?? packet.expected_frequency_hz ?? packet.metrics?.expected_frequency_hz,
    jitter_ms: packet.sample_interval_ms?.jitter_pp ?? packet.jitter_ms ?? packet.metrics?.jitter_ms,
  };
  for (const key of metricKeys) {
    if (Number.isFinite(values[key])) run.metrics[key] = values[key];
  }
  if (!Array.isArray(packet.samples)) return;
  const samples = packet.samples.filter(point => point && Number.isFinite(point.t_s) &&
    point.t_s >= 0 && Number.isFinite(point.v)).map(point => ({
    t_s: point.t_s,
    v: point.v,
    phase: ['prepare', 'charge', 'discharge'].includes(point.phase) ? point.phase : '',
  }));
  const byTime = new Map(samples.map(point => [point.t_s, point]));
  run.samples = [...byTime.values()].sort((a, b) => a.t_s - b.t_s).slice(-maxSamples);
}

function applyRecord(status) {
  if (!active || status.current_test_id !== active.id) return;
  const measurement = status.measurement_result;
  readPayload(active, measurement);
  const cleanup = status.cleanup_acknowledgements || {};
  active.controller_state = status.state;
  active.cleanup = cleanup;
  active.network_latency_ms = status.network_latency_ms || null;
  active.message = status.error_message || measurement?.summary || measurement?.message || '';
  const pendingCleanup = Object.values(cleanup).includes('pending');
  if (phases[status.state]) {
    if (pendingCleanup) {
      active.phase = 'Stopping';
    } else if (active.phase !== 'Stop unconfirmed') {
      active.phase = phases[status.state];
    }
    if (!active.message) active.message = (status.test_progress || '').replaceAll('_', ' ');
    return;
  }

  const measurementStatus = ['PASS', 'FAIL'].includes(measurement?.status) ? measurement.status : null;
  const verdict = status.state === 'COMPLETED' ? measurementStatus || 'ERROR'
    : status.state === 'ABORTED' ? 'ABORTED' : 'ERROR';
  const detected = measurement?.fault_class || measurement?.fault || measurement?.failure_reason;
  save({
    ...active, verdict, completed_at: new Date().toISOString(),
    measurement_status: measurementStatus, simulated: measurement?.simulated ?? null,
    measurement_result: measurement || null,
    fault_class: typeof detected === 'string' ? detected.slice(0, 80) : null,
    message: active.message || (status.state === 'ABORTED' ? 'Controller confirmed safe stop.' :
      verdict === 'ERROR' ? 'Controller did not return a completed measurement.' : ''),
  });
}

async function poll() {
  const previousRun = active;
  try {
    const status = await request('STATUS');
    if (commandInFlight || closing) return;
    lastController = status;
    lastStatus = Date.now();
    if (pendingRun) return;
    if (!active && idPattern.test(status.current_test_id || '') && types.has(status.test_type) &&
        !history.some(run => String(run.id) === status.current_test_id)) {
      active = newRun(status.current_test_id, status.test_type);
    }
    if (!active || (previousRun && previousRun !== active)) return;
    const run = active;
    const retained = await request('GET_RESULT,' + run.id);
    if (commandInFlight || closing || active !== run) return;
    if (retained.test_id === run.id) {
      applyRecord(retained);
    } else if (retained.error_message === 'unknown test_id') {
      save({ ...run, verdict: 'ERROR', completed_at: new Date().toISOString(),
        message: 'The controller no longer has this test record.', fault_class: null });
    }
  } catch {
    // STATUS age is used for offline detection. The next poll tries again.
  }
}

function state() {
  const online = lastStatus > 0 && Date.now() - lastStatus <= statusTimeout;
  return {
    controller, online,
    measurement: online ? lastController.board2_connected : null,
    signal: online ? lastController.board3_connected : null,
    active,
    latest: active ? null : history[0] || null,
    history: history.map(({ samples, measurement_result, ...summary }) => summary),
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
  const run = history.find(entry => String(entry.id) === id);
  if (!run) return reply(res, 404, { error: 'Run not found' });
  const csvCell = value => {
    let text = String(value ?? '');
    if (typeof value === 'string' && /^\s*[=+\-@]/.test(text)) text = "'" + text;
    return '"' + text.replaceAll('"', '""') + '"';
  };
  const rows = [[
    'run_id', 'test_type', 'requested_fault', 'detected_fault', 'verdict',
    'completed_at', 'simulated', 'measurement_status', 'board2_cleanup', 'board3_cleanup', ...metricKeys, 'time_s', 'sample_volts', 'phase',
  ]];
  for (const point of run.samples.length ? run.samples : [{}]) {
    rows.push([
      run.id, run.type, run.fault, run.fault_class, run.verdict, run.completed_at,
      run.simulated, run.measurement_status, run.cleanup?.board2, run.cleanup?.board3,
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
  const saved = /^\/api\/runs\/([A-Za-z0-9_.-]{1,79})(\/csv)?$/.exec(req.url);
  if (req.method === 'GET' && saved) {
    if (saved[2]) return download(res, saved[1]);
    const run = history.find(entry => String(entry.id) === saved[1]);
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
    if (commandInFlight) return reply(res, 409, { error: 'A dashboard command is in progress' });
    if (req.url === '/api/stop') {
      if (!active || active.id !== value.id) return reply(res, 409, { error: 'Run is no longer active' });
      const run = active;
      commandInFlight = true;
      try {
        const status = await request('ABORT,' + run.id, 3);
        if (status.test_id !== run.id) {
          return reply(res, 409, { error: status.error_message || 'Controller rejected the stop' });
        }
        applyRecord(status);
        return reply(res, 202, { id: run.id });
      } catch {
        run.phase = 'Stop unconfirmed';
        run.message = 'The controller did not confirm the stop. Retry after reconnecting.';
        return reply(res, 504, { error: run.message });
      } finally {
        commandInFlight = false;
      }
    }
    if (active) return reply(res, 409, { error: 'A run is already active' });
    if (!types.get(value.type)?.includes(value.fault)) {
      return reply(res, 400, { error: 'Choose a supported test and fault combination' });
    }
    const connection = state();
    if (!connection.online || !connection.measurement || !connection.signal) {
      return reply(res, 503, { error: 'Wait until all three boards are online' });
    }
    if (pendingRun && (pendingRun.type !== value.type || pendingRun.fault !== value.fault)) {
      return reply(res, 409, { error: 'Retry the previous test with the same settings to resolve its start' });
    }
    pendingRun ??= { id: randomUUID(), type: value.type, fault: value.fault };
    commandInFlight = true;
    try {
      const start = pendingRun;
      const status = await request(['RUN', start.id, start.type, start.fault].join(','), 3);
      if (status.request_id !== start.id || !idPattern.test(status.test_id || '')) {
        pendingRun = null;
        return reply(res, 409, { error: status.error_message || 'Controller rejected the start' });
      }
      pendingRun = null;
      active = newRun(status.test_id, start.type, start.fault);
      applyRecord(status);
      return reply(res, 202, { id: status.test_id });
    } catch {
      return reply(res, 504, { error: 'Start unconfirmed. Retry with the same test settings.' });
    } finally {
      commandInFlight = false;
    }
  } catch (error) {
    console.error(error.message);
    if (!res.destroyed) return reply(res, 500, { error: 'Dashboard request failed' });
  }
});

server.on('error', error => {
  console.error('HTTP:', error.message);
  process.exit(1);
});

async function pollLoop() {
  if (closing) return;
  if (!commandInFlight) await poll();
  if (!closing) setTimeout(pollLoop, 500).unref();
}
pollLoop();
server.listen(httpPort, '127.0.0.1', () => {
  console.log('CircuitCheck Pro: http://127.0.0.1:' + httpPort + ' → ' + controller + ':' + udpPort);
});

async function shutdown() {
  if (closing) return;
  closing = true;
  server.close();
  if (active && !active.verdict && active.phase !== 'Save error') {
    try { await request('ABORT,' + active.id); } catch { /* Controller owns hardware timeouts. */ }
  }
  process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

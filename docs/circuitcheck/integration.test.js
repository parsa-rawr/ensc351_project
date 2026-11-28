'use strict';

// Real HTTP + UDP loopback against a mock Board 1. No GPIO/SPI access.
const assert = require('node:assert/strict');
const dgram = require('node:dgram');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { setTimeout: wait } = require('node:timers/promises');
const { test } = require('node:test');

async function until(check, ms = 5000) {
  const start = Date.now();
  do {
    const result = await check();
    if (result) return result;
    await wait(50);
  } while (Date.now() - start < ms);
  assert.fail('Timed out waiting for the expected response');
}

test('dashboard bridge, result history and reconnect on loopback', { timeout: 20000 }, async t => {
  const dataDir = await fs.mkdtemp(path.join(os.tmpdir(), 'circuitcheck-test-'));
  t.after(() => fs.rm(dataDir, { recursive: true, force: true }));

  const udp = dgram.createSocket('udp4');
  udp.bind(0, '127.0.0.1');
  await once(udp, 'listening');
  t.after(() => udp.close());

  const reservation = net.createServer();
  reservation.listen(0, '127.0.0.1');
  await once(reservation, 'listening');
  const httpPort = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const addr = 'http://127.0.0.1:' + httpPort;

  let proc;
  let logs = '';
  async function stopServer() {
    if (!proc || proc.exitCode !== null || proc.signalCode !== null) return;
    const exited = once(proc, 'exit');
    proc.kill('SIGTERM');
    await exited;
  }
  t.after(stopServer);
  function startServer() {
    proc = spawn(process.execPath, [path.join(__dirname, 'server.js')], {
      env: { ...process.env, DATA_DIR: dataDir, CONTROLLER_IP: '127.0.0.1',
        CONTROLLER_PORT: String(udp.address().port), HTTP_PORT: String(httpPort) },
      stdio: ['ignore', 'ignore', 'pipe'],
    });
    proc.stderr.on('data', data => { logs += data; });
  }

  let allowStatus = false;
  let signalOnline = true;
  let record = null;
  let runCount = 0;
  const commands = [];
  const requests = new Map();
  const sendStatus = (peer, details = {}) => {
    const status = {
      state: 'IDLE', board2_connected: true, board3_connected: signalOnline,
      current_test_id: null, test_type: null, test_progress: 'idle',
      measurement_result: null, error_message: null,
      cleanup_acknowledgements: { board2: 'not_started', board3: 'not_started' },
      network_latency_ms: { board2: { last: 0.2 }, board3: { last: 0.3 } },
      ...record, ...details,
    };
    udp.send(JSON.stringify(status), peer.port, peer.address);
  };
  udp.on('message', (raw, peer) => {
    const message = raw.toString();
    commands.push(message);
    const [kind, id, type, fault] = message.split(',');
    if (message === 'STATUS') {
      if (allowStatus) sendStatus(peer);
      return;
    }
    if (kind === 'RUN') {
      if (requests.has(id)) {
        sendStatus(peer, { request_id: id, test_id: requests.get(id), duplicate_request: true });
        return;
      }
      const testId = '754-aa.' + (++runCount);
      requests.set(id, testId);
      record = { current_test_id: testId, test_type: type, state: 'RUNNING', test_progress: 'measuring' };
      if (fault === 'NO_SIGNAL') {
        record.state = 'COMPLETED';
        record.measurement_result = { status: 'FAIL', fault: 'NO_SIGNAL' };
      }
      // Drop the initial reply. The repeated RUN must use the same request ID.
      return;
    }
    assert.equal(id, record.current_test_id);
    if (kind === 'GET_RESULT') {
      sendStatus(peer, { test_id: id });
      return;
    }
    if (kind === 'ABORT') {
      record.cleanup_acknowledgements = { board2: 'pending', board3: 'pending' };
      sendStatus(peer, { test_id: id });
      setTimeout(() => {
        record.state = 'ABORTED';
        record.cleanup_acknowledgements = { board2: 'confirmed', board3: 'confirmed' };
      }, 250);
      return;
    }
    assert.fail('Unexpected UDP command: ' + message);
  });

  async function request(endpoint, body) {
    const options = body === undefined ? {} : {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    };
    return fetch(addr + endpoint, { ...options, signal: AbortSignal.timeout(2000) });
  }
  const state = async () => {
    try {
      return await (await request('/api/state')).json();
    } catch (error) {
      if (proc.exitCode !== null) throw new Error('Bridge exited: ' + logs, { cause: error });
      return null;
    }
  };

  startServer();
  assert.equal((await until(state)).online, false);
  assert.match(await (await request('/')).text(), /CircuitCheck Pro/);
  assert.equal((await request('/api/run', { type: 'DIVIDER', fault: 'NONE' })).status, 503);
  allowStatus = true;
  await until(async () => (await state())?.signal);
  assert.equal((await request('/api/run', { type: 'RC', fault: 'WRONG_FREQ' })).status, 400);

  const started = await request('/api/run', { type: 'PWM', fault: 'WRONG_FREQ' });
  assert.equal(started.status, 202);
  const { id } = await started.json();
  assert.equal(id, '754-aa.1');
  await until(async () => (await state())?.active?.phase === 'Running');
  const runCommands = commands.filter(cmd => cmd.startsWith('RUN,'));
  assert.equal(runCommands.length, 2);
  assert.equal(runCommands[0], runCommands[1]);
  assert.notEqual(runCommands[0].split(',')[1], id);
  assert.equal((await request('/api/run', { type: 'RC', fault: 'NONE' })).status, 409);

  record.measurement_result = {
    status: 'FAIL', simulated: true, fault: 'WRONG_FREQ',
    frequency_hz: 11.9, expected_hz: 20, sample_interval_ms: { jitter_pp: 0.21 },
    samples: [{ t_s: 0.4, v: 1.23 }, { t_s: 0.1, v: 0.04 }],
  };
  record.cleanup_acknowledgements = { board2: 'pending', board3: 'pending' };
  const live = await until(async () => {
    const run = (await state())?.active;
    return run?.samples?.length === 2 && run;
  });
  assert.deepEqual(live.samples.map(point => point.t_s), [0.1, 0.4]);
  assert.equal(live.phase, 'Stopping');
  record.state = 'COMPLETED';
  record.cleanup_acknowledgements = { board2: 'confirmed', board3: 'confirmed' };
  await until(async () => (await state())?.history[0]?.verdict === 'FAIL');
  const saved = await (await request('/api/runs/' + id)).json();
  assert.equal(saved.verdict, 'FAIL');
  assert.equal(saved.simulated, true);
  assert.equal(saved.samples.length, 2);
  assert.equal(saved.fault_class, 'WRONG_FREQ');
  assert.equal(saved.metrics.expected_frequency_hz, 20);
  assert.match(await (await request('/api/runs/' + id + '/csv')).text(), /"11.9","20","0.21"/);

  const next = await (await request('/api/run', { type: 'RC', fault: 'NONE' })).json();
  await until(async () => (await state())?.active?.phase === 'Running');
  assert.equal((await request('/api/stop', { id: next.id })).status, 202);
  await until(async () => (await state())?.history[0]?.verdict === 'ABORTED');

  const errorRun = await (await request('/api/run', { type: 'DIVIDER', fault: 'NONE' })).json();
  record.state = 'FAILED';
  record.error_message = 'ABORT unconfirmed by Board 3';
  record.cleanup_acknowledgements = { board2: 'confirmed', board3: 'unconfirmed' };
  record.measurement_result = { status: 'PASS', measured_v: 1.649, expected_v: 1.65 };
  await until(async () => (await state())?.history[0]?.verdict === 'ERROR');
  const failed = await (await request('/api/runs/' + errorRun.id)).json();
  assert.equal(failed.measurement_status, 'PASS');
  assert.equal(failed.metrics.voltage_v, 1.649);
  assert.equal(failed.cleanup.board3, 'unconfirmed');

  assert.equal((await request('/api/run', { type: 'DIVIDER', fault: 'NO_SIGNAL' })).status, 202);
  await until(async () => (await state())?.history[0]?.fault_class === 'NO_SIGNAL');

  signalOnline = false;
  await until(async () => (await state())?.signal === false);
  assert.equal((await request('/api/run', { type: 'DIVIDER', fault: 'NONE' })).status, 503);
  allowStatus = false;
  await until(async () => (await state())?.online === false);
  assert.equal((await state()).measurement, null);
  signalOnline = allowStatus = true;
  await until(async () => (await state())?.signal);

  await stopServer();
  startServer();
  await until(async () => (await state())?.online);
  assert.equal((await state()).history.length, 4);
  assert.equal((await (await request('/api/runs/' + id)).json()).verdict, 'FAIL');
  assert(commands.includes('GET_RESULT,' + id));
  assert(commands.includes('ABORT,' + next.id));
});

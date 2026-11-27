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
  let peer;
  const commands = [];
  const send = message => udp.send(message, peer.port, peer.address);
  udp.on('message', (raw, sender) => {
    peer = sender;
    const command = raw.toString();
    const [kind, id] = command.split(',');
    if (kind === 'STATUS') {
      if (allowStatus) send('STATUS,' + id + ',' + JSON.stringify({ measurement: true, signal: signalOnline }));
      return;
    }
    commands.push(command);
    if (kind === 'RUN' && commands.filter(text => text === command).length >= 2) send('ACK,' + id + ',RUN');
    if (kind === 'ABORT') send('ACK,' + id + ',ABORT');
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
  await until(async () => (await state())?.active?.phase === 'Running');
  assert.deepEqual(commands.slice(0, 2), Array(2).fill('RUN,' + id + ',PWM,WRONG_FREQ'));
  assert.equal((await request('/api/run', { type: 'RC', fault: 'NONE' })).status, 409);
  send('SAMPLE,' + id + ',{"metrics":{"voltage_v":1.23},"samples":[{"t_s":0.4,"v":1.23}]}');
  send('SAMPLE,' + id + ',{"samples":[{"t_s":0.1,"v":0.04}]}');
  const live = await until(async () => {
    const run = (await state())?.active;
    return run?.samples?.length === 2 && run;
  });
  assert.deepEqual(live.samples.map(point => point.t_s), [0.1, 0.4]);

  const result = 'RESULT,' + id + ',{"status":"FAIL","fault_class":"WRONG_FREQ","metrics":{"frequency_hz":11.9,"jitter_ms":0.21}}';
  send(result);
  await until(() => commands.includes('ACK,' + id + ',RESULT'));
  send(result);
  await until(() => commands.filter(text => text === 'ACK,' + id + ',RESULT').length === 2);
  const saved = await (await request('/api/runs/' + id)).json();
  assert.equal(saved.verdict, 'FAIL');
  assert.equal(saved.samples.length, 2);
  assert.equal(saved.fault_class, 'WRONG_FREQ');
  assert.equal((await state()).history.length, 1);
  assert.match(await (await request('/api/runs/' + id + '/csv')).text(), /"11.9","","0.21"/);

  const next = await (await request('/api/run', { type: 'RC', fault: 'NONE' })).json();
  await until(async () => (await state())?.active?.phase === 'Running');
  assert.equal((await request('/api/stop', { id: next.id })).status, 202);
  await until(async () => (await state())?.history[0]?.verdict === 'ABORTED');

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
  assert.equal((await state()).history.length, 2);
  assert.equal((await (await request('/api/runs/' + id)).json()).verdict, 'FAIL');
});

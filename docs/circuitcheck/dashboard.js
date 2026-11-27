'use strict';

const el = id => document.getElementById(id);
const tests = {
  DIVIDER: ['Resistor divider', 'Compare the measured divider voltage with the expected value.', ['NONE', 'WRONG_VALUE']],
  RC: ['RC charge and discharge', 'Measure both time constants and review the voltage curve.', ['NONE', 'WRONG_VALUE']],
  PWM: ['PWM verification', 'Measure frequency, duty cycle, and jitter at the measurement station.', ['NONE', 'WRONG_FREQ', 'NO_SIGNAL']],
};
const faults = { NONE: 'None', WRONG_VALUE: 'Wrong component value', WRONG_FREQ: 'Wrong frequency', NO_SIGNAL: 'No signal' };
const metrics = {
  voltage_v: ['Measured voltage', 'V'], expected_voltage_v: ['Expected voltage', 'V'],
  charge_tau_s: ['Charge τ', 's'], discharge_tau_s: ['Discharge τ', 's'], expected_tau_s: ['Expected τ', 's'],
  frequency_hz: ['Frequency', 'Hz'], expected_frequency_hz: ['Expected frequency', 'Hz'],
  jitter_ms: ['Sampling jitter', 'ms'], duty_cycle_pct: ['Duty cycle', '%'], light_v: ['Light sensor voltage', 'V'],
};
const phases = { prepare: '#7c8a9d', charge: '#2758cf', discharge: '#0c897b' };
let current = {};
let selected = null;
let sending = false;
let lastHistory = '';
let frame = 0;

function configure() {
  const [, description, options] = tests[el('test').value];
  el('test-description').textContent = description;
  el('fault').replaceChildren(...options.map(fault => new Option(faults[fault], fault)));
}

function badge(id, label, tone = '') {
  el(id).textContent = label;
  el(id).className = ('badge ' + tone).trim();
}

function connectivity() {
  badge('connection', current.online ? 'Controller connected' : 'Controller unreachable', current.online ? 'good' : 'bad');
  [current.online ?? false, current.measurement, current.signal].forEach((online, index) => {
    badge('board' + (index + 1), online == null ? 'Unknown' : online ? 'Online' : 'Offline',
      online == null ? '' : online ? 'good' : 'bad');
  });
  const active = Boolean(current.active);
  const ready = current.online && current.measurement && current.signal;
  el('start').disabled = !ready || active || sending;
  el('start').title = ready ? '' : 'Wait until all three boards are online';
  el('stop').disabled = !active || current.active.phase === 'Stopping' || sending;
  el('stop').textContent = current.active?.phase === 'Stop unconfirmed' ? 'Retry stop' : 'Stop test';
  el('test').disabled = el('fault').disabled = active || sending;
}

function runView() {
  return selected || current.active || current.latest;
}

function showResult() {
  const run = runView();
  const outcome = run?.verdict || run?.phase || 'Idle';
  el('result').textContent = outcome;
  el('result').className = 'outcome ' +
    (outcome === 'PASS' ? 'good' : ['FAIL', 'ERROR'].includes(outcome) ? 'bad' : '');
  el('run-id').textContent = run ? '#' + run.id + (selected ? ' · Saved run' : '') : '';
  const details = [];
  if (run?.fault_class) details.push('Detected fault: ' + (faults[run.fault_class] || run.fault_class));
  if (run?.message) details.push(run.message);
  el('classification').textContent = details.join(' · ') || (run ? tests[run.type][0] : 'Awaiting a test.');
  el('metrics').replaceChildren(...Object.entries(metrics).filter(([key]) => Number.isFinite(run?.metrics?.[key]))
    .map(([key, [label, unit]]) => {
      const cell = document.createElement('div');
      const name = document.createElement('dt');
      const value = document.createElement('dd');
      name.textContent = label;
      value.textContent = run.metrics[key].toLocaleString(undefined, { maximumFractionDigits: 4 }) + ' ' + unit;
      cell.append(name, value);
      return cell;
    }));
  el('show-current').hidden = !selected;
  drawGraph();
}

function drawGraph() {
  const run = runView();
  const points = run?.samples || [];
  const canvas = el('graph');
  canvas.hidden = !points.length;
  el('graph-empty').hidden = Boolean(points.length);
  el('graph-empty').textContent = run?.verdict
    ? 'No voltage samples were received for this run.'
    : 'Live ADC samples appear when the controller forwards them.';
  el('phases').hidden = !points.some(p => p.phase);
  el('sample-count').textContent = points.length ? points.length + ' samples' : '';
  if (!points.length) return;

  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = width * ratio;
  canvas.height = height * ratio;
  const ctx = canvas.getContext('2d');
  ctx.scale(ratio, ratio);
  const left = 53, right = width - 18, top = 22, bottom = height - 44;
  const tMax = Math.max(.01, points.at(-1).t_s);
  const vMax = Math.max(3.3, ...points.map(p => p.v));
  const vMin = Math.min(0, ...points.map(p => p.v));
  const x = t => left + t / tMax * (right - left);
  const y = v => bottom - (v - vMin) / (vMax - vMin) * (bottom - top);

  ctx.font = '12px system-ui';
  ctx.fillStyle = '#51627b';
  ctx.fillText('Volts', 7, 12);
  for (let i = 0; i <= 4; i++) {
    const volts = vMin + (vMax - vMin) * i / 4;
    ctx.strokeStyle = '#dce5f1';
    ctx.beginPath();
    ctx.moveTo(left, y(volts));
    ctx.lineTo(right, y(volts));
    ctx.stroke();
    ctx.fillText(volts.toFixed(2), 7, y(volts) + 4);
    const time = tMax * i / 4;
    ctx.textAlign = ['left', 'center', 'center', 'center', 'right'][i];
    ctx.fillText(time.toFixed(2), x(time), bottom + 17);
    ctx.textAlign = 'left';
  }
  ctx.textAlign = 'center';
  ctx.fillText('Elapsed time (s)', (left + right) / 2, height - 5);
  ctx.lineWidth = 2;
  for (let i = 0; i < points.length; i++) {
    const a = points[Math.max(i - 1, 0)], b = points[i];
    ctx.strokeStyle = ctx.fillStyle = phases[b.phase] || '#2758cf';
    ctx.beginPath();
    if (a.phase === b.phase && i > 0) {
      ctx.moveTo(x(a.t_s), y(a.v));
      ctx.lineTo(x(b.t_s), y(b.v));
      ctx.stroke();
    } else {
      ctx.arc(x(b.t_s), y(b.v), 2.3, 0, Math.PI * 2);
      ctx.fill();
    }
  }
}

async function json(url, options = {}) {
  const response = await fetch(url, { ...options, signal: AbortSignal.timeout(3500) });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Request failed');
  return data;
}

function showHistory() {
  const runs = current.history || [];
  el('no-history').hidden = Boolean(runs.length);
  el('history-table').hidden = !runs.length;
  const version = runs.map(run => run.id).join(',');
  if (lastHistory === version) return;
  lastHistory = version;
  el('history-body').replaceChildren(...runs.map(run => {
    const row = document.createElement('tr');
    for (const value of [
      new Date(run.completed_at).toLocaleString(), tests[run.type][0], run.verdict,
      faults[run.fault_class] || run.fault_class || '—',
    ]) row.insertCell().textContent = value;
    const report = row.insertCell();
    const view = document.createElement('button');
    view.textContent = 'View';
    view.setAttribute('aria-label', 'View run ' + run.id);
    view.addEventListener('click', async () => {
      try {
        selected = await json('/api/runs/' + run.id);
        showResult();
        el('result-heading').scrollIntoView({ block: 'center' });
      } catch (error) {
        el('feedback').textContent = error.message;
      }
    });
    const csv = document.createElement('a');
    csv.textContent = 'CSV';
    csv.href = '/api/runs/' + run.id + '/csv';
    csv.setAttribute('aria-label', 'Download CSV for run ' + run.id);
    report.append(view, csv);
    return row;
  }));
}

async function refresh() {
  const ticket = ++frame;
  try {
    const updated = await json('/api/state');
    if (ticket !== frame) return;
    current = updated;
    connectivity();
    showHistory();
    showResult();
  } catch {
    if (ticket !== frame) return;
    badge('connection', 'Dashboard disconnected', 'bad');
    for (const id of ['board1', 'board2', 'board3']) badge(id, 'Unknown');
    el('start').disabled = el('stop').disabled = true;
  }
}

async function post(url, body) {
  sending = true;
  connectivity();
  el('feedback').textContent = '';
  try {
    await json(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    selected = null;
  } catch (error) {
    el('feedback').textContent = error.message;
  } finally {
    sending = false;
    await refresh();
  }
}

el('test').addEventListener('change', configure);
el('start').addEventListener('click', () => post('/api/run', { type: el('test').value, fault: el('fault').value }));
el('stop').addEventListener('click', () => post('/api/stop', { id: current.active?.id }));
el('show-current').addEventListener('click', () => { selected = null; showResult(); });
window.addEventListener('resize', drawGraph);
configure();
refresh();
setInterval(refresh, 1000);

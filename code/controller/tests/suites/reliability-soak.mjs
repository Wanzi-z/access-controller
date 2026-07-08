import { spawnSync } from 'child_process';
import { mkdirSync, writeFileSync } from 'fs';
import { dirname, resolve } from 'path';
import { fileURLToPath } from 'url';
import { chromium } from 'playwright';

const __dirname = dirname(fileURLToPath(import.meta.url));
const controllerDir = resolve(__dirname, '../..');
const repoRoot = resolve(controllerDir, '../..');

const numberEnv = (name, fallback) => {
  const value = Number(process.env[name]);
  return Number.isFinite(value) && value > 0 ? value : fallback;
};

const boolEnv = (name, fallback = false) => {
  const value = process.env[name];
  if (value == null || value === '') return fallback;
  return ['1', 'true', 'yes', 'on'].includes(value.toLowerCase());
};

const percentile = (values, pct) => {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.ceil((pct / 100) * sorted.length) - 1);
  return sorted[idx];
};

const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms));

class Metrics {
  constructor() {
    this.rows = new Map();
    this.failures = [];
    this.events = [];
    this.lastUptime = null;
    this.reboots = 0;
    this.heap = { samples: 0, minFree: null, minLargest: null, lastFree: null, lastLargest: null };
  }

  record(name, ok, ms, detail = '') {
    const row = this.rows.get(name) || { name, count: 0, ok: 0, fail: 0, latencies: [] };
    row.count++;
    if (ok) row.ok++;
    else {
      row.fail++;
      this.failures.push({ time: new Date().toISOString(), name, detail: String(detail).slice(0, 240) });
    }
    if (Number.isFinite(ms)) row.latencies.push(ms);
    this.rows.set(name, row);
  }

  event(name, detail = '') {
    this.events.push({ time: new Date().toISOString(), name, detail });
  }

  recordHeap(system = {}) {
    const free = Number(system.freeHeap);
    const largest = Number(system.largestFreeBlock);
    if (!Number.isFinite(free)) return;
    this.heap.samples++;
    this.heap.lastFree = free;
    this.heap.minFree = this.heap.minFree == null ? free : Math.min(this.heap.minFree, free);
    if (Number.isFinite(largest)) {
      this.heap.lastLargest = largest;
      this.heap.minLargest = this.heap.minLargest == null ? largest : Math.min(this.heap.minLargest, largest);
    }
  }

  summaryRows() {
    return [...this.rows.values()].map(row => ({
      name: row.name,
      count: row.count,
      ok: row.ok,
      fail: row.fail,
      p50: Math.round(percentile(row.latencies, 50)),
      p95: Math.round(percentile(row.latencies, 95)),
      p99: Math.round(percentile(row.latencies, 99)),
      max: Math.round(row.latencies.length ? Math.max(...row.latencies) : 0),
    })).sort((a, b) => a.name.localeCompare(b.name));
  }

  totals() {
    return this.summaryRows().reduce((acc, row) => {
      acc.count += row.count;
      acc.ok += row.ok;
      acc.fail += row.fail;
      return acc;
    }, { count: 0, ok: 0, fail: 0 });
  }
}

const formatTable = (rows) => {
  const headers = ['operation', 'count', 'ok', 'fail', 'p50ms', 'p95ms', 'p99ms', 'maxms'];
  const lines = [
    `| ${headers.join(' | ')} |`,
    `| ${headers.map(() => '---').join(' | ')} |`,
  ];
  for (const row of rows) {
    lines.push(`| ${row.name} | ${row.count} | ${row.ok} | ${row.fail} | ${row.p50} | ${row.p95} | ${row.p99} | ${row.max} |`);
  }
  return lines.join('\n');
};

async function timedFetch(baseUrl, path, options, metrics, name) {
  const started = Date.now();
  const timeoutMs = options.timeoutMs || 10000;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`${baseUrl}${path}`, {
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache', 'Connection': 'close' },
      signal: controller.signal,
      ...options,
    });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${text.slice(0, 160)}`);
    }
    const data = text ? JSON.parse(text) : null;
    metrics.record(name, true, Date.now() - started);
    return data;
  } catch (error) {
    metrics.record(name, false, Date.now() - started, error.message);
    throw error;
  } finally {
    clearTimeout(timeout);
  }
}

const postJson = (baseUrl, path, body, metrics, name) => timedFetch(baseUrl, path, {
  method: 'POST',
  body: JSON.stringify(body),
}, metrics, name);

async function setQuietTestMode(baseUrl, metrics, enabled) {
  try {
    await postJson(baseUrl, '/api/buzzer/quiet', { enabled }, metrics, enabled ? 'POST /api/buzzer/quiet on' : 'POST /api/buzzer/quiet off');
  } catch (error) {
    metrics.event('quiet-mode-fail', `${enabled ? 'enable' : 'disable'}: ${error.message}`);
  }
}

async function audibleFailureAlert(baseUrl, metrics) {
  process.stdout.write('\x07');
  try {
    await postJson(baseUrl, '/api/buzzer/error-beep', { beeps: 3, channel: 1 }, metrics, 'POST /api/buzzer/error-beep');
  } catch (error) {
    metrics.event('error-beep-fail', error.message);
  }
}

async function stateWorker(baseUrl, metrics, done, workerId = 0, stateWorkerCount = 1) {
  let i = 0;
  const auxEvery = numberEnv('SOAK_AUX_EVERY', 20);
  const auxOffset = (workerId * Math.max(1, Math.floor(auxEvery / Math.max(1, stateWorkerCount)))) % auxEvery;
  while (!done()) {
    try {
      const state = await timedFetch(baseUrl, `/api/state?t=${Date.now()}`, {}, metrics, 'GET /api/state');
      const uptime = Math.floor(Number(state?.system?.uptimeSeconds) || 0);
      if (metrics.lastUptime != null && uptime + 3 < metrics.lastUptime) {
        metrics.reboots++;
        metrics.event('reboot-detected', `uptime ${metrics.lastUptime}s -> ${uptime}s`);
      }
      metrics.lastUptime = uptime;
      metrics.recordHeap(state?.system);
    } catch {}

    try {
      await timedFetch(baseUrl, `/api/signals?t=${Date.now()}`, { timeoutMs: 5000 }, metrics, 'GET /api/signals');
    } catch {}

    if ((i + auxOffset) % auxEvery === 0) {
      await Promise.allSettled([
        timedFetch(baseUrl, '/api/discovery', { timeoutMs: 5000 }, metrics, 'GET /api/discovery'),
        timedFetch(baseUrl, '/api/logs', { timeoutMs: 5000 }, metrics, 'GET /api/logs'),
        timedFetch(baseUrl, '/api/wifi/list', { timeoutMs: 5000 }, metrics, 'GET /api/wifi/list'),
        timedFetch(baseUrl, '/api/wiegand', { timeoutMs: 5000 }, metrics, 'GET /api/wiegand'),
        timedFetch(baseUrl, '/api/rf', { timeoutMs: 5000 }, metrics, 'GET /api/rf'),
      ]);
    }

    i++;
    await sleep(125);
  }
}

async function settingsWorker(baseUrl, metrics, done, workerId = 0) {
  const modes = ['momentary', 'toggle', 'latch'];
  let i = 0;
  await sleep(workerId * numberEnv('SOAK_SETTINGS_WORKER_STAGGER_MS', 150));
  while (!done()) {
    const channel = (i % 2) + 1;
    const mode = modes[i % modes.length];
    const latch = mode === 'latch';
    const delay = 4 + (i % 3);
    const updates = [
      ['/api/exit', 'POST /api/exit'],
      ['/api/fob', 'POST /api/fob'],
      ['/api/keypad', 'POST /api/keypad'],
      ['/api/motion', 'POST /api/motion'],
    ];
    const offset = workerId % updates.length;
    for (let j = 0; j < updates.length; j++) {
      const [path, name] = updates[(j + offset) % updates.length];
      try {
        await postJson(baseUrl, path, { channel, enable: false, mode, latch, delay, alert: false }, metrics, name);
      } catch {}
      await sleep(numberEnv('SOAK_SETTINGS_STEP_MS', 50));
    }
    i++;
    await sleep(300);
  }
}

async function browserWorker(baseUrl, metrics, done, workerId) {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = [];
  page.on('console', msg => {
    if (msg.type() === 'error') errors.push(msg.text());
  });
  page.on('pageerror', err => errors.push(err.message));

  try {
    while (!done()) {
      const started = Date.now();
      try {
        await page.goto(`${baseUrl}/?worker=${workerId}&t=${Date.now()}`, { waitUntil: 'domcontentloaded', timeout: 15000 });
        await page.waitForSelector('#modeExit_1', { timeout: 10000 });
        metrics.record('browser refresh', true, Date.now() - started);
      } catch (error) {
        metrics.record('browser refresh', false, Date.now() - started, error.message);
      }

      const tickStarted = Date.now();
      try {
        await page.click('.nav-item[data-target="system"]');
        await page.waitForSelector('#systemUptime', { timeout: 5000 });
        const before = await page.textContent('#systemUptime');
        await sleep(1200);
        const after = await page.textContent('#systemUptime');
        if (!before || !after || before === after) {
          throw new Error(`uptime did not tick: ${before} -> ${after}`);
        }
        metrics.record('browser uptime tick', true, Date.now() - tickStarted);
      } catch (error) {
        metrics.record('browser uptime tick', false, Date.now() - tickStarted, error.message);
      }

      for (const error of errors.splice(0)) {
        metrics.record('browser console', false, 0, error);
      }
      await sleep(numberEnv('SOAK_BROWSER_REFRESH_MS', 3000));
    }
  } finally {
    await browser.close();
  }
}

function runOta(baseUrl, metrics) {
  const started = Date.now();
  const host = baseUrl.replace(/\/+$/, '');
  const result = spawnSync('python3', [
    'tools/ota_client.py',
    '--host',
    host,
    '--binary',
    'build/controller.bin',
    '--yes',
  ], {
    cwd: controllerDir,
    encoding: 'utf8',
    timeout: 240000,
  });
  const output = `${result.stdout || ''}${result.stderr || ''}`.trim();
  const ok = result.status === 0 && /Device is back online/.test(output);
  metrics.record('OTA upload+reboot', ok, Date.now() - started, output.slice(-400));
  metrics.event(ok ? 'ota-pass' : 'ota-fail', output.slice(-400));
}

async function otaScheduler(baseUrl, metrics, done, startedAt, durationMs) {
  const repeats = numberEnv('SOAK_OTA_REPEATS', 0);
  if (!repeats) return;
  for (let i = 1; i <= repeats; i++) {
    const target = startedAt + Math.floor((durationMs * i) / (repeats + 1));
    while (!done() && Date.now() < target) {
      await sleep(1000);
    }
    if (done()) return;
    runOta(baseUrl, metrics);
    await sleep(10000);
  }
}

const serviceState = (state, key) => Array.isArray(state?.[key]) ? state[key] : [];

async function applyNonActuatingSettings(baseUrl, metrics) {
  for (const channel of [1, 2]) {
    await Promise.allSettled([
      postJson(baseUrl, '/api/exit', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'prepare /api/exit'),
      postJson(baseUrl, '/api/fob', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'prepare /api/fob'),
      postJson(baseUrl, '/api/keypad', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'prepare /api/keypad'),
      postJson(baseUrl, '/api/motion', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'prepare /api/motion'),
    ]);
  }
  await sleep(1200);
}

async function restoreSettings(baseUrl, metrics, initialState) {
  const restoreService = async (path, key, metricName) => {
    for (const item of serviceState(initialState, key)) {
      const channel = Number(item.channel);
      if (!Number.isFinite(channel) || channel < 1) continue;
      await postJson(baseUrl, path, {
        channel,
        enable: !!item.enable,
        alert: !!item.alert,
        mode: item.mode || (item.latch ? 'latch' : 'momentary'),
        latch: !!item.latch,
        delay: Number.isFinite(Number(item.delay)) ? Number(item.delay) : 4,
      }, metrics, metricName);
    }
  };
  await Promise.allSettled([
    restoreService('/api/exit', 'exits', 'restore /api/exit'),
    restoreService('/api/fob', 'fobs', 'restore /api/fob'),
    restoreService('/api/keypad', 'keypads', 'restore /api/keypad'),
    restoreService('/api/motion', 'motions', 'restore /api/motion'),
  ]);
}

async function restoreDefaults(baseUrl, metrics) {
  for (const channel of [1, 2]) {
    await Promise.allSettled([
      postJson(baseUrl, '/api/exit', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'restore /api/exit'),
      postJson(baseUrl, '/api/fob', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'restore /api/fob'),
      postJson(baseUrl, '/api/keypad', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'restore /api/keypad'),
      postJson(baseUrl, '/api/motion', { channel, enable: false, alert: false, mode: 'momentary', latch: false, delay: 4 }, metrics, 'restore /api/motion'),
    ]);
  }
}

function writeArtifacts(metrics, config, reportName = 'reliability-soak') {
  const artifactDir = resolve(repoRoot, process.env.SOAK_ARTIFACT_DIR || 'code/controller/tests/artifacts');
  mkdirSync(artifactDir, { recursive: true });
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const rows = metrics.summaryRows();
  const totals = metrics.totals();
  const jsonPath = resolve(artifactDir, `${reportName}-${stamp}.json`);
  const mdPath = resolve(artifactDir, `${reportName}-${stamp}.md`);
  writeFileSync(jsonPath, JSON.stringify({ config, totals, rows, heap: metrics.heap, events: metrics.events, failures: metrics.failures }, null, 2));
  writeFileSync(mdPath, [
    '# Access Controller Reliability Soak',
    '',
    `Started: ${config.startedAt}`,
    `Duration target: ${Math.round(config.durationMs / 1000)}s`,
    `Base URL: ${config.baseUrl}`,
    `Workers: state=${config.stateWorkers}, settings=${config.settingsWorkers}, browsers=${config.browserWorkers}`,
    `OTA repeats: ${config.otaRepeats}`,
    '',
    '## Results',
    '',
    formatTable(rows),
    '',
    '## Heap',
    '',
    `Samples: ${metrics.heap.samples}`,
    `Minimum free heap: ${metrics.heap.minFree ?? 'unknown'}`,
    `Minimum largest free block: ${metrics.heap.minLargest ?? 'unknown'}`,
    `Last free heap: ${metrics.heap.lastFree ?? 'unknown'}`,
    `Last largest free block: ${metrics.heap.lastLargest ?? 'unknown'}`,
    '',
    '## Events',
    '',
    ...(metrics.events.length ? metrics.events.map(e => `- ${e.time} ${e.name}: ${e.detail}`) : ['- none']),
    '',
    '## Failures',
    '',
    ...(metrics.failures.length ? metrics.failures.slice(0, 100).map(f => `- ${f.time} ${f.name}: ${f.detail}`) : ['- none']),
    '',
  ].join('\n'));
  return { jsonPath, mdPath };
}

const isTransientNetworkDetail = (detail = '') => (
  /fetch failed|timed out|ECONN|ERR_CONNECTION|ERR_EMPTY_RESPONSE|ERR_INCOMPLETE|reset|aborted|terminated/i.test(String(detail))
);

const isFailureNearOtaEvent = (failure, events, windowMs) => {
  const failureMs = Date.parse(failure?.time || '');
  if (!Number.isFinite(failureMs)) return false;
  return events.some(event => {
    if (!['ota-pass', 'reboot-detected'].includes(event?.name)) return false;
    const eventMs = Date.parse(event.time || '');
    return Number.isFinite(eventMs) && Math.abs(failureMs - eventMs) <= windowMs;
  });
};

export default async function run(api, report) {
  report.startSuite('Reliability Soak', 'Concurrent API, UI refresh, settings update, uptime, and OTA load test');
  const baseUrl = api.baseUrl.replace(/\/+$/, '');
  const durationMs = numberEnv('SOAK_DURATION_MS', 60 * 60 * 1000);
  const metrics = new Metrics();
  const startedAt = Date.now();
  const deadline = startedAt + durationMs;
  const done = () => Date.now() >= deadline;
  const config = {
    startedAt: new Date(startedAt).toISOString(),
    baseUrl,
    durationMs,
    stateWorkers: numberEnv('SOAK_STATE_WORKERS', 4),
    settingsWorkers: numberEnv('SOAK_SETTINGS_WORKERS', 2),
    browserWorkers: numberEnv('SOAK_BROWSER_WORKERS', 2),
    otaRepeats: numberEnv('SOAK_OTA_REPEATS', 0),
  };

  metrics.event('start', JSON.stringify(config));
  let initialState = null;
  try {
    initialState = await timedFetch(baseUrl, `/api/state?t=${Date.now()}`, {}, metrics, 'GET /api/state initial');
    await setQuietTestMode(baseUrl, metrics, true);
    await applyNonActuatingSettings(baseUrl, metrics);
  } catch (error) {
    metrics.event('prepare-fail', error.message);
  }

  const tasks = [];
  for (let i = 0; i < config.stateWorkers; i++) tasks.push(stateWorker(baseUrl, metrics, done, i, config.stateWorkers));
  for (let i = 0; i < config.settingsWorkers; i++) tasks.push(settingsWorker(baseUrl, metrics, done, i));
  for (let i = 0; i < config.browserWorkers; i++) tasks.push(browserWorker(baseUrl, metrics, done, i + 1));
  tasks.push(otaScheduler(baseUrl, metrics, done, startedAt, durationMs));

  const progressTimer = setInterval(() => {
    const elapsed = Math.round((Date.now() - startedAt) / 1000);
    const totals = metrics.totals();
    console.log(`\n[soak ${elapsed}s] requests=${totals.count} ok=${totals.ok} fail=${totals.fail} reboots=${metrics.reboots} uptime=${metrics.lastUptime ?? 'unknown'}s`);
    console.log(formatTable(metrics.summaryRows()));
  }, numberEnv('SOAK_PROGRESS_MS', 60000));

  try {
    await Promise.allSettled(tasks);
    if (initialState) {
      await restoreSettings(baseUrl, metrics, initialState);
    } else {
      await restoreDefaults(baseUrl, metrics);
    }
  } finally {
    clearInterval(progressTimer);
  }

  const artifacts = writeArtifacts(metrics, config);
  const rows = metrics.summaryRows();
  const totals = metrics.totals();
  console.log('\nFinal reliability table:');
  console.log(formatTable(rows));

  const hasUnexpectedFailures = totals.fail > 0 || metrics.reboots > config.otaRepeats;
  if (hasUnexpectedFailures && boolEnv('SOAK_AUDIBLE_ALERT', true)) {
    await audibleFailureAlert(baseUrl, metrics);
  } else {
    await setQuietTestMode(baseUrl, metrics, false);
  }

  if (totals.fail === 0) {
    report.pass('Soak completed without request failures', `${totals.ok}/${totals.count} ok; ${artifacts.mdPath}`, Date.now() - startedAt);
  } else {
    const allowOtaFailures = config.otaRepeats > 0 && boolEnv('SOAK_ALLOW_OTA_DOWNTIME', true);
    const otaWindowMs = numberEnv('SOAK_OTA_TRANSIENT_WINDOW_MS', 45000);
    const nonOtaFailures = metrics.failures.filter(f => (
      !allowOtaFailures
      || !isTransientNetworkDetail(f.detail)
      || !isFailureNearOtaEvent(f, metrics.events, otaWindowMs)
    ));
    if (nonOtaFailures.length === 0) {
      report.pass('Soak completed with only expected OTA/reboot transient failures', `${totals.ok}/${totals.count} ok, ${totals.fail} transient; ${artifacts.mdPath}`, Date.now() - startedAt);
    } else {
      report.fail('Soak request failures', `${nonOtaFailures.length} non-OTA failures; ${artifacts.mdPath}`, Date.now() - startedAt);
    }
  }

  if (metrics.reboots <= config.otaRepeats) {
    report.pass('No unexpected reboot detected', `reboots=${metrics.reboots}, otaRepeats=${config.otaRepeats}`, 0);
  } else {
    report.fail('Unexpected reboot detected', `reboots=${metrics.reboots}, otaRepeats=${config.otaRepeats}`, 0);
  }

  if (config.otaRepeats > 0) {
    const ota = rows.find(row => row.name === 'OTA upload+reboot');
    if (ota?.ok === config.otaRepeats && ota.fail === 0) {
      report.pass('Repeated OTA uploads succeeded', `${ota.ok}/${config.otaRepeats}`, 0);
    } else {
      report.fail('Repeated OTA uploads', JSON.stringify(ota || null), 0);
    }
  }

  report.endSuite();
  return {};
}

import { createReadStream, statSync } from 'fs';
import { createServer } from 'http';
import { extname, join, resolve } from 'path';
import { dirname } from 'path';
import { fileURLToPath } from 'url';
import { chromium } from 'playwright';

const __dirname = dirname(fileURLToPath(import.meta.url));
const publicDir = resolve(__dirname, '../../main/public');

const json = (res, status, body) => {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json',
    'Cache-Control': 'no-store',
    'Content-Length': Buffer.byteLength(payload),
  });
  res.end(payload);
};

const text = (res, status, body) => {
  res.writeHead(status, {
    'Content-Type': 'text/plain',
    'Content-Length': Buffer.byteLength(body),
  });
  res.end(body);
};

const channelState = (channel) => ({
  channel,
  enable: false,
  alert: false,
  delay: channel === 1 ? 4 : 5,
  latch: false,
  mode: 'momentary',
  signal: false,
});

const stateSnapshot = () => ({
  device: {
    uuid: '11111111-2222-3333-4444-555555555555',
    network: { wifi_sta_connected: true, wifi_sta_ip: '192.168.1.115' },
  },
  server: {
    url: 'https://open-automation.org/devices',
    host: 'open-automation.org',
    port: '443',
    requireReachable: false,
  },
  system: {
    uptimeSeconds: 123,
    freeHeap: 100000,
    minFreeHeap: 90000,
    largestFreeBlock: 60000,
    firmware: {
      gitBranch: 'test',
      gitCommit: 'abcdef0',
      rollbackEnabled: true,
      otaPartitionCount: 2,
      runningPartition: { label: 'ota_0' },
      nextUpdatePartition: { label: 'ota_1' },
      maxUploadBytes: 1900000,
      otaState: 'valid',
    },
  },
  locks: [], exits: [channelState(1), channelState(2)], fobs: [channelState(1), channelState(2)],
  keypads: [channelState(1), channelState(2)], motions: [channelState(1), channelState(2)],
  wiegand: { summary: true, registrationActive: false, registrationChannel: 0, registrationPending: 0, lastDuplicateCode: '', userCount: 0, users: [] },
  rf: { summary: true, busy: false, registrationActive: false, registrationPending: 0, lastDuplicateCode: '', userCount: 0, users: [], receiver: {} },
  enrollment: { active: false },
  wifi: { active_ssid: 'TestNet', networks: [] },
  keypadUsers: [],
  // Deliberately NO `schedules` key -- the real firmware never puts schedules on
  // /api/state, only on the dedicated /api/schedules endpoint. Regression coverage
  // for App.data.schedules surviving repeated polls that omit it entirely.
});

const SCHEDULE_DAY_KEYS = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
const defaultDays = () => {
  const days = {};
  for (const key of SCHEDULE_DAY_KEYS) days[key] = { enabled: true, start: '09:00', end: '17:00' };
  return days;
};

async function startMockServer() {
  let profiles = [];
  let assignments = {};
  let nextId = 1;
  const scheduleSnapshot = () => ({ profiles, assignments, utc_offset_seconds: -18000, utc_offset_resolved: true });

  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');

    if (url.pathname === '/api/state') { json(res, 200, stateSnapshot()); return; }
    if (url.pathname === '/api/signals') { json(res, 200, { locks: [], exits: [], fobs: [], keypads: [], motions: [], wiegand: {}, rf: {} }); return; }
    if (url.pathname === '/api/keypad/users') { json(res, 200, []); return; }
    if (url.pathname === '/api/wiegand') { json(res, 200, { registrationActive: false, registrationChannel: 0, registrationPending: 0, lastDuplicateCode: '', users: [] }); return; }
    if (url.pathname === '/api/rf') { json(res, 200, { registrationActive: false, registrationPending: 0, lastDuplicateCode: '', users: [], receiver: {} }); return; }

    if (url.pathname === '/api/schedules') {
      if (req.method === 'GET') { json(res, 200, scheduleSnapshot()); return; }
      if (req.method === 'POST') {
        const id = `id${nextId}`;
        profiles.push({ id, name: `Profile ${nextId}`, days: defaultDays() });
        nextId++;
        json(res, 200, scheduleSnapshot());
        return;
      }
      let body = '';
      req.on('data', (chunk) => { body += chunk; });
      req.on('end', () => {
        const payload = body ? JSON.parse(body) : {};
        if (req.method === 'PUT') {
          const profile = profiles.find((candidate) => candidate.id === payload.id);
          if (profile) {
            if (payload.name) profile.name = payload.name;
            if (payload.days) profile.days = payload.days;
          }
          json(res, 200, scheduleSnapshot());
          return;
        }
        if (req.method === 'DELETE') {
          profiles = profiles.filter((candidate) => candidate.id !== payload.id);
          delete assignments[payload.id];
          json(res, 200, scheduleSnapshot());
          return;
        }
        text(res, 405, 'method not allowed');
      });
      return;
    }

    const rel = url.pathname === '/' ? 'index.html' : url.pathname.replace(/^\/+/, '');
    const filePath = join(publicDir, rel);
    try {
      const st = statSync(filePath);
      if (!st.isFile()) { text(res, 404, 'not found'); return; }
      const mime = {
        '.html': 'text/html', '.css': 'text/css', '.js': 'application/javascript', '.ico': 'image/x-icon',
      }[extname(filePath)] || 'application/octet-stream';
      res.writeHead(200, { 'Content-Type': mime, 'Content-Length': st.size });
      createReadStream(filePath).pipe(res);
    } catch {
      text(res, 404, 'not found');
    }
  });

  await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));
  const { port } = server.address();
  return { baseUrl: `http://127.0.0.1:${port}`, close: () => new Promise((resolveClose) => server.close(resolveClose)) };
}

const chipTexts = (page) => page.$$eval('.schedule-profile-chip', (els) => els.map((el) => el.textContent.trim()));

export default async function run(_api, report) {
  report.startSuite(
    'Schedule Profile UI Resilience',
    'Mocked browser regression for custom schedule profiles disappearing after /api/state polls, plus profile delete'
  );

  const server = await startMockServer();
  let browser;
  let page;
  const consoleErrors = [];

  try {
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage();
    page.on('console', (msg) => {
      if (msg.type() === 'error' && !msg.text().includes('Failed to load resource')) {
        consoleErrors.push(msg.text());
      }
    });
    page.setDefaultTimeout(5000);
    // Accelerate the 30s /api/state poll so several land within this short test,
    // standing in for "some real time / user activity passes" between UI actions.
    await page.addInitScript(() => {
      const originalSetInterval = window.setInterval.bind(window);
      window.setInterval = (handler, timeout, ...args) => {
        const fasterTimeout = (typeof timeout === 'number' && timeout >= 5000) ? 300 : timeout;
        return originalSetInterval(handler, fasterTimeout, ...args);
      };
    });

    const t0 = Date.now();
    await page.goto(server.baseUrl, { waitUntil: 'domcontentloaded' });
    await page.waitForSelector('.schedule-profile-chip');
    const initialChips = await chipTexts(page);
    if (['Always', 'Day', 'Night'].every((name) => initialChips.includes(name))) {
      report.pass('Built-in schedule chips render on load', initialChips.join(', '), Date.now() - t0);
    } else {
      report.fail('Built-in schedule chips render on load', initialChips.join(', '), Date.now() - t0);
    }

    const tAdd = Date.now();
    await page.click('button[data-action="add-schedule-profile"]');
    await page.locator('.schedule-profile-chip', { hasText: 'Profile 1' }).waitFor();
    const editorVisibleAfterAdd = await page.isVisible('#scheduleProfileEditor');
    if (editorVisibleAfterAdd) {
      report.pass('Adding a profile creates and selects it', '', Date.now() - tAdd);
    } else {
      report.fail('Adding a profile creates and selects it', 'editor did not become visible', Date.now() - tAdd);
    }

    const tPoll = Date.now();
    await page.waitForTimeout(1200); // let several accelerated /api/state polls land
    const chipsAfterPolling = await chipTexts(page);
    if (chipsAfterPolling.includes('Profile 1')) {
      report.pass('Custom profile survives repeated /api/state polls', chipsAfterPolling.join(', '), Date.now() - tPoll);
    } else {
      report.fail('Custom profile survives repeated /api/state polls', chipsAfterPolling.join(', '), Date.now() - tPoll);
    }

    const tNight = Date.now();
    await page.locator('.schedule-profile-chip', { hasText: 'Night' }).click();
    await page.waitForTimeout(200);
    const chipsAfterNight = await chipTexts(page);
    if (chipsAfterNight.includes('Profile 1')) {
      report.pass('Custom profile stays listed after selecting a built-in schedule', chipsAfterNight.join(', '), Date.now() - tNight);
    } else {
      report.fail('Custom profile stays listed after selecting a built-in schedule', chipsAfterNight.join(', '), Date.now() - tNight);
    }

    const tBuiltinDelete = Date.now();
    const builtinHasDeleteButton = await page.isVisible('button[data-action="delete-schedule-profile"]');
    if (!builtinHasDeleteButton) {
      report.pass('Built-in schedules do not expose a delete button', '', Date.now() - tBuiltinDelete);
    } else {
      report.fail('Built-in schedules do not expose a delete button', 'delete button was visible for a built-in', Date.now() - tBuiltinDelete);
    }

    const tSelect = Date.now();
    await page.locator('.schedule-profile-chip', { hasText: 'Profile 1' }).click();
    await page.waitForTimeout(200);
    const deleteButtonVisible = await page.isVisible('button[data-action="delete-schedule-profile"]');
    if (deleteButtonVisible) {
      report.pass('Selecting a custom profile shows its delete button', '', Date.now() - tSelect);
    } else {
      report.fail('Selecting a custom profile shows its delete button', 'delete button not visible', Date.now() - tSelect);
    }

    const tDelete = Date.now();
    page.once('dialog', async (dialog) => { await dialog.accept(); });
    await page.click('button[data-action="delete-schedule-profile"]');
    await page.waitForFunction(() => !Array.from(document.querySelectorAll('.schedule-profile-chip')).some((el) => el.textContent.includes('Profile 1')));
    const editorHiddenAfterDelete = await page.isHidden('#scheduleProfileEditor');
    if (editorHiddenAfterDelete) {
      report.pass('Deleting the selected custom profile removes it and closes the editor', '', Date.now() - tDelete);
    } else {
      report.fail('Deleting the selected custom profile removes it and closes the editor', 'editor still visible', Date.now() - tDelete);
    }

    const tStayGone = Date.now();
    await page.waitForTimeout(900); // more accelerated polls
    await page.locator('.schedule-profile-chip', { hasText: 'Day' }).click();
    await page.waitForTimeout(200);
    const chipsAfterDelete = await chipTexts(page);
    if (!chipsAfterDelete.includes('Profile 1')) {
      report.pass('Deleted profile does not resurrect after further polling/selection', chipsAfterDelete.join(', '), Date.now() - tStayGone);
    } else {
      report.fail('Deleted profile does not resurrect after further polling/selection', chipsAfterDelete.join(', '), Date.now() - tStayGone);
    }

    if (consoleErrors.length === 0) {
      report.pass('Schedule profile UI has no browser console errors', '', 0);
    } else {
      report.fail('Schedule profile UI has no browser console errors', consoleErrors.join('\n'), 0);
    }
  } catch (error) {
    report.fail('Schedule profile UI resilience harness', error.stack || error.message, 0);
  } finally {
    if (browser) await browser.close();
    await server.close();
    report.endSuite();
  }

  return {};
}

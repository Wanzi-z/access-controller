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
    network: {
      wifi_sta_connected: true,
      wifi_sta_ip: '192.168.1.115',
      wifi_sta_quality: 80,
      wifi_sta_rssi: -52,
      wifi_sta_gateway: '192.168.1.1',
      wifi_sta_mac: 'aa:bb:cc:dd:ee:ff',
      wifi_sta_bssid: '11:22:33:44:55:66',
      wifi_sta_channel: 6,
      wifi_sta_auth: 'WPA2',
    },
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
  locks: [
    { channel: 1, enable: false, arm: false, polarity: false, contact: false, sense: false, enableContactAlert: false },
    { channel: 2, enable: false, arm: false, polarity: false, contact: false, sense: false, enableContactAlert: false },
  ],
  exits: [channelState(1), channelState(2)],
  fobs: [channelState(1), channelState(2)],
  keypads: [channelState(1), channelState(2)],
  motions: [channelState(1), channelState(2)],
  wiegand: {
    summary: true,
    registrationActive: false,
    registrationChannel: 0,
    registrationPending: 0,
    lastDuplicateCode: '',
    userCount: 1,
    users: [],
  },
  rf: {
    summary: true,
    busy: true,
    registrationActive: false,
    registrationPending: 0,
    lastDuplicateCode: '',
    userCount: 1,
    users: [],
    receiver: {},
  },
  enrollment: { active: false },
  wifi: { active_ssid: 'TestNet', networks: [] },
});

const keypadUsers = [
  { uuid: 'pin-user-1', name: 'Alice PIN', pin: '1234', pins: ['1234'] },
];

const wiegandState = {
  registrationActive: false,
  registrationChannel: 0,
  registrationPending: 0,
  lastDuplicateCode: '',
  users: [
    {
      id: 'card-1',
      code: '00000000000000000000101010101010',
      name: 'Alice Card',
      mode: 'momentary',
      channel: 1,
      status: 1,
      alert: true,
      sequence: 1,
      lastUsed: { age_ms: 0, unixTime: 0, used_ms: 0 },
    },
  ],
};

const rfState = {
  registrationActive: false,
  registrationPending: 0,
  lastDuplicateCode: '',
  users: [
    {
      id: 'rf-1',
      code: '1A2B3C4D',
      name: 'Garage Remote',
      mode: 'momentary',
      channel_mask: 1,
      exit_seconds: 4,
      alert: true,
      enabled: true,
      sequence: 1,
    },
  ],
  receiver: {},
};

async function startMockServer() {
  let keypadFailuresRemaining = 1;
  let credentialDetailsAvailable = true;

  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');

    if (url.pathname === '/api/state') {
      json(res, 200, stateSnapshot());
      return;
    }
    if (url.pathname === '/api/signals') {
      json(res, 200, { locks: [], exits: [], fobs: [], keypads: [], motions: [], wiegand: {}, rf: {} });
      return;
    }
    if (url.pathname === '/api/keypad/users') {
      if (keypadFailuresRemaining > 0) {
        keypadFailuresRemaining--;
        text(res, 503, 'synthetic keypad users fetch failure');
        return;
      }
      json(res, 200, keypadUsers);
      return;
    }
    if (url.pathname === '/api/wiegand') {
      if (!credentialDetailsAvailable) {
        text(res, 503, 'synthetic Wiegand detail failure');
        return;
      }
      json(res, 200, wiegandState);
      return;
    }
    if (url.pathname === '/api/rf') {
      if (!credentialDetailsAvailable) {
        text(res, 503, 'synthetic RF detail failure');
        return;
      }
      json(res, 200, rfState);
      return;
    }
    if (url.pathname === '/api/rf/rename' || url.pathname === '/api/rf/config') {
      json(res, 200, rfState);
      return;
    }

    const rel = url.pathname === '/' ? 'index.html' : url.pathname.replace(/^\/+/, '');
    const filePath = join(publicDir, rel);
    try {
      const st = statSync(filePath);
      if (!st.isFile()) {
        text(res, 404, 'not found');
        return;
      }
      const mime = {
        '.html': 'text/html',
        '.css': 'text/css',
        '.js': 'application/javascript',
        '.ico': 'image/x-icon',
      }[extname(filePath)] || 'application/octet-stream';
      res.writeHead(200, { 'Content-Type': mime, 'Content-Length': st.size });
      createReadStream(filePath).pipe(res);
    } catch {
      text(res, 404, 'not found');
    }
  });

  await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));
  const { port } = server.address();
  return {
    baseUrl: `http://127.0.0.1:${port}`,
    failCredentialDetails() {
      credentialDetailsAvailable = false;
    },
    close: () => new Promise((resolveClose) => server.close(resolveClose)),
  };
}

async function isVisible(page, selector) {
  const locator = page.locator(selector).first();
  return locator.isVisible({ timeout: 250 }).catch(() => false);
}

export default async function run(_api, report) {
  report.startSuite(
    'Credential UI Resilience',
    'Mocked browser regression for stored cards/users disappearing after summary state refreshes'
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
    await page.addInitScript(() => {
      const originalSetInterval = window.setInterval.bind(window);
      window.setInterval = (handler, timeout, ...args) => {
        const fasterTimeout = timeout === 20000 ? 250 : timeout;
        return originalSetInterval(handler, fasterTimeout, ...args);
      };
    });

    const t0 = Date.now();
    await page.goto(server.baseUrl, { waitUntil: 'domcontentloaded' });
    await page.locator('#wiegandUserList .user-name-input[value="Alice Card"]').waitFor();
    await page.locator('#rfUserList .user-name-input[value="Garage Remote"]').waitFor();
    report.pass('Detailed RFID and remote credentials render after page load', '', Date.now() - t0);

    const t1 = Date.now();
    try {
      await page.locator('#keypadUserList .user-name-input[value="Alice PIN"]').waitFor({ timeout: 3000 });
      report.pass('PIN users retry after an initial endpoint failure', '', Date.now() - t1);
    } catch (error) {
      report.fail('PIN users retry after an initial endpoint failure', error.message, Date.now() - t1);
    }

    const tIcons = Date.now();
    const credentialControlState = await page.evaluate(() => {
      const cardLists = document.querySelectorAll('#wiegandUserList, #keypadUserList, #rfUserList');
      const listText = Array.from(cardLists).map((el) => el.textContent || '').join('\n');
      return {
        actionRows: document.querySelectorAll('.credential-card .credential-card-actions').length,
        iconButtons: document.querySelectorAll('.credential-card .credential-icon-button').length,
        sectionIconButtons: document.querySelectorAll('#wiegandRemoveAllBtn.credential-icon-button, #keypadRemoveAllBtn.credential-icon-button, #rfRemoveAllBtn.credential-icon-button').length,
        enableText: document.querySelectorAll('.credential-card .card-enable-text').length,
        hasCardSaveText: /\bSave\b/.test(listText),
        hasCardDeleteText: /\bDelete\b/.test(listText),
        hasAlertText: /Alert \(beep\)/.test(listText),
        lastUsedText: document.querySelector('#wiegandUserList .credential-last-used')?.textContent?.trim() || '',
      };
    });
    if (
      credentialControlState.actionRows === 3 &&
      credentialControlState.iconButtons >= 5 &&
      credentialControlState.sectionIconButtons === 3 &&
      credentialControlState.enableText === 0 &&
      !credentialControlState.hasCardSaveText &&
      !credentialControlState.hasCardDeleteText &&
      !credentialControlState.hasAlertText &&
      credentialControlState.lastUsedText &&
      !/ago|since boot/.test(credentialControlState.lastUsedText)
    ) {
      report.pass('Credential card controls render as compact icon controls', '', Date.now() - tIcons);
    } else {
      report.fail('Credential card controls render as compact icon controls', JSON.stringify(credentialControlState), Date.now() - tIcons);
    }

    server.failCredentialDetails();
    const t2 = Date.now();
    await page.waitForTimeout(900);
    const cardVisible = await isVisible(page, '#wiegandUserList .user-name-input[value="Alice Card"]');
    const remoteVisible = await isVisible(page, '#rfUserList .user-name-input[value="Garage Remote"]');
    const pinVisible = await isVisible(page, '#keypadUserList .user-name-input[value="Alice PIN"]');
    if (cardVisible && remoteVisible && pinVisible) {
      report.pass('Summary/busy state refresh does not erase visible credential lists', '', Date.now() - t2);
    } else {
      report.fail(
        'Summary/busy state refresh does not erase visible credential lists',
        `visible: rfid=${cardVisible} remote=${remoteVisible} pin=${pinVisible}`,
        Date.now() - t2
      );
    }

    if (consoleErrors.length === 0) {
      report.pass('Credential UI has no browser console errors', '', 0);
    } else {
      report.fail('Credential UI has no browser console errors', consoleErrors.join('\n'), 0);
    }
  } catch (error) {
    report.fail('Credential UI resilience harness', error.stack || error.message, 0);
  } finally {
    if (browser) await browser.close();
    await server.close();
    report.endSuite();
  }

  return {};
}

import { createReadStream, statSync } from 'fs';
import { createServer } from 'http';
import { extname, join, resolve, dirname } from 'path';
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
  res.writeHead(status, { 'Content-Type': 'text/plain', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
};

const channelState = (channel) => ({ channel, enable: false, alert: false, delay: 4, latch: false, mode: 'momentary', signal: false });

// Starts a mock device whose keypad/wiegand/enrollment state is controlled entirely
// by the caller, so each scenario can seed exactly the "existing user" shape it needs
// to reproduce (or prove fixed) the bug where adding a second credential for an
// already-displayed user minted a disconnected duplicate instead of joining them.
async function startMockServer({ keypadUsers, wiegandUsers }) {
  const postKeypadUserCalls = [];
  const enrollmentStartCalls = [];

  const stateSnapshot = () => ({
    device: { uuid: 'dev-1', network: {} },
    server: {},
    system: { uptimeSeconds: 10, firmware: {} },
    locks: [],
    exits: [channelState(1), channelState(2)],
    fobs: [channelState(1), channelState(2)],
    keypads: [channelState(1), channelState(2)],
    motions: [channelState(1), channelState(2)],
    wiegand: { registrationActive: false, registrationChannel: 0, registrationPending: 0, lastDuplicateCode: '', users: wiegandUsers, pinEntries: [], devices: [] },
    rf: { registrationActive: false, registrationPending: 0, lastDuplicateCode: '', users: [], receiver: {} },
    enrollment: { active: false },
    wifi: { active_ssid: '', networks: [] },
  });

  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    let bodyChunks = [];
    req.on('data', (chunk) => bodyChunks.push(chunk));
    req.on('end', () => {
      let body = {};
      try { body = bodyChunks.length ? JSON.parse(Buffer.concat(bodyChunks).toString()) : {}; } catch { /* ignore */ }

      if (url.pathname === '/api/state') return json(res, 200, stateSnapshot());
      if (url.pathname === '/api/signals') return json(res, 200, { locks: [], exits: [], fobs: [], keypads: [], motions: [], wiegand: {}, rf: {} });
      if (url.pathname === '/api/keypad/users' && req.method === 'GET') return json(res, 200, keypadUsers);
      if (url.pathname === '/api/wiegand' && req.method === 'GET') return json(res, 200, stateSnapshot().wiegand);
      if (url.pathname === '/api/rf' && req.method === 'GET') return json(res, 200, stateSnapshot().rf);

      if (url.pathname === '/api/keypad/user' && req.method === 'POST') {
        postKeypadUserCalls.push(body);
        const uuid = `minted-${postKeypadUserCalls.length}`;
        keypadUsers.push({ uuid, name: body.name, pin: body.pin || '', pins: body.pin ? [body.pin] : [], enabled: true, mode: 'momentary', channel_mask: 1, keypad_mask: 3, exit_seconds: 4, alert: true, alert_target: 7 });
        return json(res, 200, keypadUsers);
      }
      if (url.pathname === '/api/enrollment/start' && req.method === 'POST') {
        enrollmentStartCalls.push(body);
        return json(res, 200, { active: true, userUuid: body.userUuid, userName: 'user' });
      }
      if (url.pathname === '/api/enrollment/stop' && req.method === 'POST') {
        return json(res, 200, { active: false });
      }

      const rel = url.pathname === '/' ? 'index.html' : url.pathname.replace(/^\/+/, '');
      const filePath = join(publicDir, rel);
      try {
        const st = statSync(filePath);
        if (!st.isFile()) return text(res, 404, 'not found');
        const mime = { '.html': 'text/html', '.css': 'text/css', '.js': 'application/javascript', '.ico': 'image/x-icon' }[extname(filePath)] || 'application/octet-stream';
        res.writeHead(200, { 'Content-Type': mime, 'Content-Length': st.size });
        createReadStream(filePath).pipe(res);
      } catch {
        text(res, 404, 'not found');
      }
    });
  });

  await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));
  const { port } = server.address();
  return {
    baseUrl: `http://127.0.0.1:${port}`,
    postKeypadUserCalls,
    enrollmentStartCalls,
    close: () => new Promise((resolveClose) => server.close(resolveClose)),
  };
}

export default async function run(_api, report) {
  report.startSuite(
    'Enrollment User Matching',
    'Adding a second credential to an already-displayed user must join that user, not mint a disconnected duplicate'
  );

  // ---- Scenario A: user exists ONLY via an RFID card (no PIN/keypad record yet) ----
  {
    const keypadUsers = [];
    const wiegandUsers = [{ id: 'w1', name: 'Default User', userUuid: '', userName: 'Default User', code: '1010', channel: 1, status: 1, alert: true, alert_target: 7, channel_mask: 3, mode: 'momentary' }];
    const server = await startMockServer({ keypadUsers, wiegandUsers });
    let browser;
    try {
      browser = await chromium.launch({ headless: true });
      const page = await browser.newPage();
      page.setDefaultTimeout(5000);

      const t0 = Date.now();
      await page.goto(server.baseUrl, { waitUntil: 'domcontentloaded' });
      await page.waitForSelector('#enrollUserSelect option', { state: 'attached' });
      const optionTexts = await page.locator('#enrollUserSelect option').allTextContents();
      if (optionTexts.includes('Default User') && optionTexts.length === 1) {
        report.pass('RFID-only user appears as a real, selectable enrollment option', '', Date.now() - t0);
      } else {
        report.fail('RFID-only user appears as a real, selectable enrollment option', JSON.stringify(optionTexts), Date.now() - t0);
      }

      const t1 = Date.now();
      await page.click('#enrollStartBtn');
      await page.waitForTimeout(400);
      const mintedOnce = server.postKeypadUserCalls.length === 1;
      const started = server.enrollmentStartCalls.length === 1 && !!server.enrollmentStartCalls[0].userUuid;
      const uuidsMatch = keypadUsers[0]?.uuid === server.enrollmentStartCalls[0]?.userUuid;
      if (mintedOnce && started && uuidsMatch) {
        report.pass('Adding a credential to that user reuses one real, persistent uuid', '', Date.now() - t1);
      } else {
        report.fail(
          'Adding a credential to that user reuses one real, persistent uuid',
          `mintedCalls=${server.postKeypadUserCalls.length} startCalls=${JSON.stringify(server.enrollmentStartCalls)}`,
          Date.now() - t1
        );
      }
    } catch (error) {
      report.fail('Enrollment user matching (RFID-only user) harness', error.stack || error.message, 0);
    } finally {
      if (browser) await browser.close();
      await server.close();
    }
  }

  // ---- Scenario B: user already has a real PIN/keypad record ----
  {
    const keypadUsers = [{ uuid: 'real-uuid-1', name: 'Default User', pin: '4242', pins: ['4242'], enabled: true, mode: 'momentary', channel_mask: 1, keypad_mask: 3, exit_seconds: 4, alert: true, alert_target: 7 }];
    const wiegandUsers = [];
    const server = await startMockServer({ keypadUsers, wiegandUsers });
    let browser;
    try {
      browser = await chromium.launch({ headless: true });
      const page = await browser.newPage();
      page.setDefaultTimeout(5000);

      await page.goto(server.baseUrl, { waitUntil: 'domcontentloaded' });
      await page.waitForSelector('#enrollUserSelect option', { state: 'attached' });

      const t0 = Date.now();
      await page.click('#enrollStartBtn');
      await page.waitForTimeout(300);
      if (server.postKeypadUserCalls.length === 0 && server.enrollmentStartCalls[0]?.userUuid === 'real-uuid-1') {
        report.pass('Selecting an existing user from the dropdown reuses its uuid (no duplicate minted)', '', Date.now() - t0);
      } else {
        report.fail(
          'Selecting an existing user from the dropdown reuses its uuid (no duplicate minted)',
          `mintedCalls=${server.postKeypadUserCalls.length} startCalls=${JSON.stringify(server.enrollmentStartCalls)}`,
          Date.now() - t0
        );
      }

      // A real user stops listening (hiding Start, showing Stop) before starting a
      // fresh enrollment for the next credential -- reload for a clean slate rather
      // than fighting the button's hidden/disabled transition timing in a headless test.
      await page.reload({ waitUntil: 'domcontentloaded' });
      await page.waitForSelector('#enrollUserSelect option', { state: 'attached' });

      const t1 = Date.now();
      await page.fill('#enrollNewUserName', 'Default User');
      await page.click('#enrollStartBtn');
      await page.waitForTimeout(300);
      const stillOneUser = keypadUsers.filter((u) => u.name === 'Default User').length === 1;
      const secondStartReused = server.enrollmentStartCalls[1]?.userUuid === 'real-uuid-1';
      if (server.postKeypadUserCalls.length === 0 && secondStartReused && stillOneUser) {
        report.pass('Typing an already-existing name reuses that user instead of duplicating it', '', Date.now() - t1);
      } else {
        report.fail(
          'Typing an already-existing name reuses that user instead of duplicating it',
          `mintedCalls=${server.postKeypadUserCalls.length} startCalls=${JSON.stringify(server.enrollmentStartCalls)} keypadUsers=${JSON.stringify(keypadUsers)}`,
          Date.now() - t1
        );
      }
    } catch (error) {
      report.fail('Enrollment user matching (existing PIN user) harness', error.stack || error.message, 0);
    } finally {
      if (browser) await browser.close();
      await server.close();
    }
  }

  report.endSuite();
  return {};
}

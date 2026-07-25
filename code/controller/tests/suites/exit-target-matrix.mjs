// Comprehensive Playwright confirmation:
// every one of the 8 "Exit" tiles can drive BOTH lock targets (Lock 1 and Lock 2).
// The 8 UI tiles map to 4 input services x 2 channels. For each tile we set its
// lock Target (channel_mask) and click its real Test button in the browser, then
// confirm via /api/logs that the intended lock actually fired.
import pw from '../node_modules/playwright/index.js';
const { chromium } = pw;

const BASE = process.env.DEVICE_URL || 'http://192.168.1.115';

// tile label -> underlying service endpoint, channel, and the log source token it emits
const TILES = [
  { label: 'Exit 1', enableId: 'enableExit_1',    endpoint: 'exit',   channel: 1, src: 'exit_test'   },
  { label: 'Exit 2', enableId: 'enableKeypad_1',  endpoint: 'keypad', channel: 1, src: 'kp_test'     },
  { label: 'Exit 3', enableId: 'enableFob_1',     endpoint: 'fob',    channel: 1, src: 'fob_test'    },
  { label: 'Exit 4', enableId: 'enableMotion_1',  endpoint: 'motion', channel: 1, src: 'motion_test' },
  { label: 'Exit 5', enableId: 'enableExit_2',    endpoint: 'exit',   channel: 2, src: 'exit_test'   },
  { label: 'Exit 6', enableId: 'enableKeypad_2',  endpoint: 'keypad', channel: 2, src: 'kp_test'     },
  { label: 'Exit 7', enableId: 'enableFob_2',     endpoint: 'fob',    channel: 2, src: 'fob_test'    },
  { label: 'Exit 8', enableId: 'enableMotion_2',  endpoint: 'motion', channel: 2, src: 'motion_test' },
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function api(path, opts = {}, tries = 8) {
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(`${BASE}${path}`, {
        ...opts,
        headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache' },
        signal: AbortSignal.timeout(7000),
      });
      if (r.ok) { const t = await r.text(); return t.trim() ? JSON.parse(t) : null; }
    } catch {}
    await sleep(1500);
  }
  return undefined;
}
const getLogs = () => api('/api/logs');
async function setMask(endpoint, channel, mask) {
  return api(`/api/${endpoint}`, { method: 'POST', body: JSON.stringify({ channel, channel_mask: mask }) });
}
async function enableLock(channel, enable) {
  return api('/api/lock', { method: 'POST', body: JSON.stringify({ channel, enable }) });
}

(async () => {
  console.log(`\n=== Exit→Lock target matrix on ${BASE} ===\n`);

  // 1) Enable BOTH locks so both targets are valid.
  await enableLock(1, true); await sleep(400);
  await enableLock(2, true); await sleep(400);
  const st = await api('/api/state');
  const locks = (st && st.locks) || [];
  console.log('Locks enabled:', locks.map((l) => `Lock${l.channel}=${l.enable}`).join(' '));
  console.log('');

  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  page.setDefaultTimeout(20000);
  await page.goto(BASE, { waitUntil: 'domcontentloaded', timeout: 30000 });
  await page.waitForSelector('.nav-item[data-target="device"]', { timeout: 20000 });

  const results = [];
  for (const tile of TILES) {
    const section = page.locator('.control-section', { has: page.locator(`#${tile.enableId}`) });
    const testBtn = section.locator('.card-test-toggle');
    for (const target of [1, 2]) {
      const label = `${tile.label} → Lock${target}`;
      try {
        await setMask(tile.endpoint, tile.channel, target);   // point this input at Lock<target>
        await sleep(500);
        // Global baseline: ignore everything already in the log ring before this click.
        const before = await getLogs();
        const bmax = Math.max(0, ...((before || []).map((e) => e.timestamp)));
        await testBtn.click();                                 // real UI Test button
        // Only this tile's *_test source can appear (only one tile was clicked); the
        // async *_auto re-arm is excluded because it carries a different source token.
        let firedLock = null;
        for (let k = 0; k < 9 && firedLock === null; k++) {
          await sleep(1200);
          const now = await getLogs();
          const fresh = (now || []).filter((e) => e.timestamp > bmax && (e.message || '').includes(`[${tile.src}]`));
          const nums = fresh.map((e) => ((e.message.match(/Lock(\d)\[/) || [])[1])).filter(Boolean);
          if (nums.length) firedLock = nums[nums.length - 1];
        }
        const ok = firedLock === String(target);
        const note = firedLock ? `fired Lock${firedLock}` : 'no test-fire seen';
        results.push({ label, ok, note });
        console.log(`${ok ? '✅' : '❌'} ${label.padEnd(18)} — ${note}`);
        await sleep(4500);   // let the momentary re-arm complete before the next firing
      } catch (e) {
        results.push({ label, ok: false, note: 'ERROR ' + e.message });
        console.log(`❌ ${label.padEnd(18)} — ERROR ${e.message}`);
      }
    }
  }

  // restore channel-1 tiles → Lock1, channel-2 tiles → Lock2 (original defaults)
  for (const tile of TILES) await setMask(tile.endpoint, tile.channel, tile.channel);

  await browser.close();
  const pass = results.filter((r) => r.ok).length;
  console.log(`\n=== RESULT: ${pass}/${results.length} target-firings confirmed ===`);
  console.log(pass === results.length ? 'ALL 8 EXITS CONTROL BOTH TARGETS ✅' : 'Some firings failed — see above');
  console.log('DONE_MATRIX');
  process.exit(pass === results.length ? 0 : 2);
})().catch((e) => { console.error('FATAL', e.message); process.exit(1); });

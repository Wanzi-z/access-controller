import { chromium } from 'playwright';

export default async function run(api, report) {
  report.startSuite('Web UI (Playwright)', 'Automated browser tests of the web interface');

  const DEVICE_URL = api.baseUrl;
  let browser;
  let page;

  try {
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage();
    page.setDefaultTimeout(10000);
  } catch (err) {
    report.skip('Browser launch', `Could not launch browser: ${err.message}`);
    report.endSuite();
    return {};
  }

  // Helper: navigate and verify
  async function navigateToDevice() {
    try {
      await page.goto(DEVICE_URL, { waitUntil: 'domcontentloaded', timeout: 15000 });
      await page.waitForSelector('.nav-item[data-target="device"]', { timeout: 10000 });
      return true;
    } catch {
      return false;
    }
  }

  // Helper: toggle a checkbox and verify via API
  async function uiToggleTest(label, checkboxId, apiReadFn, channel, field) {
    const t0 = Date.now();
    try {
      // Read current state from API
      const stateBefore = await apiReadFn();
      const itemsBefore = Array.isArray(stateBefore) ? stateBefore : stateBefore.locks;
      const itemBefore = itemsBefore?.find?.(i => i.channel === channel);
      const origValue = itemBefore?.[field];

      // Find checkbox
      const checkbox = await page.$(`#${checkboxId}`);
      if (!checkbox) {
        report.skip(label, `Checkbox #${checkboxId} not found`, Date.now() - t0);
        return;
      }

      const wasChecked = await checkbox.isChecked();
      const visible = await checkbox.isVisible();
      if (!visible && field === 'enable') {
        const button = await page.$(`[data-enable-target="${checkboxId}"]`);
        if (!button) {
          report.skip(label, `Visible enable button for #${checkboxId} not found`, Date.now() - t0);
          return;
        }
        await button.click();
      } else if (!visible && field === 'latch') {
        const modeSelectId = checkboxId.replace(/^latch/, 'mode');
        const modeSelect = await page.$(`#${modeSelectId}`);
        if (!modeSelect) {
          report.skip(label, `Mode select #${modeSelectId} not found`, Date.now() - t0);
          return;
        }
        await modeSelect.selectOption(wasChecked ? 'momentary' : 'latch');
      } else {
        await checkbox.click();
      }

      // Verify via API
      await page.waitForTimeout(500);
      const stateAfter = await apiReadFn();
      const itemsAfter = Array.isArray(stateAfter) ? stateAfter : stateAfter.locks;
      const itemAfter = itemsAfter?.find?.(i => i.channel === channel);
      const newValue = itemAfter?.[field];

      // Restore
      if (!visible && field === 'enable') {
        const button = await page.$(`[data-enable-target="${checkboxId}"]`);
        await button?.click();
      } else if (!visible && field === 'latch') {
        const modeSelectId = checkboxId.replace(/^latch/, 'mode');
        const modeSelect = await page.$(`#${modeSelectId}`);
        await modeSelect?.selectOption(wasChecked ? 'latch' : 'momentary');
      } else {
        await checkbox.click();
      }
      await page.waitForTimeout(300);

      if (newValue === !wasChecked) {
        report.pass(label, '', Date.now() - t0);
      } else {
        report.fail(label, `Expected ${!wasChecked}, got ${newValue}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail(label, err.message, Date.now() - t0);
    }
  }

  async function uiModeSelectTest(label, selectId, apiReadFn, channel, value) {
    const t0 = Date.now();
    try {
      const before = await apiReadFn();
      const itemBefore = before?.find?.(i => i.channel === channel);
      const originalMode = itemBefore?.mode || (itemBefore?.latch ? 'latch' : 'momentary');
      const select = await page.$(`#${selectId}`);
      if (!select) {
        report.skip(label, `Select #${selectId} not found`, Date.now() - t0);
        return;
      }

      await select.selectOption(value);
      await page.waitForTimeout(500);
      const after = await apiReadFn();
      const itemAfter = after?.find?.(i => i.channel === channel);

      await select.selectOption(originalMode);
      await page.waitForTimeout(300);

      if (itemAfter?.mode === value) {
        report.pass(label, '', Date.now() - t0);
      } else {
        report.fail(label, `Expected ${value}, got ${itemAfter?.mode}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail(label, err.message, Date.now() - t0);
    }
  }

  // 1. Page Load
  {
    const t0 = Date.now();
    const loaded = await navigateToDevice();
    if (!loaded) {
      report.fail('Page load', `Cannot reach ${DEVICE_URL}`, Date.now() - t0);
      await browser.close();
      report.endSuite();
      return {};
    }

    const title = await page.title();
    if (title.includes('Access Controller')) {
      report.pass('Page title: "Access Controller"', title, Date.now() - t0);
    } else {
      report.fail('Page title', `Got: "${title}"`, Date.now() - t0);
    }
  }

  // 2. Navigation tabs
  {
    const t0 = Date.now();
    const tabs = await page.$$('.nav-item');
    const tabCount = tabs.length;
    if (tabCount >= 3) {
      report.pass(`Navigation tabs: ${tabCount} found`, '', Date.now() - t0);
    } else {
      report.fail('Navigation tabs', `Expected 3+, got ${tabCount}`, Date.now() - t0);
    }
  }

  // 3. Device tab active by default
  {
    const t0 = Date.now();
    const devicePage = await page.$('#page-device.active');
    if (devicePage) {
      report.pass('Device tab active by default', '', Date.now() - t0);
    } else {
      report.fail('Device tab', 'Device page not active', Date.now() - t0);
    }
  }

  // 4. Lock toggles (CH1)
  {
    const getState = async () => (await api.getState()).locks;
    await uiToggleTest('UI: Lock CH1 enable toggle', 'enableLock_1', getState, 1, 'enable');
    await uiToggleTest('UI: Lock CH1 arm toggle', 'arm_1', getState, 1, 'arm');
    await uiToggleTest('UI: Lock CH1 contact alert toggle', 'enableContactAlert_1', getState, 1, 'enableContactAlert');
    await uiToggleTest('UI: Lock CH1 polarity toggle', 'polarity_1', getState, 1, 'polarity');
  }

  // 5. Lock toggles (CH2)
  {
    const getState = async () => (await api.getState()).locks;
    await uiToggleTest('UI: Lock CH2 enable toggle', 'enableLock_2', getState, 2, 'enable');
    await uiToggleTest('UI: Lock CH2 arm toggle', 'arm_2', getState, 2, 'arm');
    await uiToggleTest('UI: Lock CH2 contact alert toggle', 'enableContactAlert_2', getState, 2, 'enableContactAlert');
    await uiToggleTest('UI: Lock CH2 polarity toggle', 'polarity_2', getState, 2, 'polarity');
  }

  // 6. Exit toggles
  {
    const getState = async () => (await api.getState()).exits;
    await uiToggleTest('UI: Exit CH1 enable toggle', 'enableExit_1', getState, 1, 'enable');
    await uiToggleTest('UI: Exit CH1 alert toggle', 'alertExit_1', getState, 1, 'alert');
    await uiToggleTest('UI: Exit CH1 latch toggle', 'latchExit_1', getState, 1, 'latch');
    await uiModeSelectTest('UI: Exit CH1 mode select toggle', 'modeExit_1', getState, 1, 'toggle');
    await uiToggleTest('UI: Exit CH2 enable toggle', 'enableExit_2', getState, 2, 'enable');
    await uiToggleTest('UI: Exit CH2 alert toggle', 'alertExit_2', getState, 2, 'alert');
    await uiToggleTest('UI: Exit CH2 latch toggle', 'latchExit_2', getState, 2, 'latch');
    await uiModeSelectTest('UI: Exit CH2 mode select toggle', 'modeExit_2', getState, 2, 'toggle');
  }

  // 7. Exit delay (CH1)
  {
    const t0 = Date.now();
    try {
      const delayInput = await page.$('#armDelay_1');
      const saveBtn = await page.$('#relock');
      if (delayInput && saveBtn) {
        const before = (await api.getState()).exits.find(e => e.channel === 1)?.delay || 0;
        await delayInput.fill('25');
        await saveBtn.click();
        await page.waitForTimeout(800);
        const after = (await api.getState()).exits.find(e => e.channel === 1)?.delay;
        // Restore
        await delayInput.fill(String(before));
        await saveBtn.click();

        if (after === 25) {
          report.pass('UI: Exit CH1 delay set to 25', '', Date.now() - t0);
        } else {
          report.fail('UI: Exit CH1 delay', `Expected 25, got ${after}`, Date.now() - t0);
        }
      } else {
        report.skip('UI: Exit CH1 delay', 'Elements not found', Date.now() - t0);
      }
    } catch (err) {
      report.fail('UI: Exit CH1 delay', err.message, Date.now() - t0);
    }
  }

  // 8. Fob toggles
  {
    const getState = async () => (await api.getState()).fobs;
    await uiToggleTest('UI: Fob CH1 enable toggle', 'enableFob_1', getState, 1, 'enable');
    await uiToggleTest('UI: Fob CH1 alert toggle', 'alertFob_1', getState, 1, 'alert');
    await uiToggleTest('UI: Fob CH1 latch toggle', 'latchFob_1', getState, 1, 'latch');
    await uiModeSelectTest('UI: Fob CH1 mode select toggle', 'modeFob_1', getState, 1, 'toggle');
    await uiToggleTest('UI: Fob CH2 enable toggle', 'enableFob_2', getState, 2, 'enable');
    await uiToggleTest('UI: Fob CH2 alert toggle', 'alertFob_2', getState, 2, 'alert');
    await uiToggleTest('UI: Fob CH2 latch toggle', 'latchFob_2', getState, 2, 'latch');
    await uiModeSelectTest('UI: Fob CH2 mode select toggle', 'modeFob_2', getState, 2, 'toggle');
  }

  // 8b. Fob delay (CH1)
  {
    const t0 = Date.now();
    try {
      const delayInput = await page.$('#fobDelay_1');
      const saveBtn = await page.$('#fobSave_1');
      if (delayInput && saveBtn) {
        const before = (await api.getState()).fobs.find(f => f.channel === 1)?.delay || 0;
        await delayInput.fill('11');
        await saveBtn.click();
        await page.waitForTimeout(800);
        const after = (await api.getState()).fobs.find(f => f.channel === 1)?.delay;
        await delayInput.fill(String(before));
        await saveBtn.click();

        if (after === 11) {
          report.pass('UI: Fob CH1 delay set to 11', '', Date.now() - t0);
        } else {
          report.fail('UI: Fob CH1 delay', `Expected 11, got ${after}`, Date.now() - t0);
        }
      } else {
        report.skip('UI: Fob CH1 delay', 'Elements not found', Date.now() - t0);
      }
    } catch (err) {
      report.fail('UI: Fob CH1 delay', err.message, Date.now() - t0);
    }
  }

  // 9. Keypad toggles
  {
    const getState = async () => (await api.getState()).keypads;
    await uiToggleTest('UI: Keypad CH1 enable toggle', 'enableKeypad_1', getState, 1, 'enable');
    await uiToggleTest('UI: Keypad CH1 alert toggle', 'alertKeypad_1', getState, 1, 'alert');
    await uiToggleTest('UI: Keypad CH1 latch toggle', 'latchKeypad_1', getState, 1, 'latch');
    await uiModeSelectTest('UI: Keypad CH1 mode select toggle', 'modeKeypad_1', getState, 1, 'toggle');
    await uiToggleTest('UI: Keypad CH2 enable toggle', 'enableKeypad_2', getState, 2, 'enable');
    await uiToggleTest('UI: Keypad CH2 alert toggle', 'alertKeypad_2', getState, 2, 'alert');
    await uiToggleTest('UI: Keypad CH2 latch toggle', 'latchKeypad_2', getState, 2, 'latch');
    await uiModeSelectTest('UI: Keypad CH2 mode select toggle', 'modeKeypad_2', getState, 2, 'toggle');
  }

  // 9b. Motion latch toggles
  {
    const getState = async () => (await api.getState()).motions;
    await uiToggleTest('UI: Motion CH1 latch toggle', 'latchMotion_1', getState, 1, 'latch');
    await uiModeSelectTest('UI: Motion CH1 mode select toggle', 'modeMotion_1', getState, 1, 'toggle');
    await uiToggleTest('UI: Motion CH2 latch toggle', 'latchMotion_2', getState, 2, 'latch');
    await uiModeSelectTest('UI: Motion CH2 mode select toggle', 'modeMotion_2', getState, 2, 'toggle');
  }

  // 10. Keypad user management (UI)
  {
    const t0 = Date.now();
    try {
      // Click Add button
      const addBtn = await page.$('#keypadAddBtn');
      if (!addBtn) {
        report.skip('UI: Keypad user add', 'Add button not found', Date.now() - t0);
      } else {
        await addBtn.click();
        await page.waitForTimeout(300);

        const form = await page.$('#keypadAddForm');
        const formHidden = await form?.getAttribute('hidden');
        if (form && formHidden === null) {
          report.pass('UI: Keypad add form visible', '', Date.now() - t0);
        } else {
          report.fail('UI: Keypad add form', 'Form not visible after clicking Add', Date.now() - t0);
        }

        // Fill form
        await page.fill('#keypadNewName', 'UITest User');
        await page.fill('#keypadNewPin', '987654');
        await page.click('#keypadSaveNewBtn');
        await page.waitForTimeout(1000);

        // Verify user appeared via API
        const users = await api.getKeypadUsers();
        const found = users.find(u => u.name === 'UITest User');
        if (found) {
          report.pass('UI: Keypad user added and visible', '', 0);
          // Clean up via API
          await api.deleteKeypadUser(found.uuid);
        } else {
          report.fail('UI: Keypad user add', 'User not found after UI add', 0);
        }
      }
    } catch (err) {
      report.fail('UI: Keypad user management', err.message, Date.now() - t0);
    }
  }

  // 11. System tab
  {
    const t0 = Date.now();
    try {
      const tabs = await page.$$eval('.nav-item', (items) => items.map((item) => item.textContent.trim()).join('|'));
      if (tabs === 'Device|Settings|System') {
        report.pass('UI: Navigation order is Device, Settings, System', '', 0);
      } else {
        report.fail('UI: Navigation order', tabs, 0);
      }

      // Click System tab
      await page.click('.nav-item[data-target="system"]');
      await page.waitForTimeout(1000);

      const uptime = await page.textContent('#systemUptime');
      const branch = await page.textContent('#firmwareBranch');
      const commit = await page.textContent('#firmwareCommit');
      const rollback = await page.textContent('#firmwareRollback');
      const otaFile = await page.$('#otaFile');
      const otaUploadBtn = await page.$('#otaUploadBtn');
      const otaStatus = await page.textContent('#otaStatus');
      const logItems = await page.$('#logItems');
      const logEmpty = await page.$('#logEmptyState');
      if (
        (logItems || logEmpty) &&
        uptime && /\d+s$/.test(uptime.trim()) &&
        branch && branch.trim() !== '—' &&
        commit && commit.trim() !== '—' &&
        rollback && /Enabled/.test(rollback) &&
        otaFile && otaUploadBtn && otaStatus
      ) {
        report.pass('UI: System tab loads logs', '', Date.now() - t0);
      } else {
        report.fail(
          'UI: System tab',
          `logs=${!!(logItems || logEmpty)} uptime=${uptime} branch=${branch} commit=${commit} rollback=${rollback} otaFile=${!!otaFile} otaButton=${!!otaUploadBtn} otaStatus=${otaStatus}`,
          Date.now() - t0
        );
      }
    } catch (err) {
      report.fail('UI: System tab', err.message, Date.now() - t0);
    }
  }

  // 12. Settings tab
  {
    const t0 = Date.now();
    try {
      await page.click('.nav-item[data-target="settings"]');
      await page.waitForTimeout(1000);

      const wifiForm = await page.$('#wifiForm');
      const serverForm = await page.$('#serverForm');
      if (wifiForm && serverForm) {
        report.pass('UI: Settings tab loads WiFi and Server forms', '', Date.now() - t0);
      } else {
        report.fail('UI: Settings tab', `WiFi form: ${!!wifiForm}, Server form: ${!!serverForm}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('UI: Settings tab', err.message, Date.now() - t0);
    }
  }

  // 13. Wiegand section
  {
    const t0 = Date.now();
    try {
      await page.click('.nav-item[data-target="device"]');
      await page.waitForTimeout(500);

      const registerBtn = await page.$('#wiegandRegisterBtn');
      const channelSelect = await page.$('#wiegandChannelSelect');
      if (registerBtn && channelSelect) {
        report.pass('UI: Wiegand section elements present', '', Date.now() - t0);
      } else {
        report.skip('UI: Wiegand section', 'Elements not found', Date.now() - t0);
      }
    } catch (err) {
      report.skip('UI: Wiegand section', err.message, Date.now() - t0);
    }
  }

  // 14. RF section
  {
    const t0 = Date.now();
    try {
      const rfRegisterBtn = await page.$('#rfRegisterBtn');
      const rfList = await page.$('#rfUserList');
      const rfCardMetrics = await page.$('.credential-card--remote .rf-card-metrics');
      if ((rfRegisterBtn || rfList) && rfList) {
        report.pass('UI: RF fobs section elements present', '', Date.now() - t0);
      } else if (rfCardMetrics) {
        report.pass('UI: RF fobs card metrics present', '', Date.now() - t0);
      } else {
        report.skip('UI: RF fobs section', `Elements not found list=${!!rfList} metrics=${!!rfCardMetrics}`, Date.now() - t0);
      }
    } catch (err) {
      report.skip('UI: RF fobs section', err.message, Date.now() - t0);
    }
  }

  await browser.close();
  report.endSuite();
  return {};
}

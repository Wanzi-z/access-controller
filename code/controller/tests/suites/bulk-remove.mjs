import { chromium } from 'playwright';

const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

async function waitUntilEmpty(label, readFn, extractUsers, timeoutMs = 6000) {
  const deadline = Date.now() + timeoutMs;
  let lastCount = -1;

  while (Date.now() < deadline) {
    const state = await readFn();
    const users = extractUsers(state);
    lastCount = users.length;
    if (lastCount === 0) {
      return;
    }
    await wait(300);
  }

  throw new Error(`${label} still has ${lastCount} item(s) after remove all`);
}

async function acceptNextDialog(page) {
  page.once('dialog', async dialog => {
    await dialog.accept();
  });
}

async function clickRemoveAll(page, selector) {
  await acceptNextDialog(page);
  await page.click(selector);
}

export default async function run(api, report) {
  report.startSuite(
    'Bulk Remove UI',
    'Destructive live test: adds keypad users, clicks Remove All buttons, reloads, and verifies persisted device state'
  );

  const DEVICE_URL = api.baseUrl;
  let browser;
  let page;

  try {
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage();
    page.setDefaultTimeout(10000);
    await page.goto(DEVICE_URL, { waitUntil: 'networkidle', timeout: 15000 });
  } catch (err) {
    report.fail('Open live UI', err.message);
    if (browser) await browser.close();
    report.endSuite();
    return {};
  }

  try {
    const suffix = String(Date.now()).slice(-6);
    const created = [];
    const t0 = Date.now();

    for (let i = 0; i < 3; i++) {
      const name = `Bulk Remove Test ${suffix}-${i + 1}`;
      const pin = String(730000 + i);
      const users = await api.addKeypadUser(name, pin);
      const user = Array.isArray(users) ? users.find(u => u.name === name) : null;
      if (user?.uuid) {
        created.push(user.uuid);
      }
    }

    const beforeUsers = await api.getKeypadUsers();
    const createdCount = beforeUsers.filter(u => created.includes(u.uuid)).length;
    if (createdCount === 3) {
      report.pass('Added keypad users for remove-all test', `Total before clear: ${beforeUsers.length}`, Date.now() - t0);
    } else {
      report.fail('Added keypad users for remove-all test', `Expected 3 created users, found ${createdCount}`, Date.now() - t0);
    }

    await page.reload({ waitUntil: 'networkidle' });
    await page.waitForSelector('#keypadRemoveAllBtn');
    const keypadDisabled = await page.locator('#keypadRemoveAllBtn').isDisabled();
    if (keypadDisabled) {
      report.fail('Keypad Remove All button enabled', 'Button was disabled even though users exist');
    } else {
      report.pass('Keypad Remove All button enabled');
    }

    const clearUsersStart = Date.now();
    await clickRemoveAll(page, '#keypadRemoveAllBtn');
    await waitUntilEmpty('Keypad users', () => api.getKeypadUsers(), users => Array.isArray(users) ? users : []);
    await page.reload({ waitUntil: 'networkidle' });
    await page.waitForFunction(() => {
      const list = document.querySelector('#keypadUserList');
      const button = document.querySelector('#keypadRemoveAllBtn');
      return list && button && list.querySelectorAll('.user-row').length === 0 && button.disabled;
    });
    report.pass('Keypad Remove All clears UI and persisted users', '', Date.now() - clearUsersStart);

    const sections = [
      {
        label: 'RFID cards',
        selector: '#wiegandRemoveAllBtn',
        read: () => api.getWiegand(),
        extract: state => Array.isArray(state?.users) ? state.users : [],
        directClear: () => api.deleteAllWiegand(),
      },
      {
        label: 'Remote FOBs',
        selector: '#rfRemoveAllBtn',
        read: () => api.getRf(),
        extract: state => Array.isArray(state?.users) ? state.users : [],
        directClear: () => api.deleteAllRf(),
      },
    ];

    for (const section of sections) {
      const start = Date.now();
      const before = await section.read();
      const users = section.extract(before);
      await page.reload({ waitUntil: 'networkidle' });
      await page.waitForSelector(section.selector);

      if (users.length > 0) {
        await clickRemoveAll(page, section.selector);
        await waitUntilEmpty(section.label, section.read, section.extract);
        await page.reload({ waitUntil: 'networkidle' });
        const disabled = await page.locator(section.selector).isDisabled();
        if (disabled) {
          report.pass(`${section.label} Remove All clears UI and persisted entries`, '', Date.now() - start);
        } else {
          report.fail(`${section.label} Remove All button disabled after clear`, 'Button remained enabled after persisted state was empty', Date.now() - start);
        }
      } else {
        const disabled = await page.locator(section.selector).isDisabled();
        await section.directClear();
        await waitUntilEmpty(section.label, section.read, section.extract);
        if (disabled) {
          report.pass(`${section.label} Remove All empty-state is disabled and API clear is idempotent`, '', Date.now() - start);
        } else {
          report.fail(`${section.label} Remove All empty-state disabled`, 'Button was enabled with no entries', Date.now() - start);
        }
      }
    }
  } catch (err) {
    report.fail('Bulk Remove UI flow', err.message);
  } finally {
    if (browser) await browser.close();
    report.endSuite();
  }

  return {};
}

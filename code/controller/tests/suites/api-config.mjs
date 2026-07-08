export default async function run(api, report) {
  report.startSuite('API Configuration', 'Testing all configuration POST/PUT/DELETE endpoints');

  // Helper: toggle a setting and verify
  async function toggleAndVerify(label, readFn, writeFn, channel, field, value) {
    const t0 = Date.now();
    try {
      // Read current
      const before = await readFn();
      const itemBefore = Array.isArray(before) ? before.find(i => i.channel === channel) : null;
      const original = itemBefore?.[field];

      // Apply change
      await writeFn(channel, { [field]: value });

      // Verify
      const after = await readFn();
      const itemAfter = Array.isArray(after) ? after.find(i => i.channel === channel) : null;
      const changed = itemAfter?.[field];

      // Restore
      await writeFn(channel, { [field]: original });

      if (changed === value) {
        report.pass(label, `CH${channel} ${field}: ${original} → ${value}`, Date.now() - t0);
      } else {
        report.fail(label, `Expected ${value}, got ${changed}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail(label, err.message, Date.now() - t0);
    }
  }

  async function modeAndVerify(label, readFn, writeFn, channel, value) {
    const t0 = Date.now();
    try {
      const before = await readFn();
      const itemBefore = Array.isArray(before) ? before.find(i => i.channel === channel) : null;
      const originalMode = itemBefore?.mode || (itemBefore?.latch ? 'latch' : 'momentary');
      const originalLatch = !!itemBefore?.latch;

      await writeFn(channel, { mode: value, latch: value === 'latch' });

      const after = await readFn();
      const itemAfter = Array.isArray(after) ? after.find(i => i.channel === channel) : null;
      const changedMode = itemAfter?.mode;
      const changedLatch = itemAfter?.latch;

      await writeFn(channel, { mode: originalMode, latch: originalLatch });

      if (changedMode === value && changedLatch === (value === 'latch')) {
        report.pass(label, `CH${channel} mode: ${originalMode} → ${value}`, Date.now() - t0);
      } else {
        report.fail(label, `Expected mode=${value}, latch=${value === 'latch'}; got mode=${changedMode}, latch=${changedLatch}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail(label, err.message, Date.now() - t0);
    }
  }

  // ─── LOCK CONFIGURATION ───
  for (const ch of [1, 2]) {
    const readLocks = async () => (await api.getState()).locks;
    await toggleAndVerify(
      `Lock CH${ch} enable toggle`, readLocks, api.updateLock.bind(api), ch, 'enable', false
    );
    await toggleAndVerify(
      `Lock CH${ch} arm toggle`, readLocks, api.updateLock.bind(api), ch, 'arm', false
    );
    await toggleAndVerify(
      `Lock CH${ch} contactAlert toggle`, readLocks, api.updateLock.bind(api), ch, 'enableContactAlert', true
    );
    await toggleAndVerify(
      `Lock CH${ch} polarity toggle`, readLocks, api.updateLock.bind(api), ch, 'polarity', false
    );
  }

  // ─── EXIT CONFIGURATION ───
  for (const ch of [1, 2]) {
    const readExits = async () => (await api.getState()).exits;
    await toggleAndVerify(
      `Exit CH${ch} enable toggle`, readExits, api.updateExit.bind(api), ch, 'enable', true
    );
    await toggleAndVerify(
      `Exit CH${ch} alert toggle`, readExits, api.updateExit.bind(api), ch, 'alert', true
    );
    await modeAndVerify(
      `Exit CH${ch} mode toggle`, readExits, api.updateExit.bind(api), ch, 'toggle'
    );
    // Delay value test
    const t0 = Date.now();
    try {
      const before = await api.getState();
      const exitBefore = before.exits.find(e => e.channel === ch);
      const origDelay = exitBefore?.delay || 0;

      await api.updateExit(ch, { delay: 15 });
      const after = await api.getState();
      const exitAfter = after.exits.find(e => e.channel === ch);

      // Restore
      await api.updateExit(ch, { delay: origDelay });

      if (exitAfter?.delay === 15) {
        report.pass(`Exit CH${ch} delay set to 15`, '', Date.now() - t0);
      } else {
        report.fail(`Exit CH${ch} delay`, `Expected 15, got ${exitAfter?.delay}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail(`Exit CH${ch} delay`, err.message, Date.now() - t0);
    }
  }

  // ─── FOB CONFIGURATION ───
  for (const ch of [1, 2]) {
    const readFobs = async () => (await api.getState()).fobs;
    await toggleAndVerify(
      `Fob CH${ch} enable toggle`, readFobs, api.updateFob.bind(api), ch, 'enable', false
    );
    await toggleAndVerify(
      `Fob CH${ch} alert toggle`, readFobs, api.updateFob.bind(api), ch, 'alert', true
    );
    await toggleAndVerify(
      `Fob CH${ch} latch toggle`, readFobs, api.updateFob.bind(api), ch, 'latch', true
    );
    await modeAndVerify(
      `Fob CH${ch} mode toggle`, readFobs, api.updateFob.bind(api), ch, 'toggle'
    );
  }

  // ─── KEYPAD CONFIGURATION ───
  for (const ch of [1, 2]) {
    const readKeypads = async () => (await api.getState()).keypads;
    await toggleAndVerify(
      `Keypad CH${ch} enable toggle`, readKeypads, api.updateKeypad.bind(api), ch, 'enable', false
    );
    await toggleAndVerify(
      `Keypad CH${ch} alert toggle`, readKeypads, api.updateKeypad.bind(api), ch, 'alert', false
    );
    await modeAndVerify(
      `Keypad CH${ch} mode toggle`, readKeypads, api.updateKeypad.bind(api), ch, 'toggle'
    );
  }

  // ─── MOTION CONFIGURATION ───
  for (const ch of [1, 2]) {
    const readMotions = async () => (await api.getState()).motions;
    await modeAndVerify(
      `Motion CH${ch} mode toggle`, readMotions, api.updateMotion.bind(api), ch, 'toggle'
    );
  }

  // ─── KEYPAD USER CRUD ───
  {
    let testUuid = null;

    // Add user
    {
      const t0 = Date.now();
      try {
        await api.addKeypadUser('Test Suite User', '123456');
        const users = await api.getKeypadUsers();
        const found = users.find(u => u.name === 'Test Suite User');
        if (found) {
          testUuid = found.uuid;
          report.pass('Add keypad user', `Added "Test Suite User"`, Date.now() - t0);
        } else {
          report.fail('Add keypad user', 'User not found after add', Date.now() - t0);
        }
      } catch (err) {
        report.fail('Add keypad user', err.message, Date.now() - t0);
      }
    }

    // Update user name
    if (testUuid) {
      const t0 = Date.now();
      try {
        await api.updateKeypadUser(testUuid, 'Updated Test User');
        const after = await api.getKeypadUsers();
        const updated = after.find(u => u.uuid === testUuid);
        if (updated?.name === 'Updated Test User') {
          report.pass('Update keypad user name', '', Date.now() - t0);
        } else {
          report.fail('Update keypad user name', `Expected "Updated Test User", got "${updated?.name}"`, Date.now() - t0);
        }
      } catch (err) {
        report.fail('Update keypad user name', err.message, Date.now() - t0);
      }
    }

    // Delete user
    if (testUuid) {
      const t0 = Date.now();
      try {
        await api.deleteKeypadUser(testUuid);
        const after = await api.getKeypadUsers();
        const stillExists = after.find(u => u.uuid === testUuid);
        if (!stillExists) {
          report.pass('Delete keypad user', '', Date.now() - t0);
        } else {
          report.fail('Delete keypad user', 'User still exists after delete', Date.now() - t0);
        }
      } catch (err) {
        report.fail('Delete keypad user', err.message, Date.now() - t0);
      }
    }

    // Input validation
    {
      const t0 = Date.now();
      try {
        await api.addKeypadUser('Test Val', '123');
        report.fail('Validation: short PIN', 'Should have rejected 3-digit PIN', Date.now() - t0);
      } catch (err) {
        report.pass('Validation: short PIN rejected', '', Date.now() - t0);
      }
    }
  }

  // ─── CHANNEL ISOLATION ───
  {
    const t0 = Date.now();
    try {
      const state1 = await api.getState();
      await api.updateLock(1, { enable: false });
      const state2 = await api.getState();
      await api.updateLock(1, { enable: true }); // restore

      const ch2Before = state1.locks.find(l => l.channel === 2)?.enable;
      const ch2After = state2.locks.find(l => l.channel === 2)?.enable;
      if (ch2Before === ch2After) {
        report.pass('Channel isolation: CH1 change does not affect CH2', '', Date.now() - t0);
      } else {
        report.fail('Channel isolation', `CH2 changed from ${ch2Before} to ${ch2After}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('Channel isolation', err.message, Date.now() - t0);
    }
  }

  report.endSuite();
  return {};
}

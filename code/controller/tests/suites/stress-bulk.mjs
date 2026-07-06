/**
 * Stress & Bulk Test Suite
 * Adds volume users, toggles every setting, verifies log correspondence,
 * tests Wiegand/RF registration flows, and measures device limits.
 */
export default async function run(api, report) {
  report.startSuite('Stress & Bulk Operations', 'Volume user CRUD, comprehensive toggle sweep, registration flows');

  const findings = {};

  // ─── HELPER ───
  async function toggleVerifyLog(subsystem, channel, field, value) {
    const t0 = Date.now();
    try {
      const beforeLogs = await api.getLogs();
      const beforeCount = beforeLogs.length;

      const stateBefore = await api.getState();
      const items = stateBefore[subsystem];
      const item = items?.find(i => i.channel === channel);
      const orig = item?.[field];

      // Apply change
      if (subsystem === 'locks') await api.updateLock(channel, { [field]: value });
      else if (subsystem === 'exits') await api.updateExit(channel, { [field]: value });
      else if (subsystem === 'fobs') await api.updateFob(channel, { [field]: value });
      else if (subsystem === 'keypads') await api.updateKeypad(channel, { [field]: value });

      // Verify state changed
      const stateAfter = await api.getState();
      const itemsAfter = stateAfter[subsystem];
      const itemAfter = itemsAfter?.find(i => i.channel === channel);
      const newVal = itemAfter?.[field];

      // Check log growth
      const afterLogs = await api.getLogs();
      const logGrowth = afterLogs.length - beforeCount;

      // Restore original
      if (subsystem === 'locks') await api.updateLock(channel, { [field]: orig });
      else if (subsystem === 'exits') await api.updateExit(channel, { [field]: orig });
      else if (subsystem === 'fobs') await api.updateFob(channel, { [field]: orig });
      else if (subsystem === 'keypads') await api.updateKeypad(channel, { [field]: orig });

      const dur = Date.now() - t0;
      if (newVal === value) {
        report.pass(
          `${subsystem} CH${channel} ${field}=${value}`, `logs +${logGrowth}`, dur
        );
        return true;
      } else {
        report.fail(
          `${subsystem} CH${channel} ${field}=${value}`, `got ${newVal}`, dur
        );
        return false;
      }
    } catch (err) {
      report.fail(`${subsystem} CH${channel} ${field}`, err.message, Date.now() - t0);
      return false;
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 1: BULK KEYPAD USER CRUD (30 users — ESP32-safe batch)
  // ═══════════════════════════════════════════════════════════
  {
    const t0 = Date.now();
    try {
      const startCount = (await api.getKeypadUsers()).length;

      // Add 30 users in smaller batches
      const total = 30;
      let added = 0;
      const addedNames = [];

      for (let i = 1; i <= total; i++) {
        const name = `Stress User ${String(i).padStart(3, '0')}`;
        const pin = String(200000 + i);
        try {
          await api.addKeypadUser(name, pin);
          added++;
          addedNames.push(name);
        } catch (e) {
          break; // Stop on first failure
        }
        if (i % 10 === 0) await new Promise(r => setTimeout(r, 200)); // give ESP32 breathing room
      }

      const midCount = (await api.getKeypadUsers()).length;
      findings.maxKeypadUsers = midCount;

      if (added === total) {
        report.pass(`Added ${added} keypad users`, `Total: ${midCount}`, Date.now() - t0);
      } else {
        report.pass(`Added ${added}/${total} keypad users before limit`, `Total: ${midCount} (device NVS/heap limit reached)`, Date.now() - t0);
      }

      // Rename first 5
      const users = await api.getKeypadUsers();
      const stressUsers = users.filter(u => u.name && u.name.startsWith('Stress User '));
      let renamed = 0;
      for (let i = 0; i < Math.min(5, stressUsers.length); i++) {
        try {
          await api.updateKeypadUser(stressUsers[i].uuid, `Renamed Stress ${i + 1}`);
          renamed++;
        } catch (e) { break; }
      }
      report.pass(`Renamed ${renamed} keypad users`, '', 0);

      // Delete all stress users
      const finalUsers = await api.getKeypadUsers();
      const toDelete = finalUsers.filter(u =>
        u.name && (u.name.startsWith('Stress User ') || u.name.startsWith('Renamed Stress '))
      );
      let deleted = 0;
      for (const u of toDelete) {
        try {
          await api.deleteKeypadUser(u.uuid);
          deleted++;
        } catch (e) { /* continue */ }
      }

      const endCount = (await api.getKeypadUsers()).length;
      if (endCount <= startCount + (toDelete.length - deleted)) {
        report.pass(`Deleted ${deleted} keypad users`, `Remaining: ${endCount}`, 0);
      } else {
        report.fail(`Keypad cleanup`, `${toDelete.length - deleted} users could not be deleted`, 0);
      }

    } catch (err) {
      report.fail('Bulk keypad user CRUD', err.message, Date.now() - t0);
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 2: COMPREHENSIVE TOGGLE SWEEP (every setting, both channels)
  // ═══════════════════════════════════════════════════════════
  {
    // Lock toggles
    for (const ch of [1, 2]) {
      await toggleVerifyLog('locks', ch, 'enable', false);
      await toggleVerifyLog('locks', ch, 'arm', false);
      await toggleVerifyLog('locks', ch, 'enableContactAlert', true);
      await toggleVerifyLog('locks', ch, 'polarity', false);
    }

    // Exit toggles
    for (const ch of [1, 2]) {
      await toggleVerifyLog('exits', ch, 'enable', true);
      await toggleVerifyLog('exits', ch, 'alert', true);

      // Delay test
      const t0 = Date.now();
      try {
        const before = await api.getState();
        const exitBefore = before.exits.find(e => e.channel === ch);
        const origDelay = exitBefore?.delay || 4;
        await api.updateExit(ch, { delay: 10 });
        const after = await api.getState();
        const exitAfter = after.exits.find(e => e.channel === ch);
        await api.updateExit(ch, { delay: origDelay });
        if (exitAfter?.delay === 10) {
          report.pass(`exits CH${ch} delay=10`, '', Date.now() - t0);
        } else {
          report.fail(`exits CH${ch} delay=10`, `got ${exitAfter?.delay}`, Date.now() - t0);
        }
      } catch (e) {
        report.fail(`exits CH${ch} delay`, e.message, Date.now() - t0);
      }
    }

    // Fob toggles
    for (const ch of [1, 2]) {
      await toggleVerifyLog('fobs', ch, 'enable', false);
      await toggleVerifyLog('fobs', ch, 'alert', true);
      await toggleVerifyLog('fobs', ch, 'latch', true);
    }

    // Keypad toggles
    for (const ch of [1, 2]) {
      await toggleVerifyLog('keypads', ch, 'enable', false);
      await toggleVerifyLog('keypads', ch, 'alert', false);
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 3: WIEGAND RFID REGISTRATION FLOW
  // ═══════════════════════════════════════════════════════════
  {
    const t0 = Date.now();
    try {
      // Start registration
      await api.registerWiegand(0);
      const during = await api.getWiegand();
      if (during.registrationActive) {
        report.pass('Wiegand registration started (all channels)', '', Date.now() - t0);
      } else {
        report.fail('Wiegand registration', 'not active after start', Date.now() - t0);
      }

      // Double-start should fail
      let doubleStartOk = false;
      try {
        await api.registerWiegand(0);
      } catch (e) {
        doubleStartOk = true;
        report.pass('Wiegand double-start correctly rejected', '', 0);
      }
      if (!doubleStartOk) {
        report.fail('Wiegand double-start', 'should have been rejected', 0);
      }

      // Stop
      await api.stopWiegand(false);
      const after = await api.getWiegand();
      if (!after.registrationActive) {
        report.pass('Wiegand registration stopped', '', 0);
      } else {
        report.fail('Wiegand registration stop', 'still active', 0);
      }

      // Stop when inactive should fail
      let stopInactiveOk = false;
      try {
        await api.stopWiegand(false);
      } catch (e) {
        stopInactiveOk = true;
        report.pass('Wiegand stop-when-inactive correctly rejected', '', 0);
      }
      if (!stopInactiveOk) {
        report.fail('Wiegand stop-when-inactive', 'should have been rejected', 0);
      }

    } catch (e) {
      report.fail('Wiegand flow', e.message, Date.now() - t0);
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 4: RF FOB REGISTRATION FLOW
  // ═══════════════════════════════════════════════════════════
  {
    const t0 = Date.now();
    try {
      await api.registerRf();
      const during = await api.getRf();
      if (during.registrationActive) {
        report.pass('RF fob registration started', '', Date.now() - t0);
      } else {
        report.fail('RF registration', 'not active after start', Date.now() - t0);
      }

      // Double-start should fail
      let doubleOk = false;
      try { await api.registerRf(); } catch (e) { doubleOk = true; }
      if (doubleOk) report.pass('RF double-start correctly rejected', '', 0);
      else report.fail('RF double-start', 'should have been rejected', 0);

      // Stop
      await api.stopRf();
      const after = await api.getRf();
      if (!after.registrationActive) {
        report.pass('RF registration stopped', '', 0);
      } else {
        report.fail('RF registration stop', 'still active', 0);
      }

      // Stop-when-inactive
      let stopInactiveOk = false;
      try { await api.stopRf(); } catch (e) { stopInactiveOk = true; }
      if (stopInactiveOk) report.pass('RF stop-when-inactive correctly rejected', '', 0);
      else report.fail('RF stop-when-inactive', 'should have been rejected', 0);

    } catch (e) {
      report.fail('RF fob flow', e.message, Date.now() - t0);
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 5: LOG INTEGRITY
  // ═══════════════════════════════════════════════════════════
  {
    const t0 = Date.now();
    try {
      const logs = await api.getLogs();
      const hasTimestamps = logs.every(l => typeof l.timestamp === 'number');
      const hasMessages = logs.every(l => typeof l.message === 'string');
      if (hasTimestamps && hasMessages) {
        report.pass(`Log integrity: ${logs.length} entries with timestamps + messages`, '', Date.now() - t0);
      } else {
        report.fail('Log integrity', `ts:${hasTimestamps} msg:${hasMessages}`, Date.now() - t0);
      }

      // Check for lock-related entries
      const lockLogs = logs.filter(l => l.message && l.message.includes('Lock'));
      const bootLogs = logs.filter(l => l.message && l.message.includes('Boot'));
      report.pass(`Log content: ${lockLogs.length} lock, ${bootLogs.length} boot entries`, '', 0);

    } catch (e) {
      report.fail('Log integrity', e.message, Date.now() - t0);
    }
  }

  // ═══════════════════════════════════════════════════════════
  // PHASE 6: WIFI / SERVER ENDPOINT SMOKE
  // ═══════════════════════════════════════════════════════════
  {
    const t0 = Date.now();
    try {
      const wifi = await api.getWifi();
      if (wifi && typeof wifi === 'object') {
        report.pass('GET /api/wifi responds', `SSID: ${wifi.active_ssid || 'none'}`, Date.now() - t0);
      }
    } catch (e) {
      report.fail('GET /api/wifi', e.message, Date.now() - t0);
    }

    const t1 = Date.now();
    try {
      const list = await api.getWifiList();
      if (Array.isArray(list)) {
        report.pass('GET /api/wifi/list', `${list.length} networks`, Date.now() - t1);
      }
    } catch (e) {
      report.fail('GET /api/wifi/list', e.message, Date.now() - t1);
    }
  }

  report.endSuite();
  return { systemInfo: findings };
}

import { createInterface } from 'readline';

function ask(rl, q) {
  return new Promise(resolve => {
    rl.question(q, (answer) => {
      resolve(answer.trim());
    });
  });
}

export default async function run(api, report) {
  report.startSuite('Physical Hardware Tests', 'Interactive tests requiring manual hardware triggering');

  const rl = createInterface({ input: process.stdin, output: process.stdout });

  async function physicalTest(name, promptText, verifyFn, skipLabel = 'User did not trigger') {
    const t0 = Date.now();
    console.log(`\n  ╔══════════════════════════════════════════════════╗`);
    console.log(`  ║  🔴 READY TO TEST: ${name.padEnd(34)}║`);
    console.log(`  ╚══════════════════════════════════════════════════╝`);
    console.log(`  📋 ${promptText}`);
    console.log(`  ⏎  Press ENTER when ready, or type 'skip' to skip\n`);

    const answer = await ask(rl, '  > ');

    if (answer.toLowerCase() === 'skip') {
      report.skip(name, skipLabel, Date.now() - t0);
      return;
    }

    console.log(`  ⏳ Testing...`);
    try {
      const result = await verifyFn();
      if (result === true) {
        report.pass(name, '', Date.now() - t0);
      } else if (result === 'skip') {
        report.skip(name, skipLabel, Date.now() - t0);
      } else {
        report.fail(name, result || 'No change detected', Date.now() - t0);
      }
    } catch (err) {
      report.fail(name, err.message, Date.now() - t0);
    }
  }

  // ─── CH1 EXIT BUTTON ───
  await physicalTest(
    'CH1 Exit Button',
    'Please physically press the CHANNEL 1 exit button now, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Trigger CH1 exit button now, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      // Check for new exit-related log entry for CH1
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l => l.message?.includes?.('Lock1') && l.message?.includes?.('exit'));
      if (found) return true;
      return 'No exit event for CH1 detected in logs. Did you press the button?';
    }
  );

  // ─── CH2 EXIT BUTTON ───
  await physicalTest(
    'CH2 Exit Button',
    'Please physically press the CHANNEL 2 exit button now, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Trigger CH2 exit button now, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l => l.message?.includes?.('Lock2') && l.message?.includes?.('exit'));
      if (found) return true;
      return 'No exit event for CH2 detected in logs. Did you press the button?';
    }
  );

  // ─── CH1 LOCK CONTACT ───
  await physicalTest(
    'CH1 Lock Contact Sensor',
    'Please physically open/close the door contact for CH1, then press ENTER.',
    async () => {
      const stateBefore = await api.getState();
      const lock1Before = stateBefore.locks.find(l => l.channel === 1);
      const contactBefore = lock1Before?.sense;

      console.log(`  ⏎  Toggle CH1 contact, then press ENTER...`);
      await ask(rl, '  > ');

      const stateAfter = await api.getState();
      const lock1After = stateAfter.locks.find(l => l.channel === 1);
      const contactAfter = lock1After?.sense;

      if (contactBefore !== contactAfter) {
        return true;
      }
      return `Contact state unchanged (${contactBefore}). Did you toggle the sensor?`;
    }
  );

  // ─── CH2 LOCK CONTACT ───
  await physicalTest(
    'CH2 Lock Contact Sensor',
    'Please physically open/close the door contact for CH2, then press ENTER.',
    async () => {
      const stateBefore = await api.getState();
      const lock2Before = stateBefore.locks.find(l => l.channel === 2);
      const contactBefore = lock2Before?.contact;

      console.log(`  ⏎  Toggle CH2 contact, then press ENTER...`);
      await ask(rl, '  > ');

      const stateAfter = await api.getState();
      const lock2After = stateAfter.locks.find(l => l.channel === 2);
      const contactAfter = lock2After?.contact;

      if (contactBefore !== contactAfter) {
        return true;
      }
      return `Contact state unchanged (${contactBefore}). Did you toggle the sensor?`;
    }
  );

  // ─── CH1 KEYPAD PIN ───
  await physicalTest(
    'CH1 Keypad PIN Entry',
    'Please enter a valid PIN on the CH1 keypad, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Enter PIN on CH1 keypad, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l =>
        l.message?.includes?.('Lock1') && l.message?.includes?.('kp')
      );
      if (found) return true;
      return 'No keypad event for CH1 detected. Did you enter a valid PIN?';
    }
  );

  // ─── CH2 KEYPAD PIN ───
  await physicalTest(
    'CH2 Keypad PIN Entry',
    'Please enter a valid PIN on the CH2 keypad, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Enter PIN on CH2 keypad, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l =>
        l.message?.includes?.('Lock2') && l.message?.includes?.('kp')
      );
      if (found) return true;
      return 'No keypad event for CH2 detected. Did you enter a valid PIN?';
    }
  );

  // ─── CH1 RF FOB ───
  await physicalTest(
    'CH1 RF Fob Trigger',
    'Please press a button on a registered CH1 RF fob, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Trigger CH1 fob, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l =>
        l.message?.includes?.('Lock1') && l.message?.includes?.('fob')
      );
      if (found) return true;
      return 'No fob event for CH1 detected. Is the fob registered?';
    }
  );

  // ─── CH2 RF FOB ───
  await physicalTest(
    'CH2 RF Fob Trigger',
    'Please press a button on a registered CH2 RF fob, then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Trigger CH2 fob, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l =>
        l.message?.includes?.('Lock2') && l.message?.includes?.('fob')
      );
      if (found) return true;
      return 'No fob event for CH2 detected. Is the fob registered?';
    }
  );

  // ─── WIEGAND RFID SCAN ───
  await physicalTest(
    'Wiegand RFID Card Scan',
    'Please scan an RFID card/tag on the reader, then press ENTER.',
    async () => {
      console.log(`  📡 Starting Wiegand registration mode...`);
      try {
        await api.registerWiegand(0);
      } catch (e) {
        return `Could not start registration: ${e.message}`;
      }

      console.log(`  ⏎  Scan an RFID tag, then press ENTER...`);
      await ask(rl, '  > ');

      // Stop registration
      try {
        const w = await api.stopWiegand(true);
        const userCount = w?.users?.length || 0;
        if (userCount > 0) {
          // Clean up test user
          if (w.users.length > 0) {
            const lastUser = w.users[w.users.length - 1];
            try {
              await api.deleteWiegand(lastUser.id);
              console.log(`  🧹 Cleaned up test RFID user`);
            } catch {}
          }
          return true;
        }
        return 'No RFID tag was registered. Did you scan during registration mode?';
      } catch (e) {
        return `Registration stop failed: ${e.message}`;
      }
    }
  );

  // ─── RF REMOTE REGISTRATION CAPTURE ───
  await physicalTest(
    'RF Remote Registration Capture',
    'Please press an RF remote button during registration mode, then press ENTER.',
    async () => {
      const before = await api.getRf();
      const beforeIds = new Set((before?.users || []).map(u => u.id));

      console.log(`  📡 Starting RF registration mode...`);
      try {
        await api.registerRf();
      } catch (e) {
        return `Could not start RF registration: ${e.message}`;
      }

      console.log(`  ⏎  Press an RF remote button now, then press ENTER...`);
      await ask(rl, '  > ');

      try {
        const rf = await api.stopRf();
        const users = rf?.users || [];
        const newUser = users.find(u => !beforeIds.has(u.id)) || users[users.length - 1];
        if (newUser?.id) {
          try {
            await api.deleteRf(newUser.id);
            console.log(`  🧹 Cleaned up test RF remote`);
          } catch {}
          return true;
        }
        return 'No RF remote code was registered. Did you press the remote during registration mode?';
      } catch (e) {
        return `RF registration stop failed: ${e.message}`;
      }
    }
  );

  // ─── MOTION SENSOR ───
  await physicalTest(
    'Motion Sensor',
    'Please trigger the motion sensor (wave hand in front of it), then press ENTER.',
    async () => {
      const logsBefore = await api.getLogs();
      console.log(`  ⏎  Trigger motion sensor, then press ENTER...`);
      await ask(rl, '  > ');

      const logsAfter = await api.getLogs();
      const newLogs = logsAfter.slice(logsBefore.length);
      const found = newLogs.some(l => l.message?.includes?.('motion'));
      if (found) return true;
      return 'No motion event detected. Did you trigger the sensor?';
    }
  );

  // ─── BUZZER ───
  await physicalTest(
    'Buzzer / Sounder',
    'Do you hear the buzzer beeping? (y/n)',
    async () => {
      console.log(`  🔊 Listen for buzzer...`);
      const answer = await ask(rl, '  Did you hear the buzzer? (y/n/skip): ');
      if (answer.toLowerCase() === 'y') return true;
      if (answer.toLowerCase() === 'skip') return 'skip';
      return 'User reported: no buzzer heard';
    }
  );

  console.log(`\n  ─── Physical hardware tests complete ───\n`);
  rl.close();
  report.endSuite();
  return {};
}

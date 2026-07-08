export default async function run(api, report) {
  report.startSuite('API State & Monitoring', 'Testing all GET state/monitoring endpoints');
  let systemInfo = {};

  function verifyAccessFields(label, item) {
    const hasFields =
      typeof item.enable === 'boolean' &&
      typeof item.alert === 'boolean' &&
      typeof item.delay === 'number' &&
      typeof item.latch === 'boolean' &&
      typeof item.signal === 'boolean';

    if (hasFields) {
      report.pass(
        `${label} CH${item.channel}: signal=${item.signal} enable=${item.enable} alert=${item.alert} latch=${item.latch} delay=${item.delay}`,
        '',
        0
      );
    } else {
      report.fail(`${label} CH${item.channel} missing uniform access fields`, JSON.stringify(item), 0);
    }
  }

  // 1. GET /api/state — full system state
  {
    const t0 = Date.now();
    try {
      const state = await api.getState();
      const dur = Date.now() - t0;

      // Device
      if (!state?.device?.uuid || typeof state.device.uuid !== 'string') {
        report.fail('State has device.uuid', JSON.stringify(state?.device), dur);
      } else {
        const uuid = state.device.uuid;
        systemInfo.uuid = uuid;
        const valid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(uuid);
        if (valid) report.pass(`Device UUID: ${uuid}`, '', dur);
        else report.fail(`UUID format invalid: ${uuid}`, '', dur);
      }

      if (typeof state?.system?.uptimeSeconds === 'number' && state.system.uptimeSeconds >= 0) {
        systemInfo.uptimeSeconds = Math.floor(state.system.uptimeSeconds);
        report.pass(`System uptime: ${systemInfo.uptimeSeconds}s`, '', 0);
      } else {
        report.fail('State has system.uptimeSeconds', JSON.stringify(state?.system), 0);
      }

      const firmware = state?.system?.firmware;
      if (firmware?.gitCommit && firmware?.gitBranch) {
        systemInfo.gitCommit = firmware.gitCommit;
        systemInfo.gitBranch = firmware.gitBranch;
        report.pass(`Firmware build: ${firmware.gitBranch}@${firmware.gitCommit}`, '', 0);
      } else {
        report.fail('State has firmware git metadata', JSON.stringify(firmware), 0);
      }

      if (firmware?.rollbackEnabled === true && firmware?.otaPartitionCount >= 2) {
        report.pass(`OTA rollback enabled with ${firmware.otaPartitionCount} app slots`, '', 0);
      } else {
        report.fail('OTA rollback/app slots configured', JSON.stringify(firmware), 0);
      }

      if (firmware?.runningPartition?.label && firmware?.nextUpdatePartition?.label && firmware?.maxUploadBytes > 0) {
        report.pass(
          `OTA slots: running=${firmware.runningPartition.label} next=${firmware.nextUpdatePartition.label}`,
          '',
          0
        );
      } else {
        report.fail('OTA slot metadata present', JSON.stringify(firmware), 0);
      }

      const server = state?.server;
      if (
        server &&
        typeof server.url === 'string' &&
        typeof server.requireReachable === 'boolean'
      ) {
        report.pass(
          `Server policy: requireReachable=${server.requireReachable}`,
          server.url,
          0
        );
      } else {
        report.fail('State has server URL and connectivity policy', JSON.stringify(server), 0);
      }

      // Locks
      const locks = state?.locks;
      if (!Array.isArray(locks) || locks.length < 2) {
        report.fail('State has locks array (min 2)', `got ${locks?.length || 0}`, 0);
      } else {
        systemInfo.locks = locks.length;
        report.pass(`Locks: ${locks.length} channels`, '', 0);
        for (const lock of locks) {
          const ch = lock.channel;
          const hasFields = 'enable' in lock && 'arm' in lock && 'polarity' in lock;
          if (hasFields) report.pass(`Lock CH${ch} fields present`, '', 0);
          else report.fail(`Lock CH${ch} missing fields`, JSON.stringify(lock), 0);
        }
      }

      // Exits
      const exits = state?.exits;
      if (!Array.isArray(exits) || exits.length < 2) {
        report.fail('State has exits array (min 2)', `got ${exits?.length || 0}`, 0);
      } else {
        report.pass(`Exits: ${exits.length} channels`, '', 0);
        for (const e of exits) {
          verifyAccessFields('Exit', e);
        }
      }

      // Fobs
      const fobs = state?.fobs;
      if (!Array.isArray(fobs) || fobs.length < 2) {
        report.fail('State has fobs array (min 2)', `got ${fobs?.length || 0}`, 0);
      } else {
        report.pass(`Fobs: ${fobs.length} channels`, '', 0);
        for (const fob of fobs) {
          verifyAccessFields('FOB', fob);
        }
      }

      // Keypads
      const keypads = state?.keypads;
      if (!Array.isArray(keypads) || keypads.length < 2) {
        report.fail('State has keypads array (min 2)', `got ${keypads?.length || 0}`, 0);
      } else {
        report.pass(`Keypads: ${keypads.length} channels`, '', 0);
        for (const keypad of keypads) {
          verifyAccessFields('Keypad', keypad);
        }
      }

      // Motions
      const motions = state?.motions;
      if (!Array.isArray(motions) || motions.length < 2) {
        report.fail('State has motions array (min 2)', `got ${motions?.length || 0}`, 0);
      } else {
        report.pass(`Motions: ${motions.length} channels`, '', 0);
        for (const motion of motions) {
          verifyAccessFields('Motion', motion);
        }
      }

      // Wiegand
      if (state?.wiegand && typeof state.wiegand === 'object') {
        report.pass('Wiegand state present', '', 0);
        systemInfo.wiegandUsers = state.wiegand.users?.length || 0;
      } else {
        report.fail('Wiegand state missing', '', 0);
      }

      // RF
      if (state?.rf && typeof state.rf === 'object') {
        report.pass('RF state present', '', 0);
        systemInfo.rfUsers = state.rf.users?.length || 0;
        const receiver = state.rf.receiver;
        const hasQuality =
          receiver &&
          typeof receiver.qualityScore === 'number' &&
          typeof receiver.qualityLabel === 'string' &&
          typeof receiver.noisePercent === 'number' &&
          typeof receiver.decodeSuccessRatePercent === 'number' &&
          typeof receiver.lastRepeatCount === 'number' &&
          typeof receiver.lastJitterPercent === 'number';
        if (hasQuality) {
          report.pass(`RF quality: ${receiver.qualityLabel} ${receiver.qualityScore}/100`, '', 0);
        } else {
          report.fail('RF receiver quality metrics present', JSON.stringify(receiver), 0);
        }
      } else {
        report.fail('RF state missing', '', 0);
      }

      // WiFi state
      if (state?.wifi) {
        report.pass('WiFi state present', `SSID: ${state.wifi.active_ssid || 'none'}`, 0);
      }

    } catch (err) {
      report.fail('GET /api/state', err.message, Date.now() - t0);
    }
  }

  // 2. GET /api/logs
  {
    const t0 = Date.now();
    try {
      const logs = await api.getLogs();
      const dur = Date.now() - t0;
      if (Array.isArray(logs) && logs.length > 0) {
        const hasBoot = logs.some(l => l.message?.includes?.('Boot complete'));
        report.pass(`Logs: ${logs.length} entries`, hasBoot ? 'Contains boot event' : '', dur);
      } else {
        report.fail('Logs empty or not array', '', dur);
      }
    } catch (err) {
      report.fail('GET /api/logs', err.message, Date.now() - t0);
    }
  }

  // 3. GET /api/wiegand
  {
    const t0 = Date.now();
    try {
      const w = await api.getWiegand();
      const dur = Date.now() - t0;
      if (w && typeof w === 'object') {
        const users = w.users?.length || 0;
        report.pass(`Wiegand: ${users} registered users, regActive=${w.registrationActive}`, '', dur);
        systemInfo.wiegandUsers = users;
      } else {
        report.fail('Wiegand response invalid', '', dur);
      }
    } catch (err) {
      report.fail('GET /api/wiegand', err.message, Date.now() - t0);
    }
  }

  // 4. GET /api/rf
  {
    const t0 = Date.now();
    try {
      const r = await api.getRf();
      const dur = Date.now() - t0;
      if (r && typeof r === 'object') {
        report.pass(`RF fobs: ${r.users?.length || 0} registered`, '', dur);
        systemInfo.rfUsers = r.users?.length || 0;
        const receiver = r.receiver;
        const hasReceiverMetrics =
          receiver &&
          typeof receiver.qualityScore === 'number' &&
          typeof receiver.qualityLabel === 'string' &&
          typeof receiver.edgeRatePerSecond === 'number' &&
          typeof receiver.noiseRatePerSecond === 'number' &&
          typeof receiver.lastDecodeAgeMs === 'number' &&
          typeof receiver.lastJitterPercent === 'number';
        if (hasReceiverMetrics) {
          report.pass(`RF receiver metrics: noise=${receiver.noisePercent}% success=${receiver.decodeSuccessRatePercent}%`, '', 0);
        } else {
          report.fail('RF receiver metrics invalid', JSON.stringify(receiver), dur);
        }
      } else {
        report.fail('RF response invalid', '', dur);
      }
    } catch (err) {
      report.fail('GET /api/rf', err.message, Date.now() - t0);
    }
  }

  // 5. GET /api/keypad/users
  {
    const t0 = Date.now();
    try {
      const users = await api.getKeypadUsers();
      const dur = Date.now() - t0;
      if (Array.isArray(users)) {
        report.pass(`Keypad users: ${users.length} PIN users`, '', dur);
        systemInfo.keypadUsers = users.length;
      } else {
        report.fail('Keypad users not array', '', dur);
      }
    } catch (err) {
      report.fail('GET /api/keypad/users', err.message, Date.now() - t0);
    }
  }

  // 6. GET /api/wifi and /api/wifi/list
  {
    const t0 = Date.now();
    try {
      const wifi = await api.getWifi();
      report.pass('GET /api/wifi', `SSID: ${wifi?.active_ssid || 'none'}`, Date.now() - t0);
    } catch (err) {
      report.fail('GET /api/wifi', err.message, Date.now() - t0);
    }

    const t1 = Date.now();
    try {
      const list = await api.getWifiList();
      if (Array.isArray(list)) {
        report.pass(`WiFi list: ${list.length} saved networks`, '', Date.now() - t1);
      }
    } catch (err) {
      report.fail('GET /api/wifi/list', err.message, Date.now() - t1);
    }
  }

  // 2. GET /api/discovery — manager/client capability document
  {
    const t0 = Date.now();
    try {
      const discovery = await api.getDiscovery();
      const capabilities = discovery?.capabilities || [];
      const hasRequiredShape =
        discovery?.service === 'access-controller' &&
        discovery?.deviceKind === 'access_controller' &&
        discovery?.device?.uuid === systemInfo.uuid &&
        discovery?.api?.state === '/api/state' &&
        discovery?.api?.otaUpload === '/api/ota/upload' &&
        capabilities.includes('ota-upload') &&
        capabilities.includes('access-control');

      if (hasRequiredShape) {
        report.pass('Discovery document advertises access-controller capabilities', '', Date.now() - t0);
      } else {
        report.fail('GET /api/discovery shape', JSON.stringify(discovery), Date.now() - t0);
      }
    } catch (err) {
      report.fail('GET /api/discovery', err.message, Date.now() - t0);
    }
  }

  // 7. HTTP caching headers
  {
    const t0 = Date.now();
    try {
      const res = await fetch(`${api.baseUrl}/api/state`, {
        headers: { 'Cache-Control': 'no-cache' },
        signal: AbortSignal.timeout(5000),
      });
      const cc = res.headers.get('cache-control') || '';
      const pragma = res.headers.get('pragma') || '';
      if (cc.includes('no-store') || cc.includes('no-cache')) {
        report.pass('Cache-Control header present', cc, Date.now() - t0);
      } else {
        report.fail('Cache-Control header missing no-cache', cc, Date.now() - t0);
      }
    } catch (err) {
      report.fail('Cache headers check', err.message, Date.now() - t0);
    }
  }

  report.endSuite();
  return { systemInfo };
}

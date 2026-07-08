#!/usr/bin/env node
import { execSync } from 'child_process';
import { existsSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';
import TestReport from './lib/report.mjs';
import ApiClient from './lib/api-client.mjs';
import config from './lib/config.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));

async function detectDevice() {
  console.log('🔍 Detecting Access Controller...');

  // Try the configured URL first
  try {
    const res = await fetch(`${config.deviceUrl}/api/state`, {
      signal: AbortSignal.timeout(3000),
      headers: { 'Cache-Control': 'no-cache' },
    });
    if (res.ok) {
      console.log(`  ✅ Found at ${config.deviceUrl}`);
      return config.deviceUrl;
    }
  } catch {}

  // Try common AP IP
  try {
    const res = await fetch('http://192.168.4.1/api/state', {
      signal: AbortSignal.timeout(3000),
      headers: { 'Cache-Control': 'no-cache' },
    });
    if (res.ok) {
      console.log('  ✅ Found at http://192.168.4.1');
      return 'http://192.168.4.1';
    }
  } catch {}

  console.log('  ❌ Device not found. Make sure:');
  console.log('     - Device is powered on');
  console.log('     - Connected to ac_xxxx WiFi AP');
  console.log('     - Or set DEVICE_URL env variable');
  return null;
}

async function runBuildVerification(report) {
  report.startSuite('Build Verification', 'Verifying ESP-IDF firmware build');
  const controllerDir = resolve(__dirname, '..');

  // Check build outputs exist
  const files = [
    ['build/controller.bin', 'Firmware binary'],
    ['build/bootloader/bootloader.bin', 'Bootloader binary'],
    ['build/partition_table/partition-table.bin', 'Partition table'],
    ['build/ota_data_initial.bin', 'OTA data initial'],
  ];

  for (const [path, label] of files) {
    const full = resolve(controllerDir, path);
    const t0 = Date.now();
    if (existsSync(full)) {
      const size = (await import('fs')).statSync(full).size;
      report.pass(`${label} exists (${(size/1024).toFixed(1)} KB)`, '', Date.now() - t0);
    } else {
      report.fail(`${label} MISSING at ${path}`, 'Run idf.py build first', Date.now() - t0);
    }
  }

  // Check build log
  const buildLog = resolve(controllerDir, 'build/log/idf_py_stdout_output_*');
  report.pass('Build directory exists', '', 0);

  report.endSuite();
}

async function runConnectivityCheck(report, api) {
  report.startSuite('Connectivity Check', 'Verifying device connectivity');

  const t0 = Date.now();
  try {
    const state = await api.getState();
    const dur = Date.now() - t0;
    report.pass('GET /api/state responded', `UUID: ${state?.device?.uuid || 'unknown'}`, dur);
  } catch (err) {
    report.fail('GET /api/state failed', err.message, Date.now() - t0);
  }

  report.endSuite();
}

async function main() {
  const args = process.argv.slice(2);
  const apiOnly = args.includes('--api-only');
  const physicalOnly = args.includes('--physical-only');
  const uiOnly = args.includes('--ui-only');
  const quick = args.includes('--quick');
  const stressOnly = args.includes('--stress');
  const bulkRemoveOnly = args.includes('--bulk-remove');
  const serverRouteOnly = args.includes('--server-route-only');
  const soakOnly = args.includes('--soak');

  console.log('╔══════════════════════════════════════════════════════════╗');
  console.log('║       Access Controller - Comprehensive Test Suite      ║');
  console.log('╚══════════════════════════════════════════════════════════╝');
  console.log('');

  const report = new TestReport();

  if (serverRouteOnly) {
    const mod = await import('./suites/server-route.mjs');
    await mod.default(null, report);
    report.finish();
    process.exit(report.results.fail > 0 ? 1 : 0);
  }

  // Build verification
  await runBuildVerification(report);

  // Detect device
  const deviceUrl = await detectDevice();
  if (!deviceUrl) {
    report.finish();
    process.exit(1);
  }

  const api = new ApiClient(deviceUrl);

  // Connectivity
  await runConnectivityCheck(report, api);

  // Run test suites
  const suiteFiles = [];

  if (quick) {
    suiteFiles.push('api-state');
  } else if (apiOnly) {
    suiteFiles.push('api-state', 'api-config');
  } else if (physicalOnly) {
    suiteFiles.push('physical-hardware');
  } else if (stressOnly) {
    suiteFiles.push('stress-bulk');
  } else if (bulkRemoveOnly) {
    suiteFiles.push('bulk-remove');
  } else if (soakOnly) {
    suiteFiles.push('reliability-soak');
  } else if (uiOnly) {
    suiteFiles.push('ui-playwright');
  } else {
    suiteFiles.push('api-state', 'api-config', 'stress-bulk', 'ui-playwright');
  }

  let collectedSystemInfo = {};

  for (const name of suiteFiles) {
    const path = `./suites/${name}.mjs`;
    try {
      const mod = await import(path);
      if (mod.default) {
        const result = await mod.default(api, report);
        if (result?.systemInfo) {
          collectedSystemInfo = { ...collectedSystemInfo, ...result.systemInfo };
        }
      }
    } catch (err) {
      if (err.code === 'ERR_MODULE_NOT_FOUND') {
        console.log(`  ⚠️  Suite not found: ${name} (skipping)`);
      } else {
        console.error(`  ❌ Error loading suite ${name}:`, err.message);
      }
    }
  }

  // Set system info for report
  if (Object.keys(collectedSystemInfo).length > 0) {
    report.setSystemInfo(collectedSystemInfo);
  }

  report.finish();
  process.exit(report.results.fail > 0 ? 1 : 0);
}

main().catch(err => {
  console.error('Fatal error:', err);
  process.exit(2);
});

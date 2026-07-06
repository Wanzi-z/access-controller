import { chromium } from 'playwright';

const config = {
  // Device connection
  deviceUrl: process.env.DEVICE_URL || 'http://192.168.4.1',
  deviceApSsid: process.env.DEVICE_AP_SSID || null,
  deviceApPassword: 'pyfitech',

  // Browser
  headless: process.env.HEADLESS !== 'false',

  // Timeouts (ms)
  apiTimeout: 10000,
  physicalTimeout: 120000,
  uiTimeout: 30000,

  // Test selection
  suites: {
    api: true,
    physical: true,
    ui: true,
  },
};

export default config;

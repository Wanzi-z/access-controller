import config from '../lib/config.mjs';

function rootUrl(url) {
  const parsed = new URL(url);
  return `${parsed.protocol}//${parsed.host}`;
}

async function fetchWithTimeout(url, options = {}, timeoutMs = 10000) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(timeout);
  }
}

export default async function run(_api, report) {
  report.startSuite('Server Route', 'Public open-automation.org device route contract');

  const serverUrl = config.serverUrl;
  const origin = rootUrl(serverUrl);

  {
    const t0 = Date.now();
    try {
      const res = await fetchWithTimeout(`${origin}/api/health`);
      const text = await res.text();
      if (res.status === 200 && text.includes('"ok":true')) {
        report.pass('Public health endpoint returns 200', '', Date.now() - t0);
      } else {
        report.fail('Public health endpoint returns 200', `HTTP ${res.status}: ${text.slice(0, 200)}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('Public health endpoint returns 200', err.message, Date.now() - t0);
    }
  }

  {
    const t0 = Date.now();
    try {
      const res = await fetchWithTimeout(origin);
      const text = await res.text();
      if (res.status === 403) {
        report.pass('Public dashboard root stays blocked', '', Date.now() - t0);
      } else {
        report.fail('Public dashboard root stays blocked', `HTTP ${res.status}: ${text.slice(0, 200)}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('Public dashboard root stays blocked', err.message, Date.now() - t0);
    }
  }

  {
    const t0 = Date.now();
    const id = `route-smoke-${Date.now()}`;
    try {
      const res = await fetchWithTimeout(serverUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          id,
          name: 'Route Smoke',
          type: 'access_controller',
          capabilities: ['route-smoke'],
        }),
      });
      const text = await res.text();
      let payload = null;
      try {
        payload = JSON.parse(text);
      } catch {}
      if (res.status === 200 && payload?.ok === true && payload?.device_id === id) {
        report.pass('POST /devices accepts device punch', `device_id=${id}`, Date.now() - t0);
      } else {
        report.fail('POST /devices accepts device punch', `HTTP ${res.status}: ${text.slice(0, 300)}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('POST /devices accepts device punch', err.message, Date.now() - t0);
    }
  }

  {
    const t0 = Date.now();
    try {
      const res = await fetchWithTimeout(serverUrl);
      const contentType = res.headers.get('content-type') || '';
      const text = await res.text();
      if (res.status !== 200 && contentType.includes('application/json')) {
        report.pass('GET /devices does not expose dashboard HTML', `HTTP ${res.status}`, Date.now() - t0);
      } else {
        report.fail('GET /devices does not expose dashboard HTML', `HTTP ${res.status}, content-type=${contentType}: ${text.slice(0, 200)}`, Date.now() - t0);
      }
    } catch (err) {
      report.fail('GET /devices does not expose dashboard HTML', err.message, Date.now() - t0);
    }
  }

  report.endSuite();
  return {};
}

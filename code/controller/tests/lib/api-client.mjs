import config from './config.mjs';

class ApiClient {
  constructor(baseUrl) {
    this.baseUrl = baseUrl || config.deviceUrl;
  }

  async fetch(path, options = {}) {
    const url = `${this.baseUrl}${path}`;
    const opts = {
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache' },
      ...options,
    };
    if (opts.body && typeof opts.body === 'object') {
      opts.body = JSON.stringify(opts.body);
    }

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), config.apiTimeout);
    opts.signal = controller.signal;

    try {
      const res = await fetch(url, opts);
      clearTimeout(timeout);
      if (res.status === 204) return null;
      const text = await res.text();
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}: ${text}`);
      }
      try {
        return JSON.parse(text);
      } catch {
        return text;
      }
    } catch (err) {
      clearTimeout(timeout);
      if (err.name === 'AbortError') {
        throw new Error(`Request timed out: ${path}`);
      }
      throw err;
    }
  }

  async get(path) { return this.fetch(path); }
  async post(path, body) { return this.fetch(path, { method: 'POST', body }); }
  async put(path, body) { return this.fetch(path, { method: 'PUT', body }); }
  async del(path, body) { return this.fetch(path, { method: 'DELETE', body }); }

  // State
  async getState() { return this.get('/api/state'); }
  async getDiscovery() { return this.get('/api/discovery'); }
  async getLogs() { return this.get('/api/logs'); }

  // Locks
  async updateLock(channel, updates) {
    return this.post('/api/lock', { channel, ...updates });
  }

  // Exits
  async updateExit(channel, updates) {
    return this.post('/api/exit', { channel, ...updates });
  }

  // Fobs
  async updateFob(channel, updates) {
    return this.post('/api/fob', { channel, ...updates });
  }

  // Keypads
  async updateKeypad(channel, updates) {
    return this.post('/api/keypad', { channel, ...updates });
  }

  // Motion
  async updateMotion(channel, updates) {
    return this.post('/api/motion', { channel, ...updates });
  }

  // Keypad users (PIN)
  async getKeypadUsers() { return this.get('/api/keypad/users'); }
  async addKeypadUser(name, pin) {
    return this.post('/api/keypad/user', { name, pin });
  }
  async updateKeypadUser(uuid, name) {
    return this.put('/api/keypad/user', { uuid, name });
  }
  async deleteKeypadUser(uuid) {
    return this.del('/api/keypad/user', { uuid });
  }
  async deleteAllKeypadUsers() {
    return this.post('/api/keypad/users/delete-all', {});
  }

  // Wiegand (RFID)
  async getWiegand() { return this.get('/api/wiegand'); }
  async registerWiegand(channel) {
    return this.post('/api/wiegand/register', { channel });
  }
  async stopWiegand(promote = true) {
    return this.post('/api/wiegand/stop', { promote });
  }
  async renameWiegand(id, name) {
    return this.post('/api/wiegand/rename', { id, name });
  }
  async deleteWiegand(id) {
    return this.post('/api/wiegand/delete', { id });
  }
  async deleteAllWiegand() {
    return this.post('/api/wiegand/delete-all', {});
  }

  // RF Fobs
  async getRf() { return this.get('/api/rf'); }
  async registerRf() {
    return this.post('/api/rf/register', {});
  }
  async stopRf() {
    return this.post('/api/rf/stop', {});
  }
  async renameRf(id, name) {
    return this.post('/api/rf/rename', { id, name });
  }
  async configRf(id, mode, channel_mask, exit_seconds, alert) {
    return this.post('/api/rf/config', { id, mode, channel_mask, exit_seconds, alert });
  }
  async deleteRf(id) {
    return this.post('/api/rf/delete', { id });
  }
  async deleteAllRf() {
    return this.post('/api/rf/delete-all', {});
  }

  // WiFi
  async getWifi() { return this.get('/api/wifi'); }
  async getWifiList() { return this.get('/api/wifi/list'); }
  async addWifi(ssid, password) {
    return this.post('/api/wifi/add', { ssid, password });
  }
  async deleteWifi(ssid) {
    return this.post('/api/wifi/delete', { ssid });
  }

  // Server
  async updateServer(serverUrl, requireReachable = true) {
    return this.post('/api/server', { serverUrl, requireReachable });
  }
}

export default ApiClient;

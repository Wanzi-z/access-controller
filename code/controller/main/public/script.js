const App = {
  data: null,
  stateTimer: null,
  logsTimer: null,
  enrollmentPollTimer: null,
  wiegandPollTimer: null,
  rfPollTimer: null,
  signalPollTimer: null,
  rfTimestampTimer: null,
  uptimeTimer: null,
  systemUptimeBaseSeconds: 0,
  systemUptimeBaseMs: 0,
  rfAutoSavedIds: new Set(),
  rfAutoSavingIds: new Set(),
  toastTimer: null,
  elements: {},
  pageBoot: {
    device: false,
    system: false,
    settings: false,
  },
};

const WIEGAND_STATUS_META = {
  0: { label: 'Pending', className: 'pending' },
  1: { label: 'Active', className: 'active' },
  2: { label: 'Disabled', className: '' },
};

const ACTIVITY_HIGHLIGHT_MS = 3500;

// Convert binary string to hex
const binaryToHex = (binaryStr) => {
  if (!binaryStr || !/^[01]+$/.test(binaryStr)) {
    return binaryStr || '—';
  }
  // Pad to multiple of 4
  const padded = binaryStr.padStart(Math.ceil(binaryStr.length / 4) * 4, '0');
  let hex = '';
  for (let i = 0; i < padded.length; i += 4) {
    hex += parseInt(padded.substr(i, 4), 2).toString(16).toUpperCase();
  }
  return '0x' + hex;
};

const fetchJSON = async (path, options = {}) => {
  const href = window.location.href;
  const baseHref = href.endsWith('/') ? href : `${href}/`;
  const url = new URL(path, baseHref);
  const response = await fetch(url, {
    headers: { 'Content-Type': 'application/json' },
    cache: 'no-store',
    ...options,
  });

  if (!response.ok) {
    const message = await response.text();
    throw new Error(message || `Request failed: ${response.status}`);
  }

  if (response.status === 204) {
    return null;
  }

  const text = await response.text();
  if (!text) {
    return null;
  }
  try {
    return JSON.parse(text);
  } catch (error) {
    return text;
  }
};

const escapeHtml = (value) => {
  // Handle null, undefined, or any non-string value
  if (value == null) return '';
  const str = String(value);
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
};

const showToast = (message) => {
  const toast = App.elements.toast;
  if (!toast) return;

  toast.textContent = message;
  toast.hidden = false;
  toast.classList.add('show');

  clearTimeout(App.toastTimer);
  App.toastTimer = setTimeout(() => {
    toast.classList.remove('show');
    toast.hidden = true;
  }, 3200);
};

const handleError = (error, fallbackMessage) => {
  console.error(error);
  showToast(fallbackMessage || error.message || 'Something went wrong');
};

const formatChannelLabel = (channel) => (channel ? `Channel ${channel}` : 'All channels');

const formatUptime = (value) => {
  const totalSeconds = Math.max(0, Math.floor(Number(value) || 0));
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  const parts = [];

  if (days) parts.push(`${days}d`);
  if (hours || parts.length) parts.push(`${hours}h`);
  if (minutes || parts.length) parts.push(`${minutes}m`);
  parts.push(`${seconds}s`);

  return parts.join(' ');
};

const formatBytes = (value) => {
  const bytes = Math.max(0, Number(value) || 0);
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${bytes} B`;
};

const formatPartition = (partition = {}) => {
  if (!partition || !partition.label) return '—';
  const address = typeof partition.address === 'number'
    ? `0x${partition.address.toString(16)}`
    : '';
  return [partition.label, address].filter(Boolean).join(' · ');
};

const formatRemoteCode = (value) => {
  const code = Number(value) || 0;
  if (!code) return '—';
  return `0x${code.toString(16).toUpperCase().padStart(6, '0')}`;
};

const formatPercent = (value, digits = 0) => {
  const num = Number(value);
  if (!Number.isFinite(num)) return '—';
  return `${num.toFixed(digits)}%`;
};

const formatRate = (value) => {
  const num = Number(value);
  if (!Number.isFinite(num)) return '—';
  return `${Math.round(num)}/s`;
};

const formatAge = (ms) => {
  const value = Number(ms);
  if (!Number.isFinite(value) || value <= 0) return '—';
  if (value < 1000) return `${Math.round(value)}ms`;
  return `${(value / 1000).toFixed(value < 10000 ? 1 : 0)}s`;
};

const formatSinceBoot = (ms) => {
  const value = Number(ms);
  if (!Number.isFinite(value) || value <= 0) return '—';
  return `${formatUptime(Math.round(value / 1000))} since boot`;
};

const wifiSignalFromRssi = (rssi) => {
  const value = Number(rssi);
  if (!Number.isFinite(value)) return null;
  return Math.max(0, Math.min(100, Math.round(((value + 90) / 60) * 100)));
};

const formatWifiSignal = (network) => {
  if (!network) return '—';
  const rssi = Number(network.rssi);
  const quality = wifiSignalFromRssi(rssi);
  if (quality == null) return '—';
  return `${quality}% · ${rssi} dBm`;
};

const formatWifiLink = (qualityValue, rssiValue) => {
  const quality = Number(qualityValue);
  const rssi = Number(rssiValue);
  if (Number.isFinite(quality) && Number.isFinite(rssi)) {
    return `${Math.round(quality)}% · ${Math.round(rssi)} dBm`;
  }
  if (Number.isFinite(rssi)) {
    const derived = wifiSignalFromRssi(rssi);
    return derived == null ? `${Math.round(rssi)} dBm` : `${derived}% · ${Math.round(rssi)} dBm`;
  }
  return '—';
};

const isSameDay = (left, right) => (
  left.getFullYear() === right.getFullYear()
  && left.getMonth() === right.getMonth()
  && left.getDate() === right.getDate()
);

const formatCompactDateTime = (date) => {
  const now = new Date();
  const time = date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' });
  if (isSameDay(date, now)) {
    return time;
  }

  const dateOptions = date.getFullYear() === now.getFullYear()
    ? { month: 'short', day: 'numeric' }
    : { month: 'short', day: 'numeric', year: 'numeric' };
  const day = date.toLocaleDateString([], dateOptions);
  const shortTime = date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  return `${day} ${shortTime}`;
};

const formatRfReceivedLabel = (unixTime, receivedMs, ageMs) => {
  const age = formatAge(ageMs);
  const ageLabel = age === '—' ? 'just now' : `${age} ago`;
  const unix = Number(unixTime) || 0;
  if (unix > 0) {
    const date = new Date(unix * 1000);
    return `${formatCompactDateTime(date)} · ${ageLabel}`;
  }
  const uptimeSeconds = Math.round((Number(receivedMs) || 0) / 1000);
  return uptimeSeconds > 0 ? `${uptimeSeconds}s since boot · ${ageLabel}` : ageLabel;
};

const formatRfReceivedTitle = (unixTime, receivedMs) => {
  const unix = Number(unixTime) || 0;
  if (unix > 0) {
    return new Date(unix * 1000).toLocaleString();
  }
  const uptimeSeconds = Math.round((Number(receivedMs) || 0) / 1000);
  return uptimeSeconds > 0 ? `${uptimeSeconds}s since boot` : '';
};

const updateActivityHighlight = (card, ageMs, hasActivity = true) => {
  if (!card) return;
  const age = Number(ageMs);
  const active = hasActivity && Number.isFinite(age) && age >= 0 && age < ACTIVITY_HIGHLIGHT_MS;
  card.classList.toggle('is-activity-active', active);
  if (hasActivity && Number.isFinite(age) && age >= 0) {
    card.dataset.activityAgeMs = String(age);
    card.dataset.activityRenderedAtMs = String(Date.now());
  } else {
    delete card.dataset.activityAgeMs;
    delete card.dataset.activityRenderedAtMs;
  }
};

const refreshActivityHighlights = () => {
  document.querySelectorAll('.credential-card[data-activity-age-ms]').forEach((card) => {
    const baseAgeMs = Number(card.dataset.activityAgeMs || 0);
    const renderedAtMs = Number(card.dataset.activityRenderedAtMs || 0);
    const ageMs = baseAgeMs + Math.max(0, Date.now() - renderedAtMs);
    updateActivityHighlight(card, ageMs, true);
  });
};

const updateRfReceivedTimestamps = () => {
  document.querySelectorAll('.rf-last-received').forEach((el) => {
    const baseAgeMs = Number(el.dataset.ageMs || 0);
    const renderedAtMs = Number(el.dataset.renderedAtMs || 0);
    const ageMs = baseAgeMs + Math.max(0, Date.now() - renderedAtMs);
    el.textContent = formatRfReceivedLabel(el.dataset.unixTime, el.dataset.receivedMs, ageMs);
    const title = formatRfReceivedTitle(el.dataset.unixTime, el.dataset.receivedMs);
    if (title) {
      el.title = title;
    }
    updateActivityHighlight(el.closest('.credential-card'), ageMs, true);
  });
  refreshActivityHighlights();
};

const ensureRfTimestampTimer = () => {
  if (!App.rfTimestampTimer) {
    App.rfTimestampTimer = setInterval(updateRfReceivedTimestamps, 1000);
  }
};

const buildLastUsedMetric = (lastUsed, emptyText = 'Not used yet') => {
  if (!lastUsed) {
    return buildRfUserMetric('Last used', emptyText, true);
  }
  const ageMs = Number(lastUsed.age_ms) || 0;
  const usedMs = Number(lastUsed.used_ms) || 0;
  const unixTime = Number(lastUsed.unixTime) || 0;
  const title = formatRfReceivedTitle(unixTime, usedMs);
  return `
    <div class="rf-card-metric">
      <span class="label">Last used</span>
      <span class="value rf-last-received"
        data-unix-time="${unixTime}"
        data-received-ms="${usedMs}"
        data-age-ms="${ageMs}"
        data-rendered-at-ms="${Date.now()}"
        title="${escapeHtml(title)}">${formatRfReceivedLabel(unixTime, usedMs, ageMs)}</span>
    </div>
  `;
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const setActivePage = (targetId) => {
  App.elements.pages.forEach((section) => {
    section.classList.toggle('active', section.id === `page-${targetId}`);
  });

  App.elements.navItems.forEach((item) => {
    item.classList.toggle('active', item.dataset.target === targetId);
  });
};

const bindNavigation = () => {
  App.elements.navItems.forEach((button) => {
    button.addEventListener('click', () => {
      const target = button.dataset.target;
      setActivePage(target);
      onPageActivated(target);
    });
  });
};

const onPageActivated = (targetId) => {
  if (!targetId) return;

  if (targetId === 'device') {
    if (!App.pageBoot.device) {
      App.pageBoot.device = true;
      // Loaded on-demand to avoid spiking the ESP32 heap when opening via tunnel.
      loadKeypadUsers();
    }
  }

  if (targetId === 'system') {
    if (!App.pageBoot.system) {
      App.pageBoot.system = true;
      loadLogs();
    }
    if (!App.logsTimer) {
      App.logsTimer = setInterval(loadLogs, 30000);
    }
  } else if (App.logsTimer) {
    clearInterval(App.logsTimer);
    App.logsTimer = null;
  }

  if (targetId === 'settings') {
    if (!App.pageBoot.settings) {
      App.pageBoot.settings = true;
      loadWifiList();
    }
  }
};

const applyDeviceInfo = (device = {}) => {
  const uuidEl = document.getElementById('uuid');
  if (uuidEl) {
    uuidEl.textContent = device.uuid || '—';
  }

  const network = device.network || {};
  const setText = (id, value) => {
    const el = document.getElementById(id);
    if (el) el.textContent = value || '—';
  };

  setText('wifiStaIp', network.wifi_sta_ip);
  setText('wifiApIp', network.wifi_ap_ip);
  setText('ethIp', network.eth_ip);
  setText('wifiStaMac', network.wifi_sta_mac);
  setText('wifiApMac', network.wifi_ap_mac);
  setText('ethMac', network.eth_mac);
  setText('headerStaIp', network.wifi_sta_ip);
  setText('headerApIp', network.wifi_ap_ip);
};

const applyServerInfo = (server = {}) => {
  const serverUrl = server.url || 'https://open-automation.org/devices';
  const setText = (id, value) => {
    const el = document.getElementById(id);
    if (el) el.textContent = value || '—';
  };
  setText('systemServerUrl', serverUrl);
  setText('headerServerUrl', serverUrl);

  const input = document.getElementById('serverUrl');
  if (input && document.activeElement !== input) {
    input.value = serverUrl;
  }

  const requireInput = document.getElementById('serverRequireReachable');
  if (requireInput && document.activeElement !== requireInput) {
    requireInput.checked = server.requireReachable !== false;
  }
};

const applySystemInfo = (system = {}) => {
  const uptimeEl = document.getElementById('systemUptime');
  const uptimeSeconds = Number(system.uptimeSeconds);
  if (Number.isFinite(uptimeSeconds)) {
    App.systemUptimeBaseSeconds = Math.max(0, Math.floor(uptimeSeconds));
    App.systemUptimeBaseMs = Date.now();
  }
  if (uptimeEl) {
    uptimeEl.textContent = formatUptime(
      Number.isFinite(uptimeSeconds) ? uptimeSeconds : App.systemUptimeBaseSeconds
    );
  }

  const firmware = system.firmware || {};
  const setText = (id, value) => {
    const el = document.getElementById(id);
    if (el) el.textContent = value || '—';
  };

  setText('firmwareBranch', firmware.gitBranch);
  setText('firmwareCommit', firmware.gitCommit ? `${firmware.gitCommit}${firmware.gitDirty ? ' · dirty' : ''}` : '');
  setText('firmwareVersion', firmware.projectVersion);
  setText('firmwareSlot', `${formatPartition(firmware.runningPartition)} · ${firmware.otaState || 'unknown'}`);
  setText('firmwareNextSlot', formatPartition(firmware.nextUpdatePartition));
  setText(
    'firmwareRollback',
    firmware.rollbackEnabled
      ? `Enabled${firmware.rollbackPossible ? ' · ready' : ''}`
      : 'Disabled'
  );
};

const updateSystemUptimeClock = () => {
  if (!App.systemUptimeBaseMs) return;
  const uptimeEl = document.getElementById('systemUptime');
  if (!uptimeEl) return;
  const elapsedSeconds = Math.floor((Date.now() - App.systemUptimeBaseMs) / 1000);
  uptimeEl.textContent = formatUptime(App.systemUptimeBaseSeconds + elapsedSeconds);
};

const startUptimeClock = () => {
  if (App.uptimeTimer) return;
  updateSystemUptimeClock();
  App.uptimeTimer = setInterval(updateSystemUptimeClock, 1000);
};

const stopUptimeClock = () => {
  if (!App.uptimeTimer) return;
  clearInterval(App.uptimeTimer);
  App.uptimeTimer = null;
};

const applySignalDot = (elementId, value, activeText = 'Signal active', inactiveText = 'Signal inactive') => {
  const el = document.getElementById(elementId);
  if (!el) return;

  el.classList.remove('status-ok', 'status-alert');
  if (typeof value === 'boolean') {
    el.classList.add(value ? 'status-ok' : 'status-alert');
    el.title = value ? activeText : inactiveText;
  } else {
    el.title = 'Signal state unknown';
  }

  const section = el.closest('.control-section');
  if (section && typeof value === 'boolean') {
    section.classList.toggle('is-signal-active', value);
  }
};

const setEnableButtonState = (button, enabled) => {
  if (!button) return;
  button.classList.toggle('is-enabled', !!enabled);
  button.classList.toggle('is-disabled', !enabled);
  button.setAttribute('aria-pressed', enabled ? 'true' : 'false');
  button.title = enabled ? 'Disable' : 'Enable';
  const text = button.querySelector('.card-enable-text');
  if (text) text.textContent = enabled ? 'Enabled' : 'Disabled';
};

const setCardEnabledState = (enableId, enabled) => {
  const enableEl = document.getElementById(enableId);
  const section = enableEl?.closest('.control-section');
  const button = document.querySelector(`[data-enable-target="${enableId}"]`);
  if (enableEl) enableEl.checked = !!enabled;
  if (section) section.classList.toggle('is-card-disabled', !enabled);
  setEnableButtonState(button, !!enabled);
};

const normalizeCardMode = (mode, latch) => {
  if (mode === 'momentary' || mode === 'toggle' || mode === 'latch') return mode;
  return latch ? 'latch' : 'momentary';
};

const setCardModeState = (modeId, mode, latch) => {
  const modeEl = document.getElementById(modeId);
  if (modeEl && document.activeElement !== modeEl) {
    modeEl.value = normalizeCardMode(mode, latch);
  }
};

const applyLockState = (locks = []) => {
  locks.forEach((lock) => {
    const ch = lock.channel;
    const enableEl = document.getElementById(`enableLock_${ch}`);
    const armEl = document.getElementById(`arm_${ch}`);
    const contactEl = document.getElementById(`enableContactAlert_${ch}`);
    const polarityEl = document.getElementById(`polarity_${ch}`);
    const contactStatusEl = document.getElementById(`lockContact_${ch}`);
    const senseStatusEl = document.getElementById(`lockSense_${ch}`);

    if (enableEl) enableEl.checked = !!lock.enable;
    setCardEnabledState(`enableLock_${ch}`, !!lock.enable);
    if (armEl) armEl.checked = !!lock.arm;
    if (contactEl) contactEl.checked = !!lock.enableContactAlert;
    if (polarityEl) polarityEl.checked = !!lock.polarity;

    /* Ch1: contact state is in API "sense". Ch2: contact state is in API "contact". */
    const contactState = ch === 1 ? lock.sense : lock.contact;
    const signalState = ch === 1 ? lock.contact : lock.sense;

    if (contactStatusEl) applySignalDot(`lockContact_${ch}`, contactState, 'Contact closed', 'Contact open');
    if (senseStatusEl) applySignalDot(`lockSense_${ch}`, signalState);
  });
};

const applyExitState = (exits = []) => {
  exits.forEach((exit) => {
    const ch = exit.channel;
    const enableEl = document.getElementById(`enableExit_${ch}`);
    const alertEl = document.getElementById(`alertExit_${ch}`);
    const latchEl = document.getElementById(`latchExit_${ch}`);
    const delayEl = document.getElementById(`armDelay_${ch}`);

    if (enableEl) enableEl.checked = !!exit.enable;
    setCardEnabledState(`enableExit_${ch}`, !!exit.enable);
    if (alertEl) alertEl.checked = !!exit.alert;
    if (latchEl) latchEl.checked = !!exit.latch;
    setCardModeState(`modeExit_${ch}`, exit.mode, !!exit.latch);
    if (delayEl) delayEl.value = exit.delay ?? 0;
    applySignalDot(`exitSignal_${ch}`, exit.signal);
  });
};

const applyFobState = (fobs = []) => {
  fobs.forEach((fob) => {
    const ch = fob.channel;
    const enableEl = document.getElementById(`enableFob_${ch}`);
    const alertEl = document.getElementById(`alertFob_${ch}`);
    const latchEl = document.getElementById(`latchFob_${ch}`);
    const delayEl = document.getElementById(`fobDelay_${ch}`);

    if (enableEl) enableEl.checked = !!fob.enable;
    setCardEnabledState(`enableFob_${ch}`, !!fob.enable);
    if (alertEl) alertEl.checked = !!fob.alert;
    if (latchEl) latchEl.checked = !!fob.latch;
    setCardModeState(`modeFob_${ch}`, fob.mode, !!fob.latch);
    if (delayEl) delayEl.value = fob.delay ?? 4;
    applySignalDot(`fobSignal_${ch}`, fob.signal);
  });
};

const applyKeypadState = (keypads = []) => {
  keypads.forEach((pad) => {
    const ch = pad.channel;
    const enableEl = document.getElementById(`enableKeypad_${ch}`);
    const alertEl = document.getElementById(`alertKeypad_${ch}`);
    const latchEl = document.getElementById(`latchKeypad_${ch}`);
    const delayEl = document.getElementById(`keypadDelay_${ch}`);

    if (enableEl) enableEl.checked = !!pad.enable;
    setCardEnabledState(`enableKeypad_${ch}`, !!pad.enable);
    if (alertEl) alertEl.checked = !!pad.alert;
    if (latchEl) latchEl.checked = !!pad.latch;
    setCardModeState(`modeKeypad_${ch}`, pad.mode, !!pad.latch);
    if (delayEl) delayEl.value = pad.delay ?? 0;
    applySignalDot(`keypadSignal_${ch}`, pad.signal);
  });
};

const applyMotionState = (motions = []) => {
  motions.forEach((motion) => {
    const ch = motion.channel;
    const enableEl = document.getElementById(`enableMotion_${ch}`);
    const alertEl = document.getElementById(`alertMotion_${ch}`);
    const latchEl = document.getElementById(`latchMotion_${ch}`);
    const delayEl = document.getElementById(`motionDelay_${ch}`);

    if (enableEl) enableEl.checked = !!motion.enable;
    setCardEnabledState(`enableMotion_${ch}`, !!motion.enable);
    if (alertEl) alertEl.checked = !!motion.alert;
    if (latchEl) latchEl.checked = !!motion.latch;
    setCardModeState(`modeMotion_${ch}`, motion.mode, !!motion.latch);
    if (delayEl) delayEl.value = motion.delay ?? 4;
    applySignalDot(`motionSignal_${ch}`, motion.signal);
  });
};

const applyFastSignalState = (state = {}) => {
  (state.locks || []).forEach((lock) => {
    const ch = lock.channel;
    const contactState = ch === 1 ? lock.sense : lock.contact;
    const signalState = ch === 1 ? lock.contact : lock.sense;
    applySignalDot(`lockContact_${ch}`, contactState, 'Contact closed', 'Contact open');
    applySignalDot(`lockSense_${ch}`, signalState);
  });

  (state.exits || []).forEach((exit) => applySignalDot(`exitSignal_${exit.channel}`, exit.signal));
  (state.fobs || []).forEach((fob) => applySignalDot(`fobSignal_${fob.channel}`, fob.signal));
  (state.keypads || []).forEach((pad) => applySignalDot(`keypadSignal_${pad.channel}`, pad.signal));
  (state.motions || []).forEach((motion) => applySignalDot(`motionSignal_${motion.channel}`, motion.signal));
  applyCredentialActivityState(state);
};

const findCredentialCard = (containerId, id) => {
  const container = document.getElementById(containerId);
  if (!container || !id) return null;
  return Array.from(container.querySelectorAll('.credential-card'))
    .find((card) => card.dataset.id === String(id));
};

const applyCredentialActivityState = (state = {}) => {
  (state.wiegand?.users || []).forEach((user) => {
    const id = user.id || '';
    const card = findCredentialCard('wiegandUserList', id);
    const ageMs = Number(user.lastUsed?.age_ms);
    if (card) {
      updateActivityHighlight(card, ageMs, !!user.lastUsed);
    }
  });

  (state.rf?.users || []).forEach((user) => {
    const id = user.id || '';
    const card = findCredentialCard('rfUserList', id);
    const ageMs = Number(user.lastRx?.age_ms);
    if (card) {
      updateActivityHighlight(card, ageMs, !!user.lastRx);
    }
  });
  refreshActivityHighlights();
};

const renderCredentialEnableButton = (enabled, action, id) => `
  <button type="button"
    class="card-enable-toggle ${enabled ? 'is-enabled' : 'is-disabled'}"
    data-action="${action}"
    data-id="${escapeHtml(id || '')}"
    aria-pressed="${enabled ? 'true' : 'false'}"
    title="${enabled ? 'Disable' : 'Enable'}">
    <span class="card-enable-icon" aria-hidden="true"></span>
    <span class="card-enable-text">${enabled ? 'Enabled' : 'Disabled'}</span>
  </button>
`;

const buildWiegandUserRow = (user, existingValue) => {
  if (!user) return '';
  const meta = WIEGAND_STATUS_META[user.status] || WIEGAND_STATUS_META[2];
  const statusClass = meta.className ? `status-chip ${meta.className}` : 'status-chip';
  const rawCode = user.code || '';
  const hexCode = binaryToHex(rawCode);
  const preserved = existingValue && typeof existingValue === 'object' ? existingValue : { name: existingValue };
  // Use existing form values if user was editing, otherwise use stored config
  const name = escapeHtml(preserved.name !== undefined ? preserved.name : (user.name || ''));
  const channelNum = user.channel || 0;
  const userId = escapeHtml(user.id || '');
  const alert = preserved.alert !== undefined ? !!preserved.alert : (user.alert !== false);
  const mode = preserved.mode || user.mode || 'momentary';
  const enabled = user.status === 1;
  const metrics = `
    <div class="rf-card-metrics wiegand-card-metrics">
      ${buildLastUsedMetric(user.lastUsed)}
    </div>
  `;

  return `
    <div class="user-row credential-card credential-card--rfid ${enabled ? '' : 'is-card-disabled'}" data-id="${userId}" data-channel="${channelNum}" data-enabled="${enabled ? 'true' : 'false'}">
      <div class="credential-card-header">
        <div class="credential-card-title">
          <span class="credential-kind">RFID</span>
          <span class="user-code">${escapeHtml(hexCode)}</span>
        </div>
        ${renderCredentialEnableButton(enabled, 'toggle-wiegand-enabled', user.id || '')}
      </div>
      <div class="user-info">
        <label class="stacked">
          <span>Name</span>
          <input type="text" class="user-name-input" value="${name}" placeholder="Enter name...">
        </label>
        <div class="credential-meta-row">
          <span class="user-channel">Channel ${channelNum}</span>
          <span class="${statusClass}">${meta.label}</span>
        </div>
        <label class="stacked">
          <span>Mode</span>
          <select class="wiegand-mode-select" data-previous-value="${escapeHtml(mode)}">
            <option value="momentary" ${mode === 'momentary' ? 'selected' : ''}>Momentary</option>
            <option value="toggle" ${mode === 'toggle' ? 'selected' : ''}>Toggle</option>
            <option value="latch" ${mode === 'latch' ? 'selected' : ''}>Latch</option>
          </select>
        </label>
        ${metrics}
      </div>
      <div class="credential-card-footer">
        <label class="form-switch credential-alert-switch">
          <input type="checkbox" class="wiegand-alert-checkbox" ${alert ? 'checked' : ''}>
          <span>Alert (beep)</span>
        </label>
        <div class="user-actions">
          <button type="button" class="secondary" data-action="rename" data-id="${userId}">Save</button>
          <button type="button" class="secondary danger" data-action="delete-wiegand" data-id="${userId}">Delete</button>
        </div>
      </div>
    </div>
  `;
};

const startWiegandPolling = () => {
  if (App.wiegandPollTimer) return;
  App.wiegandPollTimer = setInterval(async () => {
    try {
      const wiegand = await fetchJSON('api/wiegand');
      if (App.data) {
        App.data.wiegand = wiegand;
      }
      renderWiegand(wiegand);
    } catch (error) {
      console.warn('Failed to refresh Wiegand state', error);
      stopWiegandPolling();
    }
  }, 2500);
};

const stopWiegandPolling = () => {
  if (App.wiegandPollTimer) {
    clearInterval(App.wiegandPollTimer);
    App.wiegandPollTimer = null;
  }
};

const startEnrollmentPolling = () => {
  if (App.enrollmentPollTimer) return;
  App.enrollmentPollTimer = setInterval(loadState, 1800);
};

const stopEnrollmentPolling = () => {
  if (App.enrollmentPollTimer) {
    clearInterval(App.enrollmentPollTimer);
    App.enrollmentPollTimer = null;
  }
};

const deleteItemsSequentially = async (items, endpoint, payloadKey) => {
  let latest = null;
  for (const item of items) {
    const id = typeof item === 'string' ? item : item?.[payloadKey];
    if (!id) continue;
    latest = await fetchJSON(endpoint, {
      method: endpoint.includes('keypad') ? 'DELETE' : 'POST',
      body: JSON.stringify({ [payloadKey]: id }),
    });
  }
  return latest;
};

const renderWiegand = (wiegand = {}) => {
  const {
    registrationActive = false,
    registrationChannel = 0,
    registrationPending = 0,
    lastDuplicateCode = '',
    users = [],
  } = wiegand;

  if (App.data) {
    App.data.wiegand = wiegand;
  }

  const statusEl = App.elements.wiegandStatus;
  const pendingEl = App.elements.wiegandPending;
  const duplicateEl = App.elements.wiegandDuplicate;
  const listEl = App.elements.wiegandUserList;
  const registerBtn = App.elements.wiegandRegisterBtn;
  const stopBtn = App.elements.wiegandStopBtn;
  const channelSelect = App.elements.wiegandChannelSelect;
  const statusBar = App.elements.wiegandStatusBar;

  if (statusBar) {
    statusBar.classList.toggle('registering', registrationActive);
  }

  if (statusEl) {
    statusEl.textContent = registrationActive
      ? `Registering ${formatChannelLabel(registrationChannel)}`
      : 'Idle';
  }

  if (pendingEl) {
    pendingEl.textContent = registrationPending;
  }

  if (duplicateEl) {
    if (lastDuplicateCode) {
      duplicateEl.textContent = binaryToHex(lastDuplicateCode);
      duplicateEl.classList.remove('muted');
    } else {
      duplicateEl.textContent = '—';
      duplicateEl.classList.add('muted');
    }
  }

  if (registerBtn) {
    registerBtn.hidden = registrationActive;
    registerBtn.disabled = registrationActive;
  }
  if (channelSelect) {
    channelSelect.disabled = registrationActive;
  }
  if (stopBtn) {
    stopBtn.hidden = !registrationActive;
    stopBtn.disabled = !registrationActive;
    stopBtn.classList.toggle('is-listening', registrationActive);
  }
  if (App.elements.wiegandRemoveAllBtn) {
    App.elements.wiegandRemoveAllBtn.disabled = registrationActive || !users.length;
  }

  if (listEl) {
    if (!users || users.length === 0) {
      listEl.innerHTML = '<p class="empty-state muted">No RFID cards registered yet. Click "Register" to add cards.</p>';
    } else {
      // Preserve name input values that user may be editing
      const existingValues = {};
      let focusedId = null;
      listEl.querySelectorAll('.user-row').forEach((row) => {
        const id = row.getAttribute('data-id');
        if (!id) return;
        const nameInput = row.querySelector('.user-name-input');
        const alertInput = row.querySelector('.wiegand-alert-checkbox');
        const modeInput = row.querySelector('.wiegand-mode-select');
        if (nameInput || alertInput || modeInput) {
          existingValues[id] = {
            name: nameInput ? nameInput.value : undefined,
            alert: alertInput ? alertInput.checked : undefined,
            mode: modeInput ? modeInput.value : undefined,
          };
          if (document.activeElement === nameInput) {
            focusedId = id;
          }
        }
      });

      listEl.innerHTML = users
        .map((user) => buildWiegandUserRow(user, existingValues[user.id]))
        .join('');

      // Restore focus if user was editing
      if (focusedId) {
        const row = listEl.querySelector(`.user-row[data-id="${focusedId}"]`);
        const input = row?.querySelector('.user-name-input');
        if (input) {
          input.focus();
          input.setSelectionRange(input.value.length, input.value.length);
        }
      }
    }
  }

  updateRfReceivedTimestamps();
  ensureRfTimestampTimer();
  startWiegandPolling();
};

// Remote FOBs (433 MHz)
const buildRfUserRow = (user, existingValue) => {
  if (!user) return '';
  const name = escapeHtml(existingValue?.name !== undefined ? existingValue.name : (user.name || ''));
  const code = user.code ? `0x${escapeHtml(user.code)}` : '—';
  const mode = existingValue?.mode || user.mode || 'momentary';
  const channelMask = existingValue?.channel_mask || user.channel_mask || 1;
  const exitSeconds = existingValue?.exit_seconds ?? user.exit_seconds ?? 4;
  const alert = existingValue?.alert ?? (user.alert ?? true);
  const enabled = existingValue?.enabled ?? (user.enabled !== false);
  const metrics = buildRfUserMetrics(user);

  return `
    <div class="user-row credential-card credential-card--remote ${enabled ? '' : 'is-card-disabled'}" data-id="${escapeHtml(user.id || '')}" data-enabled="${enabled ? 'true' : 'false'}">
      <div class="credential-card-header">
        <div class="credential-card-title">
          <span class="credential-kind">Remote</span>
          <span class="user-code">${code}</span>
        </div>
        ${renderCredentialEnableButton(enabled, 'toggle-rf-enabled', user.id || '')}
      </div>
      <div class="user-info">
        <label class="stacked">
          <span>Name</span>
          <input type="text" class="user-name-input" value="${name}" placeholder="Enter name...">
        </label>
        <div class="user-config">
          <label class="stacked">
            <span>Mode</span>
            <select class="rf-mode-select">
              <option value="toggle" ${mode === 'toggle' ? 'selected' : ''}>Toggle</option>
              <option value="momentary" ${mode === 'momentary' ? 'selected' : ''}>Momentary</option>
              <option value="latch" ${mode === 'latch' ? 'selected' : ''}>Latch</option>
              <option value="exit" ${mode === 'exit' ? 'selected' : ''}>Exit pulse</option>
              <option value="power_on" ${mode === 'power_on' ? 'selected' : ''}>Power ON</option>
              <option value="power_off" ${mode === 'power_off' ? 'selected' : ''}>Power OFF</option>
            </select>
          </label>
          <label class="stacked">
            <span>Channel</span>
            <select class="rf-channel-select">
              <option value="1" ${channelMask === 1 ? 'selected' : ''}>Channel 1</option>
              <option value="2" ${channelMask === 2 ? 'selected' : ''}>Channel 2</option>
              <option value="3" ${channelMask === 3 ? 'selected' : ''}>Both</option>
            </select>
          </label>
          <label class="stacked">
            <span>Exit duration (s)</span>
            <input type="number" class="rf-exit-seconds" min="1" step="1" value="${exitSeconds}">
          </label>
          <label class="form-switch">
            <input type="checkbox" class="rf-alert-checkbox" ${alert ? 'checked' : ''}>
            <span>Alert (beep)</span>
          </label>
        </div>
        ${metrics}
      </div>
      <div class="user-actions">
        <button type="button" class="secondary" data-action="save-rf" data-id="${escapeHtml(user.id || '')}">Save</button>
        <button type="button" class="secondary danger" data-action="delete-rf" data-id="${escapeHtml(user.id || '')}">Delete</button>
      </div>
    </div>
  `;
};

const getRfUserDefaults = (user = {}) => ({
  id: user.id || '',
  name: user.name || 'Remote Fob',
  mode: user.mode || 'momentary',
  channel_mask: Number(user.channel_mask) || 1,
  exit_seconds: Number(user.exit_seconds) || 4,
  alert: user.alert ?? true,
  enabled: user.enabled !== false,
});

const buildRfUserMetric = (label, value, muted = false) => `
  <div class="rf-card-metric${muted ? ' muted' : ''}">
    <span class="label">${label}</span>
    <span class="value">${value}</span>
  </div>
`;

const buildRfUserMetrics = (user = {}) => {
  const rx = user.lastRx || null;
  if (!rx) {
    return `
      <div class="rf-card-metrics">
        ${buildRfUserMetric('Last packet', 'No packets yet', true)}
      </div>
    `;
  }

  const qualityScore = Number(rx.qualityScore);
  const quality = Number.isFinite(qualityScore)
    ? `${escapeHtml(rx.qualityLabel || 'Signal')} · ${qualityScore}/100`
    : '—';
  const shortUs = Number(rx.shortUs) || 0;
  const longUs = Number(rx.longUs) || 0;
  const timing = shortUs && longUs ? `${shortUs}us / ${longUs}us` : '—';
  const decodeOk = Number(rx.decodeOkCount) || 0;
  const captures = Number(rx.captureCount) || 0;

  return `
    <div class="rf-card-metrics">
      ${buildRfUserMetric('RF quality', quality, !qualityScore)}
      <div class="rf-card-metric">
        <span class="label">Last received</span>
        <span class="value rf-last-received"
          data-unix-time="${Number(rx.unixTime) || 0}"
          data-received-ms="${Number(rx.received_ms) || 0}"
          data-age-ms="${Number(rx.age_ms) || 0}"
          data-rendered-at-ms="${Date.now()}">${formatRfReceivedLabel(rx.unixTime, rx.received_ms, rx.age_ms)}</span>
      </div>
      ${buildRfUserMetric('Timing', timing, timing === '—')}
      ${buildRfUserMetric('Jitter', formatPercent(rx.jitterPercent, 1), !shortUs)}
      ${buildRfUserMetric('Repeats', String(rx.repeatCount || 0), !rx.repeatCount)}
      ${buildRfUserMetric('Noise', `${formatPercent(rx.noisePercent)} · ${formatRate(rx.noiseRatePerSecond)}`, false)}
      ${buildRfUserMetric('Decode', `${decodeOk}/${captures} · ${formatPercent(rx.decodeSuccessRatePercent)}`, !decodeOk)}
      ${buildRfUserMetric('Capture', `${rx.lastCapturePulses || 0} pulses · sync ${rx.syncCount || 0}`, !rx.lastCapturePulses)}
    </div>
  `;
};

const autoSaveRfUser = async (user) => {
  const config = getRfUserDefaults(user);
  if (!config.id || App.rfAutoSavedIds.has(config.id) || App.rfAutoSavingIds.has(config.id)) {
    return;
  }

  App.rfAutoSavingIds.add(config.id);
  try {
    if (config.name.trim()) {
      await fetchJSON('api/rf/rename', {
        method: 'POST',
        body: JSON.stringify({ id: config.id, name: config.name.trim() }),
      });
    }
    await fetchJSON('api/rf/config', {
      method: 'POST',
      body: JSON.stringify({
        id: config.id,
        mode: config.mode,
        channel_mask: config.channel_mask,
        exit_seconds: config.exit_seconds,
        alert: !!config.alert,
        enabled: config.enabled !== false,
      }),
    });
    App.rfAutoSavedIds.add(config.id);
  } catch (error) {
    console.warn('Failed to auto-save remote defaults', error);
  } finally {
    App.rfAutoSavingIds.delete(config.id);
  }
};

const setMetricValue = (element, value, muted = false) => {
  if (!element) return;
  element.textContent = value;
  element.classList.toggle('muted', muted || value === '—');
};

const renderRfDiagnostics = (receiver = {}) => {
  const score = Number(receiver.qualityScore);
  const label = receiver.qualityLabel || 'No signal';
  const qualityText = Number.isFinite(score) ? `${label} · ${score}/100` : '—';
  setMetricValue(App.elements.rfQuality, qualityText, !score);

  const codeText = formatRemoteCode(receiver.lastCode);
  setMetricValue(App.elements.rfLastCode, codeText, codeText === '—');

  const shortUs = Number(receiver.lastShortUs) || 0;
  const longUs = Number(receiver.lastLongUs) || 0;
  setMetricValue(
    App.elements.rfTiming,
    shortUs && longUs ? `${shortUs}us / ${longUs}us` : '—',
    !(shortUs && longUs)
  );

  const hasDecodedFrame = Number(receiver.lastCode) > 0 && shortUs > 0 && longUs > 0;
  setMetricValue(
    App.elements.rfJitter,
    hasDecodedFrame ? formatPercent(receiver.lastJitterPercent, 1) : '—',
    !hasDecodedFrame
  );
  setMetricValue(App.elements.rfRepeats, String(receiver.lastRepeatCount || 0), !receiver.lastRepeatCount);
  setMetricValue(
    App.elements.rfNoise,
    `${formatPercent(receiver.noisePercent)} · ${formatRate(receiver.noiseRatePerSecond)}`,
    !receiver.edgeCount
  );
  setMetricValue(
    App.elements.rfDecode,
    `${receiver.decodeOkCount || 0}/${receiver.captureCount || 0} · ${formatPercent(receiver.decodeSuccessRatePercent)}`,
    !receiver.decodeOkCount
  );
  setMetricValue(
    App.elements.rfCaptures,
    `${receiver.lastCapturePulses || 0} pulses · sync ${receiver.syncCount || 0}`,
    !receiver.captureCount
  );
  setMetricValue(
    App.elements.rfLastInput,
    `${formatAge(receiver.lastDecodeAgeMs)} · edge ${formatRate(receiver.edgeRatePerSecond)}`,
    !receiver.lastCode
  );
  setMetricValue(
    App.elements.rfReceiver,
    `GPIO${receiver.gpio ?? '—'} · ${receiver.isrAddResult || '—'}`,
    receiver.isrAddResult !== 'ESP_OK'
  );
};

const renderRf = (rf = {}) => {
  const {
    registrationActive = false,
    registrationPending = 0,
    lastDuplicateCode = '',
    users = [],
  } = rf;

  const statusEl = App.elements.rfStatus;
  const pendingEl = App.elements.rfPending;
  const duplicateEl = App.elements.rfDuplicate;
  const listEl = App.elements.rfUserList;
  const registerBtn = App.elements.rfRegisterBtn;
  const stopBtn = App.elements.rfStopBtn;
  const statusBar = App.elements.rfStatusBar;

  if (statusBar) statusBar.classList.toggle('registering', registrationActive);
  if (statusEl) statusEl.textContent = registrationActive ? 'Registering remotes' : 'Idle';
  if (pendingEl) pendingEl.textContent = registrationPending;
  if (duplicateEl) {
    if (lastDuplicateCode) {
      duplicateEl.textContent = `0x${lastDuplicateCode}`;
      duplicateEl.classList.remove('muted');
    } else {
      duplicateEl.textContent = '—';
      duplicateEl.classList.add('muted');
    }
  }
  if (registerBtn) registerBtn.disabled = registrationActive;
  if (stopBtn) stopBtn.disabled = !registrationActive;
  if (App.elements.rfRemoveAllBtn) {
    App.elements.rfRemoveAllBtn.disabled = registrationActive || !users.length;
  }

  renderRfDiagnostics(rf.receiver || {});

  if (listEl && !listEl.contains(document.activeElement)) {
    if (!users || users.length === 0) {
      listEl.innerHTML = '<p class="empty-state muted">No remote FOBs learned yet. Click "Register" to learn codes.</p>';
    } else {
      const existingValues = {};
      listEl.querySelectorAll('.user-row').forEach((row) => {
        const id = row.getAttribute('data-id');
        if (!id) return;
        const nameInput = row.querySelector('.user-name-input');
        const modeSel = row.querySelector('.rf-mode-select');
        const chSel = row.querySelector('.rf-channel-select');
        const exitInput = row.querySelector('.rf-exit-seconds');
        const alertInput = row.querySelector('.rf-alert-checkbox');
        existingValues[id] = {
          name: nameInput ? nameInput.value : undefined,
          mode: modeSel ? modeSel.value : undefined,
          channel_mask: chSel ? Number(chSel.value) : undefined,
          exit_seconds: exitInput ? Number(exitInput.value) : undefined,
          alert: alertInput ? alertInput.checked : undefined,
          enabled: row.dataset.enabled !== 'false',
        };
      });

      listEl.innerHTML = users
        .map((u) => buildRfUserRow(u, existingValues[u.id]))
        .join('');

      users.forEach((user) => autoSaveRfUser(user));
    }
  }

  updateRfReceivedTimestamps();
  ensureRfTimestampTimer();

  if (registrationActive) {
    if (!App.rfPollTimer) {
      App.rfPollTimer = setInterval(loadState, 2000);
    }
  } else if (App.rfPollTimer) {
    clearInterval(App.rfPollTimer);
    App.rfPollTimer = null;
  }
};

const renderEnrollment = (enrollment = {}) => {
  const active = !!enrollment.active;
  const startBtn = App.elements.enrollStartBtn;
  const stopBtn = App.elements.enrollStopBtn;
  const selectEl = App.elements.enrollUserSelect;
  const newUserName = document.getElementById('enrollNewUserName');

  if (startBtn) {
    startBtn.hidden = active;
    startBtn.disabled = active;
  }
  if (stopBtn) {
    stopBtn.hidden = !active;
    stopBtn.disabled = !active;
    stopBtn.classList.toggle('is-listening', active);
  }
  if (selectEl) selectEl.disabled = active;
  if (newUserName) newUserName.disabled = active;

  if (active) {
    startEnrollmentPolling();
  } else {
    stopEnrollmentPolling();
  }
};

const renderState = (state = {}) => {
  applyDeviceInfo(state.device || {});
  applyServerInfo(state.server || {});
  applySystemInfo(state.system || {});
  applyLockState(state.locks || []);
  applyExitState(state.exits || []);
  applyFobState(state.fobs || []);
  applyKeypadState(state.keypads || []);
  applyMotionState(state.motions || []);
  renderWiegand(state.wiegand || {});
  renderRf(state.rf || {});
  renderEnrollment(state.enrollment || {});
  // Keypad users and logs are heavy; loaded via dedicated endpoints so state polling
  // can't fragment heap on the ESP32 or wipe UI lists when omitted from /api/state.
  if (Array.isArray(state.keypadUsers)) {
    renderKeypadUsers(state.keypadUsers);
  }
  if (Array.isArray(state.logs)) {
    renderLogs(state.logs);
  }
  renderWifi(state.wifi || {}, state.device?.network || {}, state.system || {});
};

const DEVICE_STATE_ERROR_THROTTLE_MS = 15000;
let lastDeviceStateErrorToast = 0;
let stateInFlight = null;
let consecutiveStateFailures = 0;
let nextStateToastAt = 0;
const DEVICE_STATE_TOAST_BACKOFF_MS = 120000;
let wifiNetworksCache = null;
let wifiScanCache = null;
let signalsInFlight = null;

const loadState = async () => {
  if (stateInFlight) return stateInFlight;

  stateInFlight = (async () => {
  try {
    const data = await fetchJSON(`api/state?t=${Date.now()}`);
    App.data = data;
    consecutiveStateFailures = 0;
    nextStateToastAt = 0;
    if (wifiNetworksCache) {
      App.data.wifi = App.data.wifi || {};
      App.data.wifi.networks = wifiNetworksCache;
    }
    if (wifiScanCache) {
      App.data.wifi = App.data.wifi || {};
      App.data.wifi.scanned = wifiScanCache;
    }
    renderState(data);
    if (App.elements.toast && !App.elements.toast.hidden) {
      App.elements.toast.hidden = true;
      App.elements.toast.classList.remove('show');
    }
  } catch (error) {
    consecutiveStateFailures++;
    const now = Date.now();
    const shouldToast =
      consecutiveStateFailures === 1 || (nextStateToastAt > 0 && now >= nextStateToastAt);
    if (shouldToast && now - lastDeviceStateErrorToast >= DEVICE_STATE_ERROR_THROTTLE_MS) {
      lastDeviceStateErrorToast = now;
      nextStateToastAt = now + DEVICE_STATE_TOAST_BACKOFF_MS;
      handleError(error, 'Unable to load device state');
    }
  }
  })().finally(() => {
    stateInFlight = null;
  });

  return stateInFlight;
};

const pollSignals = async () => {
  if (signalsInFlight || document.hidden) return signalsInFlight;

  signalsInFlight = (async () => {
    try {
      const data = await fetchJSON(`api/signals?t=${Date.now()}`);
      applyFastSignalState(data || {});
    } catch (error) {
      console.warn('Failed to load signal state', error);
    }
  })().finally(() => {
    signalsInFlight = null;
  });

  return signalsInFlight;
};

const startSignalPolling = () => {
  if (App.signalPollTimer) return;
  pollSignals();
  App.signalPollTimer = setInterval(pollSignals, 500);
};

const stopSignalPolling = () => {
  if (!App.signalPollTimer) return;
  clearInterval(App.signalPollTimer);
  App.signalPollTimer = null;
};

let wifiListInFlight = null;
const loadWifiList = async () => {
  if (wifiListInFlight) return wifiListInFlight;
  wifiListInFlight = (async () => {
    try {
      const [savedResult, scanResult] = await Promise.allSettled([
        fetchJSON(`api/wifi/list?t=${Date.now()}`),
        fetchJSON(`api/wifi/scan?t=${Date.now()}`),
      ]);
      const networks = savedResult.status === 'fulfilled' && Array.isArray(savedResult.value)
        ? savedResult.value
        : [];
      const scanned = scanResult.status === 'fulfilled' && Array.isArray(scanResult.value)
        ? scanResult.value
        : [];
      wifiNetworksCache = networks;
      wifiScanCache = scanned;
      const wifi = (App.data && App.data.wifi) ? App.data.wifi : {};
      renderWifi(
        { ...wifi, networks: wifiNetworksCache, scanned: wifiScanCache },
        App.data?.device?.network || {},
        App.data?.system || {}
      );
      if (App.data) {
        App.data.wifi = App.data.wifi || {};
        App.data.wifi.networks = wifiNetworksCache;
        App.data.wifi.scanned = wifiScanCache;
      }
    } catch (error) {
      console.warn('Failed to load Wi-Fi list', error);
    }
  })().finally(() => {
    wifiListInFlight = null;
  });
  return wifiListInFlight;
};

let keypadUsersInFlight = null;
const loadKeypadUsers = async () => {
  if (keypadUsersInFlight) return keypadUsersInFlight;
  keypadUsersInFlight = (async () => {
    try {
      const users = await fetchJSON(`api/keypad/users?t=${Date.now()}`);
      renderKeypadUsers(Array.isArray(users) ? users : []);
      if (App.data) App.data.keypadUsers = Array.isArray(users) ? users : [];
    } catch (error) {
      // Don't toast spam; state load already covers connectivity issues.
      console.warn('Failed to load keypad users', error);
    }
  })().finally(() => {
    keypadUsersInFlight = null;
  });
  return keypadUsersInFlight;
};

let logsInFlight = null;
const loadLogs = async () => {
  if (logsInFlight) return logsInFlight;
  logsInFlight = (async () => {
    try {
      const logs = await fetchJSON(`api/logs?t=${Date.now()}`);
      renderLogs(Array.isArray(logs) ? logs : []);
      if (App.data) App.data.logs = Array.isArray(logs) ? logs : [];
    } catch (error) {
      console.warn('Failed to load logs', error);
    }
  })().finally(() => {
    logsInFlight = null;
  });
  return logsInFlight;
};

const updateLock = async (channel, updates) => {
  const body = { channel, ...updates };
  try {
    const locks = await fetchJSON('api/lock', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    applyLockState(locks || []);
  } catch (error) {
    handleError(error, 'Failed to update lock state');
  }
};

const updateExit = async (channel, updates) => {
  const body = { channel, ...updates };
  try {
    const exits = await fetchJSON('api/exit', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    applyExitState(exits || []);
  } catch (error) {
    handleError(error, 'Failed to update exit state');
  }
};

const updateFob = async (channel, updates) => {
  const body = { channel, ...updates };
  try {
    const fobs = await fetchJSON('api/fob', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    applyFobState(fobs || []);
  } catch (error) {
    handleError(error, 'Failed to update FOB state');
  }
};

const updateKeypad = async (channel, updates) => {
  const body = { channel, ...updates };
  try {
    const keypads = await fetchJSON('api/keypad', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    applyKeypadState(keypads || []);
  } catch (error) {
    handleError(error, 'Failed to update keypad state');
  }
};

const updateMotion = async (channel, updates) => {
  const body = { channel, ...updates };
  try {
    const motions = await fetchJSON('api/motion', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    applyMotionState(motions || []);
  } catch (error) {
    handleError(error, 'Failed to update motion state');
  }
};

const createCardEnableButton = (enableId, label) => {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'card-enable-toggle';
  button.dataset.enableTarget = enableId;
  button.setAttribute('aria-label', `${label} enabled`);
  button.innerHTML = '<span class="card-enable-icon" aria-hidden="true"></span><span class="card-enable-text">Enabled</span>';
  return button;
};

const createCardModeSelect = (modeId, latchId) => {
  const wrap = document.createElement('label');
  wrap.className = 'card-mode-select stacked';
  wrap.innerHTML = `
    <span>Mode</span>
    <select id="${modeId}" data-latch-target="${latchId}">
      <option value="momentary">Momentary</option>
      <option value="toggle">Toggle</option>
      <option value="latch">Latch</option>
    </select>
  `;
  return wrap;
};

const setupControlCardChrome = () => {
  const configs = [];
  [1, 2].forEach((ch) => {
    configs.push(
      { label: 'Lock', enableId: `enableLock_${ch}`, update: updateLock },
      { label: 'Exit', enableId: `enableExit_${ch}`, latchId: `latchExit_${ch}`, modeId: `modeExit_${ch}`, update: updateExit },
      { label: 'Keypad', enableId: `enableKeypad_${ch}`, latchId: `latchKeypad_${ch}`, modeId: `modeKeypad_${ch}`, update: updateKeypad },
      { label: 'FOB', enableId: `enableFob_${ch}`, latchId: `latchFob_${ch}`, modeId: `modeFob_${ch}`, update: updateFob },
      { label: 'Motion', enableId: `enableMotion_${ch}`, latchId: `latchMotion_${ch}`, modeId: `modeMotion_${ch}`, update: updateMotion },
    );
  });

  configs.forEach((config) => {
    const enableEl = document.getElementById(config.enableId);
    const section = enableEl?.closest('.control-section');
    if (!enableEl || !section || section.querySelector('.control-card-header')) return;

    const channel = Number(config.enableId.split('_').pop()) || 0;
    const titleEl = section.querySelector('.section-label, h4');
    const header = document.createElement('div');
    header.className = 'control-card-header';

    const titleWrap = document.createElement('div');
    titleWrap.className = 'control-card-title';
    if (titleEl) {
      titleWrap.appendChild(titleEl);
    } else {
      const title = document.createElement('h4');
      title.textContent = config.label;
      titleWrap.appendChild(title);
    }
    header.appendChild(titleWrap);

    if (config.latchId && config.modeId) {
      const modeWrap = createCardModeSelect(config.modeId, config.latchId);
      const modeSelect = modeWrap.querySelector('select');
      const latchEl = document.getElementById(config.latchId);
      if (modeSelect && latchEl) {
        modeSelect.value = normalizeCardMode(null, latchEl.checked);
        modeSelect.addEventListener('change', (event) => {
          const mode = normalizeCardMode(event.target.value, latchEl.checked);
          const latch = mode === 'latch';
          latchEl.checked = latch;
          config.update(channel, { mode, latch });
        });
      }
      header.appendChild(modeWrap);
      latchEl?.closest('label')?.classList.add('hidden-card-control');
    }

    const enableButton = createCardEnableButton(config.enableId, config.label);
    enableButton.addEventListener('click', () => {
      config.update(channel, { enable: !enableEl.checked });
    });
    header.appendChild(enableButton);
    section.insertBefore(header, section.firstChild);
    enableEl.closest('label')?.classList.add('hidden-card-control');
    setCardEnabledState(config.enableId, enableEl.checked);
  });
};

const setupLockHandlers = () => {
  [1, 2].forEach((ch) => {
    const enableEl = document.getElementById(`enableLock_${ch}`);
    const armEl = document.getElementById(`arm_${ch}`);
    const contactEl = document.getElementById(`enableContactAlert_${ch}`);
    const polarityEl = document.getElementById(`polarity_${ch}`);

    if (enableEl) {
      enableEl.addEventListener('change', (event) => {
        updateLock(ch, { enable: event.target.checked });
      });
    }
    if (armEl) {
      armEl.addEventListener('change', (event) => {
        updateLock(ch, { arm: event.target.checked });
      });
    }
    if (contactEl) {
      contactEl.addEventListener('change', (event) => {
        updateLock(ch, { enableContactAlert: event.target.checked });
      });
    }
    if (polarityEl) {
      polarityEl.addEventListener('change', (event) => {
        updateLock(ch, { polarity: event.target.checked });
      });
    }
  });
};

const setupExitHandlers = () => {
  [1, 2].forEach((ch) => {
    const enableEl = document.getElementById(`enableExit_${ch}`);
    const alertEl = document.getElementById(`alertExit_${ch}`);
    const latchEl = document.getElementById(`latchExit_${ch}`);
    const saveBtn = document.getElementById(ch === 1 ? 'relock' : 'relock_2');
    const delayEl = document.getElementById(`armDelay_${ch}`);

    if (enableEl) {
      enableEl.addEventListener('change', (event) => {
        updateExit(ch, { enable: event.target.checked });
      });
    }
    if (alertEl) {
      alertEl.addEventListener('change', (event) => {
        updateExit(ch, { alert: event.target.checked });
      });
    }
    if (latchEl) {
      latchEl.addEventListener('change', (event) => {
        updateExit(ch, { latch: event.target.checked, mode: normalizeCardMode(null, event.target.checked) });
      });
    }
    if (saveBtn && delayEl) {
      saveBtn.addEventListener('click', () => {
        const value = parseInt(delayEl.value, 10) || 0;
        updateExit(ch, { delay: value });
      });
    }
  });
};

const setupFobHandlers = () => {
  [1, 2].forEach((ch) => {
    const enableEl = document.getElementById(`enableFob_${ch}`);
    const alertEl = document.getElementById(`alertFob_${ch}`);
    const latchEl = document.getElementById(`latchFob_${ch}`);
    const delayEl = document.getElementById(`fobDelay_${ch}`);
    const saveBtn = document.getElementById(`fobSave_${ch}`);

    if (enableEl) {
      enableEl.addEventListener('change', (event) => {
        updateFob(ch, { enable: event.target.checked });
      });
    }
    if (alertEl) {
      alertEl.addEventListener('change', (event) => {
        updateFob(ch, { alert: event.target.checked });
      });
    }
    if (latchEl) {
      latchEl.addEventListener('change', (event) => {
        updateFob(ch, { latch: event.target.checked, mode: normalizeCardMode(null, event.target.checked) });
      });
    }
    if (delayEl && saveBtn) {
      saveBtn.addEventListener('click', () => {
        const value = parseInt(delayEl.value, 10) || 0;
        updateFob(ch, { delay: value });
      });
    }
  });
};

const setupKeypadHandlers = () => {
  [1, 2].forEach((ch) => {
    const enableEl = document.getElementById(`enableKeypad_${ch}`);
    const alertEl = document.getElementById(`alertKeypad_${ch}`);
    const latchEl = document.getElementById(`latchKeypad_${ch}`);
    const delayEl = document.getElementById(`keypadDelay_${ch}`);
    const saveBtn = document.getElementById(`keypadSave_${ch}`);

    if (enableEl) {
      enableEl.addEventListener('change', (event) => {
        updateKeypad(ch, { enable: event.target.checked });
      });
    }
    if (alertEl) {
      alertEl.addEventListener('change', (event) => {
        updateKeypad(ch, { alert: event.target.checked });
      });
    }
    if (latchEl) {
      latchEl.addEventListener('change', (event) => {
        updateKeypad(ch, { latch: event.target.checked, mode: normalizeCardMode(null, event.target.checked) });
      });
    }
    if (delayEl && saveBtn) {
      saveBtn.addEventListener('click', () => {
        const value = parseInt(delayEl.value, 10) || 0;
        updateKeypad(ch, { delay: value });
      });
    }
  });
};

const setupMotionHandlers = () => {
  [1, 2].forEach((ch) => {
    const enableEl = document.getElementById(`enableMotion_${ch}`);
    const alertEl = document.getElementById(`alertMotion_${ch}`);
    const latchEl = document.getElementById(`latchMotion_${ch}`);
    const delayEl = document.getElementById(`motionDelay_${ch}`);
    const saveBtn = document.getElementById(`motionSave_${ch}`);

    if (enableEl) {
      enableEl.addEventListener('change', (event) => {
        updateMotion(ch, { enable: event.target.checked });
      });
    }
    if (alertEl) {
      alertEl.addEventListener('change', (event) => {
        updateMotion(ch, { alert: event.target.checked });
      });
    }
    if (latchEl) {
      latchEl.addEventListener('change', (event) => {
        updateMotion(ch, { latch: event.target.checked, mode: normalizeCardMode(null, event.target.checked) });
      });
    }
    if (delayEl && saveBtn) {
      saveBtn.addEventListener('click', () => {
        const value = parseInt(delayEl.value, 10) || 0;
        updateMotion(ch, { delay: value });
      });
    }
  });
};

const setupEnrollmentHandlers = () => {
  const newUserName = document.getElementById('enrollNewUserName');
  const userSelect = document.getElementById('enrollUserSelect');
  const startBtn = document.getElementById('enrollStartBtn');
  const stopBtn = document.getElementById('enrollStopBtn');

  const addUser = async () => {
    const name = (newUserName?.value || '').trim();
    if (!name) return null;

    try {
      let users = await fetchJSON('api/keypad/user', {
        method: 'POST',
        body: JSON.stringify({ name, pin: '' }),
      });
      if (!Array.isArray(users) || !users.length) {
        users = await fetchJSON(`api/keypad/users?t=${Date.now()}`);
      }
      const list = Array.isArray(users) ? users : [];
      const created = [...list].reverse().find((user) => user.name === name) || list[list.length - 1];
      if (App.data) App.data.keypadUsers = list;
      renderKeypadUsers(list);
      if (created?.uuid && userSelect) userSelect.value = created.uuid;
      if (newUserName) newUserName.value = '';
      return created?.uuid || null;
    } catch (error) {
      handleError(error, 'Failed to add user');
      return null;
    }
  };

  if (startBtn) {
    startBtn.addEventListener('click', async () => {
      let userUuid = userSelect?.value || '';
      if ((newUserName?.value || '').trim()) {
        userUuid = await addUser() || '';
        if (!userUuid) return;
      }

      startBtn.disabled = true;
      try {
        const state = await fetchJSON('api/enrollment/start', {
          method: 'POST',
          body: JSON.stringify({ userUuid }),
        });
        if (state) {
          App.data = state;
          renderState(state);
        }
        showToast('Listening for credentials.');
      } catch (error) {
        handleError(error, 'Failed to start enrollment');
        startBtn.disabled = false;
      }
    });
  }

  if (stopBtn) {
    stopBtn.addEventListener('click', async () => {
      stopBtn.disabled = true;
      try {
        const state = await fetchJSON('api/enrollment/stop', {
          method: 'POST',
          body: JSON.stringify({}),
        });
        if (state) {
          App.data = state;
          renderState(state);
        }
        await Promise.allSettled([loadKeypadUsers(), loadState()]);
        showToast('Enrollment stopped.');
      } catch (error) {
        handleError(error, 'Failed to stop enrollment');
        stopBtn.disabled = false;
      }
    });
  }
};

const setupForms = () => {
  const wifiForm = document.getElementById('wifiForm');
  const serverForm = document.getElementById('serverForm');
  const wifiList = document.getElementById('wifiNetworks');
  const wifiAvailableList = document.getElementById('wifiAvailableNetworks');
  const wifiScanBtn = document.getElementById('wifiScanBtn');

  if (wifiForm) {
    wifiForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const wifiName = document.getElementById('wifiName')?.value || '';
      const wifiPassword = document.getElementById('wifiPassword')?.value || '';

      try {
        await fetchJSON('api/wifi/add', {
          method: 'POST',
          body: JSON.stringify({ ssid: wifiName, password: wifiPassword }),
        });
        showToast('Wi‑Fi saved. Device will reboot to connect.');
      } catch (error) {
        handleError(error, 'Failed to update Wi-Fi credentials');
      }
    });
  }

  if (wifiList) {
    wifiList.addEventListener('click', async (event) => {
      const connectBtn = event.target.closest('button[data-action="wifi-connect"]');
      const deleteBtn = event.target.closest('button[data-action="wifi-delete"]');
      if (connectBtn) {
        const ssid = connectBtn.getAttribute('data-ssid');
        connectBtn.disabled = true;
        try {
          await fetchJSON('api/wifi/connect', {
            method: 'POST',
            body: JSON.stringify({ ssid }),
          });
          showToast('Connecting... device will reboot.');
        } catch (error) {
          handleError(error, 'Failed to connect');
        } finally {
          connectBtn.disabled = false;
        }
      }
      if (deleteBtn) {
        const ssid = deleteBtn.getAttribute('data-ssid');
        deleteBtn.disabled = true;
        try {
          await fetchJSON('api/wifi/delete', {
            method: 'POST',
            body: JSON.stringify({ ssid }),
          });
          showToast('Wi‑Fi removed.');
          loadState();
        } catch (error) {
          handleError(error, 'Failed to delete Wi‑Fi');
        } finally {
          deleteBtn.disabled = false;
        }
      }
    });
  }

  if (wifiScanBtn) {
    wifiScanBtn.addEventListener('click', async () => {
      wifiScanBtn.disabled = true;
      try {
        await loadWifiList();
        showToast('Wi-Fi scan refreshed.');
      } catch (error) {
        handleError(error, 'Failed to scan Wi-Fi networks');
      } finally {
        wifiScanBtn.disabled = false;
      }
    });
  }

  if (wifiAvailableList) {
    wifiAvailableList.addEventListener('click', (event) => {
      const networkBtn = event.target.closest('button[data-action="wifi-select"]');
      if (!networkBtn) return;
      const ssid = networkBtn.getAttribute('data-ssid') || '';
      const ssidInput = document.getElementById('wifiName');
      const passwordInput = document.getElementById('wifiPassword');
      if (ssidInput) ssidInput.value = ssid;
      if (passwordInput) passwordInput.focus();
    });
  }

  if (serverForm) {
    serverForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const serverUrl = (document.getElementById('serverUrl')?.value || 'https://open-automation.org/devices').trim() || 'https://open-automation.org/devices';
      const requireReachable = document.getElementById('serverRequireReachable')?.checked !== false;

      try {
        await fetchJSON('api/server', {
          method: 'POST',
          body: JSON.stringify({ serverUrl, requireReachable }),
        });
        showToast('Server information updated. Device will reboot to apply changes.');
      } catch (error) {
        handleError(error, 'Failed to update server information');
      }
    });
  }
};

const setupWiegandHandlers = () => {
  const registerBtn = App.elements.wiegandRegisterBtn;
  const stopBtn = App.elements.wiegandStopBtn;
  const channelSelect = App.elements.wiegandChannelSelect;
  const listEl = App.elements.wiegandUserList;
  const removeAllBtn = App.elements.wiegandRemoveAllBtn;

  if (registerBtn && channelSelect) {
    registerBtn.addEventListener('click', async () => {
      const channel = parseInt(channelSelect.value, 10) || 0;
      try {
        const wiegand = await fetchJSON('api/wiegand/register', {
          method: 'POST',
          body: JSON.stringify({ channel }),
        });
        renderWiegand(wiegand);
        showToast('Registration started. Tap tags to enrol.');
      } catch (error) {
        handleError(error, error.message || 'Failed to start registration');
      }
    });
  }

  if (stopBtn) {
    stopBtn.addEventListener('click', async () => {
      try {
        const wiegand = await fetchJSON('api/wiegand/stop', {
          method: 'POST',
          body: JSON.stringify({ promote: true }),
        });
        renderWiegand(wiegand);
        showToast('Registration stopped. New tags activated.');
      } catch (error) {
        handleError(error, error.message || 'Failed to stop registration');
      }
    });
  }

  if (removeAllBtn) {
    removeAllBtn.addEventListener('click', async () => {
      const users = App.data?.wiegand?.users || [];
      if (!users.length) {
        showToast('No RFID cards to remove.');
        return;
      }

      removeAllBtn.disabled = true;
      try {
        const wiegand = await fetchJSON('api/wiegand/delete-all', {
          method: 'POST',
          body: JSON.stringify({}),
        });
        renderWiegand(wiegand || { users: [] });
        await loadState();
        showToast(`Removed ${users.length} RFID card${users.length === 1 ? '' : 's'}.`);
      } catch (error) {
        handleError(error, error.message || 'Failed to remove RFID cards');
      } finally {
        removeAllBtn.disabled = false;
      }
    });
  }

  if (listEl) {
    const saveWiegandCard = async (container, trigger, { quiet = false } = {}) => {
      if (!container) return null;
      const input = container.querySelector('.user-name-input');
      const alertInput = container.querySelector('.wiegand-alert-checkbox');
      const modeInput = container.querySelector('.wiegand-mode-select');
      if (!input) return null;

      const id = container.getAttribute('data-id');
      const existing = (App.data?.wiegand?.users || []).find((user) => user.id === id) || {};
      const name = input.value.trim() || existing.name || 'RFID Card';
      const channel = parseInt(container.getAttribute('data-channel') || `${existing.channel || 0}`, 10) || 0;
      const alert = alertInput ? alertInput.checked : existing.alert !== false;
      const mode = modeInput?.value || existing.mode || 'momentary';
      const enabled = container.dataset.enabled !== 'false';
      if (!id) return null;

      if (trigger) trigger.disabled = true;
      container.classList.add('saving');
      try {
        const wiegand = await fetchJSON('api/wiegand/rename', {
          method: 'POST',
          body: JSON.stringify({ id, name, channel, alert, enabled, mode }),
        });
        renderWiegand(wiegand);
        if (!quiet) showToast('RFID card updated.');
        return wiegand;
      } catch (error) {
        handleError(error, error.message || 'Failed to update RFID card');
        throw error;
      } finally {
        if (trigger) trigger.disabled = false;
        container.classList.remove('saving');
      }
    };

    listEl.addEventListener('click', async (event) => {
      const toggleEnabledBtn = event.target.closest('button[data-action="toggle-wiegand-enabled"]');
      const renameBtn = event.target.closest('button[data-action="rename"]');
      const deleteBtn = event.target.closest('button[data-action="delete-wiegand"]');

      if (toggleEnabledBtn) {
        const container = toggleEnabledBtn.closest('.user-row');
        if (!container) return;
        const previousEnabled = container.dataset.enabled !== 'false';
        const nextEnabled = !previousEnabled;
        container.dataset.enabled = nextEnabled ? 'true' : 'false';
        container.classList.toggle('is-card-disabled', !nextEnabled);
        setEnableButtonState(toggleEnabledBtn, nextEnabled);
        try {
          await saveWiegandCard(container, toggleEnabledBtn, { quiet: true });
          showToast(`RFID card ${nextEnabled ? 'enabled' : 'disabled'}.`);
        } catch (error) {
          container.dataset.enabled = previousEnabled ? 'true' : 'false';
          container.classList.toggle('is-card-disabled', !previousEnabled);
          setEnableButtonState(toggleEnabledBtn, previousEnabled);
        }
        return;
      }

      if (renameBtn) {
        const container = renameBtn.closest('.user-row');
        if (!container) return;
        try {
          await saveWiegandCard(container, renameBtn);
        } catch (error) {
          // saveWiegandCard already surfaced the error.
        }
      }

      if (deleteBtn) {
        const id = deleteBtn.getAttribute('data-id');
        if (!id) return;

        deleteBtn.disabled = true;
        try {
          const wiegand = await fetchJSON('api/wiegand/delete', {
            method: 'POST',
            body: JSON.stringify({ id }),
          });
          renderWiegand(wiegand);
          showToast('RFID card deleted.');
        } catch (error) {
          handleError(error, error.message || 'Failed to delete card');
        } finally {
          deleteBtn.disabled = false;
        }
      }
    });

    listEl.addEventListener('change', async (event) => {
      const alertInput = event.target.closest('.wiegand-alert-checkbox');
      const modeInput = event.target.closest('.wiegand-mode-select');
      if (!alertInput && !modeInput) return;
      const container = (alertInput || modeInput).closest('.user-row');
      if (!container) return;

      const control = alertInput || modeInput;
      const previous = alertInput ? !alertInput.checked : modeInput.dataset.previousValue || 'momentary';
      control.disabled = true;
      try {
        await saveWiegandCard(container, control, { quiet: true });
        if (alertInput) {
          showToast(`RFID alert ${alertInput.checked ? 'enabled' : 'disabled'}.`);
        } else {
          modeInput.dataset.previousValue = modeInput.value;
          showToast(`RFID mode set to ${modeInput.options[modeInput.selectedIndex].text}.`);
        }
      } catch (error) {
        if (alertInput) {
          alertInput.checked = previous;
        } else {
          modeInput.value = previous;
        }
      } finally {
        control.disabled = false;
      }
    });
  }
};

const setupRfHandlers = () => {
  const registerBtn = App.elements.rfRegisterBtn;
  const stopBtn = App.elements.rfStopBtn;
  const listEl = App.elements.rfUserList;
  const removeAllBtn = App.elements.rfRemoveAllBtn;

  if (registerBtn) {
    registerBtn.addEventListener('click', async () => {
      try {
        const rf = await fetchJSON('api/rf/register', { method: 'POST', body: JSON.stringify({}) });
        renderRf(rf);
        showToast('RF registration started. Press remote buttons to learn.');
      } catch (error) {
        handleError(error, error.message || 'Failed to start RF registration');
      }
    });
  }

  if (stopBtn) {
    stopBtn.addEventListener('click', async () => {
      try {
        const rf = await fetchJSON('api/rf/stop', { method: 'POST', body: JSON.stringify({}) });
        renderRf(rf);
        showToast('RF registration stopped.');
      } catch (error) {
        handleError(error, error.message || 'Failed to stop RF registration');
      }
    });
  }

  if (removeAllBtn) {
    removeAllBtn.addEventListener('click', async () => {
      const users = App.data?.rf?.users || [];
      if (!users.length) {
        showToast('No remotes to remove.');
        return;
      }

      removeAllBtn.disabled = true;
      try {
        const rf = await fetchJSON('api/rf/delete-all', {
          method: 'POST',
          body: JSON.stringify({}),
        });
        renderRf(rf || { users: [] });
        await loadState();
        showToast(`Removed ${users.length} remote FOB${users.length === 1 ? '' : 's'}.`);
      } catch (error) {
        handleError(error, error.message || 'Failed to remove remotes');
      } finally {
        removeAllBtn.disabled = false;
      }
    });
  }

  if (listEl) {
    const saveRfCard = async (container, trigger, { quiet = false } = {}) => {
      if (!container) return null;
      const nameInput = container.querySelector('.user-name-input');
      const modeSelect = container.querySelector('.rf-mode-select');
      const channelSelect = container.querySelector('.rf-channel-select');
      const exitInput = container.querySelector('.rf-exit-seconds');
      const alertCb = container.querySelector('.rf-alert-checkbox');
      const id = container.getAttribute('data-id');
      const name = nameInput?.value.trim();
      const mode = modeSelect?.value || 'momentary';
      const channel_mask = channelSelect ? Number(channelSelect.value) : 0;
      const exit_seconds = exitInput ? Number(exitInput.value || 0) : 0;
      const alert = alertCb ? !!alertCb.checked : true;
      const enabled = container.dataset.enabled !== 'false';

      if (!id) {
        showToast('Missing id');
        return null;
      }
      if (!name) {
        showToast('Please provide a name before saving.');
        return null;
      }

      if (trigger) trigger.disabled = true;
      container.classList.add('saving');
      try {
        await fetchJSON('api/rf/rename', {
          method: 'POST',
          body: JSON.stringify({ id, name }),
        });
        const rf = await fetchJSON('api/rf/config', {
          method: 'POST',
          body: JSON.stringify({ id, mode, channel_mask, exit_seconds, alert, enabled }),
        });
        renderRf(rf);
        if (!quiet) showToast('Remote updated.');
        return rf;
      } catch (error) {
        handleError(error, error.message || 'Failed to update remote');
        throw error;
      } finally {
        if (trigger) trigger.disabled = false;
        container.classList.remove('saving');
      }
    };

    listEl.addEventListener('click', async (event) => {
      const toggleEnabledBtn = event.target.closest('button[data-action="toggle-rf-enabled"]');
      const saveBtn = event.target.closest('button[data-action="save-rf"]');
      const deleteBtn = event.target.closest('button[data-action="delete-rf"]');

      if (toggleEnabledBtn) {
        const container = toggleEnabledBtn.closest('.user-row');
        if (!container) return;
        const previousEnabled = container.dataset.enabled !== 'false';
        const nextEnabled = !previousEnabled;
        container.dataset.enabled = nextEnabled ? 'true' : 'false';
        container.classList.toggle('is-card-disabled', !nextEnabled);
        setEnableButtonState(toggleEnabledBtn, nextEnabled);
        try {
          await saveRfCard(container, toggleEnabledBtn, { quiet: true });
          showToast(`Remote ${nextEnabled ? 'enabled' : 'disabled'}.`);
        } catch (error) {
          container.dataset.enabled = previousEnabled ? 'true' : 'false';
          container.classList.toggle('is-card-disabled', !previousEnabled);
          setEnableButtonState(toggleEnabledBtn, previousEnabled);
        }
        return;
      }

      if (saveBtn) {
        const container = saveBtn.closest('.user-row');
        try {
          await saveRfCard(container, saveBtn);
        } catch (error) {
          // saveRfCard already surfaced the error.
        }
      }

      if (deleteBtn) {
        const id = deleteBtn.getAttribute('data-id');
        if (!id) return;
        const container = deleteBtn.closest('.user-row');
        if (container) container.remove();
        deleteBtn.disabled = true;
        try {
          const rf = await fetchJSON('api/rf/delete', {
            method: 'POST',
            body: JSON.stringify({ id }),
          });
          renderRf(rf);
          showToast('Remote deleted.');
        } catch (error) {
          handleError(error, error.message || 'Failed to delete remote');
          await loadState();
        } finally {
          deleteBtn.disabled = false;
        }
      }
    });
  }
};

const renderLogs = (logs = []) => {
  const list = App.elements.logItems;
  const emptyState = App.elements.logEmptyState;
  if (!list || !emptyState) {
    return;
  }

  if (!logs || logs.length === 0) {
    list.hidden = true;
    list.innerHTML = '';
    emptyState.hidden = false;
    return;
  }

  emptyState.hidden = true;
  list.hidden = false;

  const entries = logs.slice().reverse().map((entry) => {
    const timestampMs = entry.timestamp ?? 0;
    const secondsSinceBoot = Math.round(timestampMs / 1000);
    const message = escapeHtml(entry.message || '');
    let timeLabel = `${secondsSinceBoot}s since boot`;
    if (entry.unixTime) {
      const date = new Date(entry.unixTime * 1000);
      timeLabel = `${date.toLocaleString()} (${secondsSinceBoot}s since boot)`;
    }
    return `
      <li class="log-item">
        <span class="meta">${timeLabel}</span>
        <span>${message}</span>
      </li>
    `;
  });

  list.innerHTML = entries.join('');
};

// Keypad PIN Management
const buildKeypadUserRow = (user, index, existingValue) => {
  // Use existing input value if user was editing, otherwise use stored name
  const name = escapeHtml(existingValue !== undefined ? existingValue : (user.name || `User ${index + 1}`));
  const pinCount = Array.isArray(user.pins)
    ? user.pins.length
    : (user.pin ? 1 : 0);
  const pinLabel = pinCount === 1 ? '1 PIN' : `${pinCount} PINs`;
  
  return `
    <div class="user-row credential-card credential-card--pin" data-uuid="${escapeHtml(user.uuid || '')}">
      <div class="credential-card-header">
        <span class="credential-kind">User</span>
        <span class="user-code">${pinLabel}</span>
      </div>
      <div class="user-info">
        <label class="stacked">
          <span>Name</span>
          <input type="text" class="user-name-input" value="${name}" placeholder="Enter name...">
        </label>
        <span class="user-channel">Use enrollment to add RFID cards, PINs, or remotes.</span>
      </div>
      <div class="user-actions">
        <button type="button" class="secondary" data-action="save-pin" data-uuid="${escapeHtml(user.uuid || '')}">Save</button>
        <button type="button" class="secondary danger" data-action="delete-pin" data-uuid="${escapeHtml(user.uuid || '')}">Delete</button>
      </div>
    </div>
  `;
};

const renderKeypadUsers = (users = []) => {
  const listEl = App.elements.keypadUserList;
  renderEnrollmentUserOptions(users);
  if (App.elements.keypadRemoveAllBtn) {
    App.elements.keypadRemoveAllBtn.disabled = !users.length;
  }
  if (!listEl) return;

  if (!users || users.length === 0) {
    listEl.innerHTML = '<p class="empty-state muted">No PIN codes configured yet.</p>';
  } else {
    // Preserve input values that user may be editing
    const existingValues = {};
    listEl.querySelectorAll('.user-row').forEach((row) => {
      const uuid = row.getAttribute('data-uuid');
      const input = row.querySelector('.user-name-input');
      if (uuid && input && document.activeElement === input) {
        existingValues[uuid] = input.value;
      }
    });

    listEl.innerHTML = users
      .map((user, idx) => buildKeypadUserRow(user, idx, existingValues[user.uuid]))
      .join('');

    // Restore focus if user was editing
    Object.keys(existingValues).forEach((uuid) => {
      const row = listEl.querySelector(`.user-row[data-uuid="${uuid}"]`);
      if (row) {
        const input = row.querySelector('.user-name-input');
        if (input) {
          input.focus();
          input.setSelectionRange(input.value.length, input.value.length);
        }
      }
    });
  }
};

const renderEnrollmentUserOptions = (users = []) => {
  const select = App.elements.enrollUserSelect;
  if (!select) return;

  const previous = select.value;
  if (!users.length) {
    select.innerHTML = '<option value="">Default User</option>';
    return;
  }

  select.innerHTML = users
    .map((user, index) => {
      const uuid = escapeHtml(user.uuid || '');
      const name = escapeHtml(user.name || `User ${index + 1}`);
      return `<option value="${uuid}">${name}</option>`;
    })
    .join('');

  if (previous && users.some((user) => user.uuid === previous)) {
    select.value = previous;
  }
};

const renderWifi = (wifi = {}, networkState = {}, system = {}) => {
  const list = document.getElementById('wifiNetworks');
  const availableList = document.getElementById('wifiAvailableNetworks');
  const activeEl = document.getElementById('wifiActive');
  if (!list) return;
  const networks = wifi.networks || [];
  const scanned = wifi.scanned || [];
  const active = wifi.active_ssid || '';
  const savedSsids = new Set(networks.map((network) => network.ssid).filter(Boolean));
  const activeSaved = networks.find((network) => network.ssid === active) || null;
  const activeScan = scanned
    .filter((network) => network.ssid === active)
    .sort((left, right) => (Number(right.rssi) || -999) - (Number(left.rssi) || -999))[0] || null;
  if (activeEl) {
    if (!active) {
      activeEl.innerHTML = '<p class="empty-state muted">No active station network.</p>';
    } else {
      const connectedAtMs = activeSaved?.last_used_ms;
      const connectedForSeconds = Number.isFinite(Number(connectedAtMs))
        ? Math.max(0, (Number(system.uptimeSeconds) || 0) - Math.round(Number(connectedAtMs) / 1000))
        : null;
      const liveSignal = formatWifiLink(networkState.wifi_sta_quality, networkState.wifi_sta_rssi);
      const activeDetails = [
        ['Link quality', liveSignal !== '—' ? liveSignal : formatWifiSignal(activeScan)],
        ['Connected', connectedForSeconds != null ? `${formatUptime(connectedForSeconds)} ago` : formatSinceBoot(connectedAtMs)],
        ['STA IP', networkState.wifi_sta_ip || '—'],
        ['Gateway', networkState.wifi_sta_gateway || '—'],
        ['STA MAC', networkState.wifi_sta_mac || '—'],
        ['AP BSSID', networkState.wifi_sta_bssid || activeScan?.bssid || '—'],
        ['Channel', networkState.wifi_sta_channel || activeScan?.channel || '—'],
        ['Security', networkState.wifi_sta_auth || activeScan?.auth || (activeScan?.secure ? 'Secured' : '—')],
      ];
      const strength = Number.isFinite(Number(networkState.wifi_sta_quality))
        ? Math.max(0, Math.min(100, Math.round(Number(networkState.wifi_sta_quality))))
        : (wifiSignalFromRssi(activeScan?.rssi) ?? 0);
      activeEl.innerHTML = `
        <div class="wifi-active-summary">
          <div class="wifi-active-title">${escapeHtml(active)}</div>
          <div class="wifi-active-signal">
            <span class="wifi-strength" aria-hidden="true"><span style="width:${strength}%"></span></span>
            <strong>${escapeHtml(activeDetails[0][1])}</strong>
          </div>
        </div>
        <div class="wifi-active-grid">
          ${activeDetails.map(([label, value]) => `
            <div class="wifi-detail">
              <span>${escapeHtml(label)}</span>
              <strong>${escapeHtml(value || '—')}</strong>
            </div>
          `).join('')}
        </div>
      `;
    }
  }

  if (availableList) {
    if (!scanned.length) {
      availableList.innerHTML = '<p class="empty-state muted">No nearby networks found yet.</p>';
    } else {
      availableList.innerHTML = scanned
        .map((network) => {
          const ssid = network.ssid || '';
          const saved = savedSsids.has(ssid);
          const strength = wifiSignalFromRssi(network.rssi) ?? 0;
          return `
            <button type="button" class="wifi-network-card" data-action="wifi-select" data-ssid="${escapeHtml(ssid)}">
              <span class="wifi-network-name">${escapeHtml(ssid)}</span>
              <span class="wifi-network-meta">${escapeHtml(network.auth || (network.secure ? 'Secured' : 'Open'))}${saved ? ' · Saved' : ''}</span>
              <span class="wifi-strength" aria-hidden="true"><span style="width:${strength}%"></span></span>
            </button>
          `;
        })
        .join('');
    }
  }

  if (!networks.length) {
    list.innerHTML = '<p class="empty-state muted">No saved Wi‑Fi networks.</p>';
    return;
  }
  list.innerHTML = networks
    .map(
      (n) => `
      <div class="wifi-saved-card ${active === n.ssid ? 'is-active' : ''}" data-ssid="${escapeHtml(n.ssid || '')}">
        <div class="wifi-saved-info">
          <strong>${escapeHtml(n.ssid || '')}</strong>
          <span>${active === n.ssid ? 'Active now' : 'Saved network'}</span>
          <div class="meta muted">${n.last_used_ms ? `Last used: ${formatSinceBoot(n.last_used_ms)}` : 'Not used since boot'}</div>
        </div>
        <div class="wifi-saved-actions">
          <button type="button" class="secondary" data-action="wifi-connect" data-ssid="${escapeHtml(n.ssid || '')}" ${active === n.ssid ? 'disabled' : ''}>Connect</button>
          <button type="button" class="secondary danger" data-action="wifi-delete" data-ssid="${escapeHtml(n.ssid || '')}">Delete</button>
        </div>
      </div>
    `
    )
    .join('');
};

const setupKeypadPinHandlers = () => {
  const addBtn = App.elements.keypadAddBtn;
  const addForm = App.elements.keypadAddForm;
  const cancelBtn = App.elements.keypadCancelBtn;
  const saveNewBtn = App.elements.keypadSaveNewBtn;
  const listEl = App.elements.keypadUserList;
  const removeAllBtn = App.elements.keypadRemoveAllBtn;

  if (addBtn && addForm) {
    addBtn.addEventListener('click', () => {
      addForm.hidden = false;
      addBtn.disabled = true;
      const nameInput = document.getElementById('keypadNewName');
      if (nameInput) nameInput.focus();
    });
  }

  if (cancelBtn && addForm && addBtn) {
    cancelBtn.addEventListener('click', () => {
      addForm.hidden = true;
      addBtn.disabled = false;
      document.getElementById('keypadNewName').value = '';
      document.getElementById('keypadNewPin').value = '';
    });
  }

  if (saveNewBtn) {
    saveNewBtn.addEventListener('click', async () => {
      const nameInput = document.getElementById('keypadNewName');
      const pinInput = document.getElementById('keypadNewPin');
      const name = nameInput?.value.trim() || '';
      const pin = pinInput?.value.trim() || '';

      if (!name || !pin) {
        showToast('Please enter both name and PIN code.');
        return;
      }

      if (!/^\d{4,8}$/.test(pin)) {
        showToast('PIN must be 4-8 digits.');
        return;
      }

      saveNewBtn.disabled = true;
      try {
        const users = await fetchJSON('api/keypad/user', {
          method: 'POST',
          body: JSON.stringify({ name, pin }),
        });
        renderKeypadUsers(Array.isArray(users) ? users : []);
        showToast('PIN code added successfully.');
        addForm.hidden = true;
        addBtn.disabled = false;
        nameInput.value = '';
        pinInput.value = '';
        if (App.data) App.data.keypadUsers = Array.isArray(users) ? users : [];
        // Refresh list from flash to match persisted state
        loadKeypadUsers();
      } catch (error) {
        handleError(error, 'Failed to add PIN code');
      } finally {
        saveNewBtn.disabled = false;
      }
    });
  }

  if (removeAllBtn) {
    removeAllBtn.addEventListener('click', async () => {
      const users = App.data?.keypadUsers || [];
      if (!users.length) {
        showToast('No users to remove.');
        return;
      }

      removeAllBtn.disabled = true;
      try {
        const latest = await fetchJSON('api/keypad/users/delete-all', {
          method: 'POST',
          body: JSON.stringify({}),
        });
        const list = Array.isArray(latest) ? latest : [];
        renderKeypadUsers(list);
        if (App.data) App.data.keypadUsers = list;
        await loadKeypadUsers();
        await loadState();
        showToast(`Removed ${users.length} user${users.length === 1 ? '' : 's'}.`);
      } catch (error) {
        handleError(error, 'Failed to remove users');
      } finally {
        removeAllBtn.disabled = false;
      }
    });
  }

  if (listEl) {
    listEl.addEventListener('click', async (event) => {
      const saveBtn = event.target.closest('button[data-action="save-pin"]');
      const deleteBtn = event.target.closest('button[data-action="delete-pin"]');

      if (saveBtn) {
        const container = saveBtn.closest('.user-row');
        const input = container?.querySelector('.user-name-input');
        const uuid = saveBtn.getAttribute('data-uuid');
        const name = input?.value.trim();

        if (!uuid || !name) {
          showToast('Please provide a name.');
          return;
        }

        saveBtn.disabled = true;
        try {
          const users = await fetchJSON('api/keypad/user', {
            method: 'PUT',
            body: JSON.stringify({ uuid, name }),
          });
          renderKeypadUsers(Array.isArray(users) ? users : []);
          showToast('PIN user updated.');
          if (App.data) App.data.keypadUsers = Array.isArray(users) ? users : [];
        } catch (error) {
          handleError(error, 'Failed to update user');
        } finally {
          saveBtn.disabled = false;
        }
      }

      if (deleteBtn) {
        const uuid = deleteBtn.getAttribute('data-uuid');
        if (!uuid) return;

        deleteBtn.disabled = true;
        try {
          const users = await fetchJSON('api/keypad/user', {
            method: 'DELETE',
            body: JSON.stringify({ uuid }),
          });
          renderKeypadUsers(Array.isArray(users) ? users : []);
          showToast('PIN code deleted.');
          if (App.data) App.data.keypadUsers = Array.isArray(users) ? users : [];
          loadKeypadUsers();
        } catch (error) {
          handleError(error, 'Failed to delete PIN code');
        } finally {
          deleteBtn.disabled = false;
        }
      }
    });
  }
};

const setOtaProgress = (percent) => {
  const bar = App.elements.otaProgressBar;
  if (bar) {
    bar.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  }
};

const setOtaStatus = (message) => {
  if (App.elements.otaStatus) {
    App.elements.otaStatus.textContent = message;
  }
};

const waitForDeviceAfterOta = async () => {
  await sleep(3500);
  for (let attempt = 0; attempt < 45; attempt++) {
    try {
      const state = await fetchJSON(`api/state?t=${Date.now()}`);
      if (state?.system?.firmware) {
        App.data = state;
        renderState(state);
        return true;
      }
    } catch (error) {
      // Reboot drops requests briefly.
    }
    await sleep(2000);
  }
  return false;
};

const uploadOtaFile = (file) => new Promise((resolve, reject) => {
  const href = window.location.href;
  const baseHref = href.endsWith('/') ? href : `${href}/`;
  const url = new URL('api/ota/upload', baseHref);
  const xhr = new XMLHttpRequest();

  xhr.open('POST', url.toString());
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.setRequestHeader('X-Firmware-Filename', file.name || 'firmware.bin');

  xhr.upload.onprogress = (event) => {
    if (event.lengthComputable) {
      setOtaProgress((event.loaded / event.total) * 100);
      setOtaStatus(`Uploading ${formatBytes(event.loaded)} of ${formatBytes(event.total)}`);
    }
  };

  xhr.onload = () => {
    const text = xhr.responseText || '';
    if (xhr.status >= 200 && xhr.status < 300) {
      try {
        resolve(text ? JSON.parse(text) : {});
      } catch (error) {
        resolve({});
      }
      return;
    }
    reject(new Error(text || `OTA upload failed: HTTP ${xhr.status}`));
  };

  xhr.onerror = () => reject(new Error('OTA upload connection failed'));
  xhr.ontimeout = () => reject(new Error('OTA upload timed out'));
  xhr.timeout = 120000;
  xhr.send(file);
});

const setupOtaHandlers = () => {
  const form = App.elements.otaForm;
  const fileInput = App.elements.otaFile;
  const uploadBtn = App.elements.otaUploadBtn;
  const fileName = App.elements.otaFileName;
  const fileSize = App.elements.otaFileSize;
  const dropzone = App.elements.otaDropzone;
  const browseBtn = App.elements.otaBrowseBtn;
  let selectedFile = null;

  if (!form || !fileInput || !uploadBtn) return;

  const setSelectedFile = (file) => {
    selectedFile = file || null;
    uploadBtn.disabled = !file;
    if (fileName) fileName.textContent = file ? file.name : 'No file selected';
    if (fileSize) fileSize.textContent = file ? formatBytes(file.size) : '—';
    setOtaProgress(0);
    setOtaStatus(file ? 'Ready' : 'Idle');
  };

  const pickFile = () => {
    if (!fileInput.disabled) fileInput.click();
  };

  fileInput.addEventListener('change', () => {
    setSelectedFile(fileInput.files?.[0] || null);
  });

  if (browseBtn) {
    browseBtn.addEventListener('click', pickFile);
  }

  if (dropzone) {
    dropzone.addEventListener('click', (event) => {
      if (event.target === browseBtn) return;
      pickFile();
    });
    dropzone.addEventListener('keydown', (event) => {
      if (event.key !== 'Enter' && event.key !== ' ') return;
      event.preventDefault();
      pickFile();
    });
    dropzone.addEventListener('dragenter', (event) => {
      event.preventDefault();
      dropzone.classList.add('drag-over');
    });
    dropzone.addEventListener('dragover', (event) => {
      event.preventDefault();
      dropzone.classList.add('drag-over');
    });
    dropzone.addEventListener('dragleave', (event) => {
      if (!dropzone.contains(event.relatedTarget)) {
        dropzone.classList.remove('drag-over');
      }
    });
    dropzone.addEventListener('drop', (event) => {
      event.preventDefault();
      dropzone.classList.remove('drag-over');
      if (fileInput.disabled) return;
      const file = event.dataTransfer?.files?.[0] || null;
      if (!file) return;
      if (!file.name.toLowerCase().endsWith('.bin')) {
        showToast('Select a firmware .bin file.');
        return;
      }
      setSelectedFile(file);
    });
  }

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    const file = selectedFile || fileInput.files?.[0] || null;
    if (!file) {
      showToast('Select a firmware binary.');
      return;
    }

    uploadBtn.disabled = true;
    fileInput.disabled = true;
    if (browseBtn) browseBtn.disabled = true;
    setOtaProgress(0);
    setOtaStatus('Starting OTA upload');

    try {
      const result = await uploadOtaFile(file);
      setOtaProgress(100);
      setOtaStatus(`Installed ${formatBytes(result.bytes || file.size)} to ${result.partition || 'OTA slot'}. Rebooting.`);
      showToast('Firmware uploaded. Rebooting.');
      const online = await waitForDeviceAfterOta();
      if (online) {
        setOtaStatus('Back online');
        showToast('Controller is back online.');
        fileInput.value = '';
        setSelectedFile(null);
        setOtaProgress(0);
      } else {
        setOtaStatus('Reboot is taking longer than expected');
      }
    } catch (error) {
      setOtaStatus(error.message || 'OTA upload failed');
      handleError(error, 'OTA upload failed');
    } finally {
      fileInput.disabled = false;
      if (browseBtn) browseBtn.disabled = false;
      uploadBtn.disabled = !(fileInput.files && fileInput.files.length);
      uploadBtn.disabled = !selectedFile;
    }
  });
};

const setupEditableLabels = () => {
  const LABEL_STORAGE_KEY = 'ac_section_labels';

  // Load saved labels
  let savedLabels = {};
  try {
    savedLabels = JSON.parse(localStorage.getItem(LABEL_STORAGE_KEY) || '{}');
  } catch (e) { /* ignore */ }

  document.querySelectorAll('.section-label').forEach((input) => {
    const id = input.id;
    // Restore saved value if present
    if (savedLabels[id]) {
      input.value = savedLabels[id];
    }

    // Save on change
    input.addEventListener('change', () => {
      let labels = {};
      try {
        labels = JSON.parse(localStorage.getItem(LABEL_STORAGE_KEY) || '{}');
      } catch (e) { labels = {}; }
      labels[id] = input.value;
      localStorage.setItem(LABEL_STORAGE_KEY, JSON.stringify(labels));
    });
  });
};

document.addEventListener('DOMContentLoaded', () => {
  App.elements = {
    navItems: Array.from(document.querySelectorAll('.nav-item')),
    pages: Array.from(document.querySelectorAll('.page')),
    toast: document.getElementById('toast'),
    wiegandStatus: document.getElementById('wiegandStatus'),
    wiegandStatusBar: document.getElementById('wiegandStatusBar'),
    wiegandPending: document.getElementById('wiegandPending'),
    wiegandDuplicate: document.getElementById('wiegandDuplicate'),
    wiegandUserList: document.getElementById('wiegandUserList'),
    wiegandRegisterBtn: document.getElementById('wiegandRegisterBtn'),
    wiegandStopBtn: document.getElementById('wiegandStopBtn'),
    wiegandChannelSelect: document.getElementById('wiegandChannelSelect'),
    wiegandRemoveAllBtn: document.getElementById('wiegandRemoveAllBtn'),
    rfStatus: document.getElementById('rfStatus'),
    rfStatusBar: document.getElementById('rfStatusBar'),
    rfPending: document.getElementById('rfPending'),
    rfDuplicate: document.getElementById('rfDuplicate'),
    rfUserList: document.getElementById('rfUserList'),
    rfRegisterBtn: document.getElementById('rfRegisterBtn'),
    rfStopBtn: document.getElementById('rfStopBtn'),
    rfRemoveAllBtn: document.getElementById('rfRemoveAllBtn'),
    rfQuality: document.getElementById('rfQuality'),
    rfLastCode: document.getElementById('rfLastCode'),
    rfTiming: document.getElementById('rfTiming'),
    rfJitter: document.getElementById('rfJitter'),
    rfRepeats: document.getElementById('rfRepeats'),
    rfNoise: document.getElementById('rfNoise'),
    rfDecode: document.getElementById('rfDecode'),
    rfCaptures: document.getElementById('rfCaptures'),
    rfLastInput: document.getElementById('rfLastInput'),
    rfReceiver: document.getElementById('rfReceiver'),
    enrollUserSelect: document.getElementById('enrollUserSelect'),
    enrollStartBtn: document.getElementById('enrollStartBtn'),
    enrollStopBtn: document.getElementById('enrollStopBtn'),
    keypadUserList: document.getElementById('keypadUserList'),
    keypadAddBtn: document.getElementById('keypadAddBtn'),
    keypadAddForm: document.getElementById('keypadAddForm'),
    keypadCancelBtn: document.getElementById('keypadCancelBtn'),
    keypadSaveNewBtn: document.getElementById('keypadSaveNewBtn'),
    keypadRemoveAllBtn: document.getElementById('keypadRemoveAllBtn'),
    logItems: document.getElementById('logItems'),
    logEmptyState: document.getElementById('logEmptyState'),
    wifiNetworks: document.getElementById('wifiNetworks'),
    wifiActive: document.getElementById('wifiActive'),
    otaForm: document.getElementById('otaForm'),
    otaFile: document.getElementById('otaFile'),
    otaDropzone: document.getElementById('otaDropzone'),
    otaBrowseBtn: document.getElementById('otaBrowseBtn'),
    otaUploadBtn: document.getElementById('otaUploadBtn'),
    otaFileName: document.getElementById('otaFileName'),
    otaFileSize: document.getElementById('otaFileSize'),
    otaProgressBar: document.getElementById('otaProgressBar'),
    otaStatus: document.getElementById('otaStatus'),
  };

  bindNavigation();
  setupControlCardChrome();
  setupLockHandlers();
  setupExitHandlers();
  setupFobHandlers();
  setupKeypadHandlers();
  setupEnrollmentHandlers();
  setupForms();
  setupWiegandHandlers();
  setupRfHandlers();
  setupKeypadPinHandlers();
  setupMotionHandlers();
  setupOtaHandlers();
  setupEditableLabels();

  loadState();
  // Defer heavy endpoints until their tabs are opened; tunneling adds overhead
  // and these requests can clobber the ESP32 heap when fired all at once.
  onPageActivated('device');
  const startStatePolling = () => {
    if (App.stateTimer) return;
    App.stateTimer = setInterval(loadState, 20000);
  };
  const stopStatePolling = () => {
    if (App.stateTimer) {
      clearInterval(App.stateTimer);
      App.stateTimer = null;
    }
  };

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      stopStatePolling();
      stopEnrollmentPolling();
      stopSignalPolling();
      stopUptimeClock();
    } else {
      loadState();
      startStatePolling();
      startSignalPolling();
      startUptimeClock();
    }
  });

  startStatePolling();
  startSignalPolling();
  startUptimeClock();
});

/**
 * settings.js — Robot settings frontend
 *
 * Architecture
 * ────────────
 * State lives in one plain object `CFG` that mirrors robot_settings_t exactly.
 * On load:  GET /api/settings → populate CFG → render all controls.
 * On save:  collect changed fields from the relevant tab → POST /api/settings
 *           (partial update — only send what changed, server merges).
 * WS:       Reuses the same /ws endpoint as the control page for live telemetry.
 *           Only subscribes to telemetry frames; does NOT send drive commands.
 *
 * OTA
 * ───
 * Streaming XHR upload with progress. Token sent as X-OTA-Token header if set.
 * Firmware: POST /ota/firmware → device reboots.
 * Filesystem: POST /ota/filesystem → remounts, no reboot.
 *
 * Motor preview (drive tab)
 * ─────────────────────────
 * Canvas joystick → simulated ramp with current deadband/ramp_rate settings.
 * Purely local — no WS commands sent from this page. The user can feel the
 * curve before committing values to the device.
 */

'use strict';

/* ── Constants ──────────────────────────────────────────────────────────── */
const WS_URL = `ws://${location.host}/ws`;
const API    = '/api/settings';

/* Fields that require a reboot — track locally too for instant banner */
const REBOOT_FIELDS = new Set([
  'ap_ssid','ap_password','ap_channel','mdns_enable','mdns_hostname'
]);

/* ── State ──────────────────────────────────────────────────────────────── */
let CFG        = {};          // current settings from device
let pendingReboot = false;    // at least one reboot-required field was saved
let ws         = null;
let wsRetry    = null;
let lastTelem  = 0;           // timestamp of last telemetry frame

/* ── Helpers ────────────────────────────────────────────────────────────── */
const $  = id => document.getElementById(id);
const el = (tag, cls, txt) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt !== undefined) e.textContent = txt;
  return e;
};

function showToast(msg, type = 'ok', durationMs = 2800) {
  const t = $('toast');
  t.textContent  = msg;
  t.className    = `toast show ${type}`;
  clearTimeout(t._timer);
  t._timer = setTimeout(() => { t.className = 'toast'; }, durationMs);
}

function setConn(live) {
  const ind = $('connIndicator');
  ind.classList.toggle('live', live);
  ind.querySelector('.conn-label').textContent = live ? 'LIVE' : 'OFFLINE';
}

/* ── Confirm dialog ─────────────────────────────────────────────────────── */
function confirm(title, msg) {
  return new Promise(resolve => {
    let overlay = document.querySelector('.confirm-overlay');
    if (!overlay) {
      overlay = el('div','confirm-overlay');
      overlay.innerHTML = `
        <div class="confirm-box">
          <div class="confirm-title" id="cfmTitle"></div>
          <div class="confirm-msg"   id="cfmMsg"></div>
          <div class="confirm-btns">
            <button class="btn btn-ghost" id="cfmCancel">CANCEL</button>
            <button class="btn btn-danger" id="cfmOk">CONFIRM</button>
          </div>
        </div>`;
      document.body.appendChild(overlay);
    }
    $('cfmTitle').textContent = title;
    $('cfmMsg').textContent   = msg;
    overlay.classList.add('show');

    const cleanup = ok => {
      overlay.classList.remove('show');
      $('cfmOk').onclick     = null;
      $('cfmCancel').onclick = null;
      overlay.onclick        = null;
      resolve(ok);
    };
    $('cfmOk').onclick     = () => cleanup(true);
    $('cfmCancel').onclick = () => cleanup(false);
    overlay.onclick = e => { if (e.target === overlay) cleanup(false); };
  });
}

/* ── Settings API ───────────────────────────────────────────────────────── */
async function fetchSettings() {
  try {
    const r = await fetch(API);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    CFG = await r.json();
    renderAll();
    updateDeviceTitle(CFG.device_name);
  } catch (e) {
    showToast('Failed to load settings — ' + e.message, 'err', 4000);
  }
}

async function saveFields(fields) {
  const payload = {};
  let willReboot = false;

  for (const [key, val] of Object.entries(fields)) {
    payload[key] = val;
    if (REBOOT_FIELDS.has(key)) willReboot = true;
  }

  const btn = document.querySelector(`[data-save]`);  // active save btn
  if (btn) btn.classList.add('saving');

  try {
    const r = await fetch(API, {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(payload),
    });
    const data = await r.json();

    if (!r.ok) {
      showToast('Error: ' + (data.error || r.statusText), 'err', 5000);
      return;
    }

    // Merge saved values back into CFG
    Object.assign(CFG, payload);

    if (data.reboot_required || willReboot) {
      pendingReboot = true;
      $('rebootBanner').classList.remove('hidden');
    }

    showToast('Saved.', 'ok');
  } catch (e) {
    showToast('Save failed: ' + e.message, 'err', 5000);
  } finally {
    if (btn) btn.classList.remove('saving');
  }
}

/* ── Render controls from CFG ───────────────────────────────────────────── */
function renderAll() {
  // Identity
  setInput('deviceName_input',    CFG.device_name     ?? '');
  setInput('mdns_hostname',       CFG.mdns_hostname   ?? '');
  setCheck('mdns_enable',         CFG.mdns_enable     ?? false);
  updateMdnsPreview();

  // Network
  setInput('ap_ssid',             CFG.ap_ssid         ?? '');
  setInput('ap_password',         '');  // never pre-fill password
  setSlider('ap_channel',         CFG.ap_channel      ?? 1);

  // Drive
  setSlider('drive_deadband',     CFG.drive_deadband  ?? 0.05);
  setSlider('drive_ramp_rate',    CFG.drive_ramp_rate ?? 0.05);
  setSlider('drive_watchdog_ms',  CFG.drive_watchdog_ms ?? 500);

  // Telemetry
  setSlider('telemetry_interval_ms', CFG.telemetry_interval_ms ?? 200);
  setSlider('rssi_warn_dbm',         CFG.rssi_warn_dbm ?? -75);
  setSlider('display_sleep_timeout_s', CFG.display_sleep_timeout_s ?? 0);

  // Security — OTA token
  const badge = $('tokenBadge');
  if (CFG.ota_token_set) {
    badge.textContent = 'SET';
    badge.classList.add('set');
  } else {
    badge.textContent = 'NOT SET';
    badge.classList.remove('set');
  }

  // Update device name in header
  updateDeviceTitle(CFG.device_name);
}

function setInput(id, val) {
  const el = $(id);
  if (el) el.value = val;
}

function setCheck(id, val) {
  const el = $(id);
  if (el) el.checked = !!val;
}

function setSlider(id, val) {
  const el = $(id);
  if (!el) return;
  el.value = val;
  updateSliderDisplay(id);
}

function updateDeviceTitle(name) {
  const el = $('deviceName');
  if (el) el.textContent = (name || 'ROBOT').toUpperCase();
}

/* ── Slider display formatting ──────────────────────────────────────────── */
function updateSliderDisplay(id) {
  const v   = parseFloat($(id)?.value ?? 0);
  const out = $(id + '_val');
  if (!out) return;

  switch (id) {
    case 'drive_deadband':
    case 'drive_ramp_rate':
      out.textContent = v.toFixed(3);
      break;
    case 'drive_watchdog_ms':
      out.textContent = v + ' ms';
      break;
    case 'telemetry_interval_ms':
      out.textContent = v + ' ms (' + (1000 / v).toFixed(1) + ' Hz)';
      break;
    case 'rssi_warn_dbm':
      out.textContent = '−' + Math.abs(v) + ' dBm';
      break;
    case 'display_sleep_timeout_s':
      out.textContent = v === 0 ? 'OFF' : v + ' s';
      break;
    case 'ap_channel':
      out.textContent = v === 0 ? 'AUTO' : String(v);
      break;
    default:
      out.textContent = v;
  }
}

/* ── mDNS preview ───────────────────────────────────────────────────────── */
function updateMdnsPreview() {
  const h = $('mdns_hostname')?.value?.trim() || 'robot';
  const p = $('mdnsPreview');
  if (p) p.textContent = h + '.local';
}

/* ── Tab switching ──────────────────────────────────────────────────────── */
function initTabs() {
  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
      document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
      btn.classList.add('active');
      $('tab-' + btn.dataset.tab)?.classList.add('active');
    });
  });
}

/* ── Save button wiring ─────────────────────────────────────────────────── */
function initSaveButtons() {
  document.querySelectorAll('[data-save]').forEach(btn => {
    btn.addEventListener('click', () => handleSave(btn.dataset.save));
  });
}

function handleSave(tab) {
  const fields = collectTab(tab);
  if (!fields) return;

  // Client-side validation before sending
  const err = validateFields(fields);
  if (err) { showToast(err, 'err', 4000); return; }

  saveFields(fields);
}

/* Collect only the fields belonging to each tab */
function collectTab(tab) {
  switch (tab) {
    case 'identity':
      return {
        device_name:    $('deviceName_input').value.trim(),
        mdns_hostname:  $('mdns_hostname').value.trim(),
        mdns_enable:    $('mdns_enable').checked,
      };
    case 'network': {
      const fields = {
        ap_ssid:    $('ap_ssid').value.trim(),
        ap_channel: parseInt($('ap_channel').value, 10),
      };
      const pw = $('ap_password').value;
      if (pw) fields.ap_password = pw;  // only send if user typed something
      return fields;
    }
    case 'drive':
      return {
        drive_deadband:    parseFloat($('drive_deadband').value),
        drive_ramp_rate:   parseFloat($('drive_ramp_rate').value),
        drive_watchdog_ms: parseInt($('drive_watchdog_ms').value, 10),
      };
    case 'telemetry':
      return {
        telemetry_interval_ms:   parseInt($('telemetry_interval_ms').value, 10),
        rssi_warn_dbm:           parseInt($('rssi_warn_dbm').value, 10),
        display_sleep_timeout_s: parseInt($('display_sleep_timeout_s').value, 10),
      };
    case 'security': {
      const fields = {};
      const tok = $('ota_token').value;
      if (tok !== undefined) fields.ota_token = tok;  // empty = clear token
      return fields;
    }
    default:
      return null;
  }
}

/* Client-side field validation — mirrors settings_validate() constraints */
function validateFields(fields) {
  if ('device_name' in fields && !fields.device_name)
    return 'Device name must not be empty.';

  if ('ap_ssid' in fields && !fields.ap_ssid)
    return 'AP SSID must not be empty.';

  if ('ap_password' in fields) {
    const len = fields.ap_password.length;
    if (len > 0 && len < 8)
      return 'AP password must be empty (open) or at least 8 characters.';
  }

  if ('ap_channel' in fields && (fields.ap_channel < 0 || fields.ap_channel > 13))
    return 'AP channel must be 0 (auto) or 1–13.';

  if ('drive_deadband' in fields) {
    const v = fields.drive_deadband;
    if (v < 0 || v > 0.5) return 'Deadband must be between 0.0 and 0.5.';
  }

  if ('drive_ramp_rate' in fields) {
    const v = fields.drive_ramp_rate;
    if (v <= 0 || v > 1.0) return 'Ramp rate must be in (0.0, 1.0].';
  }

  if ('drive_watchdog_ms' in fields) {
    const v = fields.drive_watchdog_ms;
    if (v < 100 || v > 10000) return 'Watchdog must be between 100 and 10000 ms.';
  }

  if ('telemetry_interval_ms' in fields) {
    const v = fields.telemetry_interval_ms;
    if (v < 50 || v > 10000) return 'Telemetry interval must be 50–10000 ms.';
  }

  if ('rssi_warn_dbm' in fields) {
    const v = fields.rssi_warn_dbm;
    if (v < -120 || v > 0) return 'RSSI threshold must be between -120 and 0 dBm.';
  }

  return null;
}

/* ── WebSocket for live telemetry ───────────────────────────────────────── */
function wsConnect() {
  if (ws && ws.readyState <= WebSocket.OPEN) return;
  clearTimeout(wsRetry);

  ws = new WebSocket(WS_URL);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    setConn(true);
    // Send ping to keep watchdog alive without driving motors
    ws.send('ping');
  };

  ws.onmessage = e => {
    // This page only reads telemetry (JSON text frames)
    // Binary frames are motor axes — ignore them
    if (typeof e.data !== 'string') return;
    try {
      const d = JSON.parse(e.data);
      updateTelemetry(d);
    } catch (_) {}
  };

  ws.onerror = () => {};

  ws.onclose = () => {
    setConn(false);
    markTelemStale();
    wsRetry = setTimeout(wsConnect, 3000);
  };
}

function updateTelemetry(d) {
  lastTelem = Date.now();

  const set = (id, val) => {
    const el = $(id);
    if (!el) return;
    el.textContent = val;
    el.classList.remove('stale');
  };

  set('t_rssi',   d.rssi   != null ? d.rssi + ' dBm' : '—');
  set('t_bat',    d.battery != null ? d.battery + '%' : '—');
  set('t_temp',   d.temp    != null ? d.temp.toFixed(1) + ' °C' : 'N/A');
  set('t_heap',   d.heap    != null ? (d.heap / 1024).toFixed(0) + ' KB' : '—');
  set('t_uptime', d.uptime  != null ? fmtUptime(d.uptime) : '—');
  set('t_errors', d.errors  != null ? String(d.errors) : '—');
}

function markTelemStale() {
  ['t_rssi','t_bat','t_temp','t_heap','t_uptime','t_errors'].forEach(id => {
    const el = $(id);
    if (el) el.classList.add('stale');
  });
}

function fmtUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${sec}s`;
  return `${sec}s`;
}

/* Keep WS alive — ping every 4s so watchdog doesn't expire */
setInterval(() => {
  if (ws?.readyState === WebSocket.OPEN) ws.send('ping');
}, 4000);

/* Mark telemetry stale if no frame received in 3 s */
setInterval(() => {
  if (lastTelem && Date.now() - lastTelem > 3000) markTelemStale();
}, 1000);

/* ── Motor preview (drive tab) ──────────────────────────────────────────── */
function initMotorPreview() {
  const canvas = $('motorJoystick');
  if (!canvas) return;

  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  const cx = W / 2, cy = H / 2, R = W / 2 - 4;

  let jx = 0, jy = 0;  // joystick position, -1..1
  let dragging = false;
  let rampL = 0, rampR = 0;
  let animId = null;

  function drawJoystick() {
    ctx.clearRect(0, 0, W, H);
    // Track
    ctx.strokeStyle = '#2e332e';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, R, 0, Math.PI * 2);
    ctx.stroke();
    // Crosshair
    ctx.strokeStyle = '#222622';
    ctx.beginPath(); ctx.moveTo(cx, cy - R); ctx.lineTo(cx, cy + R); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx - R, cy); ctx.lineTo(cx + R, cy); ctx.stroke();
    // Thumb
    const tx = cx + jx * R;
    const ty = cy - jy * R;
    ctx.fillStyle = '#5aff6e';
    ctx.beginPath();
    ctx.arc(tx, ty, 7, 0, Math.PI * 2);
    ctx.fill();
  }

  function simRamp() {
    const deadband  = parseFloat($('drive_deadband')?.value ?? 0.05);
    const rampRate  = parseFloat($('drive_ramp_rate')?.value ?? 0.05);

    const ax = Math.abs(jx) < deadband ? 0 : jx;
    const ay = Math.abs(jy) < deadband ? 0 : jy;

    const wantL = Math.max(-1, Math.min(1, ay + ax));
    const wantR = Math.max(-1, Math.min(1, ay - ax));

    const clamp = (v,lo,hi) => v < lo ? lo : v > hi ? hi : v;
    const step = (cur, tgt) => {
      const d = tgt - cur;
      return cur + clamp(d, -rampRate, rampRate);
    };

    rampL = step(rampL, wantL);
    rampR = step(rampR, wantR);

    // Update bars — bar width is %, centered at 50%
    const toBar = v => Math.abs(v) * 50;
    const barL  = $('motorPreviewL');
    const barR  = $('motorPreviewR');
    if (barL) {
      barL.style.width     = toBar(rampL) + '%';
      barL.classList.toggle('active', Math.abs(rampL) > 0.01);
    }
    if (barR) {
      barR.style.width     = toBar(rampR) + '%';
      barR.classList.toggle('active', Math.abs(rampR) > 0.01);
    }

    animId = requestAnimationFrame(simRamp);
  }

  function getPos(e, rect) {
    const src = e.touches?.[0] ?? e;
    return {
      x: ((src.clientX - rect.left) - cx) / R,
      y: -((src.clientY - rect.top)  - cy) / R,
    };
  }

  canvas.addEventListener('mousedown', e => {
    dragging = true;
    const r = canvas.getBoundingClientRect();
    const p = getPos(e, r);
    const dist = Math.sqrt(p.x * p.x + p.y * p.y);
    if (dist <= 1) { jx = p.x; jy = p.y; }
    drawJoystick();
  });
  window.addEventListener('mousemove', e => {
    if (!dragging) return;
    const r = canvas.getBoundingClientRect();
    const p = getPos(e, r);
    const dist = Math.sqrt(p.x * p.x + p.y * p.y);
    jx = dist > 1 ? p.x / dist : p.x;
    jy = dist > 1 ? p.y / dist : p.y;
    drawJoystick();
  });
  window.addEventListener('mouseup', () => {
    if (!dragging) return;
    dragging = false;
    jx = 0; jy = 0;
    drawJoystick();
  });

  canvas.addEventListener('touchstart', e => {
    e.preventDefault();
    dragging = true;
    const r = canvas.getBoundingClientRect();
    const p = getPos(e, r);
    jx = Math.max(-1, Math.min(1, p.x));
    jy = Math.max(-1, Math.min(1, p.y));
    drawJoystick();
  }, { passive: false });
  canvas.addEventListener('touchmove', e => {
    e.preventDefault();
    const r = canvas.getBoundingClientRect();
    const p = getPos(e, r);
    const dist = Math.sqrt(p.x * p.x + p.y * p.y);
    jx = dist > 1 ? p.x / dist : p.x;
    jy = dist > 1 ? p.y / dist : p.y;
    drawJoystick();
  }, { passive: false });
  canvas.addEventListener('touchend', () => {
    dragging = false;
    jx = 0; jy = 0;
    drawJoystick();
  });

  drawJoystick();
  simRamp();  // start ramp simulation loop
}

/* ── OTA upload ─────────────────────────────────────────────────────────── */
function initOTA() {
  setupOtaZone({
    zone:      $('otaFwZone'),
    input:     $('otaFwFile'),
    pick:      $('otaFwPick'),
    nameEl:    $('otaFwName'),
    progressEl:$('otaFwProgress'),
    fillEl:    $('otaFwFill'),
    labelEl:   $('otaFwLabel'),
    btn:       $('otaFwBtn'),
    endpoint:  '/ota/firmware',
    onDone:    () => showToast('Firmware flashed — rebooting…', 'warn', 8000),
  });

  setupOtaZone({
    zone:      $('otaFsZone'),
    input:     $('otaFsFile'),
    pick:      $('otaFsPick'),
    nameEl:    $('otaFsName'),
    progressEl:$('otaFsProgress'),
    fillEl:    $('otaFsFill'),
    labelEl:   $('otaFsLabel'),
    btn:       $('otaFsBtn'),
    endpoint:  '/ota/filesystem',
    onDone:    () => showToast('Filesystem updated — no reboot needed.', 'ok'),
  });
}

function setupOtaZone({ zone, input, pick, nameEl, progressEl, fillEl, labelEl, btn, endpoint, onDone }) {
  let file = null;

  const setFile = f => {
    file = f;
    nameEl.textContent = f ? f.name + ' (' + (f.size / 1024).toFixed(0) + ' KB)' : '';
    btn.classList.toggle('hidden', !f);
  };

  pick.addEventListener('click', () => input.click());
  input.addEventListener('change', () => setFile(input.files[0] ?? null));

  zone.addEventListener('dragover', e => { e.preventDefault(); zone.classList.add('drag-over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('drag-over'));
  zone.addEventListener('drop', e => {
    e.preventDefault();
    zone.classList.remove('drag-over');
    setFile(e.dataTransfer.files[0] ?? null);
  });

  btn.addEventListener('click', async () => {
    if (!file) return;
    const yes = await confirm(
      'CONFIRM FLASH',
      `Upload "${file.name}" (${(file.size/1024).toFixed(0)} KB) to ${endpoint}?`
    );
    if (!yes) return;

    btn.disabled = true;
    progressEl.classList.remove('hidden');

    const xhr = new XMLHttpRequest();
    xhr.open('POST', endpoint);

    const token = CFG._ota_token_raw || '';  // never sent, but cfg has flag
    // Use ota_token from input if user typed one this session
    const inputToken = $('ota_token')?.value;
    if (inputToken) xhr.setRequestHeader('X-OTA-Token', inputToken);

    xhr.upload.onprogress = e => {
      if (!e.lengthComputable) return;
      const pct = Math.round(e.loaded / e.total * 100);
      fillEl.style.width    = pct + '%';
      labelEl.textContent   = pct + '%';
    };

    xhr.onload = () => {
      btn.disabled = false;
      progressEl.classList.add('hidden');
      if (xhr.status === 200) {
        onDone();
        setFile(null);
      } else {
        showToast('OTA failed: ' + xhr.responseText, 'err', 6000);
      }
    };

    xhr.onerror = () => {
      btn.disabled = false;
      progressEl.classList.add('hidden');
      showToast('OTA upload error — check connection.', 'err', 5000);
    };

    xhr.send(file);
  });
}

/* ── Danger zone ────────────────────────────────────────────────────────── */
function initDangerZone() {
  $('rebootBtn')?.addEventListener('click', async () => {
    const yes = await confirm('REBOOT', 'Reboot the device now?');
    if (!yes) return;
    try {
      await fetch('/api/settings/reset', {
        method: 'POST',
        body: JSON.stringify({ reboot: true }),
        headers: { 'Content-Type': 'application/json' },
      });
      showToast('Rebooting…', 'warn', 8000);
    } catch (_) {}
  });

  $('rebootDismiss')?.addEventListener('click', () => {
    $('rebootBanner').classList.add('hidden');
  });

  $('btnReboot')?.addEventListener('click', async () => {
    const yes = await confirm('REBOOT DEVICE', 'The device will restart. Connected clients will be dropped.');
    if (!yes) return;
    try {
      // POST reset with reboot:true — server reboots after response
      await fetch('/api/settings/reset', {
        method: 'POST',
        body: JSON.stringify({ reboot: true }),
        headers: { 'Content-Type': 'application/json' },
      });
      showToast('Rebooting…', 'warn', 10000);
    } catch (_) {
      showToast('Reboot command sent.', 'warn', 6000);
    }
  });

  $('btnFactoryReset')?.addEventListener('click', async () => {
    const yes = await confirm(
      'FACTORY RESET SETTINGS',
      'All tunables will be reset to firmware defaults. WiFi credentials are unaffected. The device will NOT reboot automatically.'
    );
    if (!yes) return;
    try {
      const r = await fetch('/api/settings/reset', { method: 'POST' });
      if (r.ok) {
        showToast('Settings reset to defaults.', 'warn');
        await fetchSettings();
      } else {
        showToast('Reset failed.', 'err');
      }
    } catch (e) {
      showToast('Reset error: ' + e.message, 'err');
    }
  });

  $('btnEraseWifi')?.addEventListener('click', async () => {
    const yes = await confirm(
      'ERASE WIFI CREDENTIALS',
      'The device will forget its WiFi network and restart in provisioning mode. You will need to reconnect.'
    );
    if (!yes) return;
    try {
      await fetch('/api/erase', { method: 'POST' });
      showToast('WiFi erased — device rebooting…', 'warn', 10000);
    } catch (_) {
      showToast('Erase command sent.', 'warn', 6000);
    }
  });
}

/* ── Misc UI wiring ─────────────────────────────────────────────────────── */
function initMiscUI() {
  // Slider live feedback
  document.querySelectorAll('input[type="range"]').forEach(el => {
    el.addEventListener('input', () => updateSliderDisplay(el.id));
  });

  // mDNS hostname → preview
  $('mdns_hostname')?.addEventListener('input', updateMdnsPreview);

  // Password reveal toggles
  setupTogglePw('toggleApPw',  'ap_password');
  setupTogglePw('toggleOtaPw', 'ota_token');
}

function setupTogglePw(btnId, inputId) {
  $(btnId)?.addEventListener('click', () => {
    const inp = $(inputId);
    if (!inp) return;
    inp.type = inp.type === 'password' ? 'text' : 'password';
  });
}

/* ── Boot ───────────────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
  initTabs();
  initSaveButtons();
  initMiscUI();
  initMotorPreview();
  initOTA();
  initDangerZone();
  fetchSettings();
  wsConnect();
});

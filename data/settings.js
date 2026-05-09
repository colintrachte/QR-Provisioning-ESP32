'use strict';

/* ── Constants ───────────────────────────────────────────────────────────── */
const WS_URL = `ws://${location.host}/ws`;
const API    = '/api/settings';

// Fields that require a device reboot to take effect.
// mdns_hostname and mdns_enable are sent by collectTab('mdns'), not 'identity'.
const REBOOT_FIELDS = new Set([
  'ap_ssid', 'ap_password', 'ap_channel', 'mdns_enable', 'mdns_hostname',
]);

// Palette definition: [cssVar, label, defaultHex]
const PALETTE_DEFS = [
  ['--clr-bg',           'Background',   '#0f1117'],
  ['--clr-surface',      'Surface',      '#1c1f2b'],
  ['--clr-border',       'Border',       '#2e3347'],
  ['--clr-text',         'Text',         '#e4e6f0'],
  ['--clr-muted',        'Muted',        '#4a4f6a'],
  ['--clr-accent',       'Accent',       '#3b82f6'],
  ['--clr-accent-hover', 'Accent Hover', '#2563eb'],
  ['--clr-success',      'Success',      '#22c55e'],
  ['--clr-danger',       'Danger',       '#ef4444'],
  ['--clr-warning',      'Warning',      '#f59e0b'],
];

// Built-in hardware — extend here as the board changes
const BUILTIN_PERIPHERALS = [
  { id:'oled',    icon:'🖥',  name:'OLED Display',  desc:'SSD1306 128×64 I²C',
    pins:[{label:'SDA',gpio:8},{label:'SCL',gpio:9}],                                       enabled:true, builtin:true },
  { id:'led',     icon:'💡', name:'Status LED',     desc:'LEDC PWM output',
    pins:[{label:'PWM',gpio:35}],                                                            enabled:true, builtin:true },
  { id:'battery', icon:'🔋', name:'Battery ADC',    desc:'ADC1 CH0 voltage divider',
    pins:[{label:'ADC',gpio:1}],                                                             enabled:true, builtin:true },
  { id:'motors',  icon:'⚙', name:'Motor Driver',   desc:'PWM differential drive',
    pins:[{label:'L-A',gpio:16},{label:'L-B',gpio:17},{label:'R-A',gpio:18},{label:'R-B',gpio:19}],
    enabled:true, builtin:true },
];

// Add-on catalog — rendered once at init (#14), never re-rendered
const ADDON_CATALOG = [
  { id:'bme280', icon:'🌡', name:'BME280',      desc:'Temp / humidity / pressure (I²C)', category:'sensor' },
  { id:'vl53',   icon:'📏', name:'VL53L0X',     desc:'ToF distance sensor (I²C)',        category:'sensor' },
  { id:'neo',    icon:'🔴', name:'NeoPixel',    desc:'WS2812B LED strip (single GPIO)',  category:'output' },
  { id:'servo',  icon:'🔩', name:'Servo',       desc:'PWM servo, any free GPIO',         category:'output' },
  { id:'ina219', icon:'⚡', name:'INA219',      desc:'Current/power monitor (I²C)',      category:'sensor' },
  { id:'rc522',  icon:'📡', name:'RC522',       desc:'RFID reader (SPI)',                category:'comms'  },
  { id:'lora',   icon:'📻', name:'LoRa SX1276', desc:'Long-range radio (SPI)',           category:'comms'  },
  { id:'enc28',  icon:'🔌', name:'ENC28J60',    desc:'Ethernet (SPI)',                   category:'comms'  },
];

// GPIO occupancy: gpio# → owner name | null | '__special__' | '__flash__'
function buildGpioMap(periList) {
  const map = {};
  for (let i = 0; i <= 48; i++) map[i] = null;
  [0,3,45,46].forEach(p => { map[p] = '__special__'; });
  [26,27,28,29,30,31,32].forEach(p => { map[p] = '__flash__'; });
  periList.forEach(p => {
    if (!p.enabled) return;
    (p.pins || []).forEach(pin => { if (pin.gpio != null) map[pin.gpio] = p.name; });
  });
  return map;
}

// Read a live CSS variable as #rrggbb — used by renderPinout so SVG colors
// track palette changes in real time (#13)
function getThemeColor(varName) {
  const raw = getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
  return rgbToHex(raw) || raw;
}

/* ── State ───────────────────────────────────────────────────────────────── */
let CFG           = {};
let peripherals   = JSON.parse(JSON.stringify(BUILTIN_PERIPHERALS));
let pendingReboot = false;
let dirtyTabs     = new Set();
let ws            = null;
let wsRetry       = null;
let lastTelem     = 0;
let motorRafId    = null;   // rAF handle for motor preview (#1)

/* ── DOM helper ──────────────────────────────────────────────────────────── */
const $ = id => document.getElementById(id);

/* ── Toast ───────────────────────────────────────────────────────────────── */
function showToast(msg, type = 'ok', ms = 2800) {
  const t = $('toast');
  t.textContent = msg;
  t.className   = `toast show ${type}`;
  clearTimeout(t._timer);
  t._timer = setTimeout(() => { t.className = 'toast'; }, ms);
}

/* ── Connection indicator ────────────────────────────────────────────────── */
function setConn(live) {
  const ind = $('connIndicator');
  ind.classList.toggle('live', live);
  ind.querySelector('.conn-label').textContent = live ? 'LIVE' : 'OFFLINE';
}

/* ── Confirm dialog (#6 renamed — no longer shadows window.confirm) ─────── */
function showConfirm(title, msg, okLabel = 'CONFIRM', okClass = 'btn-danger') {
  return new Promise(resolve => {
    let ov = document.querySelector('.confirm-overlay');
    if (!ov) {
      ov = document.createElement('div');
      ov.className = 'confirm-overlay';
      ov.innerHTML = `
        <div class="confirm-box">
          <div class="confirm-title" id="cfmTitle"></div>
          <div class="confirm-msg"   id="cfmMsg"></div>
          <div class="confirm-btns">
            <button class="btn btn-ghost" id="cfmCancel">CANCEL</button>
            <button class="btn"           id="cfmOk"></button>
          </div>
        </div>`;
      document.body.appendChild(ov);
    }
    $('cfmTitle').textContent = title;
    $('cfmMsg').textContent   = msg;
    $('cfmOk').textContent    = okLabel;
    $('cfmOk').className      = `btn ${okClass}`;
    ov.classList.add('show');
    const done = ok => {
      ov.classList.remove('show');
      $('cfmOk').onclick = $('cfmCancel').onclick = ov.onclick = null;
      resolve(ok);
    };
    $('cfmOk').onclick     = () => done(true);
    $('cfmCancel').onclick = () => done(false);
    ov.onclick = e => { if (e.target === ov) done(false); };
  });
}

/* ── Settings API ────────────────────────────────────────────────────────── */
async function fetchSettings() {
  try {
    const r = await fetch(API);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    CFG = await r.json();
    if (CFG.palette) applyPalette(CFG.palette);
    renderAll();
  } catch (e) {
    showToast('Failed to load settings — ' + e.message, 'err', 5000);
    renderAll();
  }
}

async function saveFields(payload, tabKey) {
  const willReboot = Object.keys(payload).some(k => REBOOT_FIELDS.has(k));
  try {
    const r = await fetch(API, {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(payload),
    });
    const text = await r.text();
    let data = {};
    try { data = JSON.parse(text); } catch (_) { data = { error: text.slice(0, 200) }; }

    if (!r.ok) {
      showToast(`Error ${r.status}: ` + (data.error || 'Unknown'), 'err', 6000);
      return false;
    }

    Object.assign(CFG, payload);
    if ('ota_token' in payload) {
      CFG.ota_token_set = payload.ota_token !== '';
      /* Cache token presence across page refreshes. The actual token value
       * is never echoed by the server, so we store only whether it is set.
       * The token itself is kept in memory (CFG) only for the OTA header;
       * it is never written to sessionStorage for security. */
      try {
        sessionStorage.setItem('ota_token_set', CFG.ota_token_set ? '1' : '0');
        if (payload.ota_token !== '') {
          /* Store the actual value for the OTA upload header; cleared on tab close */
          sessionStorage.setItem('ota_token_value', payload.ota_token);
        } else {
          sessionStorage.removeItem('ota_token_value');
        }
      } catch (_) { /* sessionStorage unavailable (e.g. private browsing) — ignore */ }
    }

    // (#4) Only show banner when THIS save requires a reboot, not persistently
    if (data.reboot_required || willReboot) {
      pendingReboot = true;
      $('rebootBanner').classList.remove('hidden');
    }

    if (tabKey) { dirtyTabs.delete(tabKey); updateSaveBar(tabKey); }
    showToast('Saved.', 'ok');
    return true;
  } catch (e) {
    showToast('Network error: ' + e.message, 'err', 5000);
    return false;
  }
}

/* ── Dirty tracking + save-bar highlight (#10) ───────────────────────────── */
function markDirty(tabKey) {
  dirtyTabs.add(tabKey);
  updateSaveBar(tabKey);
}

function updateSaveBar(tabKey) {
  const btn = document.querySelector(`[data-save="${tabKey}"]`);
  const bar = btn?.closest('.field-actions');
  if (bar) bar.classList.toggle('dirty', dirtyTabs.has(tabKey));
}

function wireDirtyTracking(tabKey, inputIds) {
  inputIds.forEach(id => {
    const el = $(id);
    if (!el) return;
    el.addEventListener('input',  () => markDirty(tabKey));
    el.addEventListener('change', () => markDirty(tabKey));
  });
}

/* ── Render ──────────────────────────────────────────────────────────────── */
function renderAll() {
  updateDeviceTitle(CFG.device_name);

  // Identity — device name only
  setVal('deviceName_input', CFG.device_name ?? '');

  // Network > mDNS accordion
  setVal('mdns_hostname', CFG.mdns_hostname ?? '');
  setChk('mdns_enable',   CFG.mdns_enable   ?? false);
  updateMdnsPreview();

  // Network — AP only
  setVal('ap_ssid',     CFG.ap_ssid ?? '');
  setVal('ap_password', '');   // never pre-fill
  setSlider('ap_channel', CFG.ap_channel ?? 1);

  // Drive
  setSlider('drive_deadband',    CFG.drive_deadband    ?? 0.05);
  setSlider('drive_ramp_rate',   CFG.drive_ramp_rate   ?? 0.05);
  setSlider('drive_watchdog_ms', CFG.drive_watchdog_ms ?? 500);

  // Telemetry
  setSlider('telemetry_interval_ms',   CFG.telemetry_interval_ms   ?? 200);
  setSlider('display_sleep_timeout_s', CFG.display_sleep_timeout_s ?? 0);

  // Signal
  setSlider('rssi_warn_dbm', CFG.rssi_warn_dbm ?? -75);

  // Security
  // ota_token_set: prefer server value; fall back to sessionStorage for
  // cross-refresh persistence (server never echoes the real token).
  if (CFG.ota_token_set === undefined) {
    try { CFG.ota_token_set = sessionStorage.getItem('ota_token_set') === '1'; } catch (_) {}
  }
  // Re-hydrate the in-memory token for the OTA upload header if available.
  if (!CFG.ota_token) {
    try { CFG.ota_token = sessionStorage.getItem('ota_token_value') || ''; } catch (_) {}
  }
  const hint = $('tokenBadgeHint');
  if (hint) hint.textContent = CFG.ota_token_set ? 'Token configured' : 'Not configured';

  renderPalette();
  renderBuiltinList();
  renderPinout();   // (#7) called after data is loaded, not at boot
}

function setVal(id, v) { const e = $(id); if (e) e.value = v; }
function setChk(id, v) { const e = $(id); if (e) e.checked = !!v; }
function setSlider(id, v) { const e = $(id); if (!e) return; e.value = v; updateSliderDisplay(id); }

function updateDeviceTitle(name) {
  const e = $('deviceName');
  if (e) e.textContent = (name || 'ROBOT').toUpperCase();
}

function updateSliderDisplay(id) {
  const v   = parseFloat($(id)?.value ?? 0);
  const out = $(id + '_val');
  if (!out) return;
  switch (id) {
    case 'drive_deadband':
    case 'drive_ramp_rate':         out.textContent = v.toFixed(3); break;
    case 'drive_watchdog_ms':       out.textContent = v + ' ms'; break;
    case 'telemetry_interval_ms':   out.textContent = v + ' ms (' + (1000/v).toFixed(1) + ' Hz)'; break;
    case 'rssi_warn_dbm':           out.textContent = '−' + Math.abs(v) + ' dBm'; break;
    case 'display_sleep_timeout_s': out.textContent = v === 0 ? 'OFF' : v + ' s'; break;
    case 'ap_channel':              out.textContent = v === 0 ? 'AUTO' : String(v); break;
    default:                        out.textContent = v;
  }
}

function updateMdnsPreview() {
  const h = $('mdns_hostname')?.value?.trim() || 'robot';
  const p = $('mdnsPreview');
  if (p) p.textContent = h + '.local';
}

/* ── Sidebar nav ─────────────────────────────────────────────────────────── */
function initNav() {
  document.querySelectorAll('.nav-item').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.nav-item').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.settings-section').forEach(s => s.classList.remove('active'));
      btn.classList.add('active');
      const sec = $('section-' + btn.dataset.section);
      if (sec) sec.classList.add('active');
    });
  });
}

/* ── Accordion (#1 rAF gated on accordion open state) ───────────────────── */
function initAccordions() {
  document.querySelectorAll('.accordion-hdr').forEach(hdr => {
    hdr.addEventListener('click', () => {
      const acc    = hdr.closest('.accordion');
      const bodyId = hdr.getAttribute('aria-controls');
      const body   = document.getElementById(bodyId);
      const open   = acc.classList.toggle('open');
      hdr.setAttribute('aria-expanded', open);
      if (body) body.hidden = !open;

      // (#1) Gate motor rAF to accordion open state
      if (bodyId === 'acc-motor-preview-body') {
        open ? startMotorRaf() : stopMotorRaf();
      }
    });
  });

  document.querySelectorAll('.accordion.open').forEach(acc => {
    const hdr    = acc.querySelector('.accordion-hdr');
    const bodyId = hdr?.getAttribute('aria-controls');
    const body   = bodyId ? document.getElementById(bodyId) : null;
    if (hdr)  hdr.setAttribute('aria-expanded', 'true');
    if (body) body.hidden = false;
  });
}

/* ── Save buttons ────────────────────────────────────────────────────────── */
function initSaveButtons() {
  document.querySelectorAll('[data-save]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const tabKey = btn.dataset.save;
      const fields = collectTab(tabKey);
      if (!fields) return;
      const err = validateFields(fields);
      if (err) { showToast(err, 'err', 4000); return; }
      btn.classList.add('saving');
      btn.disabled = true;
      await saveFields(fields, tabKey);
      btn.disabled = false;
      btn.classList.remove('saving');
      if (tabKey === 'identity') updateDeviceTitle(fields.device_name);
    });
  });

  // Wire dirty tracking (#10)
  wireDirtyTracking('identity',  ['deviceName_input']);
  wireDirtyTracking('mdns',      ['mdns_hostname', 'mdns_enable']);
  wireDirtyTracking('network',   ['ap_ssid','ap_password','ap_channel','ap_password_clear']);
  wireDirtyTracking('drive',     ['drive_deadband','drive_ramp_rate','drive_watchdog_ms']);
  wireDirtyTracking('telemetry', ['telemetry_interval_ms','display_sleep_timeout_s']);
  wireDirtyTracking('signal',    ['rssi_warn_dbm']);
  wireDirtyTracking('security',  ['ota_token']);
}

function collectTab(tab) {
  switch (tab) {
    case 'identity':
      return {
        device_name: $('deviceName_input').value.trim(),
      };
    case 'mdns':
      return {
        mdns_hostname: $('mdns_hostname').value.trim(),
        mdns_enable:   $('mdns_enable').checked,
      };
    case 'network': {
      const clearPw = $('ap_password_clear')?.checked ?? false;
      const f = { ap_ssid: $('ap_ssid').value.trim(), ap_channel: parseInt($('ap_channel').value, 10) };
      if (clearPw) {
        /* Explicitly clear the AP password — sets it to open network */
        f.ap_password_clear = true;
      } else {
        const pw = $('ap_password').value;
        if (pw) f.ap_password = pw;
      }
      return f;
    }
    case 'signal':      return { rssi_warn_dbm: parseInt($('rssi_warn_dbm').value, 10) };
    case 'drive':       return {
      drive_deadband:    parseFloat($('drive_deadband').value),
      drive_ramp_rate:   parseFloat($('drive_ramp_rate').value),
      drive_watchdog_ms: parseInt($('drive_watchdog_ms').value, 10),
    };
    case 'telemetry':   return {
      telemetry_interval_ms:   parseInt($('telemetry_interval_ms').value, 10),
      display_sleep_timeout_s: parseInt($('display_sleep_timeout_s').value, 10),
    };
    case 'security':    return { ota_token: $('ota_token').value };
    case 'appearance':  return { palette: collectPalette() };
    default:            return null;
  }
}

function validateFields(f) {
  if ('device_name'        in f && !f.device_name)  return 'Device name cannot be empty.';
  if ('ap_ssid'            in f && !f.ap_ssid)       return 'AP SSID cannot be empty.';
  if ('ap_password'        in f && f.ap_password.length > 0 && f.ap_password.length < 8)
    return 'AP password must be blank (open) or ≥ 8 characters.';
  if ('ap_channel'         in f && (f.ap_channel < 0 || f.ap_channel > 13))
    return 'AP channel must be 0 (auto) or 1–13.';
  if ('drive_deadband'     in f && (f.drive_deadband < 0 || f.drive_deadband > 0.5))
    return 'Deadband must be 0.0–0.5.';
  if ('drive_ramp_rate'    in f && (f.drive_ramp_rate <= 0 || f.drive_ramp_rate > 1))
    return 'Ramp rate must be in (0.0, 1.0].';
  if ('drive_watchdog_ms'  in f && (f.drive_watchdog_ms < 100 || f.drive_watchdog_ms > 10000))
    return 'Watchdog must be 100–10000 ms.';
  if ('telemetry_interval_ms' in f && (f.telemetry_interval_ms < 50 || f.telemetry_interval_ms > 10000))
    return 'Telemetry interval must be 50–10000 ms.';
  if ('display_sleep_timeout_s' in f && (f.display_sleep_timeout_s < 0 || f.display_sleep_timeout_s > 3600))
    return 'Display sleep timeout must be 0–3600 s.';
  if ('rssi_warn_dbm'      in f && (f.rssi_warn_dbm < -120 || f.rssi_warn_dbm > 0))
    return 'RSSI threshold must be −120 to 0 dBm.';
  return null;
}

/* ── Palette (#3 dead code gone, #12 iOS change event, #13 SVG tracks live) */
function collectPalette() {
  const p = {};
  PALETTE_DEFS.forEach(([v]) => {
    const inp = document.querySelector(`.palette-picker[data-var="${v}"]`);
    if (inp) p[v] = inp.value;
  });
  return p;
}

function applyPalette(palette) {
  if (!palette) return;
  Object.entries(palette).forEach(([v, hex]) => {
    document.documentElement.style.setProperty(v, hex);
  });
}

function rgbToHex(rgb) {
  if (!rgb) return null;
  if (rgb.startsWith('#') && rgb.length === 7) return rgb;
  const m = rgb.match(/\d+/g);
  if (!m || m.length < 3) return null;
  return '#' + m.slice(0, 3).map(n => parseInt(n).toString(16).padStart(2, '0')).join('');
}

function renderPalette() {
  const grid = $('paletteGrid');
  if (!grid) return;
  grid.innerHTML = '';

  const resolved = {};
  PALETTE_DEFS.forEach(([v,, def]) => { resolved[v] = def; });
  if (CFG.palette) Object.assign(resolved, CFG.palette);
  const liveStyle = getComputedStyle(document.documentElement);
  PALETTE_DEFS.forEach(([v]) => {
    const live = rgbToHex(liveStyle.getPropertyValue(v).trim());
    if (live) resolved[v] = live;
  });

  PALETTE_DEFS.forEach(([cssVar, label]) => {
    const hex   = resolved[cssVar] || '#888888';
    const entry = document.createElement('div');
    entry.className = 'palette-entry';
    entry.innerHTML = `
      <div class="palette-label">${label}</div>
      <div class="palette-swatch-wrap">
        <div class="palette-swatch" style="background:${hex}"></div>
        <input type="color" class="palette-picker" data-var="${cssVar}" value="${hex}" title="${label}">
      </div>
      <div class="palette-hex">${hex.toUpperCase()}</div>`;
    grid.appendChild(entry);

    const picker = entry.querySelector('.palette-picker');
    const swatch = entry.querySelector('.palette-swatch');
    const hexEl  = entry.querySelector('.palette-hex');

    const onColor = () => {
      const v = picker.value;
      swatch.style.background = v;
      hexEl.textContent = v.toUpperCase();
      document.documentElement.style.setProperty(cssVar, v);
      renderPinout();      // (#13) pinout redraws with live theme colors
      markDirty('appearance');
    };
    picker.addEventListener('input',  onColor);  // desktop — fires live
    picker.addEventListener('change', onColor);  // (#12) iOS — fires on picker dismiss
    swatch.addEventListener('click', () => picker.click());
  });
}

/* ── Peripherals ─────────────────────────────────────────────────────────── */
function renderBuiltinList() {
  const list = $('builtinList');
  if (!list) return;
  list.innerHTML = '';

  peripherals.forEach(p => {
    const card = document.createElement('div');
    card.className = 'periph-card' + (p.enabled ? ' enabled' : '');

    const pins = (p.pins || []).map(pin =>
      `<span class="pin-badge">${pin.gpio}<span class="pin-label-dim"> ${pin.label}</span></span>`
    ).join('');

    card.innerHTML = `
      <div class="periph-icon">${p.icon}</div>
      <div class="periph-info">
        <div class="periph-name">${p.name}</div>
        <div class="periph-desc">${p.desc}</div>
        ${pins ? `<div class="periph-pins">${pins}</div>` : ''}
      </div>
      <div class="periph-toggle-wrap">
        ${p.builtin ? '' : `<button class="btn-icon periph-remove" data-remove="${p.id}" title="Remove" aria-label="Remove ${p.name}">✕</button>`}
      </div>`;

    if (!p.builtin) {
      card.style.cursor = 'pointer';
      card.addEventListener('click', e => {
        if (e.target.closest('[data-remove]')) return;
        p.enabled = !p.enabled;
        renderBuiltinList();
        renderPinout();
      });
    }

    card.querySelector('[data-remove]')?.addEventListener('click', e => {
      e.stopPropagation();
      peripherals = peripherals.filter(x => x.id !== p.id);
      renderBuiltinList();
      renderPinout();
    });

    list.appendChild(card);
  });
}

// (#14) Catalog rendered once — not on every renderAll()
function initCatalog() {
  const cat = $('periphCatalog');
  if (!cat) return;
  ADDON_CATALOG.forEach(item => {
    const btn = document.createElement('button');
    btn.className = 'catalog-item';
    btn.innerHTML = `
      <div class="catalog-icon">${item.icon}</div>
      <div class="catalog-name">${item.name}</div>
      <div class="catalog-desc">${item.desc}</div>`;
    btn.addEventListener('click', () => {
      if (peripherals.find(p => p.id === item.id)) {
        showToast(`${item.name} already added.`, 'warn'); return;
      }
      peripherals.push({ id:item.id, icon:item.icon, name:item.name, desc:item.desc,
        pins:[], enabled:true, builtin:false });
      renderBuiltinList();
      renderPinout();
      showToast(`${item.name} added.`, 'ok');
    });
    cat.appendChild(btn);
  });
}

/* ── Pinout SVG (#7 called from renderAll, #13 live CSS vars) ────────────── */
function renderPinout() {
  const wrap = $('pinoutWrap');
  if (!wrap) return;

  const gpio = buildGpioMap(peripherals);

  // All colors read live so they track palette edits (#13)
  const C_ACCENT  = getThemeColor('--clr-accent');
  const C_SUCCESS = getThemeColor('--clr-success');
  const C_DANGER  = getThemeColor('--clr-danger');
  const C_WARNING = getThemeColor('--clr-warning');
  const C_TEXT    = getThemeColor('--clr-text');
  const C_MUTED   = getThemeColor('--clr-muted');
  const C_SURFACE = getThemeColor('--clr-surface');
  const C_BORDER  = getThemeColor('--clr-border');

  // ESP32-S3 WROOM DevKitC-1 38-pin layout (top → bottom)
  const LEFT  = [3,46,9,10,11,12,13,14,21,47,48,45,0,35,36,37,38,39];
  const RIGHT = ['5V','GND','3V3',43,44,1,2,42,41,40,4,5,6,7,8,'3V3','5V','GND'];

  const PIN_H = 22, PAD = 10, BODY_W = 64, COL_W = 210;
  const ROWS  = 18;
  const W = COL_W * 2 + BODY_W;
  const H = ROWS * PIN_H + PAD * 2 + 8;

  function pinColor(p) {
    if (p === '5V' || p === '3V3' || p === 'GND') return C_DANGER;
    if (gpio[p] === '__special__') return C_WARNING;
    if (gpio[p] === '__flash__')   return C_MUTED;
    if (gpio[p])                   return C_ACCENT;
    return C_SUCCESS;
  }

  function pinLabel(p) {
    if (typeof p === 'string') return p;
    const owner = gpio[p];
    if (!owner || owner === '__special__' || owner === '__flash__') return `GPIO ${p}`;
    return `GPIO ${p}  ·  ${owner}`;
  }

  let svg = `<svg viewBox="0 0 ${W} ${H}" xmlns="http://www.w3.org/2000/svg"
    style="font-family:system-ui,sans-serif;font-size:10px">`;

  svg += `<rect x="${COL_W}" y="${PAD}" width="${BODY_W}" height="${ROWS*PIN_H}"
    rx="6" fill="${C_SURFACE}" stroke="${C_BORDER}" stroke-width="1.5"/>`;
  svg += `<text x="${COL_W+BODY_W/2}" y="${PAD+(ROWS*PIN_H)/2}"
    text-anchor="middle" dominant-baseline="middle"
    fill="${C_MUTED}" font-size="8.5" letter-spacing="1.5"
    transform="rotate(-90 ${COL_W+BODY_W/2} ${PAD+(ROWS*PIN_H)/2})">ESP32-S3</text>`;

  // Connector wire stubs
  LEFT.forEach((_, i) => {
    const y = PAD + i*PIN_H + PIN_H/2;
    svg += `<line x1="${COL_W-2}" y1="${y}" x2="${COL_W-10}" y2="${y}" stroke="${C_BORDER}" stroke-width="1"/>`;
  });
  RIGHT.forEach((_, i) => {
    const y = PAD + i*PIN_H + PIN_H/2;
    svg += `<line x1="${COL_W+BODY_W+2}" y1="${y}" x2="${COL_W+BODY_W+10}" y2="${y}" stroke="${C_BORDER}" stroke-width="1"/>`;
  });

  // Pin dots + labels
  LEFT.forEach((p, i) => {
    const y   = PAD + i*PIN_H + PIN_H/2;
    const clr = pinColor(p);
    svg += `<circle cx="${COL_W-10}" cy="${y}" r="4" fill="${clr}" opacity="0.9"/>`;
    svg += `<text x="${COL_W-18}" y="${y}" text-anchor="end" dominant-baseline="middle"
      fill="${C_TEXT}" font-size="9.5">${pinLabel(p)}</text>`;
  });

  RIGHT.forEach((p, i) => {
    const y   = PAD + i*PIN_H + PIN_H/2;
    const clr = pinColor(p);
    svg += `<circle cx="${COL_W+BODY_W+10}" cy="${y}" r="4" fill="${clr}" opacity="0.9"/>`;
    svg += `<text x="${COL_W+BODY_W+18}" y="${y}" text-anchor="start" dominant-baseline="middle"
      fill="${C_TEXT}" font-size="9.5">${pinLabel(p)}</text>`;
  });

  svg += '</svg>';
  wrap.innerHTML = svg;
}

/* ── WebSocket + telemetry ───────────────────────────────────────────────── */
function wsConnect() {
  if (ws && ws.readyState <= WebSocket.OPEN) return;
  clearTimeout(wsRetry);
  ws = new WebSocket(WS_URL);
  ws.binaryType = 'arraybuffer';
  ws.onopen    = () => { setConn(true); ws.send('ping'); };
  ws.onmessage = e => {
    if (typeof e.data !== 'string') return;
    try { updateTelemetry(JSON.parse(e.data)); } catch (_) {}
  };
  ws.onerror = () => {};
  ws.onclose = () => { setConn(false); markTelemStale(); wsRetry = setTimeout(wsConnect, 3000); };
}

function updateTelemetry(d) {
  lastTelem = Date.now();
  const set = (id, v) => { const e=$(id); if(!e) return; e.textContent=v; e.classList.remove('stale'); };
  set('t_rssi',   d.rssi    != null ? d.rssi + ' dBm'                : '—');
  set('t_bat',    d.battery != null ? d.battery + '%'                 : '—');
  set('t_temp',   d.temp    != null ? d.temp.toFixed(1) + ' °C'      : 'N/A');
  set('t_heap',   d.heap    != null ? (d.heap/1024).toFixed(0)+' KB' : '—');
  set('t_uptime', d.uptime  != null ? fmtUptime(d.uptime)            : '—');
  set('t_errors', d.errors  != null ? String(d.errors)               : '—');
}

function markTelemStale() {
  ['t_rssi','t_bat','t_temp','t_heap','t_uptime','t_errors'].forEach(id => $(id)?.classList.add('stale'));
}

function fmtUptime(s) {
  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sec=s%60;
  if (h>0) return `${h}h ${m}m`;
  if (m>0) return `${m}m ${sec}s`;
  return `${sec}s`;
}

setInterval(() => { if (ws?.readyState===WebSocket.OPEN) ws.send('ping'); }, 4000);
setInterval(() => { if (lastTelem && Date.now()-lastTelem > 3000) markTelemStale(); }, 1000);

/* ── Motor preview (#1 rAF lifecycle, #9 DPI-aware, correct size) ────────── */
let _motorState = { jx:0, jy:0, dragging:false, rampL:0, rampR:0 };

function initMotorPreview() {
  const canvas = $('motorJoystick');
  if (!canvas) return;
  const ctx    = canvas.getContext('2d');
  const s      = _motorState;

  function cssSize() {
    const wrap = canvas.parentElement;
    return Math.min(Math.max(wrap ? wrap.clientWidth - 32 : 140, 120), 160);
  }

  function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const sz  = cssSize();
    canvas.style.width  = sz + 'px';
    canvas.style.height = sz + 'px';
    canvas.width  = Math.round(sz * dpr);
    canvas.height = Math.round(sz * dpr);
  }

  function drawFrame() {
    const dpr = window.devicePixelRatio || 1;
    const W   = canvas.width / dpr;
    const H   = canvas.height / dpr;
    const cx  = W/2, cy = H/2, R = W/2 - 6;
    ctx.save();
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = getThemeColor('--clr-border'); ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.arc(cx, cy, R, 0, Math.PI*2); ctx.stroke();
    ctx.strokeStyle = getThemeColor('--clr-surface'); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(cx, cy-R); ctx.lineTo(cx, cy+R); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx-R, cy); ctx.lineTo(cx+R, cy); ctx.stroke();
    ctx.fillStyle = getThemeColor('--clr-success');
    ctx.beginPath(); ctx.arc(cx + s.jx*R, cy - s.jy*R, 8, 0, Math.PI*2); ctx.fill();
    ctx.restore();
  }

  function simStep() {
    const db = parseFloat($('drive_deadband')?.value ?? 0.05);
    const rr = parseFloat($('drive_ramp_rate')?.value ?? 0.05);
    const ax = Math.abs(s.jx)<db ? 0 : s.jx;
    const ay = Math.abs(s.jy)<db ? 0 : s.jy;
    const wL = Math.max(-1, Math.min(1, ay+ax));
    const wR = Math.max(-1, Math.min(1, ay-ax));
    const step = (c,t) => c + Math.max(-rr, Math.min(rr, t-c));
    s.rampL = step(s.rampL, wL);
    s.rampR = step(s.rampR, wR);
    const bar = (id, v) => {
      const b=$(id); if(!b) return;
      b.style.width = Math.abs(v)*50+'%';
      b.classList.toggle('active', Math.abs(v)>0.01);
    };
    bar('motorPreviewL', s.rampL);
    bar('motorPreviewR', s.rampR);
  }

  function getPos(e) {
    const dpr = window.devicePixelRatio || 1;
    const W   = canvas.width / dpr;
    const H   = canvas.height / dpr;
    const cx  = W/2, cy = H/2, R = W/2 - 6;
    const rect = canvas.getBoundingClientRect();
    const src  = e.touches?.[0] ?? e;
    return { x:(src.clientX-rect.left-cx)/R, y:-(src.clientY-rect.top-cy)/R };
  }

  function clamp(p) {
    const d = Math.sqrt(p.x*p.x + p.y*p.y);
    return d > 1 ? {x:p.x/d, y:p.y/d} : p;
  }

  canvas.addEventListener('mousedown',  e => { s.dragging=true;  const p=clamp(getPos(e)); s.jx=p.x; s.jy=p.y; });
  window.addEventListener('mousemove',  e => { if(!s.dragging) return; const p=clamp(getPos(e)); s.jx=p.x; s.jy=p.y; });
  window.addEventListener('mouseup',    ()  => { if(!s.dragging) return; s.dragging=false; s.jx=0; s.jy=0; });
  canvas.addEventListener('touchstart', e => { e.preventDefault(); s.dragging=true;  const p=clamp(getPos(e)); s.jx=p.x; s.jy=p.y; }, {passive:false});
  canvas.addEventListener('touchmove',  e => { e.preventDefault(); const p=clamp(getPos(e)); s.jx=p.x; s.jy=p.y; }, {passive:false});
  canvas.addEventListener('touchend',   ()  => { s.dragging=false; s.jx=0; s.jy=0; });

  // Store draw/sim refs so startMotorRaf can use them
  canvas._draw    = drawFrame;
  canvas._sim     = simStep;
  canvas._resize  = resizeCanvas;

  resizeCanvas();
  drawFrame();   // first frame, no rAF yet — accordion is closed by default
}

function startMotorRaf() {
  if (motorRafId) return;
  const canvas = $('motorJoystick');
  if (!canvas || !canvas._draw) return;
  canvas._resize();
  function loop() {
    canvas._sim();
    canvas._draw();
    motorRafId = requestAnimationFrame(loop);
  }
  motorRafId = requestAnimationFrame(loop);
}

function stopMotorRaf() {
  if (motorRafId) { cancelAnimationFrame(motorRafId); motorRafId = null; }
}

/* ── OTA (#5 use CFG.ota_token for header) ───────────────────────────────── */
function initOTA() {
  setupOtaZone({
    zone:$('otaFwZone'), input:$('otaFwFile'), pick:$('otaFwPick'),
    nameEl:$('otaFwName'), progressEl:$('otaFwProgress'),
    fillEl:$('otaFwFill'), labelEl:$('otaFwLabel'), btn:$('otaFwBtn'),
    endpoint:'/ota/firmware',
    confirmMsg:'This will flash new firmware and reboot the device.',
    onDone: () => showToast('Firmware flashed — rebooting…', 'warn', 10000),
  });
  setupOtaZone({
    zone:$('otaFsZone'), input:$('otaFsFile'), pick:$('otaFsPick'),
    nameEl:$('otaFsName'), progressEl:$('otaFsProgress'),
    fillEl:$('otaFsFill'), labelEl:$('otaFsLabel'), btn:$('otaFsBtn'),
    endpoint:'/ota/filesystem',
    confirmMsg:'This will overwrite the LittleFS partition. It remounts live — no reboot needed.',
    onDone: () => showToast('Filesystem updated — no reboot needed.', 'ok'),
  });
}

function setupOtaZone({ zone, input, pick, nameEl, progressEl, fillEl, labelEl, btn, endpoint, confirmMsg, onDone }) {
  if (!zone) return;
  let file = null;
  const setFile = f => {
    file = f;
    nameEl.textContent = f ? `${f.name}  (${(f.size/1024).toFixed(0)} KB)` : '';
    btn.classList.toggle('hidden', !f);
  };
  pick.addEventListener('click', e => { e.stopPropagation(); input.click(); });
  input.addEventListener('change', () => setFile(input.files[0] ?? null));
  zone.addEventListener('dragover',  e => { e.preventDefault(); zone.classList.add('drag-over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('drag-over'));
  zone.addEventListener('drop', e => { e.preventDefault(); zone.classList.remove('drag-over'); setFile(e.dataTransfer.files[0] ?? null); });
  zone.addEventListener('click', e => { if(e.target===pick||e.target===btn||e.target===input) return; input.click(); });
  zone.addEventListener('keydown', e => { if(e.key==='Enter'||e.key===' ') input.click(); });

  btn.addEventListener('click', async e => {
    e.stopPropagation();
    if (!file) return;
    const yes = await showConfirm(
      'CONFIRM FLASH',
      `${confirmMsg}\n\nFile: ${file.name}  (${(file.size/1024).toFixed(0)} KB)`,
      'FLASH', 'btn-warn'
    );
    if (!yes) return;

    btn.disabled = true;
    progressEl.classList.remove('hidden');

    const xhr = new XMLHttpRequest();
    xhr.open('POST', endpoint);

    // (#5) Use the SAVED token (CFG.ota_token), falling back to sessionStorage
    // for the page-refresh case where CFG was repopulated from the server
    // (which never echoes the real token value).
    let savedToken = (CFG.ota_token || '').trim();
    if (!savedToken) {
      try { savedToken = sessionStorage.getItem('ota_token_value') || ''; } catch (_) {}
    }
    if (savedToken) xhr.setRequestHeader('X-OTA-Token', savedToken);

    xhr.upload.onprogress = ev => {
      if (!ev.lengthComputable) return;
      const pct = Math.round(ev.loaded/ev.total*100);
      fillEl.style.width  = pct + '%';
      labelEl.textContent = pct + '%';
    };
    xhr.onload = () => {
      btn.disabled = false;
      progressEl.classList.add('hidden');
      fillEl.style.width = '0%';
      if (xhr.status === 200) { onDone(); setFile(null); }
      else showToast(`OTA failed (${xhr.status}): ${xhr.responseText.slice(0,80)}`, 'err', 8000);
    };
    xhr.onerror = () => {
      btn.disabled = false;
      progressEl.classList.add('hidden');
      // Firmware OTA reboots the device — network drop = success
      if (endpoint === '/ota/firmware') { onDone(); setFile(null); }
      else showToast('OTA upload error — check connection.', 'err', 5000);
    };
    xhr.send(file);
  });
}

/* ── Danger zone (#4 pendingReboot clears on dismiss, #15/#16 endpoints) ── */
function initDangerZone() {
  $('rebootDismiss')?.addEventListener('click', () => {
    // (#4) Clear flag — future saves won't resurrect the banner unless they
    // themselves require a reboot
    pendingReboot = false;
    $('rebootBanner').classList.add('hidden');
  });

  $('rebootBtn')?.addEventListener('click', doReboot);
  $('btnReboot')?.addEventListener('click', async () => {
    if (await showConfirm('REBOOT DEVICE', 'The device will restart. All connected clients will be dropped.', 'REBOOT', 'btn-warn'))
      doReboot();
  });

  async function doReboot() {
    try {
      const r = await fetch('/api/reboot', { method:'POST' });
      if (r.status === 404) {
        showToast('⚠ /api/reboot not found — see comment in settings.js', 'warn', 8000);
        return;
      }
      showToast('Rebooting…', 'warn', 10000);
    } catch (_) {
      showToast('Device rebooting…', 'warn', 10000);
    }
  }

  $('btnFactoryReset')?.addEventListener('click', async () => {
    if (!await showConfirm(
      'FACTORY RESET SETTINGS',
      'All tunables reset to firmware defaults. WiFi credentials are unaffected. Device will NOT reboot.'
    )) return;
    try {
      const r = await fetch('/api/settings/reset', { method:'POST' });
      if (r.ok) { showToast('Settings reset to defaults.', 'warn'); await fetchSettings(); }
      else showToast('Reset failed — check device log.', 'err');
    } catch (e) { showToast('Error: ' + e.message, 'err'); }
  });

  $('btnEraseWifi')?.addEventListener('click', async () => {
    if (!await showConfirm(
      'ERASE WIFI CREDENTIALS',
      'The device will forget its WiFi network and restart in provisioning mode. You will need to reconnect.'
    )) return;
    try {
      const r = await fetch('/api/erase', { method:'POST' });
      if (r.status === 404) {
        showToast('⚠ /api/erase not found — see comment in settings.js', 'warn', 8000);
        return;
      }
      showToast('WiFi erased — device rebooting…', 'warn', 10000);
    } catch (_) {
      showToast('Erase command sent.', 'warn', 6000);
    }
  });
}

/* ── Misc wiring ─────────────────────────────────────────────────────────── */
function initMiscUI() {
  document.querySelectorAll('input[type="range"]').forEach(el => {
    el.addEventListener('input', () => updateSliderDisplay(el.id));
  });
  $('mdns_hostname')?.addEventListener('input', updateMdnsPreview);
  setupPwToggle('toggleApPw',  'ap_password');
  setupPwToggle('toggleOtaPw', 'ota_token');

  $('btnResetPalette')?.addEventListener('click', () => {
    const defaults = {};
    PALETTE_DEFS.forEach(([v,,def]) => { defaults[v] = def; });
    applyPalette(defaults);
    renderPalette();
    renderPinout();
    showToast('Palette reset to defaults.', 'ok');
  });
}

function setupPwToggle(btnId, inputId) {
  $(btnId)?.addEventListener('click', () => {
    const inp = $(inputId);
    if (!inp) return;
    inp.type = inp.type === 'password' ? 'text' : 'password';
  });
}

/* ── Boot ────────────────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
  initNav();
  initAccordions();
  initSaveButtons();
  initMiscUI();
  initMotorPreview();  // sets up input listeners; rAF not started (#1)
  initCatalog();       // one-time render (#14)
  initOTA();
  initDangerZone();
  fetchSettings();     // renderAll() → renderPinout() fires after data (#7)
  wsConnect();
});

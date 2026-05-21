'use strict';

// API endpoints
const API_SCAN    = '/api/scan';
const API_RESCAN  = '/api/scan?refresh=1';
const API_CONNECT = '/api/connect';
const API_SETTINGS = '/api/settings';

const SLIDE_COUNT = 5;

// ── State ──────────────────────────────────────────────────────────────────
let g_slide     = 0;
let g_networks  = [];
let g_selected  = null;
let g_accent    = '#3b82f6';

// ── DOM refs ───────────────────────────────────────────────────────────────
const elTrack     = document.getElementById('slide-track');
const elDots      = document.getElementById('wizard-dots');
const elSpinner   = document.getElementById('spinner');
const elList      = document.getElementById('network-list');
const elRescan    = document.getElementById('btn-rescan');
const elConnect   = document.getElementById('btn-connect');
const elSsid      = document.getElementById('ssid');
const elPass      = document.getElementById('password');
const elWifiStatus = document.getElementById('wifi-status');
const elToggle    = document.querySelector('.toggle-pass');
const elDevName   = document.getElementById('device-name-input');
const elPaletteRow = document.getElementById('palette-row');
const elSavePersona = document.getElementById('btn-save-persona');
const elPersonaStatus = document.getElementById('persona-status');
const elPeriphList = document.getElementById('periph-list');
const elMaxSpeed  = document.getElementById('max-speed');
const elMaxSpeedVal = document.getElementById('max-speed-val');
const elExpo      = document.getElementById('expo-val');
const elExpoDisp  = document.getElementById('expo-disp');
const elDeadzone  = document.getElementById('deadzone-val');
const elDzDisp    = document.getElementById('dz-disp');
const elSaveDrive = document.getElementById('btn-save-drive');
const elDriveStatus = document.getElementById('drive-status');

// Test joystick DOM refs
const elTestZone  = document.getElementById('test-joy-zone');
const elTestKnob  = document.getElementById('test-joy-knob');
const elTestBarL  = document.getElementById('test-bar-left');
const elTestBarR  = document.getElementById('test-bar-right');
const elTestCardL = document.getElementById('test-card-left');
const elTestCardR = document.getElementById('test-card-right');

// ── Slide engine ───────────────────────────────────────────────────────────
function buildDots() {
    for (let i = 0; i < SLIDE_COUNT; i++) {
        const btn = document.createElement('button');
        btn.className = 'wizard-dot' + (i === 0 ? ' active' : '');
        btn.setAttribute('role', 'tab');
        btn.setAttribute('aria-label', `Go to step ${i + 1}`);
        btn.addEventListener('click', () => goTo(i));
        elDots.appendChild(btn);
    }
}

function updateDots() {
    const dots = elDots.querySelectorAll('.wizard-dot');
    dots.forEach((d, i) => {
        d.classList.toggle('active', i === g_slide);
        d.classList.toggle('done',   i < g_slide);
    });
}

function goTo(idx) {
    idx = Math.max(0, Math.min(SLIDE_COUNT - 1, idx));
    g_slide = idx;
    updateDots();

    const slides = elTrack.querySelectorAll('.slide');
    slides.forEach((sl, i) => {
        const offset = (i - idx) * 100;
        sl.style.transform = `translateX(${offset}%)`;
    });
}

// ── Swipe support ──────────────────────────────────────────────────────────
(function initSwipe() {
    let startX = 0, startY = 0, dragging = false;

    elTrack.addEventListener('pointerdown', e => {
        startX = e.clientX;
        startY = e.clientY;
        dragging = true;
    }, { passive: true });

    elTrack.addEventListener('pointerup', e => {
        if (!dragging) return;
        dragging = false;
        const dx = e.clientX - startX;
        const dy = Math.abs(e.clientY - startY);
        if (Math.abs(dx) > 45 && dy < 60) {
            if (dx < 0) goTo(g_slide + 1);
            else        goTo(g_slide - 1);
        }
    }, { passive: true });

    elTrack.addEventListener('pointercancel', () => { dragging = false; }, { passive: true });
})();

// ── Nav button delegation ──────────────────────────────────────────────────
document.addEventListener('click', e => {
    const next = e.target.closest('[data-next]');
    const prev = e.target.closest('[data-prev]');
    const skip = e.target.closest('[data-skip]');
    if (next) goTo(parseInt(next.dataset.next, 10) + 1);
    if (prev) goTo(parseInt(prev.dataset.prev, 10) - 1);
    if (skip) goTo(parseInt(skip.dataset.skip, 10) + 1);
});

// ── WiFi — load cached scan ────────────────────────────────────────────────
let g_scanAbort = null;  // tracks the active scan AbortController

async function loadNetworks() {
    // Cancel any in-flight scan request before starting a new one.
    if (g_scanAbort) g_scanAbort.abort();
    g_scanAbort = new AbortController();
    const signal = g_scanAbort.signal;

    try {
        const res  = await fetch(API_SCAN, { signal });
        const data = await res.json();

        if (data.status === 'scanning') {
            setWifiStatus('Scanning\u2026 please wait');
            setTimeout(loadNetworks, 700);
            return;
        }

        g_networks = data;
        renderNetList();
        setWifiStatus(data.length ? `${data.length} network(s) found` : 'No networks found');
    } catch (err) {
        if (err.name === 'AbortError') return;
        setWifiStatus('Could not load networks');
        console.error(err);
    }
}

async function onRescan() {
    elRescan.disabled = true;
    showSpinner(true);
    setWifiStatus('Scanning\u2026 stay on this page');
    try {
        await fetch(API_RESCAN);
        await loadNetworks();
    } catch (err) {
        if (err.name !== 'AbortError') {
            setWifiStatus('Rescan failed');
            console.error(err);
        }
    } finally {
        showSpinner(false);
        elRescan.disabled = false;
    }
}

function renderNetList() {
    elList.innerHTML = '';
    if (!g_networks.length) {
        elList.innerHTML = '<p class="scan-status">No networks found — try rescanning</p>';
        return;
    }
    g_networks.forEach((ap, idx) => {
        const btn       = document.createElement('button');
        btn.type        = 'button';
        btn.className   = 'network-btn' + (g_selected === idx ? ' selected' : '');
        const lock      = ap.secure ? '\uD83D\uDD12\u202F' : '\uD83D\uDD13\u202F';
        const bars      = qualityBars(ap.quality);
        btn.innerHTML   =
            `<span class="network-name">${escapeHtml(ap.ssid)}</span>` +
            `<span class="signal">${lock}${bars}</span>`;
        btn.addEventListener('click', () => selectNet(idx));
        elList.appendChild(btn);
    });
}

function selectNet(idx) {
    g_selected    = idx;
    elSsid.value  = g_networks[idx].ssid;
    elPass.value  = '';
    elPass.focus();
    renderNetList();
}

async function onConnect() {
    const ssid = elSsid.value.trim();
    const pass = elPass.value;
    if (!ssid) { setWifiStatus('Please select or enter an SSID'); return; }

    elConnect.disabled = true;
    setWifiStatus('Saving\u2026');

    const body = `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`;
    try {
        const res  = await fetch(API_CONNECT, {
            method:  'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body,
        });
        // Device reboots immediately after ACK; response may be truncated.
        // Any 2xx is a success; non-2xx means something went wrong before reboot.
        if (res.ok) {
            setWifiStatus('Saved \u2014 the device is rebooting. Reconnect to your home network, then visit the device\u2019s new IP.');
            elWifiStatus.classList.add('ok');
        } else {
            setWifiStatus(`Server returned ${res.status} \u2014 check SSID and password`);
            elWifiStatus.classList.add('error');
        }
    } catch (err) {
        // Network drop here means the device has already rebooted and torn down the AP interface.
        setWifiStatus('Device is rebooting \u2014 reconnect to your home network, then visit the device\u2019s IP.');
        elWifiStatus.classList.add('ok');
        console.info('Fetch aborted by device reboot (expected):', err);
    } finally {
        elConnect.disabled = false;
    }
}

// ── Personalization ────────────────────────────────────────────────────────
function initPalette() {
    elPaletteRow.querySelectorAll('.swatch[data-color]').forEach(sw => {
        sw.addEventListener('click', () => selectSwatch(sw, sw.dataset.color));
    });

    const pickerInput = document.getElementById('color-picker');
    const customSwatch = document.getElementById('swatch-custom');

    if (pickerInput) {
        pickerInput.addEventListener('input', () => {
            const c = pickerInput.value;
            customSwatch.style.setProperty('--sw', c);
            selectSwatch(customSwatch, c);
        });
    }
}

function selectSwatch(el, color) {
    elPaletteRow.querySelectorAll('.swatch').forEach(s => s.classList.remove('selected'));
    el.classList.add('selected');
    g_accent = color;
    applyAccent(color);
}

function applyAccent(hex) {
    document.documentElement.style.setProperty('--clr-accent', hex);
    document.documentElement.style.setProperty('--clr-accent-hover', shadeHex(hex, -20));
}

function shadeHex(hex, amt) {
    const n = parseInt(hex.replace('#', ''), 16);
    const r = Math.max(0, Math.min(255, (n >> 16) + amt));
    const g = Math.max(0, Math.min(255, ((n >> 8) & 0xff) + amt));
    const b = Math.max(0, Math.min(255, (n & 0xff) + amt));
    return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}

async function savePersona() {
    const name  = elDevName.value.trim();
    elSavePersona.disabled = true;
    setStatus(elPersonaStatus, 'Saving\u2026', '');

    const payload = {};
    if (name) payload.device_name = name;
    // accent_color lives inside the palette object, which the firmware
    // stores as a serialised JSON string in settings.palette.
    payload.palette = { accent_color: g_accent };

    try {
        const res = await fetch(API_SETTINGS, {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(payload),
        });
        if (res.ok) {
            setStatus(elPersonaStatus, 'Saved!', 'ok');
        } else {
            setStatus(elPersonaStatus, 'Save failed (' + res.status + ')', 'error');
        }
    } catch (err) {
        setStatus(elPersonaStatus, 'Offline \u2014 settings saved locally', '');
        console.warn('Settings API unavailable:', err);
    } finally {
        elSavePersona.disabled = false;
    }
}

// ── Peripherals ────────────────────────────────────────────────────────────
async function loadPeripherals() {
    try {
        const res  = await fetch(API_SETTINGS);
        const data = await res.json();
        // GET /api/settings does not yet emit a "peripherals" array.
        // buildFallbackPeriphs() is used until that endpoint is extended.
        renderPeriph(data.peripherals || buildFallbackPeriphs());
    } catch (_) {
        renderPeriph(buildFallbackPeriphs());
    }
}

function buildFallbackPeriphs() {
    return [
        { name: 'Drive motors',  enabled: true,  meta: 'PWM left/right' },
        { name: 'OLED display',  enabled: false, meta: 'I²C 128×64' },
        { name: 'Battery ADC',   enabled: true,  meta: 'GPIO 34' },
        { name: 'WS2812 LED',    enabled: false, meta: 'GPIO 48' },
    ];
}

function renderPeriph(list) {
    elPeriphList.innerHTML = '';
    list.forEach(p => {
        const li = document.createElement('li');
        li.className = 'periph-item';
        li.innerHTML =
            `<span class="periph-name">${escapeHtml(p.name)}</span>` +
            (p.meta ? `<span class="periph-meta">${escapeHtml(p.meta)}</span>` : '') +
            `<span class="periph-badge ${p.enabled ? 'enabled' : 'disabled'}">${p.enabled ? 'ON' : 'OFF'}</span>`;
        elPeriphList.appendChild(li);
    });
}

// ── Drive settings ─────────────────────────────────────────────────────────
function initDriveSliders() {
    elMaxSpeed.addEventListener('input', () => {
        elMaxSpeedVal.textContent = elMaxSpeed.value + '%';
        joyRender();  // re-apply curve to current knob position
    });
    elExpo.addEventListener('input', () => {
        elExpoDisp.textContent = elExpo.value + '%';
        joyRender();
    });
    elDeadzone.addEventListener('input', () => {
        elDzDisp.textContent = elDeadzone.value + '%';
        joyRender();
    });
}

// ── Test joystick ──────────────────────────────────────────────────────────
// Mirrors the exact arcade-mix math in index.js / ctrl_drive.c.
// No WebSocket — purely visual so the user can feel the curve live.

let joyActivePid = null;
let joyRawX = 0;
let joyRawY = 0;
let joyRafPending = false;

function joyClamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function joyApplyResponse(raw) {
    const dz   = parseInt(elDeadzone.value, 10) / 100;
    const expo = parseInt(elExpo.value, 10) / 100;
    const sign = raw >= 0 ? 1 : -1;
    const abs  = Math.abs(raw);
    if (abs < dz) return 0;
    const range = 1 - dz;
    if (range <= 0) return sign;  // dz === 1.0: full deadzone collapses to max — snap rather than NaN
    const rescaled = (abs - dz) / range;
    const power    = 1 + expo * 2;
    return sign * joyClamp(Math.pow(rescaled, power), 0, 1);
}

function joyPointerToAxes(cx, cy) {
    const rect  = elTestZone.getBoundingClientRect();
    const ox    = cx - (rect.left + rect.width  / 2);
    const oy    = cy - (rect.top  + rect.height / 2);
    const r     = rect.width / 2;
    const mag   = Math.hypot(ox, oy);
    const scale = mag > r ? r / mag : 1;
    joyRawX =  joyClamp(ox * scale / r, -1, 1);
    joyRawY = -joyClamp(oy * scale / r, -1, 1);  // Y flipped: up = positive
    const knobDx = ox * scale;
    const knobDy = oy * scale;
    elTestKnob.style.transform =
        `translate(calc(-50% + ${knobDx}px), calc(-50% + ${knobDy}px))`;
}

function joyRender() {
    if (joyRafPending) return;
    joyRafPending = true;
    requestAnimationFrame(() => {
        joyRafPending = false;
        const maxDuty = parseInt(elMaxSpeed.value, 10) / 100;
        const ax = joyApplyResponse(joyRawX);
        const ay = joyApplyResponse(joyRawY);
        const leftDuty  = joyClamp((ay + ax) * maxDuty, -1, 1);
        const rightDuty = joyClamp((ay - ax) * maxDuty, -1, 1);
        renderTestBar(elTestBarL, elTestCardL, leftDuty);
        renderTestBar(elTestBarR, elTestCardR, rightDuty);
    });
}

function renderTestBar(bar, card, duty) {
    const pct = Math.round(Math.abs(duty) * 100);
    bar.style.width = pct + '%';
    const stopped = pct < 2;
    if (stopped) {
        bar.style.background = 'var(--clr-muted)';
        card.dataset.state   = 'stopped';
    } else if (duty > 0) {
        bar.style.background = 'var(--clr-success)';
        card.dataset.state   = 'running';
    } else {
        bar.style.background = 'var(--clr-accent)';
        card.dataset.state   = 'reverse';
    }
}

function initTestJoystick() {
    if (!elTestZone) return;

    elTestZone.addEventListener('pointerdown', e => {
        if (joyActivePid !== null) return;
        joyActivePid = e.pointerId;
        elTestZone.setPointerCapture(e.pointerId);
        joyPointerToAxes(e.clientX, e.clientY);
        joyRender();
    });

    elTestZone.addEventListener('pointermove', e => {
        if (e.pointerId !== joyActivePid) return;
        joyPointerToAxes(e.clientX, e.clientY);
        joyRender();
    });

    function joyRelease(e) {
        if (e.pointerId !== joyActivePid) return;
        joyActivePid = null;
        joyRawX = 0;
        joyRawY = 0;
        elTestKnob.style.transform = 'translate(-50%, -50%)';
        joyRender();
    }

    elTestZone.addEventListener('pointerup',     joyRelease);
    elTestZone.addEventListener('pointercancel', joyRelease);
}

async function saveDrive() {
    elSaveDrive.disabled = true;
    setStatus(elDriveStatus, 'Saving\u2026', '');

    const payload = {
        drive_max_duty: parseInt(elMaxSpeed.value, 10) / 100,
        drive_expo:     parseInt(elExpo.value, 10) / 100,
        drive_deadband: parseInt(elDeadzone.value, 10) / 100,
    };

    try {
        const res = await fetch(API_SETTINGS, {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(payload),
        });
        if (res.ok) {
            setStatus(elDriveStatus, 'Saved!', 'ok');
        } else {
            setStatus(elDriveStatus, 'Save failed (' + res.status + ')', 'error');
        }
    } catch (err) {
        setStatus(elDriveStatus, 'Offline \u2014 changes not persisted', 'error');
        console.warn('Drive save unavailable:', err);
    } finally {
        elSaveDrive.disabled = false;
    }
}

// ── Pre-fill from existing settings ───────────────────────────────────────
async function prefillSettings() {
    try {
        const res  = await fetch(API_SETTINGS);
        if (!res.ok) return;
        const data = await res.json();
        if (data.device_name)  elDevName.value = data.device_name;
        const savedAccent = data.palette?.accent_color;
        if (savedAccent) {
            g_accent = savedAccent;
            applyAccent(savedAccent);
            const match = elPaletteRow.querySelector(`[data-color="${savedAccent}"]`);
            if (match) {
                elPaletteRow.querySelectorAll('.swatch').forEach(s => s.classList.remove('selected'));
                match.classList.add('selected');
            }
        }
        if (data.drive_max_duty  !== undefined) { elMaxSpeed.value = Math.round(data.drive_max_duty * 100);  elMaxSpeedVal.textContent = elMaxSpeed.value + '%'; }
        if (data.drive_expo       !== undefined) { elExpo.value     = Math.round(data.drive_expo * 100);       elExpoDisp.textContent    = elExpo.value + '%'; }
        if (data.drive_deadband   !== undefined) { elDeadzone.value = Math.round(data.drive_deadband * 100);   elDzDisp.textContent      = elDeadzone.value + '%'; }
    } catch (_) {
        // Offline — defaults are fine
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────
function showSpinner(show) {
    elSpinner.classList.toggle('hidden', !show);
}

function setWifiStatus(msg) {
    elWifiStatus.textContent = msg;
}

function setStatus(el, msg, cls) {
    el.textContent = msg;
    el.className   = 'status' + (cls ? ' ' + cls : '');
}

function qualityBars(q) {
    const filled = Math.round((q || 0) / 25);
    let s = '';
    for (let i = 0; i < 4; i++) s += i < filled ? '\u25AE' : '\u25AF';
    return s;
}

function escapeHtml(str) {
    const d = document.createElement('div');
    d.textContent = str;
    return d.innerHTML;
}

window.onerror = (msg, url, line) => {
    fetch('/api/jserror', { method: 'POST', body: `${msg} @ ${url}:${line}` }).catch(() => {});
};

// ── Init ───────────────────────────────────────────────────────────────────
async function init() {
    buildDots();
    goTo(0);

    // WiFi
    elRescan.addEventListener('click', onRescan);
    elConnect.addEventListener('click', onConnect);

    if (elToggle) {
        elToggle.addEventListener('click', () => {
            const isPass   = elPass.type === 'password';
            elPass.type    = isPass ? 'text' : 'password';
            elToggle.textContent = isPass ? '\u{1F648}' : '\u{1F441}';
        });
    }

    // Persona
    initPalette();
    elSavePersona.addEventListener('click', savePersona);

    // Drive sliders + test joystick
    initDriveSliders();
    initTestJoystick();
    elSaveDrive.addEventListener('click', saveDrive);

    // Pre-fill known settings
    await prefillSettings();

    // Start background fetches
    showSpinner(true);
    loadNetworks().finally(() => showSpinner(false));
    loadPeripherals();
}

if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', init);
else
    init();

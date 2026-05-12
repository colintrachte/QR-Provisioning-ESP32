'use strict';

/* ── Config ──────────────────────────────────────────────────────────────── */
const CFG = {
    sendRateMs:  20,      // 50 Hz command rate
    reconnectMs: 2000,
    pingRateMs:  2000,
    keyRampMs:   180,     // ms ramp per axis, 0→1
    deadzone:    0.06,
    expo:        0.35,
};

// Pre-allocated binary axes buffer: [type=0x01][i16 x][i16 y] = 5 bytes
const AXES_BUF  = new ArrayBuffer(5);
const AXES_VIEW = new DataView(AXES_BUF);
AXES_VIEW.setUint8(0, 0x01);   // WS_MSG_AXES

/* ── State ───────────────────────────────────────────────────────────────── */
const S = {
    rawX: 0, rawY: 0,
    axisX: 0, axisY: 0,
    armed: false,
    leftDuty: 0, rightDuty: 0,
    rssi: null, battery: null, temp: null,
    latencyMs: null, uptime: null, heap: null, errors: 0,
    rafPending: false, lastLedMag: -1, pingSentAt: 0,
    _knobHalfW: 0,
};

const KEY_RAMP = {
    w: { held: false, v: 0 },
    a: { held: false, v: 0 },
    s: { held: false, v: 0 },
    d: { held: false, v: 0 },
};
const KEY_MAP = { w: [0, 1], a: [-1, 0], s: [0, -1], d: [1, 0] };

/* ── DOM refs ────────────────────────────────────────────────────────────── */
const D = {
    // Header
    deviceName:     document.getElementById('device-name'),
    menuDeviceName: document.getElementById('menu-device-name'),
    pageTitle:      document.getElementById('page-title'),
    armBtn:         document.getElementById('arm-btn'),
    armLabel:       document.querySelector('#arm-btn .arm-label'),
    connBadge:      document.getElementById('conn-badge'),
    connLabel:      document.querySelector('#conn-badge .conn-label'),

    // Menu
    menuBtn:        document.getElementById('menu-btn'),
    menuDrawer:     document.getElementById('menu-drawer'),
    menuOverlay:    document.getElementById('menu-overlay'),
    menuClose:      document.getElementById('menu-close'),
    menuReboot:     document.getElementById('menu-reboot'),

    // Sys-bar
    rssiStat:       document.getElementById('rssi-stat'),
    rssiVal:        document.getElementById('rssi-val'),
    rssiBars:       document.getElementById('rssi-bars'),
    rssiBtns:       Array.from(document.querySelectorAll('.rssi-bar')),
    battStat:       document.getElementById('batt-stat'),
    battVal:        document.getElementById('batt-val'),
    battFill:       document.getElementById('batt-fill'),
    tempStat:       document.getElementById('temp-stat'),
    tempVal:        document.getElementById('temp-val'),

    // Joystick
    zone:   document.getElementById('joystick-zone'),
    knob:   document.getElementById('joystick-knob'),

    // Motor bars
    barL:   document.getElementById('bar-left'),
    barR:   document.getElementById('bar-right'),
    cardL:  document.getElementById('card-left'),
    cardR:  document.getElementById('card-right'),

    // Diagnostics
    latVal:  document.getElementById('latency-val'),
    uptime:  document.getElementById('uptime-val'),
    heap:    document.getElementById('heap-val'),
    errors:  document.getElementById('err-val'),
};

/* ── Hamburger menu ──────────────────────────────────────────────────────── */
function openMenu() {
    D.menuDrawer.classList.add('open');
    D.menuOverlay.classList.add('open');
    D.menuOverlay.removeAttribute('aria-hidden');
    D.menuBtn.setAttribute('aria-expanded', 'true');
}

function closeMenu() {
    D.menuDrawer.classList.remove('open');
    D.menuOverlay.classList.remove('open');
    D.menuOverlay.setAttribute('aria-hidden', 'true');
    D.menuBtn.setAttribute('aria-expanded', 'false');
}

D.menuBtn.addEventListener('click', () => {
    D.menuDrawer.classList.contains('open') ? closeMenu() : openMenu();
});
D.menuClose.addEventListener('click', closeMenu);
D.menuOverlay.addEventListener('click', closeMenu);

D.menuReboot.addEventListener('click', async () => {
    closeMenu();
    try {
        await fetch('/api/reboot', { method: 'POST' });
    } catch (_) { /* connection will drop anyway */ }
});

// Close menu on Escape
document.addEventListener('keydown', e => {
    if (e.key === 'Escape') closeMenu();
});

/* ── Device name ─────────────────────────────────────────────────────────── */
function applyDeviceName(name) {
    if (!name) return;
    D.deviceName.textContent     = name;
    D.menuDeviceName.textContent = name;
    document.title               = name;
}

async function loadDeviceName() {
    try {
        const r = await fetch('/api/settings');
        if (!r.ok) return;
        const j = await r.json();
        if (j.device_name) applyDeviceName(j.device_name);
    } catch (_) { /* offline — keep default */ }
}

/* ── Math helpers ────────────────────────────────────────────────────────── */
function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function applyResponse(raw) {
    const dz   = CFG.deadzone;
    const sign = raw >= 0 ? 1 : -1;
    const abs  = Math.abs(raw);
    if (abs < dz) return 0;
    const rescaled = (abs - dz) / (1 - dz);
    const power    = 1 + CFG.expo * 2;   // 0.35 expo → power 1.7
    return sign * clamp(Math.pow(rescaled, power), 0, 1);
}

function setRaw(x, y) {
    S.rawX = clamp(x, -1, 1);
    S.rawY = clamp(y, -1, 1);
    S.axisX = applyResponse(S.rawX);
    S.axisY = applyResponse(S.rawY);
    // Mirror ctrl_drive.c arcade mix for bar display
    S.leftDuty  = S.armed ? clamp(S.axisY + S.axisX, -1, 1) : 0;
    S.rightDuty = S.armed ? clamp(S.axisY - S.axisX, -1, 1) : 0;
    scheduleRaf();
}

function zeroAxes() { setRaw(0, 0); }

/* ── Arm / disarm ────────────────────────────────────────────────────────── */
function setArmed(v) {
    S.armed = v;
    D.armBtn.className     = 'arm-btn ' + (v ? 'armed' : 'disarmed');
    D.armLabel.textContent = v ? 'ARMED' : 'DISARMED';
    D.zone.classList.toggle('disarmed', !v);
    // --- BINARY ARMING SIGNAL ---
    if (ws && ws.readyState === WebSocket.OPEN) {
        const armBuf = new Uint8Array([0x02, v ? 0x01 : 0x00]);
        ws.send(armBuf);
    }
    if (!v) {
        zeroAxes();
        resetKeys();
        S.lastLedMag = -1;
    }
    scheduleRaf();
}

D.armBtn.addEventListener('click', () => setArmed(!S.armed));

/* ── WebSocket ───────────────────────────────────────────────────────────── */
let ws = null, reconnectTimer = null, pingTimer = null, sendTimer = null;

function connectWS() {
    if (ws) { ws.onclose = null; ws.close(); }
    ws = new WebSocket('ws://' + location.hostname + '/ws');

    ws.onopen = () => {
        clearTimeout(reconnectTimer);
        setConnState(true);
        schedulePing();
    };

    ws.onclose = () => {
        setConnState(false);
        if (S.armed) setArmed(false);   // never stay armed while disconnected
        zeroAxes();
        resetKeys();
        clearTimeout(pingTimer);
        reconnectTimer = setTimeout(connectWS, CFG.reconnectMs);
    };

    ws.onerror = () => ws.close();

    ws.onmessage = ({ data }) => {
        if (data === 'pong') {
            S.latencyMs = Date.now() - S.pingSentAt;
            schedulePing();
            scheduleRaf();
            return;
        }
        try {
            const msg = JSON.parse(data);
            let changed = false;
            // Device name from firmware (optional field)
            if (msg.name    !== undefined) { applyDeviceName(msg.name); }
            if (msg.rssi    !== undefined) { S.rssi    = msg.rssi;    changed = true; }
            if (msg.battery !== undefined) { S.battery = msg.battery; changed = true; }
            if (msg.temp    !== undefined) { S.temp    = msg.temp;    changed = true; }
            if (msg.uptime  !== undefined) { S.uptime  = msg.uptime;  changed = true; }
            if (msg.heap    !== undefined) { S.heap    = msg.heap;    changed = true; }
            if (msg.errors  !== undefined) { S.errors  = msg.errors;  changed = true; }
            if (msg.estop   !== undefined) { updateEstop(msg.estop, msg.resume); }
            // Firmware-initiated arm state change (e.g. watchdog disarm)
            if (msg.armed !== undefined && msg.armed !== S.armed) {
                setArmed(msg.armed);
                if (!msg.armed && msg.disarm_reason) {
                    showDisarmToast(msg.disarm_reason);
                }
            }
            if (changed) scheduleRaf();
        } catch (_) { /* ignore malformed */ }
    };
}

function schedulePing() {
    clearTimeout(pingTimer);
    pingTimer = setTimeout(() => {
        if (ws && ws.readyState === WebSocket.OPEN) {
            S.pingSentAt = Date.now();
            ws.send('ping');
        }
    }, CFG.pingRateMs);
}

/* ── Send loop (50 Hz) ───────────────────────────────────────────────────── */
function sendAxes() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    if (!S.armed || (S.axisX === 0 && S.axisY === 0)) {
        ws.send('stop');
        return;
    }
    const ix = Math.max(-32768, Math.min(32767, (S.axisX * 10000) | 0));
    const iy = Math.max(-32768, Math.min(32767, (S.axisY * 10000) | 0));
    AXES_VIEW.setInt16(1, ix, true);
    AXES_VIEW.setInt16(3, iy, true);
    ws.send(AXES_BUF);
}

function sendLed() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const mag = S.armed ? Math.min(1, Math.hypot(S.axisX, S.axisY)) : 0;
    const r   = Math.round(mag * 100) / 100;
    if (r !== S.lastLedMag) {
        S.lastLedMag = r;
        ws.send('led:' + r.toFixed(2));
    }
}

function startSendLoop() {
    clearInterval(sendTimer);
    sendTimer = setInterval(() => {
        sendAxes();
        sendLed();
        scheduleRaf();
    }, CFG.sendRateMs);
}

/* ── Joystick ────────────────────────────────────────────────────────────── */
let zoneRect = null, activePid = null;

new ResizeObserver(() => { zoneRect = null; S._knobHalfW = 0; }).observe(D.zone);

function getZoneRect() {
    if (!zoneRect) zoneRect = D.zone.getBoundingClientRect();
    return zoneRect;
}

function pointerToAxes(clientX, clientY) {
    const r   = getZoneRect();
    const cx  = r.left + r.width  / 2;
    const cy  = r.top  + r.height / 2;
    let dx    = clientX - cx;
    let dy    = clientY - cy;
    if (!S._knobHalfW) S._knobHalfW = D.knob.offsetWidth / 2;
    const maxR = (r.width / 2) - S._knobHalfW - 4;
    const dist = Math.hypot(dx, dy);
    if (dist > maxR) { const s = maxR / dist; dx *= s; dy *= s; }
    setRaw(dx / maxR, -dy / maxR);
    return { dx, dy };
}

function moveKnob(dx = 0, dy = 0) {
    D.knob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
}

D.zone.addEventListener('pointerdown', e => {
    if (!S.armed || activePid !== null) return;
    activePid = e.pointerId;
    D.zone.setPointerCapture(e.pointerId);
    const { dx, dy } = pointerToAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

D.zone.addEventListener('pointermove', e => {
    if (e.pointerId !== activePid) return;
    const { dx, dy } = pointerToAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

function releasePointer(e) {
    if (e.pointerId !== activePid) return;
    activePid = null;
    zeroAxes();
    moveKnob(0, 0);
}
D.zone.addEventListener('pointerup',     releasePointer);
D.zone.addEventListener('pointercancel', releasePointer);

/* ── Keyboard (WASD) ─────────────────────────────────────────────────────── */
let rampRafId = null, lastRampTick = 0;

function rampTick(now) {
    const dt   = lastRampTick ? Math.min(now - lastRampTick, 32) : 16;
    lastRampTick = now;
    const step = dt / CFG.keyRampMs;

    let any = false;
    for (const k in KEY_RAMP) {
        const ks = KEY_RAMP[k];
        if (ks.held) { ks.v = Math.min(1, ks.v + step); any = true; }
        else         { ks.v = Math.max(0, ks.v - step); if (ks.v > 0) any = true; }
    }

    let x = 0, y = 0;
    for (const k in KEY_RAMP) {
        const v = KEY_RAMP[k].v;
        if (v > 0) { x += KEY_MAP[k][0] * v; y += KEY_MAP[k][1] * v; }
    }
    const mag = Math.hypot(x, y);
    if (mag > 1) { x /= mag; y /= mag; }

    setRaw(x, y);
    rampRafId = any ? requestAnimationFrame(rampTick) : null;
    if (!any) lastRampTick = 0;
}

function ensureRamp() {
    if (!rampRafId) { lastRampTick = 0; rampRafId = requestAnimationFrame(rampTick); }
}

function resetKeys() {
    for (const k in KEY_RAMP) { KEY_RAMP[k].held = false; KEY_RAMP[k].v = 0; }
    zeroAxes();
}

document.addEventListener('keydown', e => {
    const k = e.key.toLowerCase();
    if (!KEY_RAMP[k] || KEY_RAMP[k].held || !S.armed) return;
    KEY_RAMP[k].held = true;
    ensureRamp();
});
document.addEventListener('keyup', e => {
    const k = e.key.toLowerCase();
    if (!KEY_RAMP[k]) return;
    KEY_RAMP[k].held = false;
    ensureRamp();
});

/* ── Disarm toast ────────────────────────────────────────────────────────── */
let _toastTimer = null;
function showDisarmToast(reason) {
    let toast = document.getElementById('disarm-toast');
    if (!toast) {
        toast = document.createElement('div');
        toast.id = 'disarm-toast';
        toast.style.cssText = [
            'position:fixed', 'bottom:24px', 'left:50%', 'transform:translateX(-50%)',
            'background:var(--clr-danger,#e74c3c)', 'color:#fff',
            'padding:10px 20px', 'border-radius:8px', 'font-size:14px',
            'font-weight:600', 'z-index:9999', 'pointer-events:none',
            'transition:opacity 0.3s', 'white-space:nowrap',
        ].join(';');
        document.body.appendChild(toast);
    }
    const labels = {
        watchdog: 'Disarmed — command timeout (watchdog)',
        estop:    'Disarmed — emergency stop',
        client:   'Disarmed — client disconnected',
    };
    toast.textContent = '⚠ ' + (labels[reason] || ('Disarmed: ' + reason));
    toast.style.opacity = '1';
    clearTimeout(_toastTimer);
    _toastTimer = setTimeout(() => { toast.style.opacity = '0'; }, 4000);
}

/* ── E-stop ──────────────────────────────────────────────────────────────── */
function updateEstop(estop, resume) {
    if (estop || resume) {
        D.armBtn.classList.add('estopped');
        D.armLabel.textContent = resume ? 'SEND ZERO' : 'E-STOP';
    } else {
        D.armBtn.classList.remove('estopped');
        D.armLabel.textContent = S.armed ? 'ARMED' : 'DISARMED';
    }
}

/* ── Rendering (all DOM writes batched in rAF) ───────────────────────────── */
function scheduleRaf() {
    if (S.rafPending) return;
    S.rafPending = true;
    requestAnimationFrame(renderFrame);
}

// Per-side motor bar cache to avoid unnecessary DOM writes
const BAR_STATE = { left: null, right: null };

function renderMotorBar(side, duty) {
    const absD  = Math.abs(duty);
    const pct   = Math.round(absD * 100);
    const prev  = BAR_STATE[side];
    if (prev && prev.pct === pct && prev.fwd === (duty > 0)) return;

    const bar  = side === 'left' ? D.barL : D.barR;
    const card = side === 'left' ? D.cardL : D.cardR;
    bar.style.width = pct + '%';

    const running = absD > 0.01;
    if (!running)      bar.style.background = 'var(--clr-stopped)';
    else if (duty > 0) bar.style.background = 'var(--clr-running)';
    else               bar.style.background = 'var(--clr-accent)';   // reverse: blue

    card.dataset.state = running ? 'RUNNING' : 'STOPPED';
    BAR_STATE[side] = { pct, fwd: duty > 0 };
}

function renderFrame() {
    S.rafPending = false;

    // Motor bars
    renderMotorBar('left',  S.leftDuty);
    renderMotorBar('right', S.rightDuty);

    // RSSI bars
    if (S.rssi !== null) {
        const r     = S.rssi;
        const level = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
        const weak  = r < -75;
        D.rssiBars.classList.toggle('warn', weak);
        D.rssiBtns.forEach((bar, i) => bar.classList.toggle('active', i < level));
        D.rssiVal.textContent = r + ' dBm';
        D.rssiStat.classList.toggle('warn', weak);
    }

    // Battery fill
    if (S.battery !== null) {
        const pct  = clamp(S.battery, 0, 100);
        const low  = pct < 20;
        D.battFill.style.width      = pct + '%';
        D.battFill.style.background = low ? 'var(--clr-danger)' :
                                      pct < 50 ? 'var(--clr-warning)' : 'var(--clr-running)';
        D.battVal.textContent = pct + '%';
        D.battStat.classList.toggle('warn', low);
    }

    // Temperature — from ESP32-S3 internal sensor via health_monitor
    if (S.temp !== null) {
        const t   = S.temp;
        const hot = t > 70;
        D.tempVal.textContent = t.toFixed(1) + '°';
        D.tempStat.classList.toggle('warn', hot);
    }

    // Diagnostics
    D.latVal.textContent = S.latencyMs !== null ? S.latencyMs + ' ms' : '—';

    if (S.uptime !== null) {
        const sec = S.uptime;
        const h   = Math.floor(sec / 3600);
        const m   = Math.floor((sec % 3600) / 60);
        const s   = sec % 60;
        D.uptime.textContent = h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${s}s` : `${s}s`;
    }
    if (S.heap !== null) {
        D.heap.textContent = (S.heap / 1024).toFixed(1) + ' kB';
    }
    D.errors.textContent = S.errors;
}

/* ── Connection state ────────────────────────────────────────────────────── */
function setConnState(connected) {
    D.connBadge.className   = 'conn-badge ' + (connected ? 'connected' : 'disconnected');
    D.connLabel.textContent = connected ? 'Connected' : 'Disconnected';
    if (!connected) {
        S.rssi = S.battery = S.temp = S.latencyMs = null;
        S.lastLedMag = -1;
        D.rssiStat.classList.remove('warn');
        D.battStat.classList.remove('warn');
        D.tempStat.classList.remove('warn');
        // Reset bar visuals
        D.rssiBtns.forEach(b => b.classList.remove('active', 'warn'));
        D.battFill.style.width = '0%';
        D.rssiVal.textContent  = '—';
        D.battVal.textContent  = '—';
        D.tempVal.textContent  = '—';
        D.latVal.textContent   = '—';
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */
loadDeviceName();
connectWS();
startSendLoop();

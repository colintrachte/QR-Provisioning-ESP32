'use strict';

// ── Config ────────────────────────────────────────────────────────────────────
// All tunable values in one place. These could be exposed in a settings UI.
const CFG = {
    sendRateMs:   20,      // 50 Hz command rate — halves worst-case queuing latency
    reconnectMs:  2000,
    pingRateMs:   2000,
    keyRampMs:    180,     // ms from 0→1 while key held; same rate to decay
    deadzone:     0.06,    // normalised; rescaled out after cutoff
    expo:         0.35,    // 0 = linear, 1 = full cubic; fine low-speed feel
    ledPrefix:    'led',
};

// Pre-allocated binary axes buffer: [type=0x01][i16 x][i16 y] = 5 bytes
// Reused every frame — no garbage generated on the hot send path.
const _axesBuf  = new ArrayBuffer(5);
const _axesView = new DataView(_axesBuf);
_axesView.setUint8(0, 0x01);   // WS_MSG_AXES — matches app_server.c

// ── State ─────────────────────────────────────────────────────────────────────
// Single source of truth — no scattered globals.
const state = {
    // Input
    rawX:      0,    // post-deadzone, pre-expo, [-1,1]
    rawY:      0,
    axisX:     0,    // post-expo, sent to robot
    axisY:     0,

    armed:     false,
    inputMode: 'auto',    // 'touch' | 'pc' | 'auto'
    lastInput: null,      // 'touch' | 'pointer' — last physical event type

    // Track display (arcade mix of axisX/Y, mirrored from ctrl_drive.c)
    leftDuty:  0,
    rightDuty: 0,

    // Telemetry from firmware
    rssi:      null,
    battery:   null,
    temp:      null,
    latencyMs: null,
    uptime:    null,
    heap:      null,
    errors:    0,

    // Render
    rafPending: false,
    lastLedMag: -1,
    pingSentAt: 0,
};

// Keyboard per-key ramp state (separate from main state to keep iteration fast)
const keyRamp = {
    w: { held: false, v: 0 },
    a: { held: false, v: 0 },
    s: { held: false, v: 0 },
    d: { held: false, v: 0 },
};
// KEY → [xContrib, yContrib]
const KEY_AXIS = { w: [0, 1], a: [-1, 0], s: [0, -1], d: [1, 0] };

// ── DOM refs ──────────────────────────────────────────────────────────────────
const dom = {
    armBtn:    document.getElementById('arm-btn'),
    armLabel:  document.querySelector('#arm-btn .arm-label'),
    connBadge: document.getElementById('conn-badge'),
    connLabel: document.querySelector('#conn-badge .conn-label'),
    zone:      document.getElementById('joystick-zone'),
    knob:      document.getElementById('joystick-knob'),
    rssiVal:   document.getElementById('rssi-val'),
    rssiStat:  document.getElementById('rssi-stat'),
    battVal:   document.getElementById('batt-val'),
    battStat:  document.getElementById('batt-stat'),
    tempVal:   document.getElementById('temp-val'),
    latVal:    document.getElementById('latency-val'),
    modeBtn:   document.getElementById('mode-btn'),
    modeLabel: document.getElementById('mode-label'),
    joySec:    document.getElementById('joystick-section'),
    cards:  { left: document.getElementById('card-left'),   right: document.getElementById('card-right')  },
    bars:   { left: document.getElementById('bar-left'),    right: document.getElementById('bar-right')   },
    speeds: { left: document.getElementById('speed-left'),  right: document.getElementById('speed-right') },
    states: { left: document.getElementById('state-left'),  right: document.getElementById('state-right') },
    dirs:   { left: document.getElementById('dir-left'),    right: document.getElementById('dir-right')   },
    uptime: document.getElementById('uptime-val'),
    heap:   document.getElementById('heap-val'),
    errors: document.getElementById('err-val'),
};

// ── Math helpers ──────────────────────────────────────────────────────────────
function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

/**
 * Apply deadzone with rescaling and exponential response curve.
 *
 * Rescaling: after stripping the deadzone, remap [dz, 1] → [0, 1] so
 * output is continuous with no step at the deadzone boundary.
 *
 * Expo: output = sign(x) * |x|^(1 + expo*2). At expo=0 this is linear;
 * at expo=0.35 it gives a gentle cube-root feel near centre for fine
 * low-speed control while still reaching full output at the edges.
 *
 * Both axes are processed independently so diagonal normalization
 * (done by the caller) works on post-expo values.
 */
function applyResponse(raw) {
    const dz   = CFG.deadzone;
    const sign = raw >= 0 ? 1 : -1;
    const abs  = Math.abs(raw);
    if (abs < dz) return 0;

    // Rescale: [dz, 1] → [0, 1]  (removes discontinuity at deadzone edge)
    const rescaled = (abs - dz) / (1 - dz);

    // Expo curve: power > 1 compresses low-speed range
    const power    = 1 + CFG.expo * 2;   // 0.35 expo → power 1.7
    const shaped   = Math.pow(rescaled, power);

    return sign * clamp(shaped, 0, 1);
}

/**
 * Set raw axes (pre-expo) from any input source, compute post-expo axes,
 * update the arcade mix for track card display, and schedule a render.
 */
function setRaw(x, y) {
    // Clamp before response to handle numerical edge cases from pointer math
    state.rawX = clamp(x, -1, 1);
    state.rawY = clamp(y, -1, 1);

    state.axisX = applyResponse(state.rawX);
    state.axisY = applyResponse(state.rawY);

    // Mirror ctrl_drive.c arcade mix for display
    state.leftDuty  = state.armed ? clamp(state.axisY + state.axisX, -1, 1) : 0;
    state.rightDuty = state.armed ? clamp(state.axisY - state.axisX, -1, 1) : 0;

    scheduleRaf();
}

function zeroAxes() { setRaw(0, 0); }

// ── Input mode ────────────────────────────────────────────────────────────────
// "auto" uses the last observed physical event type rather than a media query.
// pointer:coarse is unreliable on hybrid devices (Surface, iPad + keyboard).
function resolvedMode() {
    if (state.inputMode !== 'auto') return state.inputMode;
    return state.lastInput === 'touch' ? 'touch' : 'pc';
}

function applyMode() {
    const mode = resolvedMode();
    dom.joySec.hidden      = (mode !== 'touch');
    dom.modeLabel.textContent = (mode === 'touch') ? '⌨ PC' : '🕹 Touch';
    if (mode !== 'touch') { moveKnob(0, 0); }
}

dom.modeBtn.addEventListener('click', () => {
    const cur = resolvedMode();
    state.inputMode = (cur === 'touch') ? 'pc' : 'touch';
    applyMode();
});

// Track last physical input type so auto-detection is event-driven, not guessed
window.addEventListener('touchstart', () => {
    state.lastInput = 'touch';
    applyMode();
}, { passive: true });
window.addEventListener('pointerdown', e => {
    if (e.pointerType === 'mouse') { state.lastInput = 'pointer'; applyMode(); }
}, { passive: true });

// ── Arm / disarm ──────────────────────────────────────────────────────────────
function setArmed(value) {
    state.armed = value;
    dom.armBtn.className      = `arm-btn ${value ? 'armed' : 'disarmed'}`;
    dom.armLabel.textContent  = value ? 'ARMED' : 'DISARMED';
    dom.zone.classList.toggle('disarmed', !value);
    if (!value) {
        zeroAxes();
        resetKeyRamps();
        state.lastLedMag = -1;
    }
    scheduleRaf();
}

dom.armBtn.addEventListener('click', () => setArmed(!state.armed));

// ── WebSocket ─────────────────────────────────────────────────────────────────
let socket         = null;
let reconnectTimer = null;
let pingTimer      = null;
let sendTimer      = null;

function connectWS() {
    if (socket) { socket.onclose = null; socket.close(); }
    socket = new WebSocket(`ws://${location.hostname}/ws`);

    socket.onopen = () => {
        clearTimeout(reconnectTimer);
        setConnectionState(true);
        schedulePing();
    };

    socket.onclose = () => {
        setConnectionState(false);
        zeroAxes();
        resetKeyRamps();
        clearTimeout(pingTimer);
        reconnectTimer = setTimeout(connectWS, CFG.reconnectMs);
    };

    socket.onerror = () => socket.close();

    socket.onmessage = ({ data }) => {
        if (data === 'pong') {
            state.latencyMs = Date.now() - state.pingSentAt;
            schedulePing();
            scheduleRaf();
            return;
        }
        try {
            const msg = JSON.parse(data);
            let changed = false;
            if (msg.estop   !== undefined) { updateEstop(msg.estop, msg.resume); }
            if (msg.rssi    !== undefined) { state.rssi    = msg.rssi;    changed = true; }
            if (msg.battery !== undefined) { state.battery = msg.battery; changed = true; }
            if (msg.temp    !== undefined) { state.temp    = msg.temp;    changed = true; }
            if (msg.uptime  !== undefined) { state.uptime  = msg.uptime;  changed = true; }
            if (msg.heap    !== undefined) { state.heap    = msg.heap;    changed = true; }
            if (msg.errors  !== undefined) { state.errors  = msg.errors;  changed = true; }
            if (changed) scheduleRaf();
        } catch (_) {}
    };
}

function schedulePing() {
    clearTimeout(pingTimer);
    pingTimer = setTimeout(() => {
        if (socket && socket.readyState === WebSocket.OPEN) {
            state.pingSentAt = Date.now();
            socket.send('ping');
        }
    }, CFG.pingRateMs);
}

// ── Send loop (50 Hz) ─────────────────────────────────────────────────────────
// Binary axes packet: [0x01][int16 x*10000][int16 y*10000] = 5 bytes.
// int16 gives 0.0001 resolution — well below any meaningful motor step.
// Pre-allocated buffer: zero garbage per frame on the hot path.
//
// "stop" is sent as text (infrequent, keeps firmware dispatch simple).
// LED is sent as text only when value changes (already change-gated).
function sendAxes() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    if (!state.armed || (state.axisX === 0 && state.axisY === 0)) {
        socket.send('stop');
        return;
    }
    // Clamp to int16 range after scaling — handles floating-point edge cases
    const ix = Math.max(-32768, Math.min(32767, (state.axisX * 10000) | 0));
    const iy = Math.max(-32768, Math.min(32767, (state.axisY * 10000) | 0));
    _axesView.setInt16(1, ix, true);   // little-endian
    _axesView.setInt16(3, iy, true);
    socket.send(_axesBuf);
}

function startSendLoop() {
    clearInterval(sendTimer);
    sendTimer = setInterval(() => {
        sendAxes();
        sendLed();
        scheduleRaf();   // keep track bars in sync with what we're sending
    }, CFG.sendRateMs);
}

// ── Joystick ──────────────────────────────────────────────────────────────────
let zoneRect        = null;
let activePointerId = null;

new ResizeObserver(() => { zoneRect = null; state._knobHalfW = 0; }).observe(dom.zone);

function getZoneRect() {
    if (!zoneRect) zoneRect = dom.zone.getBoundingClientRect();
    return zoneRect;
}

function pointerToAxes(clientX, clientY) {
    const r    = getZoneRect();
    const cx   = r.left + r.width  / 2;
    const cy   = r.top  + r.height / 2;
    let dx     = clientX - cx;
    let dy     = clientY - cy;
    // dom.knob.offsetWidth triggers layout — cache it after first read
    if (!state._knobHalfW) state._knobHalfW = dom.knob.offsetWidth / 2;
    const maxR = (r.width / 2) - state._knobHalfW - 4;
    const dist = Math.hypot(dx, dy);
    if (dist > maxR) { const s = maxR / dist; dx *= s; dy *= s; }
    setRaw(dx / maxR, -dy / maxR);
    return { dx, dy };
}

function moveKnob(dx = 0, dy = 0) {
    dom.knob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
}

dom.zone.addEventListener('pointerdown', e => {
    if (!state.armed || activePointerId !== null) return;
    state.lastInput  = e.pointerType === 'touch' ? 'touch' : 'pointer';
    activePointerId  = e.pointerId;
    dom.zone.setPointerCapture(e.pointerId);
    const { dx, dy } = pointerToAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

dom.zone.addEventListener('pointermove', e => {
    if (e.pointerId !== activePointerId) return;
    const { dx, dy } = pointerToAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

const releasePointer = e => {
    if (e.pointerId !== activePointerId) return;
    activePointerId = null;
    zeroAxes();
    moveKnob(0, 0);
};
dom.zone.addEventListener('pointerup',     releasePointer);
dom.zone.addEventListener('pointercancel', releasePointer);

// ── Keyboard (WASD) ───────────────────────────────────────────────────────────
// Each key has an independent ramp value [0,1]. On keydown the ramp rises
// at 1/keyRampMs per ms; on keyup it falls at the same rate. This gives the
// same smooth acceleration feel as the joystick without snapping.
//
// After accumulating all key contributions, the combined vector is
// normalized to the unit circle (matching joystick behaviour) so diagonal
// input (W+D) doesn't produce a faster top speed than pure forward.
// The result passes through the same applyResponse() pipeline as the joystick.

let rampRafId    = null;
let lastRampTick = 0;

function rampTick(now) {
    const dt   = lastRampTick ? Math.min(now - lastRampTick, 32) : 16;
    lastRampTick = now;
    const step  = dt / CFG.keyRampMs;

    let anyActive = false;
    for (const k in keyRamp) {
        const ks = keyRamp[k];
        if (ks.held) { ks.v = Math.min(1, ks.v + step); anyActive = true; }
        else         { ks.v = Math.max(0, ks.v - step);  if (ks.v > 0) anyActive = true; }
    }

    // Accumulate contributions
    let x = 0, y = 0;
    for (const k in keyRamp) {
        const v = keyRamp[k].v;
        if (v > 0) { x += KEY_AXIS[k][0] * v; y += KEY_AXIS[k][1] * v; }
    }

    // Normalize diagonal to unit circle — same as joystick clamping
    const mag = Math.hypot(x, y);
    if (mag > 1) { x /= mag; y /= mag; }

    // Feed through the unified pipeline (deadzone + expo apply here too)
    setRaw(x, y);

    rampRafId = anyActive ? requestAnimationFrame(rampTick) : null;
    if (!anyActive) lastRampTick = 0;
}

function ensureRampRunning() {
    if (!rampRafId) { lastRampTick = 0; rampRafId = requestAnimationFrame(rampTick); }
}

function resetKeyRamps() {
    for (const k in keyRamp) { keyRamp[k].held = false; keyRamp[k].v = 0; }
    zeroAxes();
}

document.addEventListener('keydown', e => {
    const k = e.key.toLowerCase();
    if (!keyRamp[k] || keyRamp[k].held || !state.armed) return;
    keyRamp[k].held = true;
    ensureRampRunning();
});
document.addEventListener('keyup', e => {
    const k = e.key.toLowerCase();
    if (!keyRamp[k]) return;
    keyRamp[k].held = false;
    ensureRampRunning();
});

// ── LED ───────────────────────────────────────────────────────────────────────
// Drive LED brightness from joystick magnitude. Decoupled from axis pipeline —
// only sends when value actually changes.
function sendLed() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    const mag     = state.armed ? Math.min(1, Math.hypot(state.axisX, state.axisY)) : 0;
    const rounded = Math.round(mag * 100) / 100;
    if (rounded !== state.lastLedMag) {
        state.lastLedMag = rounded;
        socket.send(`${CFG.ledPrefix}:${rounded.toFixed(2)}`);
    }
}

// ── Rendering (all DOM writes batched in one rAF) ─────────────────────────────
function scheduleRaf() {
    if (state.rafPending) return;
    state.rafPending = true;
    requestAnimationFrame(renderFrame);
}

function renderFrame() {
    state.rafPending = false;

    // ── Track cards (locally computed from axes) ──────────────────────────
    renderTrackCard('left',  state.leftDuty);
    renderTrackCard('right', state.rightDuty);

    // ── Sys-bar ───────────────────────────────────────────────────────────
    if (state.rssi !== null) {
        const r    = state.rssi;
        const bars = r > -50 ? '▂▄▆█' : r > -65 ? '▂▄▆·' : r > -75 ? '▂▄··' : '▂···';
        dom.rssiVal.textContent = `${bars} ${r} dBm`;
        dom.rssiStat.classList.toggle('warn', r < -75);
    }

    if (state.battery !== null) {
        dom.battVal.textContent = state.battery + '%';
        dom.battStat.classList.toggle('warn', state.battery < 20);
    }

    if (state.temp !== null) {
        dom.tempVal.textContent = state.temp.toFixed(1) + ' °C';
    }

    dom.latVal.textContent = state.latencyMs !== null ? state.latencyMs + ' ms' : '—';

    // ── Diagnostics drawer ────────────────────────────────────────────────
    if (state.uptime !== null) {
        const s = state.uptime;
        const h = Math.floor(s / 3600);
        const m = Math.floor((s % 3600) / 60);
        const sec = s % 60;
        dom.uptime.textContent = h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${sec}s` : `${sec}s`;
    }
    if (state.heap !== null) {
        dom.heap.textContent = (state.heap / 1024).toFixed(1) + ' kB';
    }
    dom.errors.textContent = state.errors;
}

// Per-side render state cache — avoids redundant DOM writes
const _cardState = { left: null, right: null };

function renderTrackCard(side, duty) {
    const absD    = Math.abs(duty);
    const pct     = Math.round(absD * 100);
    const running = absD > 0.01;
    const state_s = running ? 'RUNNING' : 'STOPPED';
    const prev    = _cardState[side];

    // Only write DOM properties that actually changed
    if (!prev || prev.pct !== pct) {
        dom.speeds[side].textContent = pct + '%';
        dom.bars[side].style.width   = pct + '%';
    }
    if (!prev || prev.fwd !== (duty > 0) || prev.running !== running) {
        dom.dirs[side].textContent = absD < 0.01 ? '—' : duty > 0 ? '▲ FWD' : '▼ REV';
    }
    if (!prev || prev.state_s !== state_s) {
        dom.states[side].textContent    = state_s;
        dom.bars[side].style.background = running ? 'var(--clr-running)' : 'var(--clr-stopped)';
        dom.cards[side].dataset.state   = state_s;
    }
    _cardState[side] = { pct, fwd: duty > 0, running, state_s };
}

// ── E-stop ────────────────────────────────────────────────────────────────────
function updateEstop(estop, resume) {
    if (estop || resume) {
        dom.armBtn.classList.add('estopped');
        dom.armLabel.textContent = resume ? 'SEND ZERO' : 'E-STOP';
    } else {
        dom.armBtn.classList.remove('estopped');
        dom.armLabel.textContent = state.armed ? 'ARMED' : 'DISARMED';
    }
}

// ── Connection state ──────────────────────────────────────────────────────────
function setConnectionState(connected) {
    dom.connBadge.className   = `conn-badge ${connected ? 'connected' : 'disconnected'}`;
    dom.connLabel.textContent = connected ? 'Connected' : 'Disconnected';
    if (!connected) {
        state.rssi = state.battery = state.temp = state.latencyMs = null;
        dom.rssiStat.classList.remove('warn');
        dom.battStat.classList.remove('warn');
        state.lastLedMag = -1;
        // Clear display immediately without waiting for telemetry
        dom.rssiVal.textContent = '—';
        dom.battVal.textContent = '—';
        dom.tempVal.textContent = '—';
        dom.latVal.textContent  = '—';
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────
// Initialise mode from media query as a starting hint, then override on
// first real event (touchstart / pointerdown with mouse type)
state.lastInput = window.matchMedia('(pointer: coarse)').matches ? 'touch' : 'pointer';
applyMode();
connectWS();
startSendLoop();

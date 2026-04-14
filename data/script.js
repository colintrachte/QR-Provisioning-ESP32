'use strict';

// ── Constants ─────────────────────────────────────────────────────────────────
// Duty values from firmware are floats in range -1.0..+1.0
// (tank-mix already applied; sign = direction)
const DEADZONE     = 0.03;
const SEND_RATE_MS = 50;     // 20 Hz command rate
const RECONNECT_MS = 2000;
const PING_RATE_MS = 2000;
const KEY_RAMP_MS  = 180;    // ms from 0→1 while key held, 1→0 on release
const LED_PREFIX   = 'led';

// ── State ─────────────────────────────────────────────────────────────────────
let axisX      = 0;
let axisY      = 0;
let armed      = false;
let inputMode  = 'auto';   // 'touch' | 'pc' | 'auto'
let activeMode = 'touch';  // resolved this frame

const keyState = {
    w: { held: false, ramp: 0 },
    a: { held: false, ramp: 0 },
    s: { held: false, ramp: 0 },
    d: { held: false, ramp: 0 },
};
const KEY_AXIS = { w: [0, 1], a: [-1, 0], s: [0, -1], d: [1, 0] };

let pendingTelemetry = null;
let rafScheduled     = false;
let lastLedMag       = -1;
let pingSentAt       = 0;
let lastLatencyMs    = null;

// ── DOM refs ──────────────────────────────────────────────────────────────────
const dom = {
    shell:      document.getElementById('shell'),
    armBtn:     document.getElementById('arm-btn'),
    armLabel:   document.getElementById('arm-btn').querySelector('.arm-label'),
    connBadge:  document.getElementById('conn-badge'),
    connLabel:  document.getElementById('conn-badge').querySelector('.conn-label'),
    zone:       document.getElementById('joystick-zone'),
    knob:       document.getElementById('joystick-knob'),
    rssiVal:    document.getElementById('rssi-val'),
    rssiStat:   document.getElementById('rssi-stat'),
    battVal:    document.getElementById('batt-val'),
    battStat:   document.getElementById('batt-stat'),
    tempVal:    document.getElementById('temp-val'),
    latVal:     document.getElementById('latency-val'),
    modeBtn:    document.getElementById('mode-btn'),
    modeLabel:  document.getElementById('mode-label'),
    joySection: document.getElementById('joystick-section'),
    cards:  { left: document.getElementById('card-left'),   right: document.getElementById('card-right')  },
    bars:   { left: document.getElementById('bar-left'),    right: document.getElementById('bar-right')   },
    speeds: { left: document.getElementById('speed-left'),  right: document.getElementById('speed-right') },
    states: { left: document.getElementById('state-left'),  right: document.getElementById('state-right') },
    dirs:   { left: document.getElementById('dir-left'),    right: document.getElementById('dir-right')   },
    // Extended telemetry
    uptime: document.getElementById('uptime-val'),
    heap:   document.getElementById('heap-val'),
    cpu:    document.getElementById('cpu-val'),
    errors: document.getElementById('err-val'),
};

// ── Input mode ────────────────────────────────────────────────────────────────
function detectNaturalMode() { return window.matchMedia('(pointer: coarse)').matches ? 'touch' : 'pc'; }

function resolveMode()
{
    activeMode = (inputMode === 'auto') ? detectNaturalMode() : inputMode;
    applyMode();
}

function applyMode()
{
    dom.joySection.hidden = (activeMode !== 'touch');
    dom.modeLabel.textContent = (activeMode === 'touch') ? '⌨ PC' : '🕹 Touch';
    if (activeMode !== 'touch') centerJoystick();
}

dom.modeBtn.addEventListener('click', () =>
{
    inputMode = (activeMode === 'touch') ? 'pc' : 'touch';
    resolveMode();
});

window.addEventListener('touchstart', () =>
{
    if (inputMode === 'auto' && activeMode !== 'touch') { activeMode = 'touch'; applyMode(); }
}, { passive: true });

// ── Arm / disarm ──────────────────────────────────────────────────────────────
function setArmed(state)
{
    armed = state;
    dom.armBtn.className     = `arm-btn ${armed ? 'armed' : 'disarmed'}`;
    dom.armLabel.textContent = armed ? 'ARMED' : 'DISARMED';
    dom.zone.classList.toggle('disarmed', !armed);
    if (!armed) { centerJoystick(); resetKeyRamps(); lastLedMag = -1; }
}

dom.armBtn.addEventListener('click', () => setArmed(!armed));

// ── WebSocket ─────────────────────────────────────────────────────────────────
let socket         = null;
let reconnectTimer = null;
let pingTimer      = null;

function connectWS()
{
    if (socket) { socket.onclose = null; socket.close(); }
    socket = new WebSocket(`ws://${location.hostname}/ws`);

    socket.onopen = () =>
    {
        clearTimeout(reconnectTimer);
        setConnectionState(true);
        schedulePing();
    };

    socket.onclose = () =>
    {
        setConnectionState(false);
        centerJoystick();
        resetKeyRamps();
        clearTimeout(pingTimer);
        reconnectTimer = setTimeout(connectWS, RECONNECT_MS);
    };

    socket.onerror = () => socket.close();

    socket.onmessage = ({ data }) =>
    {
        if (data === 'pong')
        {
            lastLatencyMs = Date.now() - pingSentAt;
            dom.latVal.textContent = lastLatencyMs + ' ms';
            schedulePing();
            return;
        }
        try
        {
            const msg = JSON.parse(data);
            if (msg.left || msg.right) { pendingTelemetry = msg; scheduleRaf(); }
            if (msg.estop  !== undefined) updateEstop(msg.estop, msg.resume);
            if (msg.rssi    !== undefined) updateRssi(msg.rssi);
            if (msg.battery !== undefined) updateBattery(msg.battery);
            if (msg.temp    !== undefined) updateTemp(msg.temp);
            if (msg.uptime  !== undefined) updateUptime(msg.uptime);
            if (msg.heap    !== undefined) dom.heap.textContent   = (msg.heap / 1024).toFixed(1) + ' kB';
            if (msg.cpu     !== undefined) dom.cpu.textContent    = msg.cpu + '%';
            if (msg.errors  !== undefined) dom.errors.textContent = msg.errors;
        }
        catch (_) {}
    };
}

function schedulePing()
{
    clearTimeout(pingTimer);
    pingTimer = setTimeout(() =>
    {
        if (socket && socket.readyState === WebSocket.OPEN)
        {
            pingSentAt = Date.now();
            socket.send('ping');
        }
    }, PING_RATE_MS);
}

// ── Send loop (20 Hz) ─────────────────────────────────────────────────────────
let sendTimer = null;

function startSendLoop()
{
    clearInterval(sendTimer);
    sendTimer = setInterval(() =>
    {
        if (!socket || socket.readyState !== WebSocket.OPEN) return;
        socket.send((!armed || (axisX === 0 && axisY === 0))
            ? 'stop'
            : `x:${axisX.toFixed(3)},y:${axisY.toFixed(3)}`);
        sendLed();
    }, SEND_RATE_MS);
}

// ── Axis helpers ──────────────────────────────────────────────────────────────
function setAxes(rawX, rawY)
{
    axisX = Math.abs(rawX) < DEADZONE ? 0 : clamp(rawX, -1, 1);
    axisY = Math.abs(rawY) < DEADZONE ? 0 : clamp(rawY, -1, 1);
}

// ── Joystick ──────────────────────────────────────────────────────────────────
let zoneRect = null;
new ResizeObserver(() => { zoneRect = null; }).observe(dom.zone);
function getZoneRect() { if (!zoneRect) zoneRect = dom.zone.getBoundingClientRect(); return zoneRect; }

function pointerAxes(clientX, clientY)
{
    const r    = getZoneRect();
    const cx   = r.left + r.width  / 2;
    const cy   = r.top  + r.height / 2;
    let dx     = clientX - cx;
    let dy     = clientY - cy;
    const maxR = (r.width / 2) - (dom.knob.offsetWidth / 2) - 4;
    const dist = Math.hypot(dx, dy);
    if (dist > maxR) { const s = maxR / dist; dx *= s; dy *= s; }
    setAxes(dx / maxR, -dy / maxR);
    return { dx, dy };
}

function moveKnob(dx = 0, dy = 0)
{
    dom.knob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
}

function centerJoystick() { setAxes(0, 0); moveKnob(0, 0); }

let activePointerId = null;

dom.zone.addEventListener('pointerdown', e =>
{
    if (!armed || activePointerId !== null) return;
    activePointerId = e.pointerId;
    dom.zone.setPointerCapture(e.pointerId);
    const { dx, dy } = pointerAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

dom.zone.addEventListener('pointermove', e =>
{
    if (e.pointerId !== activePointerId) return;
    const { dx, dy } = pointerAxes(e.clientX, e.clientY);
    moveKnob(dx, dy);
});

const releasePointer = e =>
{
    if (e.pointerId !== activePointerId) return;
    activePointerId = null;
    centerJoystick();
};
dom.zone.addEventListener('pointerup',     releasePointer);
dom.zone.addEventListener('pointercancel', releasePointer);

// ── Keyboard (WASD) with smooth ramp ─────────────────────────────────────────
// Keys are listened to globally; no on-screen buttons needed.
// Each axis ramps 0→1 while held and decays 1→0 on release over KEY_RAMP_MS.

let lastRampTick = 0;
let rampRafId    = null;

function rampTick(now)
{
    const dt   = lastRampTick ? Math.min(now - lastRampTick, 32) : 16;
    lastRampTick = now;
    const step = dt / KEY_RAMP_MS;

    let anyActive = false;
    for (const k in keyState)
    {
        const ks = keyState[k];
        if (ks.held) { ks.ramp = Math.min(1, ks.ramp + step); anyActive = true; }
        else         { ks.ramp = Math.max(0, ks.ramp - step); if (ks.ramp > 0) anyActive = true; }
    }

    let x = 0, y = 0;
    for (const k in keyState)
    {
        const r = keyState[k].ramp;
        if (r > 0) { x += KEY_AXIS[k][0] * r; y += KEY_AXIS[k][1] * r; }
    }
    const mag = Math.hypot(x, y);
    if (mag > 1) { x /= mag; y /= mag; }
    setAxes(x, y);

    rampRafId = anyActive ? requestAnimationFrame(rampTick) : null;
}

function ensureRampRunning()
{
    if (!rampRafId) { lastRampTick = 0; rampRafId = requestAnimationFrame(rampTick); }
}

function resetKeyRamps()
{
    for (const k in keyState) { keyState[k].held = false; keyState[k].ramp = 0; }
    setAxes(0, 0);
}

document.addEventListener('keydown', e =>
{
    const k = e.key.toLowerCase();
    if (!keyState[k] || keyState[k].held || !armed) return;
    keyState[k].held = true;
    ensureRampRunning();
});

document.addEventListener('keyup', e =>
{
    const k = e.key.toLowerCase();
    if (!keyState[k]) return;
    keyState[k].held = false;
    ensureRampRunning();
});

// ── Telemetry rendering (batched via rAF) ─────────────────────────────────────
function scheduleRaf()
{
    if (rafScheduled) return;
    rafScheduled = true;
    requestAnimationFrame(() =>
    {
        rafScheduled = false;
        if (!pendingTelemetry) return;
        const msg = pendingTelemetry;
        pendingTelemetry = null;
        if (msg.left)  updateTrackCard('left',  msg.left);
        if (msg.right) updateTrackCard('right', msg.right);
    });
}

function updateTrackCard(side, data)
{
    // data.duty is float -1..+1; bar shows absolute magnitude
    const absD = Math.abs(data.duty || 0);
    const pct  = Math.round(absD * 100);
    dom.speeds[side].textContent    = pct + '%';
    dom.dirs[side].textContent      = data.forward ? '▲ FWD' : '▼ REV';
    dom.bars[side].style.width      = pct + '%';
    // Derive a display state from duty magnitude
    const state = absD > 0.01 ? 'RUNNING' : 'STOPPED';
    dom.states[side].textContent    = state;
    dom.bars[side].style.background = stateColor(state);
    dom.cards[side].dataset.state   = state;
}

function stateColor(state)
{
    if (state === 'RUNNING') return 'var(--clr-running)';
    if (state === 'RAMP_UP') return 'var(--clr-ramping)';
    return 'var(--clr-stopped)';
}

// ── E-stop / resume state ────────────────────────────────────────────────────
function updateEstop(estop, resume)
{
    // Visual indicator: if estopped, flash the arm button red regardless of armed state
    if (estop || resume)
    {
        dom.armBtn.classList.add('estopped');
        dom.armLabel.textContent = resume ? 'SEND ZERO' : 'E-STOP';
    }
    else
    {
        dom.armBtn.classList.remove('estopped');
        dom.armLabel.textContent = armed ? 'ARMED' : 'DISARMED';
    }
}

// ── System health ─────────────────────────────────────────────────────────────
function updateRssi(rssi)
{
    const bars = rssi > -50 ? '▂▄▆█' : rssi > -65 ? '▂▄▆·' : rssi > -75 ? '▂▄··' : '▂···';
    dom.rssiVal.textContent = `${bars} ${rssi} dBm`;
    dom.rssiStat.classList.toggle('warn', rssi < -75);
}

function updateBattery(pct)
{
    dom.battVal.textContent = pct + '%';
    dom.battStat.classList.toggle('warn', pct < 20);
}

function updateTemp(celsius)
{
    dom.tempVal.textContent = celsius.toFixed(1) + ' °C';
}

function updateUptime(seconds)
{
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    dom.uptime.textContent = h > 0
        ? `${h}h ${m}m`
        : m > 0 ? `${m}m ${s}s` : `${s}s`;
}

// ── Connection state ──────────────────────────────────────────────────────────
function setConnectionState(connected)
{
    dom.connBadge.className   = `conn-badge ${connected ? 'connected' : 'disconnected'}`;
    dom.connLabel.textContent = connected ? 'Connected' : 'Disconnected';
    if (!connected)
    {
        dom.latVal.textContent  = '—';
        dom.rssiVal.textContent = '—';
        dom.battVal.textContent = '—';
        dom.tempVal.textContent = '—';
        dom.rssiStat.classList.remove('warn');
        dom.battStat.classList.remove('warn');
        lastLedMag = -1;
    }
}

// ── LED magnitude ─────────────────────────────────────────────────────────────
// Sends "led:<mag>" (0.00–1.00) when joystick magnitude changes.
// Firmware writes value to ledc_set_duty() on LED GPIO.
function sendLed()
{
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    const mag     = armed ? Math.min(1, Math.hypot(axisX, axisY)) : 0;
    const rounded = Math.round(mag * 100) / 100;
    if (rounded !== lastLedMag)
    {
        lastLedMag = rounded;
        socket.send(`${LED_PREFIX}:${rounded.toFixed(2)}`);
    }
}

// ── Utility ───────────────────────────────────────────────────────────────────
function clamp(v, min, max) { return Math.max(min, Math.min(max, v)); }

// ── Init ──────────────────────────────────────────────────────────────────────
resolveMode();
connectWS();
startSendLoop();

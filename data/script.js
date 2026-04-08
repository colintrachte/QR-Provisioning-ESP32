'use strict';

// ── Constants ─────────────────────────────────────────────────────────────────

const MAX_PWM       = 1023;
const DEADZONE      = 0.03;
const SEND_RATE_MS  = 50;     // command send interval (20 Hz)
const RECONNECT_MS  = 2000;
const PING_RATE_MS  = 2000;   // latency probe interval

// Keyboard ramp: axis value ramps from 0 → 1 over this many ms when a key
// is held, and decays back to 0 over the same duration when released.
const KEY_RAMP_MS   = 180;

// ── State ─────────────────────────────────────────────────────────────────────

let axisX      = 0;
let axisY      = 0;
let armed      = false;
let inputMode  = 'auto';   // 'touch' | 'pc' | 'auto'
let activeMode = 'touch';  // resolved mode used this frame

// Keyboard ramp state per key
const keyState = {
  w: { held: false, ramp: 0 },
  a: { held: false, ramp: 0 },
  s: { held: false, ramp: 0 },
  d: { held: false, ramp: 0 },
};
const KEY_AXIS = { w: [0, 1], a: [-1, 0], s: [0, -1], d: [1, 0] };

// Telemetry batching
let pendingTelemetry = null;
let rafScheduled     = false;

// Latency
let pingSentAt    = 0;
let lastLatencyMs = null;

// ── DOM refs ──────────────────────────────────────────────────────────────────

const dom = {
  shell:     document.getElementById('shell'),
  armBtn:    document.getElementById('arm-btn'),
  armLabel:  document.getElementById('arm-btn').querySelector('.arm-label'),
  connBadge: document.getElementById('conn-badge'),
  connLabel: document.getElementById('conn-badge').querySelector('.conn-label'),
  zone:      document.getElementById('joystick-zone'),
  knob:      document.getElementById('joystick-knob'),
  rssiVal:   document.getElementById('rssi-val'),
  rssiStat:  document.getElementById('rssi-stat'),
  battVal:   document.getElementById('batt-val'),
  battStat:  document.getElementById('batt-stat'),
  latVal:    document.getElementById('latency-val'),
  modeBtn:   document.getElementById('mode-btn'),
  modeLabel: document.getElementById('mode-label'),
  joySection: document.getElementById('joystick-section'),
  kbSection:  document.getElementById('keyboard-section'),
  cards:  { left: document.getElementById('card-left'),   right: document.getElementById('card-right')  },
  bars:   { left: document.getElementById('bar-left'),    right: document.getElementById('bar-right')   },
  speeds: { left: document.getElementById('speed-left'),  right: document.getElementById('speed-right') },
  states: { left: document.getElementById('state-left'),  right: document.getElementById('state-right') },
  dirs:   { left: document.getElementById('dir-left'),    right: document.getElementById('dir-right')   },
};

// ── Input mode detection ──────────────────────────────────────────────────────

function detectNaturalMode() {
  // On first touchstart, we know it's a touchscreen.
  // On first mousemove without touch, it's a PC.
  // matchMedia coarse pointer is the fastest heuristic.
  return window.matchMedia('(pointer: coarse)').matches ? 'touch' : 'pc';
}

function resolveMode() {
  activeMode = (inputMode === 'auto') ? detectNaturalMode() : inputMode;
  applyMode();
}

function applyMode() {
  if (activeMode === 'touch') {
    dom.joySection.hidden = false;
    dom.kbSection.hidden  = true;
    dom.modeLabel.textContent = '⌨ PC';
  } else {
    dom.joySection.hidden = true;
    dom.kbSection.hidden  = false;
    dom.modeLabel.textContent = '🕹 Touch';
    centerJoystick(); // stop any joystick motion when switching away
  }
}

dom.modeBtn.addEventListener('click', () => {
  // Toggle between pc and touch, locking out 'auto'
  inputMode  = (activeMode === 'touch') ? 'pc' : 'touch';
  resolveMode();
});

// Switch to touch mode the first time a touch event fires (auto mode only)
window.addEventListener('touchstart', () => {
  if (inputMode === 'auto' && activeMode !== 'touch') {
    activeMode = 'touch';
    applyMode();
  }
}, { passive: true, once: false });

// ── Arm / disarm ──────────────────────────────────────────────────────────────

function setArmed(state) {
  armed = state;
  dom.armBtn.className  = `arm-btn ${armed ? 'armed' : 'disarmed'}`;
  dom.armLabel.textContent = armed ? 'ARMED' : 'DISARMED';
  dom.zone.classList.toggle('disarmed', !armed);
  document.querySelectorAll('.kbd-btn').forEach(b =>
    b.classList.toggle('disarmed', !armed));

  if (!armed) {
    centerJoystick();
    resetKeyRamps();
  }
}

dom.armBtn.addEventListener('click', () => setArmed(!armed));

// ── WebSocket ─────────────────────────────────────────────────────────────────

let socket         = null;
let reconnectTimer = null;
let pingTimer      = null;

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
    centerJoystick();
    resetKeyRamps();
    clearTimeout(pingTimer);
    reconnectTimer = setTimeout(connectWS, RECONNECT_MS);
  };

  socket.onerror = () => socket.close();

  socket.onmessage = ({ data }) => {
    // Pong detection: single-word "pong" message
    if (data === 'pong') {
      lastLatencyMs = Date.now() - pingSentAt;
      dom.latVal.textContent = lastLatencyMs + ' ms';
      schedulePing();
      return;
    }
    try {
      const msg = JSON.parse(data);
      // Robot telemetry
      if (msg.left || msg.right) {
        pendingTelemetry = msg;
        scheduleRaf();
      }
      // System health fields (rssi, battery) sent alongside or separately
      if (msg.rssi !== undefined) updateRssi(msg.rssi);
      if (msg.battery !== undefined) updateBattery(msg.battery);
    } catch (_) {}
  };
}

function sendCommand(msg) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(msg);
}

function schedulePing() {
  clearTimeout(pingTimer);
  pingTimer = setTimeout(() => {
    if (socket && socket.readyState === WebSocket.OPEN) {
      pingSentAt = Date.now();
      socket.send('ping');
    }
  }, PING_RATE_MS);
}

// ── Control send loop ─────────────────────────────────────────────────────────
// Commands are sent at a fixed 20 Hz rate regardless of input events.
// When disarmed, always sends 'stop'.

let sendTimer = null;

function startSendLoop() {
  clearInterval(sendTimer);
  sendTimer = setInterval(() => {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    if (!armed || (axisX === 0 && axisY === 0)) {
      socket.send('stop');
    } else {
      socket.send(`x:${axisX.toFixed(3)},y:${axisY.toFixed(3)}`);
    }
  }, SEND_RATE_MS);
}

// ── Axis helpers ──────────────────────────────────────────────────────────────

function setAxes(rawX, rawY) {
  axisX = Math.abs(rawX) < DEADZONE ? 0 : clamp(rawX, -1, 1);
  axisY = Math.abs(rawY) < DEADZONE ? 0 : clamp(rawY, -1, 1);
}

// ── Joystick ──────────────────────────────────────────────────────────────────

let zoneRect = null;
new ResizeObserver(() => { zoneRect = null; }).observe(dom.zone);

function getZoneRect() {
  if (!zoneRect) zoneRect = dom.zone.getBoundingClientRect();
  return zoneRect;
}

function pointerAxes(clientX, clientY) {
  const r   = getZoneRect();
  const cx  = r.left + r.width  / 2;
  const cy  = r.top  + r.height / 2;
  let dx    = clientX - cx;
  let dy    = clientY - cy;
  const maxR = (r.width / 2) - (dom.knob.offsetWidth / 2) - 4;
  const dist = Math.hypot(dx, dy);
  if (dist > maxR) { const s = maxR / dist; dx *= s; dy *= s; }
  setAxes(dx / maxR, -dy / maxR);
  return { dx, dy };
}

function moveKnob(dx = 0, dy = 0) {
  dom.knob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
}

function centerJoystick() {
  setAxes(0, 0);
  moveKnob(0, 0);
}

let activePointerId = null;

dom.zone.addEventListener('pointerdown', e => {
  if (!armed || activePointerId !== null) return;
  activePointerId = e.pointerId;
  dom.zone.setPointerCapture(e.pointerId);
  const { dx, dy } = pointerAxes(e.clientX, e.clientY);
  moveKnob(dx, dy);
});

dom.zone.addEventListener('pointermove', e => {
  if (e.pointerId !== activePointerId) return;
  const { dx, dy } = pointerAxes(e.clientX, e.clientY);
  moveKnob(dx, dy);
});

const releasePointer = e => {
  if (e.pointerId !== activePointerId) return;
  activePointerId = null;
  centerJoystick();
};
dom.zone.addEventListener('pointerup',     releasePointer);
dom.zone.addEventListener('pointercancel', releasePointer);

// ── Keyboard with smooth ramp ─────────────────────────────────────────────────
// Each key ramp value rises from 0 → 1 while held and falls 1 → 0 on release.
// This prevents the jarring snap between stopped and full speed on a PC.

let lastRampTick = 0;
let rampRafId    = null;

function rampTick(now) {
  const dt = lastRampTick ? Math.min(now - lastRampTick, 32) : 16;
  lastRampTick = now;
  const step = dt / KEY_RAMP_MS;

  let anyActive = false;
  for (const k in keyState) {
    const ks = keyState[k];
    if (ks.held) {
      ks.ramp = Math.min(1, ks.ramp + step);
      anyActive = true;
    } else {
      ks.ramp = Math.max(0, ks.ramp - step);
      if (ks.ramp > 0) anyActive = true;
    }
  }

  // Compose axis vector from ramped key values
  let x = 0, y = 0;
  for (const k in keyState) {
    const r = keyState[k].ramp;
    if (r > 0) {
      x += KEY_AXIS[k][0] * r;
      y += KEY_AXIS[k][1] * r;
    }
  }
  // Normalise diagonal
  const mag = Math.hypot(x, y);
  if (mag > 1) { x /= mag; y /= mag; }
  setAxes(x, y);

  rampRafId = anyActive ? requestAnimationFrame(rampTick) : null;
}

function ensureRampRunning() {
  if (!rampRafId) {
    lastRampTick = 0;
    rampRafId = requestAnimationFrame(rampTick);
  }
}

function resetKeyRamps() {
  for (const k in keyState) { keyState[k].held = false; keyState[k].ramp = 0; }
  setAxes(0, 0);
  // highlight off
  document.querySelectorAll('.kbd-btn').forEach(b => b.classList.remove('held'));
}

document.addEventListener('keydown', e => {
  const k = e.key.toLowerCase();
  if (!keyState[k] || keyState[k].held) return;
  if (!armed) return;
  keyState[k].held = true;
  const btn = document.querySelector(`.kbd-btn[data-key="${k}"]`);
  if (btn) btn.classList.add('held');
  ensureRampRunning();
});

document.addEventListener('keyup', e => {
  const k = e.key.toLowerCase();
  if (!keyState[k]) return;
  keyState[k].held = false;
  const btn = document.querySelector(`.kbd-btn[data-key="${k}"]`);
  if (btn) btn.classList.remove('held');
  ensureRampRunning();
});

// On-screen keyboard buttons: touch events so they work in touch mode too
document.querySelectorAll('.kbd-btn').forEach(btn => {
  const k = btn.dataset.key;
  btn.addEventListener('pointerdown', e => {
    if (!armed) return;
    e.preventDefault();
    btn.setPointerCapture(e.pointerId);
    keyState[k].held = true;
    btn.classList.add('held');
    ensureRampRunning();
  });
  btn.addEventListener('pointerup',     () => { keyState[k].held = false; btn.classList.remove('held'); ensureRampRunning(); });
  btn.addEventListener('pointercancel', () => { keyState[k].held = false; btn.classList.remove('held'); ensureRampRunning(); });
});

// ── Telemetry rendering (batched via rAF) ─────────────────────────────────────

function scheduleRaf() {
  if (rafScheduled) return;
  rafScheduled = true;
  requestAnimationFrame(() => {
    rafScheduled = false;
    if (!pendingTelemetry) return;
    const msg = pendingTelemetry;
    pendingTelemetry = null;
    if (msg.left)  updateTrackCard('left',  msg.left);
    if (msg.right) updateTrackCard('right', msg.right);
  });
}

function updateTrackCard(side, data) {
  const pct = Math.round((data.speed / MAX_PWM) * 100);
  dom.speeds[side].textContent = data.speed;
  dom.states[side].textContent = data.state;
  dom.dirs[side].textContent   = data.forward ? '▲ FWD' : '▼ REV';
  dom.bars[side].style.width      = pct + '%';
  dom.bars[side].style.background = stateColor(data.state);
  dom.cards[side].dataset.state   = data.state;
}

function stateColor(state) {
  if (state === 'RUNNING') return 'var(--clr-running)';
  if (state === 'RAMP_UP') return 'var(--clr-ramping)';
  return 'var(--clr-stopped)';
}

// ── System health display ─────────────────────────────────────────────────────

function updateRssi(rssi) {
  const bars = rssi > -50 ? '▂▄▆█' : rssi > -65 ? '▂▄▆·' : rssi > -75 ? '▂▄··' : '▂···';
  dom.rssiVal.textContent = `${bars} ${rssi} dBm`;
  dom.rssiStat.classList.toggle('warn', rssi < -75);
}

function updateBattery(pct) {
  dom.battVal.textContent = pct + '%';
  dom.battStat.classList.toggle('warn', pct < 20);
}

// ── Connection state ──────────────────────────────────────────────────────────

function setConnectionState(connected) {
  dom.connBadge.className  = `conn-badge ${connected ? 'connected' : 'disconnected'}`;
  dom.connLabel.textContent = connected ? 'Connected' : 'Disconnected';
  if (!connected) {
    dom.latVal.textContent  = '—';
    dom.rssiVal.textContent = '—';
    dom.battVal.textContent = '—';
    dom.rssiStat.classList.remove('warn');
    dom.battStat.classList.remove('warn');
  }
}

// ── Utility ───────────────────────────────────────────────────────────────────

function clamp(v, min, max) { return Math.max(min, Math.min(max, v)); }

// ── Init ──────────────────────────────────────────────────────────────────────

resolveMode();
connectWS();
startSendLoop();

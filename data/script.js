'use strict';

// ── Constants ─────────────────────────────────────────────────────────────────

const MAX_PWM        = 1023;
const DEADZONE       = 0.03;  
const SEND_INTERVAL  = 50;    
const RECONNECT_MS   = 2000;  

// ── DOM refs ──────────────────────────────────────────────────────────────────

const dom = 
{
  connBadge:   document.getElementById('conn-badge'),
  connLabel:   document.getElementById('conn-badge').querySelector('.conn-label'),
  zone:        document.getElementById('joystick-zone'),
  knob:        document.getElementById('joystick-knob'),
  cards: 
  {
    left:  document.getElementById('card-left'),
    right: document.getElementById('card-right'),
  },
  bars: 
  {
    left:  document.getElementById('bar-left'),
    right: document.getElementById('bar-right'),
  },
  speeds: 
  {
    left:  document.getElementById('speed-left'),
    right: document.getElementById('speed-right'),
  },
  states: 
  {
    left:  document.getElementById('state-left'),
    right: document.getElementById('state-right'),
  },
  dirs: 
  {
    left:  document.getElementById('dir-left'),
    right: document.getElementById('dir-right'),
  },
};

// ── WebSocket ─────────────────────────────────────────────────────────────────

let socket          = null;
let reconnectTimer  = null;

function connectWS() 
{
  // Safety: close existing if trying to reconnect
  if (socket) 
  {
    socket.onclose = null;
    socket.close();
  }

  socket = new WebSocket(`ws://${location.hostname}/ws`);

  socket.onopen = () => 
  {
    console.log("Connected to Tank");
    clearTimeout(reconnectTimer);
    setConnectionState(true);
  };

  socket.onclose = () => 
  {
    setConnectionState(false);
    centerJoystick(); 
    reconnectTimer = setTimeout(connectWS, RECONNECT_MS);
  };

  socket.onerror = (err) => 
  {
    console.error("WS Error", err);
    socket.close();
  };

  socket.onmessage = ({ data }) => 
  {
    try 
    {
      const telemetry = JSON.parse(data);
      if (telemetry.left)  updateTrackCard('left',  telemetry.left);
      if (telemetry.right) updateTrackCard('right', telemetry.right);
    } 
    catch (e) 
    { 
      /* Silent ignore malformed frames */ 
    }
  };
}

function sendCommand(msg) 
{
  if (socket && socket.readyState === WebSocket.OPEN) 
  {
    socket.send(msg);
  }
}

// ── Control Logic ─────────────────────────────────────────────────────────────

let axisX = 0;
let axisY = 0;
const heldKeys = new Set();
let sendScheduled = false;

function scheduleSend() 
{
  if (sendScheduled) return;
  sendScheduled = true;

  setTimeout(() => 
  {
    sendScheduled = false;
    const msg = (axisX === 0 && axisY === 0) 
      ? 'stop' 
      : `x:${axisX.toFixed(3)},y:${axisY.toFixed(3)}`;
    sendCommand(msg);
  }, SEND_INTERVAL);
}

function setAxes(rawX, rawY) 
{
  axisX = Math.abs(rawX) < DEADZONE ? 0 : clamp(rawX, -1, 1);
  axisY = Math.abs(rawY) < DEADZONE ? 0 : clamp(rawY, -1, 1);
  scheduleSend();
}

// ── Joystick Handling ─────────────────────────────────────────────────────────

let zoneRect = null;
const resizeObserver = new ResizeObserver(() => { zoneRect = null; });
resizeObserver.observe(dom.zone);

function getZoneRect() 
{
  if (!zoneRect) zoneRect = dom.zone.getBoundingClientRect();
  return zoneRect;
}

function pointerAxes(clientX, clientY) 
{
  const r    = getZoneRect();
  const cx   = r.left + r.width  / 2;
  const cy   = r.top  + r.height / 2;
  let   dx   = clientX - cx;
  let   dy   = clientY - cy;
  
  const maxR = (r.width / 2) - (dom.knob.offsetWidth / 2) - 4;
  const dist = Math.hypot(dx, dy);

  if (dist > maxR) 
  { 
    const s = maxR / dist; 
    dx *= s; 
    dy *= s; 
  }

  setAxes(dx / maxR, -dy / maxR);
  return { dx, dy };
}

function moveKnob(dx = 0, dy = 0) 
{
  dom.knob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
}

function centerJoystick() 
{
  setAxes(0, 0);
  moveKnob(0, 0);
}

let activePointerId = null;

dom.zone.addEventListener('pointerdown', e => 
{
  if (activePointerId !== null) return;
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

dom.zone.addEventListener('pointerup', releasePointer);
dom.zone.addEventListener('pointercancel', releasePointer);

// ── Keyboard Handling ─────────────────────────────────────────────────────────

const KEY_MAP = { w: [0, 1], a: [-1, 0], s: [0, -1], d: [1, 0] };

document.addEventListener('keydown', e => 
{
  const k = e.key.toLowerCase();
  if (KEY_MAP[k] && !heldKeys.has(k)) 
  {
    heldKeys.add(k);
    recomputeKeyAxes();
  }
});

document.addEventListener('keyup', e => 
{
  const k = e.key.toLowerCase();
  if (heldKeys.has(k)) 
  {
    heldKeys.delete(k);
    recomputeKeyAxes();
  }
});

function recomputeKeyAxes() 
{
  let x = 0, y = 0;
  heldKeys.forEach(k => 
  {
    x += KEY_MAP[k][0];
    y += KEY_MAP[k][1];
  });

  const mag = Math.hypot(x, y);
  if (mag > 1) 
  { 
    x /= mag; 
    y /= mag; 
  }
  setAxes(x, y);
}

// ── UI Updates ───────────────────────────────────────────────────────────────

function updateTrackCard(side, data) 
{
  const pct = Math.round((data.speed / MAX_PWM) * 100);

  dom.speeds[side].textContent = data.speed;
  dom.states[side].textContent = data.state;
  dom.dirs[side].textContent   = data.forward ? '▲ FWD' : '▼ REV';

  dom.bars[side].style.width      = pct + '%';
  dom.bars[side].style.background = getStatusColor(data.state);
  dom.cards[side].dataset.state   = data.state;
}

function getStatusColor(state) 
{
  if (state === 'RUNNING') return 'var(--clr-running)';
  if (state === 'RAMP_UP') return 'var(--clr-ramping)';
  return 'var(--clr-stopped)';
}

function setConnectionState(isConnected) 
{
  const stateClass = isConnected ? 'connected' : 'disconnected';
  dom.connBadge.className = `conn-badge ${stateClass}`;
  dom.connLabel.textContent = isConnected ? 'Connected' : 'Disconnected';
}

function clamp(v, min, max) { return Math.max(min, Math.min(max, v)); }

// ── Init ──────────────────────────────────────────────────────────────────────

connectWS();
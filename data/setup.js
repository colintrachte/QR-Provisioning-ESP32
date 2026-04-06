'use strict';

/* =============================================================================
 * setup.js — Client logic for the WiFi provisioning portal.
 *
 * Served from LittleFS at /setup.js.
 *
 * Flow:
 *  1. Page load  → startScan() polls GET /scan until the ESP32 returns a
 *                  JSON array of nearby APs (see scan_task in wifi_manager.c).
 *  2. User picks a network (or types one) and enters a password.
 *  3. Submit     → POST /save with ssid= & pass= form-encoded body.
 *  4. The ESP32 saves credentials to NVS and reboots (~1.2 s later).
 *  5. The AP disappears; fetch() .catch() fires → success message shown.
 *
 * No /connect-status polling is needed: the reboot IS the connection attempt.
 * ============================================================================= */

// ── Helpers ───────────────────────────────────────────────────────────────────

function escHtml(s) {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

/**
 * Return a 4-character signal-strength glyph string from a 0-100 quality value.
 * Uses Unicode block-element characters (▂▄▆█ / ·).
 */
function signalBars(quality) {
  if (quality > 80) return '\u2582\u2584\u2586\u2588'; // ▂▄▆█
  if (quality > 60) return '\u2582\u2584\u2586\xb7';   // ▂▄▆·
  if (quality > 40) return '\u2582\u2584\xb7\xb7';     // ▂▄··
  return '\u2582\xb7\xb7\xb7';                          // ▂···
}

// ── Network list ──────────────────────────────────────────────────────────────

const listEl    = document.getElementById('network-list');
const ssidInput = document.getElementById('ssid');
const passInput = document.getElementById('pass');

/**
 * Render a sorted list of AP buttons from the /scan JSON response.
 * @param {Array<{ssid: string, quality: number, secure: boolean}>} networks
 */
function renderNetworks(networks) {
  listEl.innerHTML = '';

  if (!networks || networks.length === 0) {
    listEl.innerHTML =
      '<p class="scan-status">No networks found. ' +
      '<button class="rescan-btn" id="rescan-empty">Rescan</button></p>';
    document.getElementById('rescan-empty').addEventListener('click', startScan);
    return;
  }

  networks.sort((a, b) => b.quality - a.quality);

  networks.forEach(net => {
    const btn = document.createElement('button');
    btn.type      = 'button';
    btn.className = 'network-btn';
    btn.innerHTML =
      '<span class="network-name">' + escHtml(net.ssid) + '</span>' +
      '<span class="network-meta">' +
        '<span class="signal">' + signalBars(net.quality) + ' ' + net.quality + '%</span>' +
        (net.secure ? '<span>&#128274;</span>' : '') +
      '</span>';

    btn.addEventListener('click', () => {
      listEl.querySelectorAll('.network-btn').forEach(b => b.classList.remove('selected'));
      btn.classList.add('selected');
      ssidInput.value = net.ssid;
      if (net.secure) passInput.focus();
      else passInput.value = '';
    });

    listEl.appendChild(btn);
  });
}

// ── Scan polling ──────────────────────────────────────────────────────────────

let scanTimer = null;

/**
 * Poll GET /scan repeatedly until the server returns a JSON array.
 * The ESP32's scan_task runs a blocking WiFi scan on a dedicated FreeRTOS
 * task; until the first scan completes the server returns {"status":"scanning"}.
 */
function pollScan() {
  fetch('/scan')
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(data => {
      if (Array.isArray(data)) {
        renderNetworks(data);
      } else {
        // Still scanning — try again shortly.
        scanTimer = setTimeout(pollScan, 1500);
      }
    })
    .catch(() => {
      // Network error (e.g. AP not up yet) — back off and retry.
      scanTimer = setTimeout(pollScan, 3000);
    });
}

function startScan() {
  listEl.innerHTML = '<p class="scan-status">Scanning&hellip;</p>';
  clearTimeout(scanTimer);
  pollScan();
}

document.getElementById('rescan-btn').addEventListener('click', startScan);

// ── Password visibility toggle ────────────────────────────────────────────────

document.querySelector('.toggle-pass').addEventListener('click', () => {
  passInput.type = passInput.type === 'password' ? 'text' : 'password';
});

// ── Save / submit flow ────────────────────────────────────────────────────────

const statusEl  = document.getElementById('status');
const submitBtn = document.getElementById('submit-btn');

/**
 * POST credentials to /save.
 *
 * On success the ESP32 reboots (~1.2 s) and the AP disappears, so the
 * fetch() promise will either:
 *   a) resolve with HTTP 200 (fast path: response arrived before reboot), or
 *   b) reject with a network error (AP shut down before response flushed).
 *
 * Both outcomes mean the credentials were saved — we show a success message
 * in both the .then() and .catch() branches.
 */
document.getElementById('setup-form').addEventListener('submit', e => {
  e.preventDefault();

  const ssid = ssidInput.value.trim();
  if (!ssid) return;

  submitBtn.disabled    = true;
  submitBtn.textContent = 'Saving...';
  statusEl.textContent  = 'Sending credentials\u2026';
  statusEl.className    = 'status';

  const body =
    'ssid=' + encodeURIComponent(ssid) +
    '&pass=' + encodeURIComponent(passInput.value);

  fetch('/save', {
    method:  'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body,
  })
    .then(response => {
      if (response.ok) {
        showSaveSuccess();
      } else {
        showSaveError('Server returned ' + response.status);
      }
    })
    .catch(() => {
      // Network error = device rebooted and AP is gone = expected success path.
      showSaveSuccess();
    });
});

function showSaveSuccess() {
  statusEl.textContent = 'Saved! Device is joining your network\u2026';
  statusEl.className   = 'status ok';
}

function showSaveError(msg) {
  statusEl.textContent = msg || 'Error saving credentials.';
  statusEl.className   = 'status error';
  submitBtn.disabled    = false;
  submitBtn.textContent = 'Save & Connect';
}

// ── Boot ──────────────────────────────────────────────────────────────────────

startScan();

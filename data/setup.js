'use strict';

/* =============================================================================
 * setup.js — WiFi provisioning portal client.
 *
 * Flow:
 *  1. Page load  → startScan() polls GET /scan until the ESP32 returns a
 *                  JSON array of nearby APs.
 *  2. User picks (or types) a network and enters a password.
 *  3. Submit     → POST /save with ssid= & pass= form-encoded body.
 *  4. ESP32 saves to NVS and reboots; AP disappears → success message shown.
 *
 * Skip link: navigates directly to http://<host>/ so the developer can reach
 * the robot control page without completing WiFi setup (e.g. when already
 * connected via a different route, or for local testing).
 * ============================================================================= */

// ── Helpers ───────────────────────────────────────────────────────────────────
function escHtml(s)
{
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;')
            .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// Returns a 4-char signal-strength glyph string from a 0-100 quality value.
function signalBars(quality)
{
    if (quality > 80) return '\u2582\u2584\u2586\u2588';
    if (quality > 60) return '\u2582\u2584\u2586\xb7';
    if (quality > 40) return '\u2582\u2584\xb7\xb7';
    return '\u2582\xb7\xb7\xb7';
}

// ── Network list ──────────────────────────────────────────────────────────────
const listEl    = document.getElementById('network-list');
const ssidInput = document.getElementById('ssid');
const passInput = document.getElementById('pass');

function renderNetworks(networks)
{
    listEl.innerHTML = '';

    if (!networks || networks.length === 0)
    {
        listEl.innerHTML =
            '<p class="scan-status">No networks found. ' +
            '<button class="rescan-btn" id="rescan-empty">Rescan</button></p>';
        document.getElementById('rescan-empty').addEventListener('click', startScan);
        return;
    }

    networks.sort((a, b) => b.quality - a.quality);

    networks.forEach(net =>
    {
        const btn = document.createElement('button');
        btn.type      = 'button';
        btn.className = 'network-btn';
        btn.innerHTML =
            '<span class="network-name">' + escHtml(net.ssid) + '</span>' +
            '<span class="network-meta">' +
                '<span class="signal">' + signalBars(net.quality) + ' ' + net.quality + '%</span>' +
                (net.secure ? '<span>&#128274;</span>' : '') +
            '</span>';

        btn.addEventListener('click', () =>
        {
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
// Polls GET /scan until the server returns a JSON array (not {"status":"scanning"}).
// Back-off: 1.5 s while scanning, 3 s on network error.

let scanTimer = null;

function pollScan()
{
    fetch('/scan')
        .then(r => r.ok ? r.json() : Promise.reject(r.status))
        .then(data =>
        {
            if (Array.isArray(data)) renderNetworks(data);
            else scanTimer = setTimeout(pollScan, 1500);
        })
        .catch(() => { scanTimer = setTimeout(pollScan, 3000); });
}

function startScan()
{
    listEl.innerHTML = '<p class="scan-status">Scanning&hellip;</p>';
    clearTimeout(scanTimer);
    pollScan();
}

document.getElementById('rescan-btn').addEventListener('click', startScan);

// ── Password visibility toggle ────────────────────────────────────────────────
document.querySelector('.toggle-pass').addEventListener('click', () =>
{
    passInput.type = passInput.type === 'password' ? 'text' : 'password';
});

// ── Save / submit ─────────────────────────────────────────────────────────────
// On success the ESP32 reboots and the AP disappears, so fetch() will either
// resolve with HTTP 200 (fast path) or reject with a network error (reboot
// happened first). Both outcomes mean saved — show success in both branches.

const statusEl  = document.getElementById('status');
const submitBtn = document.getElementById('submit-btn');

document.getElementById('setup-form').addEventListener('submit', e =>
{
    e.preventDefault();
    const ssid = ssidInput.value.trim();
    if (!ssid) return;

    submitBtn.disabled    = true;
    submitBtn.textContent = 'Saving...';
    statusEl.textContent  = 'Sending credentials\u2026';
    statusEl.className    = 'status';

    const body = 'ssid=' + encodeURIComponent(ssid) +
                 '&pass=' + encodeURIComponent(passInput.value);

    fetch('/save', {
        method:  'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body,
    })
    .then(r => { if (r.ok) showSaveSuccess(); else showSaveError('Server returned ' + r.status); })
    .catch(() => showSaveSuccess());  /* network gone = device rebooted = success */
});

function showSaveSuccess()
{
    statusEl.textContent = 'Saved! Device is joining your network\u2026';
    statusEl.className   = 'status ok';
}

function showSaveError(msg)
{
    statusEl.textContent  = msg || 'Error saving credentials.';
    statusEl.className    = 'status error';
    submitBtn.disabled    = false;
    submitBtn.textContent = 'Save & Connect';
}

// ── Skip / direct access ──────────────────────────────────────────────────────
// Navigates to the robot control page without completing WiFi setup.
// Useful when the device is already reachable on the network, or for local
// testing where the developer is bypassing provisioning entirely.
document.getElementById('skip-btn').addEventListener('click', e =>
{
    e.preventDefault();
    window.location.href = 'http://' + location.hostname + '/';
});

// ── Boot ──────────────────────────────────────────────────────────────────────
startScan();

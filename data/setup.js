/**
 * setup.js — Captive portal WiFi setup page.
 *
 * API:
 *   GET  /api/scan    → cached AP list JSON (immediate)
 *   GET  /api/rescan  → blocking scan, then JSON (~3 s)
 *   POST /api/connect → body: ssid=<enc>&password=<enc>
 */

const API_SCAN    = '/api/scan';
const API_RESCAN  = '/api/rescan';
const API_CONNECT = '/api/connect';

let g_networks = [];
let g_selected = null;

/* ── DOM refs ──────────────────────────────────────────────────────────────*/
const elList      = document.getElementById('network-list');
const elSpinner   = document.getElementById('spinner');
const elRescan    = document.getElementById('btn-rescan');
const elConnect   = document.getElementById('btn-connect');
const elSsid      = document.getElementById('ssid');
const elPass      = document.getElementById('password');
const elForm      = document.getElementById('connect-form');
const elStatus    = document.getElementById('status');
const elToggle    = document.querySelector('.toggle-pass');
const elSkip      = document.getElementById('skip-btn');

/* ── Init ──────────────────────────────────────────────────────────────────*/
async function init() {
    elRescan.addEventListener('click', onRescan);
    elForm.addEventListener('submit', e => { e.preventDefault(); onConnect(); });

    /* Password visibility toggle */
    if (elToggle) {
        elToggle.addEventListener('click', () => {
            const isPass = elPass.type === 'password';
            elPass.type  = isPass ? 'text' : 'password';
            elToggle.textContent = isPass ? '\u{1F648}' : '\u{1F441}';
        });
    }

    /* Skip button — navigate to robot index once connected */
    if (elSkip) {
        elSkip.addEventListener('click', e => {
            e.preventDefault();
            window.location.href = 'http://' + window.location.hostname + '/';
        });
    }

    showSpinner(true);
    await loadNetworks();
    showSpinner(false);
}

/* ── Load cached networks ──────────────────────────────────────────────────*/
async function loadNetworks() {
    try {
        const res  = await fetch(API_SCAN);
        const data = await res.json();

        if (data.status === 'scanning') {
            setStatus('Scanning\u2026 please wait');
            setTimeout(loadNetworks, 600);
            return;
        }

        g_networks = data;
        renderList();
        setStatus(data.length ? `${data.length} network(s) found` : 'No networks found');
    } catch (err) {
        setStatus('Failed to load networks');
        console.error(err);
    }
}

/* ── Manual rescan ─────────────────────────────────────────────────────────*/
async function onRescan() {
    elRescan.disabled = true;
    showSpinner(true);
    setStatus('Scanning\u2026 stay on this page');

    try {
        const res  = await fetch(API_RESCAN);
        const data = await res.json();
        g_networks = data;
        renderList();
        setStatus(data.length ? `${data.length} network(s) found` : 'No networks found');
    } catch (err) {
        setStatus('Rescan failed');
        console.error(err);
    } finally {
        showSpinner(false);
        elRescan.disabled = false;
    }
}

/* ── Render list ───────────────────────────────────────────────────────────*/
function renderList() {
    elList.innerHTML = '';
    if (!g_networks.length) {
        elList.innerHTML = '<p class="scan-status">No networks found</p>';
        return;
    }

    g_networks.forEach((ap, idx) => {
        const btn = document.createElement('button');
        btn.type      = 'button';
        btn.className = 'network-btn' + (g_selected === idx ? ' selected' : '');

        const lock  = ap.secure ? '\uD83D\uDD12 ' : '\uD83D\uDD13 ';
        const bars  = qualityBars(ap.quality);

        btn.innerHTML =
            `<span class="network-name">${escapeHtml(ap.ssid)}</span>` +
            `<span class="signal">${lock}${bars}</span>`;

        btn.addEventListener('click', () => selectNetwork(idx));
        elList.appendChild(btn);
    });
}

function selectNetwork(idx) {
    g_selected    = idx;
    elSsid.value  = g_networks[idx].ssid;
    elPass.value  = '';
    elPass.focus();
    renderList();
}

/* ── Connect ───────────────────────────────────────────────────────────────*/
async function onConnect() {
    const ssid = elSsid.value.trim();
    const pass = elPass.value;

    if (!ssid) {
        setStatus('Please select or enter an SSID');
        return;
    }

    elConnect.disabled = true;
    setStatus('Saving\u2026');

    const body = `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(pass)}`;
    try {
        const res  = await fetch(API_CONNECT, {
            method:  'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body,
        });
        const text = await res.text();
        setStatus(text);
        /* Device reboots; user will reconnect to their home WiFi */
    } catch (err) {
        /* Fetch may throw when the device reboots mid-response — treat as success */
        setStatus('Saved \u2014 device is connecting\u2026');
        console.info('Expected fetch abort on reboot:', err);
    } finally {
        elConnect.disabled = false;
    }
}

/* ── Helpers ───────────────────────────────────────────────────────────────*/
function showSpinner(show) {
    if (elSpinner) elSpinner.style.display = show ? 'block' : 'none';
}

function setStatus(msg) {
    if (elStatus) elStatus.textContent = msg;
}

function qualityBars(q) {
    const filled = Math.round(q / 25);
    let s = '';
    for (let i = 0; i < 4; i++) s += (i < filled) ? '\u25AE' : '\u25AF';
    return s;
}

function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

/* ── Boot ──────────────────────────────────────────────────────────────────*/
if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', init);
else
    init();

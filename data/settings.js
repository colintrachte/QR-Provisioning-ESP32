'use strict';

/* =============================================================================
 * settings.js — Robot admin / settings page.
 *
 * Tabs:
 *   WiFi     — show current connection, scan for networks, reconnect or erase
 *   Firmware — upload firmware.bin via POST /ota/firmware with progress
 *   Web UI   — upload littlefs.bin via POST /ota/filesystem with progress
 *
 * OTA upload uses XMLHttpRequest (not fetch) because XHR exposes upload
 * progress events. fetch() with ReadableStream would work too but requires
 * more ceremony for progress tracking and has patchy support on older
 * mobile browsers that might be used to access the settings page.
 * ============================================================================= */

// ── Tabs ──────────────────────────────────────────────────────────────────────
document.querySelectorAll('.tab').forEach(tab => {
    tab.addEventListener('click', () => {
        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
        document.querySelectorAll('.tab-panel').forEach(p => p.classList.add('hidden'));
        tab.classList.add('active');
        document.getElementById('tab-' + tab.dataset.tab).classList.remove('hidden');
    });
});

// ── WiFi tab ──────────────────────────────────────────────────────────────────

const wifiSsidEl   = document.getElementById('wifi-ssid');
const wifiIpEl     = document.getElementById('wifi-ip');
const wifiDotEl    = document.getElementById('wifi-dot');
const wifiSignalEl = document.getElementById('wifi-signal');
const networkList  = document.getElementById('network-list');
const ssidInput    = document.getElementById('ssid-input');
const passInput    = document.getElementById('pass-input');

function escHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;')
            .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function signalBars(quality) {
    if (quality > 80) return '\u2582\u2584\u2586\u2588';
    if (quality > 60) return '\u2582\u2584\u2586\xb7';
    if (quality > 40) return '\u2582\u2584\xb7\xb7';
    return '\u2582\xb7\xb7\xb7';
}

function fmtBytes(n) {
    if (n < 1024)       return n + ' B';
    if (n < 1024*1024)  return (n/1024).toFixed(1) + ' kB';
    return (n/(1024*1024)).toFixed(2) + ' MB';
}

// Load current WiFi status from /api/status
function loadStatus() {
    fetch('/api/status')
        .then(r => r.ok ? r.json() : null)
        .then(d => {
            if (!d) return;
            wifiSsidEl.textContent = d.ssid || '—';
            wifiIpEl.textContent   = d.ip   || '';
            wifiDotEl.className    = 'status-dot ' + (d.connected ? 'connected' : 'disconnected');
            if (d.rssi !== undefined) {
                const q = Math.max(0, Math.min(100, 2 * (d.rssi + 100)));
                wifiSignalEl.textContent = signalBars(q) + ' ' + d.rssi + ' dBm';
            }
        })
        .catch(() => {
            wifiSsidEl.textContent = 'Unknown';
        });
}

// Scan for networks via /api/scan — returns cached results immediately,
// or {"status":"scanning"} while a scan is in progress.
let scanTimer = null;

function pollScan() {
    fetch('/api/scan')
        .then(r => r.ok ? r.json() : Promise.reject())
        .then(data => {
            if (Array.isArray(data)) {
                renderNetworks(data);
            } else {
                // Still scanning — poll again
                scanTimer = setTimeout(pollScan, 1500);
            }
        })
        .catch(() => {
            networkList.innerHTML = '<p class="list-placeholder">Scan failed — try again</p>';
        });
}

function startScan() {
    clearTimeout(scanTimer);
    networkList.innerHTML = '<p class="list-placeholder">Scanning…</p>';
    // Trigger a fresh scan then start polling
    fetch('/api/scan?refresh=1').catch(() => {});
    scanTimer = setTimeout(pollScan, 1200);
}

function renderNetworks(networks) {
    networkList.innerHTML = '';
    if (!networks.length) {
        networkList.innerHTML = '<p class="list-placeholder">No networks found</p>';
        return;
    }
    networks.sort((a, b) => b.quality - a.quality);
    networks.forEach(net => {
        const btn = document.createElement('button');
        btn.type      = 'button';
        btn.className = 'network-btn';
        btn.innerHTML =
            '<span>' + escHtml(net.ssid) + '</span>' +
            '<span class="network-meta">' +
                signalBars(net.quality) + ' ' + net.quality + '%' +
                (net.secure ? ' &#128274;' : '') +
            '</span>';
        btn.addEventListener('click', () => {
            networkList.querySelectorAll('.network-btn').forEach(b => b.classList.remove('selected'));
            btn.classList.add('selected');
            ssidInput.value = net.ssid;
            if (net.secure) passInput.focus();
            else passInput.value = '';
        });
        networkList.appendChild(btn);
    });
}

document.getElementById('rescan-btn').addEventListener('click', startScan);

// Password toggle
document.querySelector('.toggle-pass').addEventListener('click', () => {
    passInput.type = passInput.type === 'password' ? 'text' : 'password';
});

// Save & reconnect — POST /api/connect
document.getElementById('connect-btn').addEventListener('click', () => {
    const ssid = ssidInput.value.trim();
    if (!ssid) { ssidInput.focus(); return; }

    const btn = document.getElementById('connect-btn');
    btn.disabled    = true;
    btn.textContent = 'Saving…';

    const body = 'ssid=' + encodeURIComponent(ssid) +
                 '&pass=' + encodeURIComponent(passInput.value);

    fetch('/api/connect', {
        method:  'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body,
    })
    .then(r => r.ok ? r.text() : Promise.reject(r.status))
    .then(() => {
        btn.textContent = 'Saved — rebooting…';
    })
    .catch(err => {
        // Network gone = reboot happened = success
        if (typeof err !== 'number') btn.textContent = 'Saved — rebooting…';
        else {
            btn.textContent = 'Save & Reconnect';
            btn.disabled    = false;
        }
    });
});

// Erase credentials & factory reset
document.getElementById('erase-btn').addEventListener('click', () => {
    if (!confirm('Erase saved WiFi credentials and restart in setup mode?')) return;
    fetch('/api/erase', { method: 'POST' })
        .catch(() => {})  // reboot = connection loss = fine
        .finally(() => {
            document.getElementById('erase-btn').textContent = 'Erasing — restarting…';
        });
});

// ── OTA upload — shared logic ─────────────────────────────────────────────────
// Uses XHR for upload progress events. fetch() doesn't expose upload progress
// without ReadableStream gymnastics that aren't worth it for a settings page.

function setupUpload({ dropId, fileInputId, fileRowId, fileNameId, fileSizeId,
                       clearId, progressWrapId, progressBarId, progressLabelId,
                       uploadBtnId, statusId, endpoint, onSuccess }) {
    const dropEl       = document.getElementById(dropId);
    const fileInput    = document.getElementById(fileInputId);
    const fileRow      = document.getElementById(fileRowId);
    const fileNameEl   = document.getElementById(fileNameId);
    const fileSizeEl   = document.getElementById(fileSizeId);
    const clearBtn     = document.getElementById(clearId);
    const progressWrap = document.getElementById(progressWrapId);
    const progressBar  = document.getElementById(progressBarId);
    const progressLbl  = document.getElementById(progressLabelId);
    const uploadBtn    = document.getElementById(uploadBtnId);
    const statusEl     = document.getElementById(statusId);

    let selectedFile = null;

    function setFile(f) {
        if (!f || !f.name.endsWith('.bin')) {
            statusEl.textContent = 'Select a .bin file';
            statusEl.className   = 'status-msg error';
            return;
        }
        selectedFile          = f;
        fileNameEl.textContent = f.name;
        fileSizeEl.textContent = fmtBytes(f.size);
        fileRow.classList.remove('hidden');
        dropEl.classList.add('hidden');
        uploadBtn.disabled     = false;
        statusEl.textContent   = '';
        statusEl.className     = 'status-msg';
    }

    function clearFile() {
        selectedFile          = null;
        fileInput.value       = '';
        fileRow.classList.add('hidden');
        dropEl.classList.remove('hidden');
        uploadBtn.disabled    = true;
        progressWrap.classList.add('hidden');
        statusEl.textContent  = '';
        statusEl.className    = 'status-msg';
    }

    // File picker
    dropEl.addEventListener('click', () => fileInput.click());
    fileInput.addEventListener('change', () => {
        if (fileInput.files[0]) setFile(fileInput.files[0]);
    });

    // Drag and drop
    dropEl.addEventListener('dragover', e => {
        e.preventDefault();
        dropEl.classList.add('drag-over');
    });
    ['dragleave', 'dragend'].forEach(ev =>
        dropEl.addEventListener(ev, () => dropEl.classList.remove('drag-over'))
    );
    dropEl.addEventListener('drop', e => {
        e.preventDefault();
        dropEl.classList.remove('drag-over');
        const f = e.dataTransfer.files[0];
        if (f) setFile(f);
    });

    clearBtn.addEventListener('click', clearFile);

    // Upload
    uploadBtn.addEventListener('click', () => {
        if (!selectedFile) return;

        uploadBtn.disabled       = true;
        progressWrap.classList.remove('hidden');
        progressBar.style.width  = '0%';
        progressLbl.textContent  = '0%';
        statusEl.textContent     = 'Uploading…';
        statusEl.className       = 'status-msg';

        const xhr = new XMLHttpRequest();
        xhr.open('POST', endpoint, true);

        xhr.upload.addEventListener('progress', e => {
            if (!e.lengthComputable) return;
            const pct = Math.round(e.loaded / e.total * 100);
            progressBar.style.width = pct + '%';
            progressLbl.textContent = pct + '%';
        });

        xhr.addEventListener('load', () => {
            if (xhr.status === 200) {
                progressBar.style.width = '100%';
                progressLbl.textContent = '100%';
                statusEl.textContent    = xhr.responseText || 'Done';
                statusEl.className      = 'status-msg ok';
                if (onSuccess) onSuccess();
            } else {
                statusEl.textContent = 'Error ' + xhr.status + ': ' + xhr.responseText;
                statusEl.className   = 'status-msg error';
                uploadBtn.disabled   = false;
            }
        });

        xhr.addEventListener('error', () => {
            // Network gone during firmware OTA = device rebooted = success
            if (endpoint.includes('firmware')) {
                progressBar.style.width = '100%';
                progressLbl.textContent = '100%';
                statusEl.textContent    = 'Upload complete — device rebooting';
                statusEl.className      = 'status-msg ok';
            } else {
                statusEl.textContent = 'Upload failed — connection lost';
                statusEl.className   = 'status-msg error';
                uploadBtn.disabled   = false;
            }
        });

        xhr.send(selectedFile);
    });
}

// ── Firmware tab setup ────────────────────────────────────────────────────────
setupUpload({
    dropId:          'fw-drop',
    fileInputId:     'fw-file',
    fileRowId:       'fw-file-row',
    fileNameId:      'fw-file-name',
    fileSizeId:      'fw-file-size',
    clearId:         'fw-clear',
    progressWrapId:  'fw-progress-wrap',
    progressBarId:   'fw-progress-bar',
    progressLabelId: 'fw-progress-label',
    uploadBtnId:     'fw-upload-btn',
    statusId:        'fw-status',
    endpoint:        '/ota/firmware',
    onSuccess: () => {
        // Firmware OTA reboots the device — connection will drop shortly
        setTimeout(() => {
            document.getElementById('fw-status').textContent =
                'Rebooting… reconnect in a few seconds';
        }, 1000);
    },
});

// ── Web UI tab setup ──────────────────────────────────────────────────────────
setupUpload({
    dropId:          'fs-drop',
    fileInputId:     'fs-file',
    fileRowId:       'fs-file-row',
    fileNameId:      'fs-file-name',
    fileSizeId:      'fs-file-size',
    clearId:         'fs-clear',
    progressWrapId:  'fs-progress-wrap',
    progressBarId:   'fs-progress-bar',
    progressLabelId: 'fs-progress-label',
    uploadBtnId:     'fs-upload-btn',
    statusId:        'fs-status',
    endpoint:        '/ota/filesystem',
    onSuccess: () => {
        document.getElementById('fs-status').textContent =
            'Web UI updated — reload the page to see changes';
    },
});

window.onerror = (msg, url, line) => {
    fetch('/api/jserror', {
        method: 'POST',
        body: `${msg} @ ${url}:${line}`
    });
};
// ── Boot ──────────────────────────────────────────────────────────────────────
loadStatus();

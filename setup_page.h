#ifndef SETUP_PAGE_H
#define SETUP_PAGE_H

/*
  setup_page.h
  ------------
  Served at 192.168.4.1 when ESP32 is in AP fallback mode.
  Features:
  - Live WiFi network scan with signal strength bars
  - Click a network to auto-fill SSID
  - Password show/hide toggle
  - ThingSpeak API key field
  - Saves to EEPROM and reboots
*/

const char ap_setup_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>SkyWatch Pro — WiFi Setup</title>
<style>
  :root {
    --bg:      #0f172a;
    --surface: #1e293b;
    --card:    #273549;
    --border:  #334155;
    --blue:    #3b82f6;
    --green:   #22c55e;
    --red:     #ef4444;
    --amber:   #f59e0b;
    --text:    #e2e8f0;
    --dim:     #94a3b8;
    --mono:    'Courier New', monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    font-size: 14px; min-height: 100vh;
    display: flex; align-items: center; justify-content: center;
    padding: 16px;
  }
  .container { width: 100%; max-width: 480px; }

  /* Header */
  .header { text-align: center; margin-bottom: 24px; }
  .diamond {
    width: 36px; height: 36px; background: var(--blue);
    transform: rotate(45deg); margin: 0 auto 12px;
    box-shadow: 0 0 20px rgba(59,130,246,0.5);
  }
  h1 { font-size: 20px; font-weight: 700; letter-spacing: 0.05em; }
  .subtitle { color: var(--dim); font-size: 12px; margin-top: 4px; }

  /* Cards */
  .card {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 8px; margin-bottom: 12px; overflow: hidden;
  }
  .card-head {
    display: flex; align-items: center; justify-content: space-between;
    padding: 12px 16px; border-bottom: 1px solid var(--border);
    font-size: 11px; font-weight: 600; text-transform: uppercase;
    letter-spacing: 0.1em; color: var(--dim);
  }
  .card-body { padding: 16px; }

  /* WiFi network list */
  .net-list { display: flex; flex-direction: column; gap: 6px; }
  .net-item {
    display: flex; align-items: center; gap: 10px;
    padding: 10px 12px; border-radius: 6px;
    background: var(--card); border: 1px solid var(--border);
    cursor: pointer; transition: border-color .15s, background .15s;
    user-select: none;
  }
  .net-item:hover, .net-item.selected {
    border-color: var(--blue);
    background: rgba(59,130,246,0.08);
  }
  .net-item.selected { border-color: var(--blue); }
  .net-icon { font-size: 18px; flex-shrink: 0; }
  .net-name { flex: 1; font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .net-meta { font-size: 10px; color: var(--dim); margin-top: 1px; }
  .net-right { display: flex; flex-direction: column; align-items: flex-end; gap: 3px; flex-shrink: 0; }
  .net-lock { font-size: 11px; }

  /* Signal bars */
  .sig-bars { display: flex; align-items: flex-end; gap: 2px; height: 14px; }
  .sig-bar {
    width: 4px; border-radius: 1px;
    background: var(--border);
  }
  .sig-bar.lit { background: var(--green); }
  .sig-bar.lit.med  { background: var(--amber); }
  .sig-bar.lit.weak { background: var(--red); }

  /* Scan button */
  .scan-btn {
    font-size: 11px; font-weight: 600; letter-spacing: 0.05em;
    text-transform: uppercase; padding: 5px 12px;
    border: 1px solid var(--border); border-radius: 4px;
    background: transparent; color: var(--dim); cursor: pointer;
    transition: all .15s;
  }
  .scan-btn:hover { border-color: var(--blue); color: var(--blue); }
  .scan-btn:disabled { opacity: 0.4; cursor: default; }

  /* Loading */
  .loading {
    text-align: center; padding: 20px;
    color: var(--dim); font-size: 12px; display: none;
  }
  .spinner {
    display: inline-block; width: 18px; height: 18px;
    border: 2px solid var(--border); border-top-color: var(--blue);
    border-radius: 50%; animation: spin 0.8s linear infinite;
    vertical-align: middle; margin-right: 8px;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
  .empty { text-align: center; padding: 20px; color: var(--dim); font-size: 12px; }

  /* Form fields */
  .field-label {
    font-size: 11px; font-weight: 600; text-transform: uppercase;
    letter-spacing: 0.08em; color: var(--dim);
    margin-bottom: 6px; margin-top: 14px; display: block;
  }
  .field-label:first-child { margin-top: 0; }
  .field-wrap { position: relative; }
  .field-input {
    width: 100%; background: var(--card); color: var(--text);
    border: 1px solid var(--border); border-radius: 6px;
    padding: 10px 40px 10px 12px;
    font-family: var(--mono); font-size: 13px;
    outline: none; transition: border-color .15s;
  }
  .field-input:focus { border-color: var(--blue); }
  .field-input.readonly { opacity: 0.7; }
  .eye-btn {
    position: absolute; right: 10px; top: 50%; transform: translateY(-50%);
    background: none; border: none; cursor: pointer;
    color: var(--dim); font-size: 16px; padding: 0; line-height: 1;
  }
  .eye-btn:hover { color: var(--text); }

  /* Selected SSID chip */
  .ssid-chip {
    display: none; align-items: center; gap: 8px;
    padding: 8px 12px; border-radius: 6px;
    background: rgba(59,130,246,0.1); border: 1px solid rgba(59,130,246,0.3);
    font-size: 13px; font-weight: 600; color: var(--blue);
    margin-bottom: 12px;
  }
  .ssid-chip.show { display: flex; }
  .ssid-chip-clear {
    margin-left: auto; font-size: 16px; cursor: pointer;
    color: var(--dim); line-height: 1; background: none; border: none;
  }

  /* Status messages */
  .msg {
    padding: 10px 14px; border-radius: 6px; font-size: 12px;
    font-weight: 500; margin-top: 12px; display: none; align-items: center; gap: 8px;
  }
  .msg.show { display: flex; }
  .msg.err  { background: rgba(239,68,68,.1); border: 1px solid rgba(239,68,68,.3); color: var(--red); }
  .msg.ok   { background: rgba(34,197,94,.1); border: 1px solid rgba(34,197,94,.3); color: var(--green); }
  .msg.info { background: rgba(59,130,246,.1); border: 1px solid rgba(59,130,246,.3); color: var(--blue); }

  /* Submit */
  .submit-btn {
    width: 100%; padding: 13px; margin-top: 16px;
    background: var(--blue); color: #fff; border: none;
    border-radius: 6px; font-size: 14px; font-weight: 700;
    letter-spacing: 0.05em; text-transform: uppercase;
    cursor: pointer; transition: opacity .15s;
  }
  .submit-btn:hover { opacity: 0.88; }
  .submit-btn:disabled { opacity: 0.4; cursor: default; }

  /* Footer */
  .footer { text-align: center; margin-top: 20px; font-size: 11px; color: var(--dim); }
</style>
</head>
<body>
<div class="container">

  <!-- Header -->
  <div class="header">
    <div class="diamond"></div>
    <h1>SKYWATCH PRO</h1>
    <div class="subtitle">WiFi Configuration — AP Mode (192.168.4.1)</div>
  </div>

  <!-- Network Scanner -->
  <div class="card">
    <div class="card-head">
      <span>📡 Available Networks</span>
      <button class="scan-btn" id="scan-btn" onclick="scanNetworks()">Scan</button>
    </div>
    <div class="card-body" style="padding:12px">
      <div class="loading" id="scan-loading">
        <span class="spinner"></span>Scanning for networks…
      </div>
      <div class="net-list" id="net-list">
        <div class="empty" id="net-empty">Press Scan to discover networks</div>
      </div>
    </div>
  </div>

  <!-- Credentials -->
  <div class="card">
    <div class="card-head"><span>🔐 Credentials</span></div>
    <div class="card-body">

      <!-- Selected network chip -->
      <div class="ssid-chip" id="ssid-chip">
        <span>📶</span>
        <span id="chip-name">—</span>
        <button class="ssid-chip-clear" onclick="clearSelection()" title="Clear">✕</button>
      </div>

      <label class="field-label" for="ssid-input">WiFi SSID</label>
      <div class="field-wrap">
        <input class="field-input" type="text" id="ssid-input"
               placeholder="Network name (or tap above)" autocomplete="off">
      </div>

      <label class="field-label" for="pass-input">WiFi Password</label>
      <div class="field-wrap">
        <input class="field-input" type="password" id="pass-input"
               placeholder="Password" autocomplete="current-password">
        <button class="eye-btn" type="button" onclick="togglePass()" id="eye-btn">👁</button>
      </div>

      <label class="field-label" for="key-input">ThingSpeak API Key <span style="color:var(--dim);font-weight:400;text-transform:none">(optional)</span></label>
      <div class="field-wrap">
        <input class="field-input" type="text" id="key-input"
               placeholder="16-character write key" autocomplete="off"
               maxlength="20">
      </div>

      <div class="msg err" id="msg-err">⚠ <span id="msg-err-text"></span></div>
      <div class="msg ok"  id="msg-ok">✓ <span id="msg-ok-text"></span></div>
      <div class="msg info" id="msg-info">ℹ <span id="msg-info-text"></span></div>

      <button class="submit-btn" id="save-btn" onclick="saveSettings()">
        Save &amp; Connect
      </button>
    </div>
  </div>

  <div class="footer">SkyWatch Pro v17.0 &nbsp;·&nbsp; Node: SkyWatch-Roof</div>
</div>

<script>
let selectedSSID = '';

// ── Signal bars SVG ─────────────────────────────────────────────
function signalBars(rssi) {
  // rssi: -100 (worst) to 0 (best)
  // 4 bars
  let strength = 0;
  if (rssi > -55) strength = 4;
  else if (rssi > -67) strength = 3;
  else if (rssi > -78) strength = 2;
  else if (rssi > -89) strength = 1;

  const cls = rssi > -67 ? '' : rssi > -78 ? 'med' : 'weak';
  const heights = [4, 7, 10, 14];
  let html = '<div class="sig-bars">';
  for (let i = 0; i < 4; i++) {
    const lit = (i < strength) ? `lit ${cls}` : '';
    html += `<div class="sig-bar ${lit}" style="height:${heights[i]}px"></div>`;
  }
  html += '</div>';
  return html;
}

// ── Scan networks ────────────────────────────────────────────────
function scanNetworks() {
  const btn = document.getElementById('scan-btn');
  const loading = document.getElementById('scan-loading');
  const list = document.getElementById('net-list');
  const empty = document.getElementById('net-empty');

  btn.disabled = true;
  btn.innerText = 'Scanning…';
  loading.style.display = 'block';
  list.innerHTML = '';

  fetch('/api/scan')
    .then(r => r.json())
    .then(networks => {
      loading.style.display = 'none';
      btn.disabled = false;
      btn.innerText = 'Rescan';

      if (!networks || networks.length === 0) {
        list.innerHTML = '<div class="empty">No networks found. Try rescanning.</div>';
        return;
      }

      // Sort by RSSI descending
      networks.sort((a, b) => b.rssi - a.rssi);

      list.innerHTML = networks.map(n => `
        <div class="net-item${selectedSSID === n.ssid ? ' selected' : ''}"
             onclick="selectNetwork('${n.ssid.replace(/'/g, "\\'")}', ${n.open})"
             id="net-${CSS.escape(n.ssid)}">
          <div style="flex:1;min-width:0">
            <div class="net-name">${escHtml(n.ssid)}</div>
            <div class="net-meta">Ch ${n.channel} · ${n.rssi} dBm · ${n.open ? 'Open' : 'Secured'}</div>
          </div>
          <div class="net-right">
            ${signalBars(n.rssi)}
            <span class="net-lock">${n.open ? '🔓' : '🔒'}</span>
          </div>
        </div>`).join('');
    })
    .catch(err => {
      loading.style.display = 'none';
      btn.disabled = false;
      btn.innerText = 'Retry';
      list.innerHTML = '<div class="empty" style="color:var(--red)">Scan failed — try again</div>';
    });
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

// ── Select a network ─────────────────────────────────────────────
function selectNetwork(ssid, isOpen) {
  selectedSSID = ssid;
  document.getElementById('ssid-input').value = ssid;
  document.getElementById('chip-name').innerText = ssid;
  document.getElementById('ssid-chip').classList.add('show');

  // If open network, clear and disable password field
  const passEl = document.getElementById('pass-input');
  if (isOpen) {
    passEl.value = '';
    passEl.placeholder = 'No password (open network)';
    passEl.disabled = true;
  } else {
    passEl.disabled = false;
    passEl.placeholder = 'Password';
    passEl.focus();
  }

  // Highlight selected row
  document.querySelectorAll('.net-item').forEach(el => el.classList.remove('selected'));
  const row = document.getElementById('net-' + CSS.escape(ssid));
  if (row) row.classList.add('selected');

  hideMessages();
}

function clearSelection() {
  selectedSSID = '';
  document.getElementById('ssid-input').value = '';
  document.getElementById('ssid-chip').classList.remove('show');
  document.getElementById('pass-input').disabled = false;
  document.getElementById('pass-input').placeholder = 'Password';
  document.querySelectorAll('.net-item').forEach(el => el.classList.remove('selected'));
}

// ── Password toggle ──────────────────────────────────────────────
function togglePass() {
  const inp = document.getElementById('pass-input');
  const btn = document.getElementById('eye-btn');
  if (inp.type === 'password') { inp.type = 'text'; btn.innerText = '🙈'; }
  else { inp.type = 'password'; btn.innerText = '👁'; }
}

// ── Message helpers ──────────────────────────────────────────────
function showMsg(type, text) {
  hideMessages();
  document.getElementById('msg-' + type).classList.add('show');
  document.getElementById('msg-' + type + '-text').innerText = text;
}
function hideMessages() {
  ['err','ok','info'].forEach(t => document.getElementById('msg-' + t).classList.remove('show'));
}

// ── Save settings ────────────────────────────────────────────────
function saveSettings() {
  const ssid = document.getElementById('ssid-input').value.trim();
  const pass = document.getElementById('pass-input').value;
  const key  = document.getElementById('key-input').value.trim();

  if (!ssid) { showMsg('err', 'Please enter or select a WiFi network.'); return; }

  const btn = document.getElementById('save-btn');
  btn.disabled = true;
  btn.innerText = 'Saving…';
  hideMessages();

  const params = new URLSearchParams();
  params.append('ssid', ssid);
  params.append('pass', pass);
  if (key) params.append('key', key);

  fetch('/save', { method: 'POST', body: params })
    .then(r => r.text())
    .then(t => {
      showMsg('ok', 'Settings saved! ESP32 is rebooting and connecting to "' + ssid + '"…');
      btn.innerText = 'Rebooting…';
      // Show countdown
      let countdown = 15;
      const iv = setInterval(() => {
        countdown--;
        document.getElementById('msg-ok-text').innerText =
          'Saved! Rebooting… (trying to connect to "' + ssid + '"). ' +
          'If successful, access SkyWatch at the new IP in ~' + countdown + 's.';
        if (countdown <= 0) {
          clearInterval(iv);
          showMsg('info', 'Reboot complete. Connect your device to "' + ssid + '" and find the ESP32 IP on your router.');
          btn.disabled = false;
          btn.innerText = 'Save & Connect';
        }
      }, 1000);
    })
    .catch(err => {
      showMsg('err', 'Save failed: ' + err.message);
      btn.disabled = false;
      btn.innerText = 'Save & Connect';
    });
}

// ── Auto-scan on load ────────────────────────────────────────────
window.addEventListener('load', () => {
  setTimeout(scanNetworks, 300);
});
</script>
</body>
</html>
)=====";

#endif // SETUP_PAGE_H

#pragma once
#include <string>

const std::string DASHBOARD_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Sluice Gateway</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600;700&display=swap" rel="stylesheet">
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#f6f8fa;
  --s1:#ffffff;
  --s2:#f3f4f6;
  --border:#d0d7de;
  --border2:#e1e4e8;
  --text:#24292f;
  --bright:#000000;
  --dim:#57606a;
  --up:#1a7f37;
  --down:#cf222e;
  --addr:#0969da;
  --bytes:#9a6700;
  --warn:#9a6700;
  --accent:#0969da;
  --f:-apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif;
  --mono:'JetBrains Mono',SFMono-Regular,Consolas,monospace;
  --radius:3px;
  --shadow:none;
}
html,body{height:100%;background:var(--bg);color:var(--text);font-family:var(--f);font-size:12px;overflow:hidden;-webkit-font-smoothing:antialiased}

/* ── Topbar ── */
#topbar{position:fixed;top:0;left:0;right:0;height:40px;background:var(--s1);border-bottom:1px solid var(--border);display:flex;align-items:center;padding:0 16px;gap:16px;z-index:100}
.gw-title{color:var(--bright);font-weight:700;font-size:12px;letter-spacing:1px;font-family:var(--mono)}
.led{width:10px;height:10px;border-radius:50%;flex-shrink:0;border:1px solid rgba(0,0,0,0.1)}
.led.up{background:var(--up)}
.led.down{background:var(--down)}
.led.warn{background:var(--warn)}
.led.unknown{background:var(--dim)}
.tb-stat{font-size:11px;color:var(--dim);font-family:var(--mono)}
.tb-stat span{color:var(--text);font-weight:600}
.tb-stat.br span{color:var(--bright)}
.sep{color:var(--border);user-select:none}
.ml{margin-left:auto}
.btn{background:var(--s2);border:1px solid var(--border);color:var(--text);font-family:var(--f);font-size:11px;font-weight:600;padding:4px 10px;cursor:pointer;border-radius:var(--radius)}
.btn:hover{background:var(--border2);color:var(--bright)}
.btn-primary{background:var(--accent);border:1px solid var(--accent);color:#ffffff;}
.btn-primary:hover{background:#0353a4;border-color:#0353a4;color:#ffffff}
.btn-danger{color:var(--down);border-color:var(--border)}
.btn-danger:hover{background:#ffebe9;border-color:#ff8182;color:var(--down)}
#rld-msg{font-size:11px;color:var(--warn);font-family:var(--mono)}

/* ── Tabbar ── */
#tabbar{position:fixed;top:40px;left:0;right:0;height:32px;background:var(--s1);border-bottom:1px solid var(--border);display:flex;align-items:stretch;padding:0 16px;z-index:99}
.tab{background:none;border:none;border-bottom:2px solid transparent;color:var(--dim);font-family:var(--mono);font-size:11px;font-weight:600;padding:0 16px;cursor:pointer;margin-bottom:-1px;display:flex;align-items:center;gap:6px}
.tab:hover{color:var(--text)}
.tab.active{color:var(--bright);border-bottom-color:var(--accent)}
.lock-icon{font-size:10px;color:var(--dim);font-weight:normal}

/* ── Panes ── */
.pane{display:none;padding:16px;margin-top:72px;height:calc(100vh - 72px);overflow:auto}
.pane.active{display:block}
.pane::-webkit-scrollbar{width:8px;height:8px}
.pane::-webkit-scrollbar-track{background:var(--bg)}
.pane::-webkit-scrollbar-thumb{background:var(--border);border-radius:4px}
.pane::-webkit-scrollbar-thumb:hover{background:var(--dim)}

/* ── Auth Overlay ── */
#auth-overlay{position:fixed;inset:0;background:rgba(255,255,255,0.8);z-index:1000;display:none;align-items:center;justify-content:center}
#auth-overlay.show{display:flex}
.auth-card{position:relative;background:var(--s1);border:1px solid var(--border);border-radius:var(--radius);padding:24px;width:340px;max-width:90vw;box-shadow:0 4px 12px rgba(0,0,0,0.1)}
.auth-logo{font-family:var(--mono);font-size:14px;font-weight:700;color:var(--bright);margin-bottom:4px}
.auth-subtitle{font-size:12px;color:var(--dim);margin-bottom:20px}
.auth-notice{background:#fff8c5;border:1px solid #d4a72c;padding:10px;font-size:11px;color:var(--text);margin-bottom:16px;border-radius:var(--radius)}
.auth-field{margin-bottom:12px}
.auth-field label{display:block;font-size:11px;color:var(--text);margin-bottom:4px;font-weight:600}
.auth-field input{width:100%;background:var(--s1);border:1px solid var(--border);border-radius:var(--radius);color:var(--bright);font-family:var(--mono);font-size:12px;padding:6px 8px;outline:none}
.auth-field input:focus{border-color:var(--accent)}
.auth-err{font-size:11px;color:var(--down);min-height:16px;margin-bottom:8px}
.auth-btn{width:100%;padding:8px}
.auth-close{position:absolute;top:8px;right:12px;background:none;border:none;font-size:14px;color:var(--dim);cursor:pointer;font-family:var(--mono)}
.auth-close:hover{color:var(--bright)}

/* ── Banners & Health ── */
.banner{background:#fff8c5;border:1px solid #d4a72c;border-radius:var(--radius);padding:10px 16px;font-size:12px;color:var(--text);margin-bottom:16px}
.banner a{color:var(--accent);text-decoration:none;font-weight:600}
.banner a:hover{text-decoration:underline}
.health-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}
.health-header h2{font-size:16px;font-weight:600;color:var(--bright)}
.health-updated{font-size:11px;color:var(--dim);font-family:var(--mono)}
.h-banner{padding:12px 16px;border:1px solid var(--border);border-radius:var(--radius);font-weight:600;font-size:13px;margin-bottom:20px;display:flex;align-items:center;gap:10px;background:var(--s1)}
.h-banner.h-ok{border-left:4px solid var(--up)}
.h-banner.h-warn{border-left:4px solid var(--warn)}
.h-banner.h-err{border-left:4px solid var(--down)}
.health-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px;margin-bottom:24px}
.health-card{background:var(--s1);border:1px solid var(--border);border-radius:var(--radius);padding:16px}
.hc-label{font-size:11px;color:var(--dim);font-weight:600;margin-bottom:8px}
.hc-value{font-size:24px;font-weight:700;color:var(--bright);font-family:var(--mono);line-height:1}
.hc-value.hc-ok{color:var(--up)}.hc-value.hc-warn{color:var(--warn)}.hc-value.hc-err{color:var(--down)}.hc-value.hc-bytes{color:var(--bytes)}.hc-value.hc-blue{color:var(--accent)}
.hc-sub{font-size:11px;color:var(--dim);margin-top:8px;font-family:var(--mono)}
.health-footer{padding-top:16px;border-top:1px solid var(--border);display:flex;justify-content:space-between;font-size:11px;color:var(--dim)}

/* ── Strips & Tables ── */
.strip{border:1px solid var(--border);background:var(--s1);padding:10px 16px;display:flex;gap:24px;flex-wrap:wrap;font-size:12px;border-radius:var(--radius)}
.strip+.strip{margin-top:8px}
.sum-item{color:var(--dim)}
.sum-item span{color:var(--bright);font-family:var(--mono);font-weight:600;margin-left:4px}
.sum-item .ok{color:var(--up)}.sum-item .bad{color:var(--down)}.sum-item .b{color:var(--bytes)}
#sys-bar{margin-bottom:16px}

.rtable{width:100%;border-collapse:collapse;background:var(--s1);border:1px solid var(--border);border-radius:var(--radius)}
.rtable th{text-align:left;padding:8px 12px;color:var(--dim);font-size:11px;font-weight:600;border-bottom:1px solid var(--border);background:var(--s2);white-space:nowrap}
.rtable td{padding:8px 12px;border-bottom:1px solid var(--border2);font-size:12px;font-family:var(--mono)}
.rtable tr:last-child td{border-bottom:none}
.rname{color:var(--bright);font-weight:600}
.port{color:var(--addr)}
.addr{color:var(--addr)}
.tag{font-size:10px;font-weight:600;padding:2px 6px;border-radius:10px;display:inline-block}
.tag-up{color:var(--up);background:#e6ffec;border:1px solid rgba(26,127,55,0.2)}
.tag-down{color:var(--down);background:#ffebe9;border:1px solid rgba(207,34,46,0.2)}
.tag-disabled{color:var(--dim);background:var(--s2);border:1px solid var(--border)}
.tag-retiring{color:var(--warn);background:#fff8c5;border:1px solid rgba(154,103,0,0.2)}
.tag-udp{color:var(--accent);background:var(--s2);border:1px solid var(--border)}
.tag-tcp{color:var(--dim);background:var(--s2);border:1px solid var(--border)}
.tag-na{color:var(--dim);background:var(--s2);border:1px dashed var(--border)}
.udp-note{font-size:11px;color:var(--dim);margin:4px 0 8px;padding:6px 8px;background:var(--s1);border-left:2px solid var(--accent);border-radius:3px}
.dimval{color:var(--dim)}
.no-routes{padding:24px;text-align:center;color:var(--dim);font-style:italic}
.drain-section{margin-top:24px}
.drain-hdr{font-size:12px;font-weight:600;color:var(--text);margin-bottom:8px;display:flex;justify-content:space-between}

/* ── Logs ── */
#log-toolbar{display:flex;align-items:center;gap:16px;margin-bottom:12px;font-size:12px}
#log-toolbar label{display:flex;align-items:center;gap:6px;cursor:pointer;color:var(--text)}
#log-count{margin-left:auto;color:var(--dim);font-family:var(--mono)}
#log-out{background:var(--s1);border:1px solid var(--border);border-radius:var(--radius);padding:12px;height:calc(100vh - 170px);overflow-y:auto;font-size:11px;font-family:var(--mono);line-height:1.5}
.ll{color:var(--text)}.le{color:var(--down)}

/* ── Config ── */
.cfg-block{border:1px solid var(--border);background:var(--s1);margin-bottom:16px;border-radius:var(--radius)}
.cfg-hdr{padding:10px 16px;background:var(--s2);border-bottom:1px solid var(--border);font-size:12px;font-weight:600;display:flex;align-items:center;justify-content:space-between;color:var(--text)}
.cfg-body{padding:16px}
.frow{display:flex;gap:12px;margin-bottom:12px;flex-wrap:wrap;align-items:flex-start}
.fld{display:flex;flex-direction:column;gap:4px}
.fld label{font-size:10px;color:var(--text);font-weight:600}
.fld input[type=text],.fld input[type=number],.fld select{background:var(--s1);border:1px solid var(--border);color:var(--bright);font-family:var(--mono);font-size:12px;padding:6px 8px;outline:none;border-radius:var(--radius)}
.fld input:focus,.fld select:focus,.fld textarea:focus{border-color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.fld input.err,.fld textarea.err{border-color:var(--down)!important;box-shadow:inset 0 0 0 1px var(--down)!important}
.fld textarea{background:var(--s1);border:1px solid var(--border);color:var(--bright);font-family:var(--mono);font-size:11px;padding:8px;outline:none;resize:vertical;width:100%;border-radius:var(--radius)}
.emsg{font-size:10px;color:var(--down);min-height:14px}
.fld-s{width:100px}.fld-m{width:160px}.fld-l{flex:1;min-width:180px}
.chk-row{display:flex;align-items:center;gap:6px;margin-top:6px}
.sec-label{font-size:11px;color:var(--dim);margin-bottom:8px;font-weight:600;text-transform:uppercase}
.be-wrap{border:1px solid var(--border);margin-bottom:8px;background:var(--s1);border-radius:var(--radius)}
.be-main{display:flex;align-items:flex-start;gap:12px;padding:12px;flex-wrap:wrap;background:var(--s2)}
.be-access{padding:12px;border-top:1px solid var(--border)}
.bl-row{display:flex;gap:8px;margin-bottom:8px;align-items:flex-start}
.btn-text{background:none;border:none;color:var(--accent);font-family:var(--f);font-size:11px;font-weight:600;cursor:pointer;padding:4px}
.btn-text:hover{text-decoration:underline}
.save-bar{display:flex;align-items:center;gap:16px;padding:16px 0}
.smsg{font-size:12px;font-weight:600}.smsg.ok{color:var(--up)}.smsg.er{color:var(--down)}
.af-panel{padding:12px;border-top:1px solid var(--border)}
.af-row{display:flex;gap:8px;margin-bottom:8px;align-items:flex-start;flex-wrap:wrap}
</style>
</head>
<body>

<!-- ── Auth Overlay ── -->
<div id="auth-overlay">
  <div class="auth-card">
    <button class="auth-close" onclick="hideAuthOverlay()">[X]</button>
    <div class="auth-logo">SLUICE</div>
    <div class="auth-subtitle" id="auth-subtitle-text">Gateway Management</div>
    <div id="auth-login">
      <div class="auth-field">
        <label>Password</label>
        <input type="password" id="auth-pwd" placeholder="Enter password" onkeydown="if(event.key==='Enter')doLogin()">
      </div>
      <div class="auth-err" id="auth-err"></div>
      <button class="btn btn-primary auth-btn" onclick="doLogin()">Authenticate</button>
    </div>
    <div id="auth-change" style="display:none">
      <div class="auth-notice">Default password is active. Please set a secure password.</div>
      <div class="auth-field">
        <label>New Password</label>
        <input type="password" id="auth-new-pwd" placeholder="Min 6 chars">
      </div>
      <div class="auth-field">
        <label>Confirm Password</label>
        <input type="password" id="auth-confirm-pwd" onkeydown="if(event.key==='Enter')doChangePassword()">
      </div>
      <div class="auth-err" id="auth-change-err"></div>
      <button class="btn btn-primary auth-btn" onclick="doChangePassword()">Set Password</button>
    </div>
  </div>
</div>

<!-- ── Topbar ── -->
<div id="topbar">
  <span class="gw-title">SLUICE_GW</span>
  <div style="display:flex;align-items:center;gap:8px">
    <span class="led unknown" id="led"></span>
    <span id="status-label" style="font-size:11px;font-weight:600;color:var(--dim)">CONNECTING</span>
  </div>
  <span class="sep">|</span>
  <span class="tb-stat br">uptime: <span id="uptime">--:--:--</span></span>
  <span class="sep">|</span>
  <span class="tb-stat" id="tb-routes">routes: <span>--</span></span>
  <span class="tb-stat" id="tb-health">backends: <span>--</span></span>
  <span class="tb-stat" id="tb-conns">conns: <span>--</span></span>
  <span class="ml" style="display:flex;align-items:center;gap:12px">
    <span id="auth-actions"></span>
    <span id="rld-msg"></span>
  </span>
</div>

<!-- ── Tabbar ── -->
<div id="tabbar">
  <button class="tab active" onclick="showTab('health')">HEALTH</button>
  <button class="tab" onclick="showTab('status')">STATUS <span class="lock-icon" id="lock-status">[LOCKED]</span></button>
  <button class="tab" onclick="showTab('logs')">LOGS <span class="lock-icon" id="lock-logs">[LOCKED]</span></button>
  <button class="tab" onclick="showTab('config')">CONFIG <span class="lock-icon" id="lock-config">[LOCKED]</span></button>
</div>

<!-- ── Health pane ── -->
<div class="pane active" id="pane-health">
  <div class="health-header">
    <h2>System Health</h2>
    <span class="health-updated" id="health-ts">connecting...</span>
  </div>
  <div id="pwd-banner" class="banner" style="display:none">
    [!] Default password is active. <a href="#" onclick="showAuthOverlay(true);return false;">Set a secure password</a> to manage the gateway.
  </div>
  <div id="health-status-banner"></div>
  <div class="health-grid" id="health-grid">
    <!-- Static framework to avoid complete DOM rewrite (stops flicker/animations) -->
    <div class="health-card"><div class="hc-label">STATUS</div><div class="hc-value hc-blue" id="hc-status">...</div></div>
    <div class="health-card"><div class="hc-label">UPTIME</div><div class="hc-value" id="hc-uptime">--:--:--</div></div>
    <div class="health-card"><div class="hc-label">ROUTES</div><div class="hc-value" id="hc-routes">--</div></div>
    <div class="health-card"><div class="hc-label">BACKENDS</div><div class="hc-value" id="hc-backends">--</div><div class="hc-sub" id="hc-backends-sub"></div></div>
    <div class="health-card"><div class="hc-label">ACTIVE CONNS</div><div class="hc-value" id="hc-conns">--</div></div>
    <div class="health-card"><div class="hc-label">DATA RELAYED</div><div class="hc-value hc-bytes" id="hc-relayed">0 B</div></div>
  </div>
  <div class="health-footer">
    <span id="health-footer-status">Waiting for data...</span>
    <span>Sluice Network Gateway</span>
  </div>
</div>

<!-- ── Status pane ── -->
<div class="pane" id="pane-status">
  <div id="summary" class="strip">loading...</div>
  <div id="sys-bar" class="strip">loading...</div>
  <table class="rtable">
    <thead>
      <tr>
        <th>ROUTE</th><th>PORT</th><th>BACKEND</th>
        <th>STATUS</th><th>ACTIVE</th><th>TOTAL</th><th>RELAYED</th>
        <th>DATAGRAMS</th><th>DROPPED</th>
      </tr>
    </thead>
    <tbody id="rtbody"></tbody>
  </table>
  <div id="drain-section" class="drain-section" style="display:none">
    <div class="drain-hdr">
      <span>DRAINING ROUTES</span>
      <span id="drain-info" class="tag tag-retiring"></span>
    </div>
    <table class="rtable">
      <thead><tr><th>ROUTE</th><th>PORT</th><th>BACKEND</th><th>ACTIVE CONNS</th></tr></thead>
      <tbody id="drain-tbody"></tbody>
    </table>
  </div>
</div>

<!-- ── Logs pane ── -->
<div class="pane" id="pane-logs">
  <div id="log-toolbar">
    <button class="btn" onclick="clearLog()">Clear</button>
    <label><input type="checkbox" id="asc" checked> Auto-scroll</label>
    <span id="log-count">0 lines</span>
  </div>
  <div id="log-out"></div>
</div>

<!-- ── Config pane ── -->
<div class="pane" id="pane-config">
  <div id="cfg-body"><div style="color:var(--dim);font-size:12px;font-family:var(--mono)">Loading configuration...</div></div>
</div>

<script>
let isAuthed = false;
let isDefaultPassword = false;
let pendingTab = null;
let statusInterval = null;
let logsInterval = null;

let logLines = [];
let cfgReady = false;
let reloading = false;
let openAccess = new Set();
let openAffinity = new Set();

let fHb = { enabled: true, interval_seconds: 10 };
let fGlobal = {
    worker_auto: false, worker_processes: 1,
    log_file: '', log_rotation: 'none',
    rate_max: 1000, backoff_base: 60, backoff_max: 3600,
    idle_timeout: 0, udp_timeout: 30, max_udp_sessions: 10000, udp_rate: 0,
    whitelist_text: '', blacklist: []
};
let fRoutes = [];

async function sha256(str) {
    if (crypto && crypto.subtle) {
        const buf = new TextEncoder().encode(str);
        const hash = await crypto.subtle.digest('SHA-256', buf);
        return Array.from(new Uint8Array(hash)).map(b => b.toString(16).padStart(2, '0')).join('');
    }
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193); }
    return 'fb-' + (h >>> 0).toString(16).padStart(8, '0');
}

function authHeaders() {
    const h = {};
    const t = sessionStorage.getItem('sluice_token');
    if (t) h['X-Auth-Token'] = t;
    return h;
}
function needsPasswordChange() {
    return isDefaultPassword;
}
function showAuthOverlay(forceChange) {
    const loginDiv = document.getElementById('auth-login');
    const changeDiv = document.getElementById('auth-change');
    const subtitle = document.getElementById('auth-subtitle-text');
    if (forceChange) {
        loginDiv.style.display = 'none';
        changeDiv.style.display = 'block';
        subtitle.textContent = 'Initial Setup';
    } else {
        loginDiv.style.display = 'block';
        changeDiv.style.display = 'none';
        subtitle.textContent = 'Gateway Management';
    }
    document.getElementById('auth-err').textContent = '';
    document.getElementById('auth-change-err').textContent = '';
    document.getElementById('auth-pwd').value = '';
    document.getElementById('auth-new-pwd').value = '';
    document.getElementById('auth-confirm-pwd').value = '';
    document.getElementById('auth-overlay').classList.add('show');
    setTimeout(() => {
        const el = forceChange ? document.getElementById('auth-new-pwd') : document.getElementById('auth-pwd');
        if (el) el.focus();
    }, 100);
}
function hideAuthOverlay() {
    document.getElementById('auth-overlay').classList.remove('show');
    pendingTab = null;
}
async function doLogin() {
    const pwd = document.getElementById('auth-pwd').value;
    const errEl = document.getElementById('auth-err');
    if (!pwd) { errEl.textContent = 'Password required'; return; }
    const hash = await sha256(pwd);
    try {
        const r = await fetch('/api/login', { method: 'POST', body: JSON.stringify({password_hash: hash}) });
        const d = await r.json();
        if (d.ok) {
            sessionStorage.setItem('sluice_token', d.token);
            isAuthed = true;
            hideAuthOverlay();
            updateAuthUI();
            updateLockIcons();
            startAuthIntervals();
            if (pendingTab) { const t = pendingTab; pendingTab = null; showTab(t); }
        } else {
            errEl.textContent = 'Invalid password';
        }
    } catch {
        errEl.textContent = 'Login request failed';
    }
}
async function doChangePassword() {
    const newPwd = document.getElementById('auth-new-pwd').value;
    const confirm = document.getElementById('auth-confirm-pwd').value;
    const errEl = document.getElementById('auth-change-err');
    if (newPwd.length < 6) { errEl.textContent = 'Minimum 6 characters required'; return; }
    if (newPwd !== confirm) { errEl.textContent = 'Passwords do not match'; return; }
    const hash = await sha256(newPwd);
    try {
        const r = await fetch('/api/password', { method: 'POST', headers: authHeaders(), body: JSON.stringify({password_hash: hash}) });
        const d = await r.json();
        if (d.ok) {
            sessionStorage.setItem('sluice_token', d.token);
            isAuthed = true;
            isDefaultPassword = false;
            hideAuthOverlay();
            updateAuthUI();
            updatePwdBanner();
            updateLockIcons();
            startAuthIntervals();
            if (pendingTab) { const t = pendingTab; pendingTab = null; showTab(t); }
        } else {
            errEl.textContent = 'Failed: ' + (d.error || 'Unknown error');
        }
    } catch {
        errEl.textContent = 'Password update request failed';
    }
}
function doLogout() {
    isAuthed = false;
    sessionStorage.removeItem('sluice_token');
    stopAuthIntervals();
    cfgReady = false;
    showTab('health');
    updateAuthUI();
    updateLockIcons();
}
function updateAuthUI() {
    const el = document.getElementById('auth-actions');
    if (isAuthed) {
        el.innerHTML =
            '<button class="btn" id="btn-reload" onclick="triggerReload()">RELOAD CONFIG</button>' +
            '<button class="btn btn-danger" onclick="doLogout()">LOGOUT</button>';
    } else {
        el.innerHTML = '<button class="btn btn-primary" onclick="showAuthOverlay(needsPasswordChange())">LOGIN</button>';
    }
}
function updateLockIcons() {
    ['status','logs','config'].forEach(t => {
        const el = document.getElementById('lock-' + t);
        if (el) el.style.display = isAuthed ? 'none' : 'inline';
    });
}
function updatePwdBanner() {
    document.getElementById('pwd-banner').style.display = needsPasswordChange() ? 'block' : 'none';
}
function startAuthIntervals() {
    if (!statusInterval) { fetchStatus(); statusInterval = setInterval(fetchStatus, 3000); }
    if (!logsInterval)   { fetchLogs();   logsInterval   = setInterval(fetchLogs, 5000);   }
}
function stopAuthIntervals() {
    if (statusInterval) { clearInterval(statusInterval); statusInterval = null; }
    if (logsInterval)   { clearInterval(logsInterval);   logsInterval   = null; }
}
async function initAuth() {
    const t = sessionStorage.getItem('sluice_token');
    isAuthed = !!t;
    updateAuthUI();
    updateLockIcons();
    if (isAuthed) startAuthIntervals();
}

function esc(s){ return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;') }
function fmtBytes(b){if(!b||b===0)return'0 B';const k=1024,s=['B','KB','MB','GB','TB'];const i=Math.min(Math.floor(Math.log(b)/Math.log(k)),s.length-1);return(b/Math.pow(k,i)).toFixed(i>0?1:0)+' '+s[i]}
function fmtUptime(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;return[h,m,ss].map(v=>String(v).padStart(2,'0')).join(':')}
function validIPv4(s){return/^(\d{1,3}\.){3}\d{1,3}$/.test(s)&&s.split('.').every(p=>+p>=0&&+p<=255)}
const V6RE=/^(([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|:((:[0-9a-fA-F]{1,4}){1,7}|:)|::(ffff(:0{1,4}){0,1}:){0,1}((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.){3,3}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])|([0-9a-fA-F]{1,4}:){1,4}:((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.){3,3}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9]))$/;
function validIPv6(s){return V6RE.test(s)}
function validIP(s){return validIPv4(s)||validIPv6(s)}
function validCidr(s){const i=s.indexOf('/');if(i<0)return false;const addr=s.slice(0,i),plen=s.slice(i+1);if(!/^\d{1,3}$/.test(plen))return false;const n=+plen;if(validIPv4(addr))return n>=0&&n<=32;if(validIPv6(addr))return n>=0&&n<=128;return false;}
function validIPOrCidr(s){return s.includes('/')?validCidr(s):validIP(s)}
function validPort(p){const n=+p;return Number.isInteger(n)&&n>=1&&n<=65535}
function validDT(s){if(!s||!s.trim())return true;return/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?$/.test(s.trim())}
function normUntil(s){s=(s||'').trim();if(!s)return null;return/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}$/.test(s)?s+':00':s}
function ce(id){const e=document.getElementById('e-'+id);if(e)e.textContent='';const i=document.getElementById(id);if(i)i.classList.remove('err')}
function se(id,msg){const e=document.getElementById('e-'+id);if(e)e.textContent=msg;const i=document.getElementById(id);if(i)i.classList.add('err')}

function showTab(name){
    if(['status','logs','config'].includes(name) && !isAuthed){
        pendingTab = name;
        showAuthOverlay(needsPasswordChange());
        return;
    }
    document.querySelectorAll('.pane').forEach(p=>p.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
    document.getElementById('pane-'+name).classList.add('active');
    const tabNames=['health','status','logs','config'];
    document.querySelectorAll('.tab')[tabNames.indexOf(name)].classList.add('active');
    if(name==='config'&&!cfgReady) loadCfg();
}

async function fetchHealth(){
    try{
        const d=await(await fetch('/api/health')).json();
        isDefaultPassword = d.default_password_active === true;
        updatePwdBanner();
        renderHealth(d);
        updateTopbar(d);
    } catch {
        document.getElementById('led').className='led down';
        document.getElementById('status-label').textContent='UNREACHABLE';
        document.getElementById('status-label').style.color='var(--down)';
        renderHealthOffline();
    }
}
function updateTopbar(d){
    document.getElementById('uptime').textContent=fmtUptime(d.uptime_seconds||0);
    document.getElementById('tb-routes').innerHTML='routes: <span>'+d.routes_total+'</span>';
    const hu=d.backends_healthy,tb=d.backends_total;
    const hc=hu<tb?'<span style="color:var(--down)">'+hu+'/'+tb+'</span>':'<span style="color:var(--up)">'+hu+'/'+tb+'</span>';
    document.getElementById('tb-health').innerHTML='backends: '+hc;
    document.getElementById('tb-conns').innerHTML='conns: <span>'+d.active_connections+'</span>';
    if(d.status==='healthy'){
        document.getElementById('led').className='led up';
        document.getElementById('status-label').textContent='HEALTHY';
        document.getElementById('status-label').style.color='var(--up)';
    } else if(d.status==='degraded'){
        document.getElementById('led').className='led warn';
        document.getElementById('status-label').textContent='DEGRADED';
        document.getElementById('status-label').style.color='var(--warn)';
    } else {
        document.getElementById('led').className='led down';
        document.getElementById('status-label').textContent='CRITICAL';
        document.getElementById('status-label').style.color='var(--down)';
    }
}
function renderHealth(d){
    const st=d.status;
    const bannerCls=st==='healthy'?'h-ok':st==='degraded'?'h-warn':'h-err';
    const bannerTxt=st==='healthy'?'[OK] All Systems Operational':st==='degraded'?'[WARN] Partial System Degradation':'[ERR] Major System Outage';
    
    document.getElementById('health-status-banner').innerHTML=
        '<div class="h-banner '+bannerCls+'"><span>'+bannerTxt+'</span></div>';

    const hu=d.backends_healthy,tb=d.backends_total;
    const beCls=tb===0?'':hu<tb?(hu===0?'hc-err':'hc-warn'):'hc-ok';
    
    document.getElementById('hc-status').textContent = st.toUpperCase();
    document.getElementById('hc-status').className = 'hc-value ' + (st==='healthy'?'hc-ok':st==='degraded'?'hc-warn':'hc-err');
    
    document.getElementById('hc-uptime').textContent = fmtUptime(d.uptime_seconds||0);
    document.getElementById('hc-routes').textContent = d.routes_total;
    
    document.getElementById('hc-backends').textContent = hu+'/'+tb;
    document.getElementById('hc-backends').className = 'hc-value ' + beCls;
    document.getElementById('hc-backends-sub').textContent = 'healthy / total';
    
    document.getElementById('hc-conns').textContent = d.active_connections;
    document.getElementById('hc-relayed').textContent = fmtBytes(d.total_bytes_relayed);

    document.getElementById('health-ts').textContent = 'Last update: ' + new Date().toLocaleTimeString();
    document.getElementById('health-footer-status').textContent = st==='healthy'?'System operations normal':'Degraded operations';
}
function renderHealthOffline(){
    document.getElementById('health-status-banner').innerHTML=
        '<div class="h-banner h-err"><span>[ERR] Gateway Unreachable</span></div>';
    document.getElementById('health-footer-status').textContent='Unable to connect to gateway instance';
}

async function fetchStatus(){
    try{
        const r=await fetch('/api/status',{headers:authHeaders()});
        if(!r.ok) return;
        const d=await r.json();
        renderStatus(d);
    } catch {}
}
function backendBadge(b){
    if(b.retiring) return'<span class="tag tag-retiring">RETIRING</span>';
    if(b.disabled) return'<span class="tag tag-disabled">DISABLED</span>';
    if(b.health_checked===false){
        // UDP: not actively probed, so a presumed-OK backend gets a neutral
        // marker rather than a misleading "UP". But a passive failure (a real
        // ECONNREFUSED from the kernel, not a guess) is a confirmed signal —
        // show DOWN for that exactly like an actively-probed backend would.
        if(!b.healthy) return'<span class="tag tag-down">DOWN</span>';
        return'<span class="tag tag-na" title="UDP: not actively health-checked">—</span>';
    }
    return b.healthy?'<span class="tag tag-up">UP</span>':'<span class="tag tag-down">DOWN</span>';
}
function renderStatus(d){
    let tr=d.routes.length,tb=0,hu=0,ac=0,by=0;
    d.routes.forEach(r=>r.backends.forEach(b=>{tb++;if(b.healthy&&!b.disabled&&!b.retiring)hu++;ac+=b.active_connections;by+=b.bytes_relayed;}));
    document.getElementById('summary').innerHTML=
        '<span class="sum-item">ROUTES <span>'+tr+'</span></span>'+
        '<span class="sum-item">BACKENDS <span class="'+(hu<tb?'bad':'ok')+'">'+hu+'/'+tb+' OK</span></span>'+
        '<span class="sum-item">ACTIVE CONNS <span>'+ac+'</span></span>'+
        '<span class="sum-item">DATA RELAYED <span class="b">'+fmtBytes(by)+'</span></span>';
    const wp=d.worker_processes||1;
    const logStr=d.log_file?esc(d.log_file)+' ('+esc(d.log_rotation||'none')+')':'stderr';
    document.getElementById('sys-bar').innerHTML=
        '<span class="sum-item">WORKERS <span>'+wp+'</span></span>'+
        '<span class="sum-item">LOG DEST <span>'+logStr+'</span></span>';
    
    let rows='';
    d.routes.forEach(route=>{
        const udp=route.protocol==='udp';
        const protoBadge=' <span class="tag '+(udp?'tag-udp':'tag-tcp')+'">'+(udp?'UDP':'TCP')+'</span>';
        // For UDP, the "active" count is synthesized sessions, not connections.
        const actLabel=udp?' <span class="dimval" style="font-size:9px">sess</span>':'';
        route.backends.forEach((b,bi)=>{
            const dgFwd  = b.datagrams_forwarded  || 0;
            const dgDrop = b.datagrams_dropped_rate || 0;
            const maxDgPill = (bi===0 && udp && route.max_datagram_bytes > 0)
                ? ' <span class="tag tag-na" title="max datagram size">≤'+route.max_datagram_bytes+'B</span>'
                : '';
            rows+='<tr>'+
                '<td>'+(bi===0?'<span class="rname">'+esc(route.name)+'</span>'+protoBadge+maxDgPill:'')+'</td>'+
                '<td>'+(bi===0?'<span class="port">:'+route.listen_port+'</span>':'')+'</td>'+
                '<td><span class="addr">'+esc(b.host)+':'+b.port+'</span></td>'+
                '<td>'+backendBadge(b)+'</td>'+
                '<td>'+b.active_connections+actLabel+'</td>'+
                '<td><span class="dimval">'+Number(b.total_connections).toLocaleString()+'</span></td>'+
                '<td><span class="dimval">'+fmtBytes(b.bytes_relayed)+'</span></td>'+
                (udp
                    ? '<td><span class="dimval">'+Number(dgFwd).toLocaleString()+'</span></td>'+
                      '<td><span class="dimval'+(dgDrop>0?' bad':'')+'">'+Number(dgDrop).toLocaleString()+'</span></td>'
                    : '<td><span class="dimval" title="TCP">—</span></td>'+
                      '<td><span class="dimval" title="TCP">—</span></td>'
                )+
            '</tr>';
        });
    });
    document.getElementById('rtbody').innerHTML=rows||'<tr><td colspan="9" class="no-routes">No routes configured</td></tr>';
    
    const dr=d.draining||[];
    const dsec=document.getElementById('drain-section');
    if(dr.length>0){
        dsec.style.display='block';
        let total=0,drows='';
        dr.forEach(r=>{r.backends.forEach((b,bi)=>{total+=b.active_connections;drows+='<tr>'+
            '<td>'+(bi===0?'<span class="rname">'+esc(r.name)+'</span>':'')+'</td>'+
            '<td>'+(bi===0?'<span class="port">:'+r.listen_port+'</span>':'')+'</td>'+
            '<td><span class="addr">'+esc(b.host)+':'+b.port+'</span></td>'+
            '<td>'+b.active_connections+'</td></tr>';
        });});
        document.getElementById('drain-info').textContent=dr.length+' route(s) | '+total+' conn(s) remaining';
        document.getElementById('drain-tbody').innerHTML=drows;
    } else dsec.style.display='none';
}

async function triggerReload(){
    if(reloading)return;
    reloading=true;
    const btn=document.getElementById('btn-reload');
    const msg=document.getElementById('rld-msg');
    btn.disabled=true; btn.textContent='RELOADING...'; msg.textContent='';
    try{
        const r=await fetch('/api/reload',{method:'POST',headers:authHeaders()});
        const d=await r.json();
        if(d.ok){
            msg.textContent='Reload signal sent';
            setTimeout(async()=>{await fetchHealth();msg.textContent='';},2000);
        } else { msg.textContent='FAILED'; }
    } catch { msg.textContent='REQUEST FAILED'; }
    setTimeout(()=>{btn.disabled=false;btn.textContent='RELOAD CONFIG';reloading=false;},2500);
}

async function fetchLogs(){
    try{
        const r=await fetch('/api/logs',{headers:authHeaders()});
        if(!r.ok) return;
        logLines=await r.json();renderLogs();
    }catch{}
}
function renderLogs(){
    const box=document.getElementById('log-out');
    const atBottom=box.scrollHeight-box.clientHeight<=box.scrollTop+30;
    box.innerHTML=logLines.map(l=>'<div class="'+(l.includes('[ERR]')?'le':'ll')+'">'+esc(l)+'</div>').join('');
    document.getElementById('log-count').textContent=logLines.length+' lines';
    if(document.getElementById('asc').checked&&atBottom)box.scrollTop=box.scrollHeight;
}
function clearLog(){logLines=[];renderLogs();}

function getBlArr(prefix){
    if(prefix==='g')return fGlobal.blacklist;
    const m=prefix.match(/^r(\d+)-b(\d+)$/);
    return m?fRoutes[+m[1]].backends[+m[2]].blacklist:null;
}
function setBl(prefix,i,field,val){const a=getBlArr(prefix);if(a&&a[i])a[i][field]=val;}
function addBl(prefix){const a=getBlArr(prefix);if(a){a.push({ip:'',until:''});renderCfg();}}
function rmBl(prefix,i){const a=getBlArr(prefix);if(a){a.splice(i,1);renderCfg();}}
function setWl(prefix,val){
    if(prefix==='g'){fGlobal.whitelist_text=val;return;}
    const m=prefix.match(/^r(\d+)-b(\d+)$/);if(m)fRoutes[+m[1]].backends[+m[2]].whitelist_text=val;
}
function toggleAccess(ri,bi){
    const k=ri+'-'+bi;
    if(openAccess.has(k))openAccess.delete(k);else openAccess.add(k);
    renderCfg();
}
function toggleAffinity(ri){
    if(openAffinity.has(ri))openAffinity.delete(ri);else openAffinity.add(ri);
    renderCfg();
}

async function loadCfg(){
    cfgReady=true;
    try{
        const r=await fetch('/api/config',{headers:authHeaders()});
        if(!r.ok)throw new Error();
        const cfg=await r.json();
        fHb={
            enabled:cfg.heartbeat?cfg.heartbeat.enabled!==false:true,
            interval_seconds:(cfg.heartbeat&&cfg.heartbeat.interval_seconds)||10
        };
        const wp=cfg.worker_processes;
        fGlobal.worker_auto=(wp==='auto');
        fGlobal.worker_processes=(wp==='auto'||wp==null)?1:+wp;
        fGlobal.log_file=cfg.log_file||'';
        fGlobal.log_rotation=cfg.log_rotation||'none';
        const rl=cfg.rate_limit||{};
        fGlobal.rate_max=rl.max_per_minute||1000;
        fGlobal.backoff_base=rl.backoff_base_seconds||60;
        fGlobal.backoff_max=rl.backoff_max_seconds||3600;
        fGlobal.idle_timeout=cfg.idle_timeout_seconds||0;
        fGlobal.udp_timeout=(cfg.udp_session_timeout_seconds!=null)?cfg.udp_session_timeout_seconds:30;
        fGlobal.max_udp_sessions=(cfg.max_udp_sessions!=null)?cfg.max_udp_sessions:10000;
        fGlobal.udp_rate=(cfg.udp_datagram_rate_limit!=null)?cfg.udp_datagram_rate_limit:0;
        fGlobal.whitelist_text=(cfg.whitelist||[]).join('\n');
        fGlobal.blacklist=(cfg.blacklist||[]).map(e=>typeof e==='string'?{ip:e,until:''}:{ip:e.ip||'',until:e.until||''});
        fRoutes=(cfg.routes||[]).map(r=>({
            name:r.name||'', listen_port:r.listen_port||'',
            protocol:(r.protocol==='udp')?'udp':'tcp',
            idle_timeout:(r.idle_timeout_seconds!=null)?String(r.idle_timeout_seconds):'',
            udp_timeout:(r.udp_session_timeout_seconds!=null)?String(r.udp_session_timeout_seconds):'',
            udp_rate:(r.udp_datagram_rate_limit!=null)?String(r.udp_datagram_rate_limit):'',
            affinity:(r.affinity||[]).map(a=>({client:a.client||'',host:a.host||'',port:a.port||''})),
            backends:(r.backends||[]).map(b=>({
                host:b.host||'', port:b.port||'', heartbeat:b.heartbeat!==false,
                disabled:b.disabled||false, proxy_protocol:b.proxy_protocol||false,
                rate_max:(b.rate_limit&&b.rate_limit.max_per_minute)||0,
                whitelist_text:(b.whitelist||[]).join('\n'),
                blacklist:(b.blacklist||[]).map(e=>typeof e==='string'?{ip:e,until:''}:{ip:e.ip||'',until:e.until||''})
            }))
        }));
    } catch {
        fRoutes=[{name:'',listen_port:'',protocol:'tcp',idle_timeout:'',udp_timeout:'',udp_rate:'',affinity:[],backends:[{host:'',port:'',heartbeat:true,disabled:false,proxy_protocol:false,rate_max:0,whitelist_text:'',blacklist:[]}]}];
    }
    openAccess.clear();
    openAffinity.clear();
    renderCfg();
}

function renderBl(arr,prefix){
    return (arr||[]).map((e,i)=>`
        <div class="bl-row">
            <div class="fld fld-m">
                <input type="text" id="${prefix}-bl-ip-${i}" value="${esc(e.ip)}" placeholder="192.168.1.1 or 10.0.0.0/24"
                    oninput="setBl('${prefix}',${i},'ip',this.value)">
                <span class="emsg" id="e-${prefix}-bl-ip-${i}"></span>
            </div>
            <div class="fld fld-l">
                <input type="text" id="${prefix}-bl-until-${i}" value="${esc(e.until)}"
                    placeholder="empty = permanent  |  YYYY-MM-DDTHH:MM"
                    oninput="setBl('${prefix}',${i},'until',this.value)">
                <span class="emsg" id="e-${prefix}-bl-until-${i}"></span>
            </div>
            <div class="fld" style="align-self:flex-end">
                <button class="btn btn-danger" style="padding:4px 8px" onclick="rmBl('${prefix}',${i})">Remove</button>
            </div>
        </div>`).join('');
}

function renderAccessPanel(prefix,wlText,blArr){
    return `
        <div class="fld" style="margin-bottom:12px">
            <label>WHITELIST (One IP/CIDR per line, empty = allow all)</label>
            <textarea rows="3" id="${prefix}-wl" placeholder="192.168.1.1&#10;10.0.0.0/24" oninput="setWl('${prefix}',this.value)">${esc(wlText||'')}</textarea>
            <span class="emsg" id="e-${prefix}-wl"></span>
        </div>
        <div class="sec-label">BLACKLIST</div>
        ${renderBl(blArr,prefix)}
        <button class="btn-text" onclick="addBl('${prefix}')">+ Add Blacklist Entry</button>`;
}

function setAffinity(ri,i,field,val){if(fRoutes[ri]&&fRoutes[ri].affinity[i])fRoutes[ri].affinity[i][field]=val;}
function addAffinity(ri){fRoutes[ri].affinity.push({client:'',host:'',port:''});renderCfg();}
function rmAffinity(ri,i){fRoutes[ri].affinity.splice(i,1);renderCfg();}

function renderAffinityPanel(ri,rules){
    const rows=(rules||[]).map((a,i)=>`
        <div class="af-row">
            <div class="fld fld-m">
                <label>CLIENT (IP or CIDR)</label>
                <input type="text" id="af-${ri}-${i}-c" value="${esc(a.client)}" placeholder="10.0.0.0/24 or 1.2.3.4"
                    oninput="setAffinity(${ri},${i},'client',this.value)">
                <span class="emsg" id="e-af-${ri}-${i}-c"></span>
            </div>
            <div class="fld fld-m">
                <label>BACKEND HOST</label>
                <input type="text" id="af-${ri}-${i}-h" value="${esc(a.host)}" placeholder="127.0.0.1"
                    oninput="setAffinity(${ri},${i},'host',this.value)">
                <span class="emsg" id="e-af-${ri}-${i}-h"></span>
            </div>
            <div class="fld fld-s">
                <label>BACKEND PORT</label>
                <input type="number" id="af-${ri}-${i}-p" value="${esc(a.port)}" min="1" max="65535"
                    oninput="setAffinity(${ri},${i},'port',+this.value)">
                <span class="emsg" id="e-af-${ri}-${i}-p"></span>
            </div>
            <div class="fld" style="align-self:flex-end">
                <button class="btn btn-danger" style="padding:4px 8px" onclick="rmAffinity(${ri},${i})">Remove</button>
            </div>
        </div>`).join('');
    return `<div class="af-panel">
        <div class="sec-label" style="margin-bottom:8px">AFFINITY RULES
            <span style="font-weight:normal;text-transform:none;font-size:10px;color:var(--dim)">
                &nbsp;— first matching rule wins; falls back to round-robin if backend ineligible
            </span>
        </div>
        ${rows||'<div style="color:var(--dim);font-size:11px;margin-bottom:8px">No rules — all clients use round-robin</div>'}
        <button class="btn-text" onclick="addAffinity(${ri})">+ Add Affinity Rule</button>
    </div>`;
}

function renderCfg(){
    const wauto=fGlobal.worker_auto;
    document.getElementById('cfg-body').innerHTML=`
    <div class="cfg-block">
      <div class="cfg-hdr">GLOBAL SETTINGS</div>
      <div class="cfg-body">
        <div class="frow">
          <div class="fld fld-s">
            <label>WORKERS</label>
            <input type="number" id="g-workers" value="${fGlobal.worker_processes}" min="1"
              ${wauto?'disabled':''} oninput="fGlobal.worker_processes=+this.value;ce('g-workers')">
            <span class="emsg" id="e-g-workers"></span>
          </div>
          <div class="fld" style="justify-content:flex-end">
            <label>&nbsp;</label>
            <div class="chk-row">
              <input type="checkbox" id="g-wauto" ${wauto?'checked':''}
                onchange="fGlobal.worker_auto=this.checked;renderCfg()">
              <span style="font-size:11px">Auto</span>
            </div>
          </div>
          <div class="fld fld-l">
            <label>LOG FILE (Empty = stderr)</label>
            <input type="text" id="g-logfile" value="${esc(fGlobal.log_file)}"
              placeholder="/var/log/gateway.log" oninput="fGlobal.log_file=this.value">
          </div>
          <div class="fld fld-s">
            <label>ROTATION</label>
            <select id="g-logrot" onchange="fGlobal.log_rotation=this.value">
              <option value="none" ${fGlobal.log_rotation==='none'?'selected':''}>None</option>
              <option value="hourly" ${fGlobal.log_rotation==='hourly'?'selected':''}>Hourly</option>
              <option value="daily" ${fGlobal.log_rotation==='daily'?'selected':''}>Daily</option>
            </select>
          </div>
        </div>
      </div>
    </div>

    <div class="cfg-block">
      <div class="cfg-hdr">GLOBAL RATE LIMIT (Per IP, per worker)</div>
      <div class="cfg-body">
        <div class="frow">
          <div class="fld fld-s">
            <label>MAX / MIN</label>
            <input type="number" id="g-ratemax" value="${fGlobal.rate_max}" min="1"
              oninput="fGlobal.rate_max=+this.value;ce('g-ratemax')">
            <span class="emsg" id="e-g-ratemax"></span>
          </div>
          <div class="fld fld-s">
            <label>BACKOFF BASE (s)</label>
            <input type="number" id="g-ratebase" value="${fGlobal.backoff_base}" min="1"
              oninput="fGlobal.backoff_base=+this.value;ce('g-ratebase')">
            <span class="emsg" id="e-g-ratebase"></span>
          </div>
          <div class="fld fld-s">
            <label>BACKOFF MAX (s)</label>
            <input type="number" id="g-ratecap" value="${fGlobal.backoff_max}" min="1"
              oninput="fGlobal.backoff_max=+this.value;ce('g-ratecap')">
            <span class="emsg" id="e-g-ratecap"></span>
          </div>
        </div>
      </div>
    </div>

    <div class="cfg-block">
      <div class="cfg-hdr">GLOBAL ACCESS CONTROL</div>
      <div class="cfg-body">${renderAccessPanel('g',fGlobal.whitelist_text,fGlobal.blacklist)}</div>
    </div>

    <div class="cfg-block">
      <div class="cfg-hdr">CONNECTION SETTINGS</div>
      <div class="cfg-body">
        <div class="frow">
          <div class="fld fld-s">
            <label>IDLE TIMEOUT (s)</label>
            <input type="number" id="g-idle" value="${fGlobal.idle_timeout}" min="0"
              oninput="fGlobal.idle_timeout=+this.value;ce('g-idle')">
            <span class="emsg" id="e-g-idle"></span>
          </div>
          <div class="fld fld-s">
            <label>UDP TIMEOUT (s)</label>
            <input type="number" id="g-udpto" value="${fGlobal.udp_timeout}" min="5"
              oninput="fGlobal.udp_timeout=+this.value;ce('g-udpto')">
            <span class="emsg" id="e-g-udpto"></span>
          </div>
          <div class="fld fld-s">
            <label>MAX UDP SESSIONS</label>
            <input type="number" id="g-udpmax" value="${fGlobal.max_udp_sessions}" min="0"
              oninput="fGlobal.max_udp_sessions=+this.value;ce('g-udpmax')">
            <span class="emsg" id="e-g-udpmax"></span>
          </div>
          <div class="fld fld-s">
            <label>UDP DATAGRAM RATE /s</label>
            <input type="number" id="g-udprate" value="${fGlobal.udp_rate}" min="0" placeholder="0 = unlimited"
              oninput="fGlobal.udp_rate=+this.value;ce('g-udprate')">
            <span class="emsg" id="e-g-udprate"></span>
          </div>
          <div class="fld fld-s" style="margin-left:16px">
            <label>HEARTBEAT</label>
            <div class="chk-row">
              <input type="checkbox" id="hb-en" ${fHb.enabled?'checked':''}
                onchange="fHb.enabled=this.checked">
              <span style="font-size:11px">Enabled</span>
            </div>
          </div>
          <div class="fld fld-s">
            <label>HB INTERVAL (s)</label>
            <input type="number" id="hb-iv" value="${fHb.interval_seconds}" min="1" max="3600"
              oninput="fHb.interval_seconds=+this.value">
            <span class="emsg" id="e-hb-iv"></span>
          </div>
        </div>
      </div>
    </div>

    ${fRoutes.map((rt,ri)=>routeBlock(rt,ri)).join('')}
    <button class="btn" onclick="addRoute()" style="width:100%;padding:10px">+ ADD ROUTE</button>

    <div class="save-bar">
      <button class="btn btn-primary" id="btn-sv" onclick="saveCfg()" style="padding:8px 24px">SAVE CONFIGURATION</button>
      <span style="font-size:11px;color:var(--dim)">Restart gateway to apply network binding changes</span>
      <span id="smsg" class="smsg"></span>
    </div>`;
}

function routeBlock(rt,ri){
    const udp=rt.protocol==='udp';
    const timeoutFld = udp
      ? `<div class="fld fld-s">
            <label>SESSION TIMEOUT (s)</label>
            <input type="text" id="r${ri}-udpto" value="${esc(rt.udp_timeout)}" placeholder="inherit"
              oninput="fRoutes[${ri}].udp_timeout=this.value;ce('r${ri}-udpto')">
            <span class="emsg" id="e-r${ri}-udpto"></span>
          </div>
          <div class="fld fld-s">
            <label>DATAGRAM RATE /s</label>
            <input type="text" id="r${ri}-udprate" value="${esc(rt.udp_rate)}" placeholder="inherit"
              oninput="fRoutes[${ri}].udp_rate=this.value;ce('r${ri}-udprate')">
            <span class="emsg" id="e-r${ri}-udprate"></span>
          </div>`
      : `<div class="fld fld-s">
            <label>IDLE TIMEOUT (s)</label>
            <input type="text" id="r${ri}-idle" value="${esc(rt.idle_timeout)}" placeholder="inherit"
              oninput="fRoutes[${ri}].idle_timeout=this.value;ce('r${ri}-idle')">
            <span class="emsg" id="e-r${ri}-idle"></span>
          </div>`;
    const note = udp
      ? `<div></div>`
      : '';
    const affinityOpen=openAffinity.has(ri);
    const hasAffinity=rt.affinity&&rt.affinity.length>0;
    return `<div class="cfg-block" id="rb-${ri}">
      <div class="cfg-hdr">
        <span>ROUTE: ${esc(rt.name||`Unnamed ${ri+1}`)} <span class="tag ${udp?'tag-udp':'tag-tcp'}">${udp?'UDP':'TCP'}</span></span>
        <div style="display:flex;gap:8px;align-items:center">
          <button class="btn" onclick="toggleAffinity(${ri})">
            ${affinityOpen?'Hide Affinity':(hasAffinity?'Affinity [Configured]':'Affinity [None]')}
          </button>
          <button class="btn btn-danger" onclick="rmRoute(${ri})">Delete Route</button>
        </div>
      </div>
      <div class="cfg-body">
        <div class="frow">
          <div class="fld fld-l">
            <label>ROUTE NAME</label>
            <input type="text" id="r${ri}-n" value="${esc(rt.name)}" placeholder="e.g. proxy_main"
              oninput="fRoutes[${ri}].name=this.value;ce('r${ri}-n')">
            <span class="emsg" id="e-r${ri}-n"></span>
          </div>
          <div class="fld fld-s">
            <label>PROTOCOL</label>
            <select id="r${ri}-proto" onchange="setProto(${ri},this.value)">
              <option value="tcp" ${udp?'':'selected'}>TCP</option>
              <option value="udp" ${udp?'selected':''}>UDP</option>
            </select>
          </div>
          <div class="fld fld-s">
            <label>LISTEN PORT</label>
            <input type="number" id="r${ri}-p" value="${rt.listen_port}" min="1" max="65535"
              oninput="fRoutes[${ri}].listen_port=+this.value;ce('r${ri}-p')">
            <span class="emsg" id="e-r${ri}-p"></span>
          </div>
          ${timeoutFld}
        </div>
        ${note}
        <div class="sec-label">BACKENDS</div>
        ${rt.backends.map((b,bi)=>backendRow(b,ri,bi,udp)).join('')}
        <button class="btn-text" onclick="addBe(${ri})">+ Add Backend</button>
      </div>
      ${affinityOpen?renderAffinityPanel(ri,rt.affinity):''}
    </div>`;
}

function backendRow(b,ri,bi,udp){
    const prefix=`r${ri}-b${bi}`;
    const key=`${ri}-${bi}`;
    const isOpen=openAccess.has(key);
    const hasAC=!!((b.whitelist_text&&b.whitelist_text.trim())||(b.blacklist&&b.blacklist.length)||(+b.rate_max>0));
    // HB is TCP-only — hidden for UDP (heartbeat probes have no meaning for datagrams).
    // PROXY protocol applies to both: v1 text header for TCP, v2 binary header for UDP.
    const tcpToggles = udp ? '' : `
        <div class="fld">
          <label>HB</label>
          <div class="chk-row">
            <input type="checkbox" id="${prefix}-hb" ${b.heartbeat?'checked':''}
              onchange="fRoutes[${ri}].backends[${bi}].heartbeat=this.checked">
          </div>
        </div>`;
    const proxyToggle = `
        <div class="fld">
          <label>${udp?'PROXY v2':'PROXY v1'}</label>
          <div class="chk-row">
            <input type="checkbox" id="${prefix}-pp" ${b.proxy_protocol?'checked':''}
              onchange="fRoutes[${ri}].backends[${bi}].proxy_protocol=this.checked">
          </div>
        </div>`;
    return `<div class="be-wrap" id="be-${prefix}">
      <div class="be-main">
        <div class="fld fld-m">
          <label>HOST IP</label>
          <input type="text" id="${prefix}-h" value="${esc(b.host)}" placeholder="127.0.0.1"
            oninput="fRoutes[${ri}].backends[${bi}].host=this.value;ce('${prefix}-h')">
          <span class="emsg" id="e-${prefix}-h"></span>
        </div>
        <div class="fld fld-s">
          <label>PORT</label>
          <input type="number" id="${prefix}-p" value="${b.port}" min="1" max="65535"
            oninput="fRoutes[${ri}].backends[${bi}].port=+this.value;ce('${prefix}-p')">
          <span class="emsg" id="e-${prefix}-p"></span>
        </div>
        <div class="fld fld-s">
          <label>RATE /MIN</label>
          <input type="number" id="${prefix}-rate" value="${b.rate_max||0}" min="0"
            oninput="fRoutes[${ri}].backends[${bi}].rate_max=+this.value;ce('${prefix}-rate')">
          <span class="emsg" id="e-${prefix}-rate"></span>
        </div>
        <div class="fld">
          <label>DISABLE</label>
          <div class="chk-row">
            <input type="checkbox" id="${prefix}-dis" ${b.disabled?'checked':''}
              onchange="fRoutes[${ri}].backends[${bi}].disabled=this.checked">
          </div>
        </div>
        ${tcpToggles}${proxyToggle}
        <div class="fld" style="justify-content:flex-end">
          <button class="btn" onclick="toggleAccess(${ri},${bi})">
            ${isOpen ? 'Hide Access' : (hasAC ? 'Access [Configured]' : 'Access [Default]')}
          </button>
        </div>
        <div class="fld" style="justify-content:flex-end;margin-left:auto">
          <button class="btn btn-danger" onclick="rmBe(${ri},${bi})">Delete</button>
        </div>
      </div>
      ${isOpen?`<div class="be-access">${renderAccessPanel(prefix,b.whitelist_text,b.blacklist)}</div>`:''}
    </div>`;
}

function addRoute(){fRoutes.push({name:'',listen_port:'',protocol:'tcp',idle_timeout:'',udp_timeout:'',udp_rate:'',affinity:[],backends:[{host:'',port:'',heartbeat:true,disabled:false,proxy_protocol:false,rate_max:0,whitelist_text:'',blacklist:[]}]});renderCfg();}
function setProto(ri,v){fRoutes[ri].protocol=v;renderCfg();}
function rmRoute(i){if(fRoutes.length<=1){alert('At least one route required.');return;}fRoutes.splice(i,1);renderCfg();}
function addBe(ri){fRoutes[ri].backends.push({host:'',port:'',heartbeat:true,disabled:false,proxy_protocol:false,rate_max:0,whitelist_text:'',blacklist:[]});renderCfg();}
function rmBe(ri,bi){if(fRoutes[ri].backends.length<=1){alert('At least one backend required.');return;}fRoutes[ri].backends.splice(bi,1);renderCfg();}

function validateAC(prefix,wlText,blArr){
    let ok=true;
    const wlIPs=(wlText||'').split('\n').map(s=>s.trim()).filter(s=>s);
    let wlErr='';
    for(const ip of wlIPs){if(!validIPOrCidr(ip)){wlErr='Invalid IP/CIDR: "'+ip+'"';ok=false;break;}}
    if(wlErr)se(prefix+'-wl',wlErr);else ce(prefix+'-wl');
    (blArr||[]).forEach((e,i)=>{
        if(e.ip.trim()&&!validIPOrCidr(e.ip.trim())){se(prefix+'-bl-ip-'+i,'Invalid IP/CIDR');ok=false;}else ce(prefix+'-bl-ip-'+i);
        if(!validDT(e.until)){se(prefix+'-bl-until-'+i,'Format: YYYY-MM-DDTHH:MM');ok=false;}else ce(prefix+'-bl-until-'+i);
    });
    return ok;
}
function validateCfg(){
    let ok=true;
    const iv=+fHb.interval_seconds;
    if(!iv||iv<1){document.getElementById('e-hb-iv').textContent='Must be >= 1';ok=false;}
    else document.getElementById('e-hb-iv').textContent='';
    if(!fGlobal.worker_auto){const wp=+fGlobal.worker_processes;if(!wp||wp<1){se('g-workers','Must be >= 1');ok=false;}else ce('g-workers');}
    if(+fGlobal.rate_max<1){se('g-ratemax','Must be >= 1');ok=false;}else ce('g-ratemax');
    if(+fGlobal.backoff_base<1){se('g-ratebase','Must be >= 1');ok=false;}else ce('g-ratebase');
    if(+fGlobal.backoff_max<1){se('g-ratecap','Must be >= 1');ok=false;}else ce('g-ratecap');
    if(!validateAC('g',fGlobal.whitelist_text,fGlobal.blacklist))ok=false;
    if(fGlobal.idle_timeout===''||+fGlobal.idle_timeout<0||!Number.isInteger(+fGlobal.idle_timeout)){se('g-idle','Must be >= 0');ok=false;}else ce('g-idle');
    if(+fGlobal.udp_timeout<5||!Number.isInteger(+fGlobal.udp_timeout)){se('g-udpto','Must be >= 5');ok=false;}else ce('g-udpto');
    if(+fGlobal.max_udp_sessions<0||!Number.isInteger(+fGlobal.max_udp_sessions)){se('g-udpmax','Must be >= 0');ok=false;}else ce('g-udpmax');
    if(+fGlobal.udp_rate<0||!Number.isInteger(+fGlobal.udp_rate)){se('g-udprate','Must be >= 0');ok=false;}else ce('g-udprate');
    const seenN=new Set(),seenP=new Set();
    fRoutes.forEach((r,ri)=>{
        const udp=r.protocol==='udp';
        const nm=r.name.trim();
        if(!nm){se('r'+ri+'-n','Required');ok=false;}
        else if(seenN.has(nm)){se('r'+ri+'-n','Duplicate name');ok=false;}
        else{seenN.add(nm);ce('r'+ri+'-n');}
        // Port uniqueness is per (protocol, port): tcp:9000 and udp:9000 may coexist.
        const pk=(udp?'udp:':'tcp:')+(+r.listen_port);
        if(!validPort(r.listen_port)){se('r'+ri+'-p','1-65535');ok=false;}
        else if(seenP.has(pk)){se('r'+ri+'-p','Duplicate '+(udp?'udp':'tcp')+' port');ok=false;}
        else{seenP.add(pk);ce('r'+ri+'-p');}
        if(udp){
            const ut=(r.udp_timeout+'').trim();
            if(ut!==''&&(!Number.isInteger(+ut)||+ut<0)){se('r'+ri+'-udpto','Blank=inherit, or integer >= 0');ok=false;}else ce('r'+ri+'-udpto');
            const ur=(r.udp_rate+'').trim();
            if(ur!==''&&(!Number.isInteger(+ur)||+ur<0)){se('r'+ri+'-udprate','Blank=inherit, or integer >= 0');ok=false;}else ce('r'+ri+'-udprate');
        }else{
            const it=(r.idle_timeout+'').trim();
            if(it!==''&&(!Number.isInteger(+it)||+it<0)){se('r'+ri+'-idle','Blank=inherit, or integer >= 0');ok=false;}else ce('r'+ri+'-idle');
        }
        (r.affinity||[]).forEach((a,ai)=>{
            if(!a.client.trim()||!validIPOrCidr(a.client.trim())){se('af-'+ri+'-'+ai+'-c','Required: valid IP or CIDR');ok=false;}else ce('af-'+ri+'-'+ai+'-c');
            if(!a.host.trim()||!validIP(a.host.trim())){se('af-'+ri+'-'+ai+'-h','Required: valid IPv4/IPv6');ok=false;}else ce('af-'+ri+'-'+ai+'-h');
            if(!validPort(a.port)){se('af-'+ri+'-'+ai+'-p','1-65535');ok=false;}else ce('af-'+ri+'-'+ai+'-p');
        });
        r.backends.forEach((b,bi)=>{
            const prefix='r'+ri+'-b'+bi;
            if(!validIP(b.host)){se(prefix+'-h','Valid IP');ok=false;}else ce(prefix+'-h');
            if(!validPort(b.port)){se(prefix+'-p','1-65535');ok=false;}else ce(prefix+'-p');
            const br=+b.rate_max;if(br<0||(br>0&&br<1)){se(prefix+'-rate','0 or >= 1');ok=false;}else ce(prefix+'-rate');
            if(!validateAC(prefix,b.whitelist_text,b.blacklist))ok=false;
        });
    });
    return ok;
}
function parseIPs(text){return(text||'').split('\n').map(s=>s.trim()).filter(s=>s);}
function buildBL(arr){
    return(arr||[]).filter(e=>e.ip.trim()).map(e=>{
        const u=normUntil(e.until);
        return u?{ip:e.ip.trim(),until:u}:e.ip.trim();
    });
}
function buildCfgJSON(){
    const obj={};
    obj.routes=fRoutes.map(r=>{
        const udp=r.protocol==='udp';
        const ro={name:r.name.trim(), listen_port:+r.listen_port};
        if(udp)ro.protocol='udp';
        if(udp){
            const ut=(r.udp_timeout+'').trim();
            if(ut!=='')ro.udp_session_timeout_seconds=+ut;
            const ur=(r.udp_rate+'').trim();
            if(ur!=='')ro.udp_datagram_rate_limit=+ur;
        }else{
            const it=(r.idle_timeout+'').trim();
            if(it!=='')ro.idle_timeout_seconds=+it;
        }
        const af=(r.affinity||[]).filter(a=>a.client.trim()&&a.host.trim()&&validPort(a.port));
        if(af.length)ro.affinity=af.map(a=>({client:a.client.trim(),host:a.host.trim(),port:+a.port}));
        ro.backends=r.backends.map(b=>{
            const be={host:b.host.trim(),port:+b.port};
            // heartbeat is TCP-only; proxy_protocol applies to both TCP (v1)
            // and UDP (v2 binary header prepended to each datagram).
            if(!udp){
                be.heartbeat=b.heartbeat;
            }
            if(b.proxy_protocol)be.proxy_protocol=true;
            if(b.disabled)be.disabled=true;
            if(+b.rate_max>0)be.rate_limit={max_per_minute:+b.rate_max};
            const wl=parseIPs(b.whitelist_text);if(wl.length)be.whitelist=wl;
            const bl=buildBL(b.blacklist);if(bl.length)be.blacklist=bl;
            return be;
        });
        return ro;
    });
    obj.heartbeat={enabled:fHb.enabled,interval_seconds:+fHb.interval_seconds};
    obj.rate_limit={max_per_minute:+fGlobal.rate_max,backoff_base_seconds:+fGlobal.backoff_base,backoff_max_seconds:+fGlobal.backoff_max};
    if(+fGlobal.idle_timeout>0)obj.idle_timeout_seconds=+fGlobal.idle_timeout;
    if(+fGlobal.udp_timeout!==30)obj.udp_session_timeout_seconds=+fGlobal.udp_timeout;
    if(+fGlobal.max_udp_sessions!==10000)obj.max_udp_sessions=+fGlobal.max_udp_sessions;
    if(+fGlobal.udp_rate>0)obj.udp_datagram_rate_limit=+fGlobal.udp_rate;
    if(fGlobal.worker_auto)obj.worker_processes='auto';
    else if(+fGlobal.worker_processes!==1)obj.worker_processes=+fGlobal.worker_processes;
    if(fGlobal.log_file.trim()){obj.log_file=fGlobal.log_file.trim();if(fGlobal.log_rotation!=='none')obj.log_rotation=fGlobal.log_rotation;}
    const gwl=parseIPs(fGlobal.whitelist_text);if(gwl.length)obj.whitelist=gwl;
    const gbl=buildBL(fGlobal.blacklist);if(gbl.length)obj.blacklist=gbl;
    return obj;
}
async function saveCfg(){
    if(!validateCfg())return;
    const btn=document.getElementById('btn-sv');
    const msg=document.getElementById('smsg');
    btn.disabled=true;btn.textContent='SAVING...';msg.className='smsg';msg.textContent='';
    try{
        const r=await fetch('/api/config/save',{method:'POST',headers:{'Content-Type':'application/json',...authHeaders()},body:JSON.stringify(buildCfgJSON(),null,2)});
        const d=await r.json();
        if(d.ok){
            msg.className='smsg ok';msg.textContent='Configuration saved. Reloading...';btn.textContent='RELOADING...';
            try{
                const rr=await fetch('/api/reload',{method:'POST',headers:authHeaders()});
                const dr=await rr.json();
                if(dr.ok){
                    msg.textContent='Saved and reloaded successfully.';
                    setTimeout(()=>fetchHealth(),1500);
                } else {
                    msg.className='smsg er';msg.textContent='Saved, but reload failed.';
                }
            } catch { msg.className='smsg er'; msg.textContent='Saved, but reload failed.'; }
        }
        else{msg.className='smsg er';msg.textContent='Error: '+(d.error||'Unknown error');}
    } catch{msg.className='smsg er';msg.textContent='Request failed';}
    btn.disabled=false;btn.textContent='SAVE CONFIGURATION';
}

initAuth();
fetchHealth();
setInterval(fetchHealth, 3000);
</script>
</body>
</html>)HTML";

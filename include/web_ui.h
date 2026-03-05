#pragma once
#include <pgmspace.h>

// Auto-served from flash via server->send_P() — zero heap allocation.
// All rendering is client-side JS. ESP32 only serves this once per page load,
// then the browser polls /api/status, /api/logs, /api/config, /api/sd-activity.

static const char WEB_UI_HTML[] PROGMEM = R"HTMLEOF(<!DOCTYPE html><html><head>
<title>CPAP Uploader</title><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1923;color:#c7d5e0;min-height:100vh;padding:16px;overflow-x:hidden}
.wrap{max-width:900px;margin:0 auto;overflow-x:hidden}
h1{font-size:1.5em;color:#fff;margin-bottom:2px}
.sub{color:#66c0f4;font-size:0.85em;margin-bottom:14px}
nav{display:flex;gap:6px;margin-bottom:14px;flex-wrap:wrap}
nav button{padding:7px 14px;border-radius:6px;background:#2a475e;color:#c7d5e0;border:none;cursor:pointer;font-size:0.84em;transition:background .2s}
nav button.act{background:#66c0f4;color:#0f1923;font-weight:700}
nav button:hover:not(.act){background:#3a5a7e}
.page{display:none}.page.on{display:block}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(255px,1fr));gap:12px;margin-bottom:14px}
.card{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:15px}
.card h2{font-size:.8em;text-transform:uppercase;letter-spacing:1px;color:#66c0f4;margin-bottom:9px;border-bottom:1px solid #2a475e;padding-bottom:6px}
.row{display:flex;justify-content:space-between;padding:3px 0;font-size:.85em}
.k{color:#8f98a0}.v{color:#c7d5e0;font-weight:500;text-align:right;max-width:55%}
.badge{display:inline-block;padding:2px 9px;border-radius:20px;font-weight:700;font-size:.76em}
.bi{background:#2a475e;color:#8f98a0}.bl{background:#1a3a1a;color:#44ff44;animation:puG 2.5s ease-in-out infinite}
.ba,.bu{background:#1a2a4a;color:#66c0f4;animation:puB 2.5s ease-in-out infinite}
.bc,.br{background:#3a2a1a;color:#ffaa44;animation:puA 2.5s ease-in-out infinite}
.bco{background:#1a3a1a;color:#44ff44;animation:puG 2.5s ease-in-out infinite}
.bm{background:#2a1a3a;color:#bb88ff;animation:puP 2.5s ease-in-out infinite}
@keyframes puG{0%,100%{box-shadow:0 0 3px rgba(68,255,68,.3)}50%{box-shadow:0 0 10px rgba(68,255,68,.6)}}
@keyframes puB{0%,100%{box-shadow:0 0 3px rgba(102,192,244,.3)}50%{box-shadow:0 0 10px rgba(102,192,244,.6)}}
@keyframes puA{0%,100%{box-shadow:0 0 3px rgba(255,170,68,.3)}50%{box-shadow:0 0 10px rgba(255,170,68,.6)}}
@keyframes puP{0%,100%{box-shadow:0 0 3px rgba(187,136,255,.3)}50%{box-shadow:0 0 10px rgba(187,136,255,.6)}}
@keyframes puM{0%,100%{box-shadow:0 0 4px rgba(255,204,68,.2);border-color:#2a6a8a}50%{box-shadow:0 0 14px rgba(255,204,68,.5);border-color:#ccaa33}}
.mode-badge{display:inline-block;background:#1a3a4a;border:1px solid #2a6a8a;border-radius:4px;padding:1px 8px;color:#66c0f4;font-weight:700;font-size:.95em;animation:puM 2.5s ease-in-out infinite}
.prog{background:#2a475e;border-radius:5px;height:8px;margin-top:5px;overflow:hidden}
.pf{background:linear-gradient(90deg,#66c0f4,#44aaff);height:100%;border-radius:5px;transition:width .5s}
.actions{display:flex;flex-wrap:wrap;gap:7px;margin-top:7px}
.btn{display:inline-flex;align-items:center;gap:5px;padding:8px 15px;border-radius:6px;font-size:.83em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all .2s}
.bp{background:#66c0f4;color:#0f1923}.bp:hover{background:#88d0ff}
.bs{background:#2a475e;color:#c7d5e0}.bs:hover{background:#3a5a7e}
.bd{background:#c0392b;color:#fff}.bd:hover{background:#e04030}
.bo{background:#aa6622;color:#fff}.bo:hover{background:#cc8833}
.bg{background:#2f8f57;color:#fff}.bg:hover{background:#3aaa6a}
.sig-exc{color:#44ff44}.sig-good{color:#88dd44}.sig-fair{color:#ddcc44}.sig-weak{color:#dd8844}.sig-vweak{color:#dd4444}
.toast{position:fixed;right:12px;bottom:12px;max-width:310px;background:#1b2838;border:1px solid #2a475e;color:#c7d5e0;padding:9px 11px;border-radius:8px;font-size:.82em;box-shadow:0 5px 20px rgba(0,0,0,.4);opacity:0;transform:translateY(7px);transition:opacity .2s,transform .2s;pointer-events:none;z-index:9999}
.toast.on{opacity:1;transform:translateY(0)}.toast.ok{border-color:#2f8f57}.toast.er{border-color:#c0392b}.toast.warn{border-color:#c07830;color:#f0c070}
#log-box{background:#1b2838;border:1px solid #2a475e;border-radius:6px;padding:12px;white-space:pre-wrap;word-wrap:break-word;font-family:Consolas,Monaco,monospace;font-size:11.5px;height:68vh;overflow-y:auto}
#cfg-box{background:#1b2838;border:1px solid #2a475e;border-radius:6px;padding:12px;white-space:pre-wrap;font-family:Consolas,Monaco,monospace;font-size:11.5px;color:#aaddff;overflow-x:auto}
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:9px;margin-bottom:12px}
.stat-box{background:#16213e;padding:12px;border-radius:8px;text-align:center;border:1px solid #2a475e}
.sv{font-size:1.55em;color:#66c0f4;display:block;margin:3px 0;font-family:monospace}.sl{color:#8f98a0;font-size:.8em}
.chart{background:#16213e;padding:12px;border-radius:8px;border:1px solid #2a475e;overflow-x:auto;min-height:160px}
.br2{display:flex;align-items:center;height:15px;margin:2px 0;font-size:.76em;font-family:monospace}
.bl2{width:42px;color:#8f98a0;text-align:right;padding-right:6px}.bt{flex-grow:1;background:#0f1923;height:100%;border-radius:2px;overflow:hidden}
.bf{height:100%;transition:width .3s}.bf.a{background:#ff4444}.bf.i{background:#2a475e}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-left:7px}
.dot.busy{background:#ff4444;box-shadow:0 0 6px #ff4444}.dot.idle{background:#44ff44;box-shadow:0 0 6px #44ff44}
.fg{margin-bottom:12px}.fg label{display:block;margin-bottom:4px;color:#8f98a0;font-size:.86em}
.fg input{width:100%;padding:8px;background:#0f1923;border:1px solid #2a475e;color:#fff;border-radius:6px;font-size:.86em}
.fg input:focus{outline:none;border-color:#66c0f4}
.sm{margin-top:7px;font-size:.86em;min-height:1.2em}.sm.ok{color:#44ff44}.sm.er{color:#ff4444}.sm.info{color:#66c0f4}
.wb{background:#3a2a1a;border:1px solid #aa6622;border-radius:8px;padding:12px;margin-bottom:12px}
.wb h3{color:#ffaa44;font-size:.86em;margin-bottom:5px}.wb ul{padding-left:16px;color:#c7d5e0;font-size:.82em}.wb li{margin-bottom:3px}
.pfc{background:linear-gradient(90deg,#aa66ff,#cc88ff)}
.be{margin-bottom:13px}.bh{display:flex;justify-content:space-between;align-items:center;margin-bottom:5px}
.bt-s{color:#66c0f4;font-size:.82em;font-weight:700;letter-spacing:.5px}
.bt-c{color:#aa66ff;font-size:.82em;font-weight:700;letter-spacing:.5px}
.bd-i{font-size:.79em;color:#8f98a0;margin-top:4px;min-height:1.1em;padding-left:2px}
.hdr{margin-bottom:2px}
.hdr svg{height:72px;width:auto;display:block}
.hdr-spin{transform-origin:25px 25px;animation:hSpin 8s linear infinite}
@keyframes hSpin{100%{transform:rotate(360deg)}}
.hdr-arrow{animation:hUp 2s ease-in-out infinite}
@keyframes hUp{0%{transform:translateY(1.5px);opacity:0}40%{opacity:1}100%{transform:translateY(-4px);opacity:0}}
.hdr-wave{animation:hPulse 3.5s ease-in-out infinite}
@keyframes hPulse{0%,100%{opacity:.6}50%{opacity:1}}
.hw1{animation:hWifi 1.5s infinite 0s}.hw2{animation:hWifi 1.5s infinite .25s}.hw3{animation:hWifi 1.5s infinite .5s}
@keyframes hWifi{0%,100%{opacity:.4}50%{opacity:1}}
#reboot-overlay{display:none;background:#1a2a1a;border:1px solid #2f8f57;border-radius:10px;padding:16px 20px;margin-bottom:14px;text-align:center;animation:rbPulse 2.5s ease-in-out infinite}
#reboot-overlay h3{color:#44ff44;font-size:1em;margin-bottom:6px}
#reboot-overlay p{color:#a0c0b0;font-size:.84em;line-height:1.5}
@keyframes rbPulse{0%,100%{border-color:#2f8f57}50%{border-color:#44ff44}}
@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}
.log-spinner{display:inline-block;width:14px;height:14px;border:2px solid #2a475e;border-top-color:#66c0f4;border-radius:50%;animation:spin .8s linear infinite;vertical-align:middle;margin-right:6px}
#mon-active-banner{display:none;background:#2a1a2a;border:1px solid #8b4dbb;border-radius:10px;padding:14px 18px;margin-bottom:14px;text-align:center;animation:monPulse 2.5s ease-in-out infinite}
#mon-active-banner h3{color:#bb88ff;font-size:1em;margin-bottom:6px}
#mon-active-banner p{color:#b0a0c0;font-size:.84em;line-height:1.5}
@keyframes monPulse{0%,100%{border-color:#8b4dbb}50%{border-color:#bb88ff}}
@media(max-width:600px){.hdr svg{height:72px}nav{gap:4px}nav button{padding:5px 9px;font-size:.78em}.log-btns{flex-direction:column;gap:4px}.log-btns button{width:100%}}
</style></head><body>
<div class=wrap>
<div class=hdr>
<svg viewBox="0 0 420 50" xmlns="http://www.w3.org/2000/svg" font-family="'Segoe UI',system-ui,-apple-system,sans-serif"><defs><linearGradient id="cg" x1="0%" y1="0%" x2="100%" y2="100%"><stop offset="0%" stop-color="#1a6b8a"/><stop offset="100%" stop-color="#0d4a6b"/></linearGradient><linearGradient id="wg" x1="0%" y1="0%" x2="100%" y2="0%"><stop offset="0%" stop-color="#2dd4bf"/><stop offset="50%" stop-color="#38bdf8"/><stop offset="100%" stop-color="#818cf8"/></linearGradient><linearGradient id="ag" x1="0%" y1="100%" x2="0%" y2="0%"><stop offset="0%" stop-color="#38bdf8"/><stop offset="100%" stop-color="#2dd4bf"/></linearGradient></defs><circle class="hdr-spin" cx="25" cy="25" r="23.5" fill="none" stroke="url(#wg)" stroke-width="1.2" stroke-dasharray="7 5 12 5 19 5 31 5 50 5" stroke-linecap="round" opacity=".85"/><circle cx="25" cy="25" r="22" fill="url(#cg)"/><circle cx="25" cy="25" r="22" fill="none" stroke="url(#wg)" stroke-width=".7" opacity=".6"/><g transform="translate(15,15)"><path d="M2.5,0 L10,0 L13,3 L13,12.5 Q13,13 12,13 L1,13 Q0,13 0,12 L0,1 Q0,0 1,0 Z" fill="#0a2233" stroke="#38bdf8" stroke-width=".8" opacity=".9"/><rect x="2" y="9" width="1.5" height="3.5" rx=".3" fill="#2dd4bf" opacity=".85"/><rect x="4.2" y="9" width="1.5" height="3.5" rx=".3" fill="#2dd4bf" opacity=".85"/><rect x="6.4" y="9" width="1.5" height="3.5" rx=".3" fill="#2dd4bf" opacity=".85"/><rect x="8.6" y="9" width="1.5" height="3.5" rx=".3" fill="#2dd4bf" opacity=".85"/></g><g transform="translate(33.5,21.5)"><g class="hdr-arrow"><line x1="0" y1="8" x2="0" y2="2" stroke="url(#ag)" stroke-width="1.5" stroke-linecap="round"/><polyline points="-2.5,4 0,.5 2.5,4" fill="none" stroke="url(#ag)" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></g><line x1="-3" y1="9.5" x2="3" y2="9.5" stroke="#38bdf8" stroke-width="1.2" stroke-linecap="round" opacity=".7"/></g><g transform="translate(25,37)"><path class="hdr-wave" d="M-10,0 C-8.5,-.8 -8,-.8 -6.5,0 C-5,.8 -4.5,.8 -3,0 C-1.5,-3.5 -.8,-3.5 .7,-1.5 C1.5,-.3 1.8,3 3,3 C4,3 4.3,.3 5.5,-1.5 C6.5,-3.5 7.2,-3.5 8.5,-1.5 C9.5,0 10,0 10,0" fill="none" stroke="url(#wg)" stroke-width="1" stroke-linecap="round"/></g><g transform="translate(25,11)"><path class="hw1" d="M-3.5,3.5 Q0,-.8 3.5,3.5" fill="none" stroke="#38bdf8" stroke-width="1" stroke-linecap="round"/><path class="hw2" d="M-2,5 Q0,2.8 2,5" fill="none" stroke="#38bdf8" stroke-width="1" stroke-linecap="round"/><circle class="hw3" cx="0" cy="6" r=".8" fill="#38bdf8"/></g><text x="56" y="22" font-size="18" font-weight="700" letter-spacing="-.3" fill="white">CPAP<tspan fill="url(#wg)"> Data</tspan></text><text x="56" y="40" font-size="18" font-weight="700" letter-spacing="-.3" fill="white">Uploader</text><rect x="56" y="44" width="140" height="1.5" rx=".75" fill="url(#wg)" opacity=".5"/></svg>
</div>
<p class=sub id=sub>Connecting...</p>
<div id=reboot-overlay><h3>&#8635; Device is unreachable or rebooting&hellip;</h3><p>If the device is rebooting, this is normal &mdash; it may reboot periodically by design to maintain stability and will be back online in a few seconds.<br>If it has been powered off or moved out of WiFi range, this page will reconnect automatically once the device is reachable again.</p></div>
<div id=mon-active-banner><h3>&#128270; SD Access Monitoring is active</h3><p>All automatic uploads are <strong>paused</strong> while monitoring is running.<br>Go to the <strong>SD Access</strong> tab and click <strong>Stop</strong> to resume normal operation.</p></div>
<nav>
<button id=t-dash onclick="tab('dash')" class=act>Dashboard</button>
<button id=t-logs onclick="tab('logs')">Logs</button>
<button id=t-cfg onclick="tab('cfg')">Config</button>
<button id=t-mon onclick="tab('mon')">SD Access</button>
<button id=t-mem onclick="tab('mem')">System</button>
<button id=t-ota onclick="tab('ota')">OTA</button>
</nav>

<!-- DASHBOARD -->
<div id=dash class="page on">
<div class=cards>
<div class=card><h2>Upload Engine</h2>
<div class=row><span class=k>State</span><span id=d-st class=v></span></div>
<div class=row><span class=k>In state</span><span id=d-ins class=v></span></div>
<div class=row><span class=k>Upload mode</span><span id=d-mode class=v></span></div>
<div class=row><span class=k>Time synced</span><span id=d-tsync class=v></span></div>
<div class=row><span class=k>Upload window</span><span id=d-win class=v></span></div>
<div class=row><span class=k>Inactivity threshold</span><span id=d-inact class=v></span></div>
<div class=row><span class=k>Exclusive access</span><span id=d-excl class=v></span></div>
<div class=row><span class=k>Cooldown</span><span id=d-cool class=v></span></div>
<div class=row><span class=k>Next upload</span><span id=d-next class=v></span></div>
</div>
<div class=card><h2>System</h2>
<div class=row><span class=k>Time</span><span id=d-time class=v></span></div>
<div class=row><span class=k>Free heap</span><span id=d-fh class=v></span></div>
<div class=row><span class=k>Max alloc</span><span id=d-ma class=v></span></div>
<div class=row><span class=k>WiFi</span><span id=d-wifi class=v></span></div>
<div class=row><span class=k>IP</span><span id=d-ip class=v></span></div>
<div class=row><span class=k>Endpoint</span><span id=d-ep class=v></span></div>
<div class=row><span class=k>GMT offset</span><span id=d-gmt class=v></span></div>
<div class=row><span class=k>Uptime</span><span id=d-up class=v></span></div>
</div>
</div>
<div class=cards>
<div class=card style="grid-column:1/-1"><h2>Upload Progress</h2>
<div class=be>
<div class=bh><span id=d-ab-name class=bt-s>—</span><span id=d-ab-st class=v>—</span></div>
<div class=prog><div id=d-pf-active class=pf style=width:0%></div></div>
<div id=d-ab-det class=bd-i></div>
</div>
<div class=be id=d-next-be style=display:none>
<div class=bh><span id=d-nb-name class="bt-s" style=color:#8f98a0>Next: —</span><span id=d-nb-st class=v style="font-size:.78em;color:#8f98a0">—</span></div>
<div class=prog><div id=d-pf-next class=pf style="width:0%;background:linear-gradient(90deg,#556070,#6a7a8a)"></div></div>
<div id=d-nb-det class=bd-i style=color:#8f98a0></div>
</div>
<div class=row style="border-top:1px solid #2a475e;padding-top:8px;margin-top:2px"><span class=k>Status</span><span id=d-fst class=v></span></div>
</div>
</div>
<div id=d-mode-help style="background:#16213e;border:1px solid #2a475e;border-radius:8px;padding:10px 14px;margin-bottom:14px;font-size:.82em;color:#8f98a0;line-height:1.5"></div>
<div style="border:1px solid #8b2020;border-radius:10px;padding:15px;margin-bottom:14px">
<h2 style="font-size:.8em;text-transform:uppercase;letter-spacing:1px;color:#e04030;margin-bottom:10px;border-bottom:1px solid #8b2020;padding-bottom:6px">Danger Zone</h2>
<div style="display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap">
<div style="flex:1;min-width:200px">
<button id=btn-up class="btn bo" onclick=triggerUpload() style="min-height:38px;line-height:1.2">&#9650; Force Upload</button>
<div style="border-top:2px solid #aa6622;margin:8px 0;width:100%"></div>
<p style="font-size:.78em;color:#8f98a0;line-height:1.45" id=d-danger-upload>The firmware automatically detects when your CPAP finishes therapy and uploads new data. Forcing an upload bypasses this detection and immediately takes control of the SD card, which <strong style="color:#ffaa44">increases the risk of an SD card error</strong> if the CPAP is actively writing <strong style="color:#ffaa44">or attempts to write at any point</strong> during the upload (which may take several minutes). Only use this if automatic uploads have not run for an unusual amount of time and you are confident the CPAP will remain idle.</p>
</div>
<div style="flex:1;min-width:200px;text-align:right">
<button id=btn-rst class="btn bd" onclick=resetState() style="min-height:38px;line-height:1.2">&#9762; Reset State</button>
<div style="border-top:2px solid #c0392b;margin:8px 0;width:100%"></div>
<p style="font-size:.78em;color:#8f98a0;line-height:1.45;text-align:left" id=d-danger-reset>Erases all upload tracking state and reboots the device. Every data folder will be re-scanned and re-uploaded from scratch on the next cycle. Under normal use (CPAP used daily with regular uploads), this is <strong style="color:#ffaa44">never needed</strong>. Only use this if uploads are stuck in a persistent de-sync state &mdash; for example, files appear as uploaded in the dashboard but are missing on your server, or the progress counter is clearly wrong after multiple upload cycles.</p>
</div>
</div>
</div>
</div>

<!-- LOGS -->
<div id=logs class=page>
<div class=card style="margin-bottom:10px">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px">
<h2 style=margin:0>System Logs <span id=log-st style="font-size:.9em;color:#8f98a0;font-weight:400"></span></h2>
<div class=log-btns style="display:flex;gap:6px">
<button class="btn bg" onclick=downloadSavedLogs() style="padding:4px 10px;font-size:.8em" title="Download all logs (saved + current) for troubleshooting">&#11015; Download All Logs</button>
<button class="btn bs" onclick=copyLogBuf() style="padding:4px 10px;font-size:.8em" title="Copy all buffered log lines to clipboard">&#128203; Copy to clipboard</button>
<button class="btn bs" onclick=clearLogBuf() style="padding:4px 10px;font-size:.8em">&#128465; Clear buffer</button>
</div>
</div>
<div id=log-box>Loading...</div>
</div>
</div>

<!-- CONFIG SD-ACCESS WARNING MODAL -->
<div id="cfg-warn-modal" style="display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.85);z-index:9999;align-items:center;justify-content:center;overflow:auto;padding:16px">
<div style="background:#1b2838;border:1px solid #aa9900;border-radius:12px;padding:20px;max-width:480px;width:100%;box-sizing:border-box;box-shadow:0 10px 40px rgba(170,153,0,0.2);margin:auto">
<h2 style="color:#ffcc44;margin-bottom:10px;font-size:1.2em">&#9888; SD Card Access Warning</h2>
<p style="font-size:.88em;color:#c7d5e0;line-height:1.6;margin-bottom:16px">
Editing and saving config requires <strong>reading from and writing to the CPAP&rsquo;s SD card</strong>. This carries a small risk of an SD card error if the CPAP machine is actively writing at the same time.
</p>
<div style="background:#0f1923;padding:14px;border-radius:8px;margin-bottom:18px;border:1px solid #2a475e">
<ul style="font-size:.84em;color:#8f98a0;padding-left:18px;line-height:1.7;margin:0">
<li><strong style="color:#44ff44">Safest:</strong> Edit config when the CPAP machine is <strong>off</strong> or between therapy sessions.</li>
<li><strong style="color:#ddcc44">Acceptable:</strong> Quick edits during the day while CPAP is idle (not in therapy).</li>
<li><strong style="color:#ff6b6b">Risky:</strong> Editing while the CPAP is actively recording therapy data.</li>
</ul>
</div>
<div style="display:flex;gap:10px;justify-content:flex-end">
<button class="btn bs" onclick="document.getElementById('cfg-warn-modal').style.display='none'">Cancel</button>
<button class="btn bp" onclick="document.getElementById('cfg-warn-modal').style.display='none';_doCfgLock();" style="background:#aa8800;color:#fff">I Understand, Edit Config</button>
</div>
</div>
</div>

<!-- CONFIG -->
<div id=cfg class=page>
<div id=cfg-lock-banner style="display:none;background:#2a2a00;border:1px solid #aa9900;border-radius:6px;padding:10px 14px;margin-bottom:10px;font-size:.85em;color:#ddcc88">
&#128274; <strong>Upload running</strong> &mdash; Config editor is active. Press <em>Cancel</em> to close without saving, or <em>Save &amp; Reboot</em> to apply changes. After reboot, <strong>always</strong> physically eject and reinsert the CPAP&rsquo;s SD card before powering it on.
</div>
<div class=card>
<h2>Edit config.txt
<span id=cfg-lock-badge style="display:none;margin-left:8px;background:#4a8a4a;color:#fff;font-size:.65em;padding:2px 7px;border-radius:10px;font-weight:600;vertical-align:middle">LOCKED</span>
</h2>
<p style="font-size:.82em;color:#8f98a0;margin-bottom:8px">Direct editor for the SD card config file. Passwords stored securely in flash appear as <code>***STORED_IN_FLASH***</code> &mdash; leave them unchanged to keep existing credentials. Max 4096 bytes. <strong>Changes take effect after reboot.</strong></p>
<p style="font-size:.82em;color:#ffcc44;margin-bottom:8px">&#9888; Click <strong>Edit</strong> to start editing. Uploads continue while the editor is open.</p>
<textarea id=cfg-raw style="width:100%;box-sizing:border-box;height:320px;background:#111820;color:#6a7a8a;border:1px solid #2d3440;border-radius:4px;padding:8px;font-family:monospace;font-size:.8em;resize:vertical" maxlength=4096 oninput=cfgRawCount() placeholder="Click Edit to begin..." readonly></textarea>
<div style="display:flex;justify-content:space-between;align-items:center;margin-top:6px">
<span id=cfg-raw-cnt style="font-size:.8em;color:#8f98a0">0 / 4096 bytes</span>
<div class=actions style=margin:0>
<button id=btn-cfg-edit class="btn bp" onclick=acquireCfgLock() style="padding:6px 14px">&#9998; Edit</button>
<button id=btn-cfg-reload class="btn bs" onclick=loadRawCfg() style="padding:6px 14px;display:none">&#8635; Reload</button>
<button id=btn-cfg-savereboot class="btn bd" onclick=saveAndReboot() style="padding:6px 14px;display:none">Save &amp; Reboot</button>
<button id=btn-cfg-cancel class="btn bs" onclick=releaseCfgLock() style="padding:6px 14px;display:none">&#10005; Cancel</button>
</div>
</div>
<div id=cfg-raw-msg style="margin-top:6px;font-size:.83em"></div>
</div>
</div>

<!-- MONITOR -->
<div id=mon class=page>
<div class=card style="margin-bottom:10px"><h2>SD Activity Monitor</h2>
<p style="font-size:.85em;color:#c7d5e0;line-height:1.5;margin-bottom:10px">Monitors SD card bus activity in real time. Start monitoring when your CPAP machine is on to observe write patterns.</p>
<div id=mon-upwarn style="display:none;background:#2a2a1a;border:1px solid #665522;border-radius:6px;padding:9px 13px;margin-bottom:10px;font-size:.84em;color:#ddcc88">&#9889; Upload in progress &mdash; monitoring cannot start until the upload finishes.</div>
<div class=actions>
<button id=btn-mst class="btn bp" onclick=startMon()>Start Monitoring</button>
<button id=btn-msp class="btn bd" onclick=stopMon() style=display:none>Stop</button>
<button class="btn bs" onclick=openProfilerWizard()>&#9881; Profiler Wizard</button>
</div>
</div>
<div class=stats-grid>
<div class=stat-box><span class=sl>Pulse Count (1s)</span><span class=sv id=m-p>--</span></div>
<div class=stat-box><span class=sl>Consecutive Idle</span><span class=sv id=m-i>--</span></div>
<div class=stat-box><span class=sl>Longest Idle</span><span class=sv id=m-l>--</span></div>
<div class=stat-box><span class=sl>Active/Idle</span><span class=sv id=m-r>--</span></div>
</div>
<div class=card style="margin-bottom:10px">
<h2>Activity Timeline <span style="font-size:.65em;color:#8f98a0;font-weight:400">&nbsp;last ~2 min &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#ff4444;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Bus Activity</span></h2>
<div style="background:#0f1923;border-radius:6px;padding:8px 4px 2px">
<svg id=mon-svg viewBox="0 0 600 120" preserveAspectRatio="none" style="width:100%;height:120px;display:block"></svg>
</div>
<div style="display:flex;justify-content:space-between;font-size:.72em;color:#3a5070;padding:2px 6px 0"><span>~2m ago</span><span>~1m ago</span><span>now</span></div>
</div>
<div class=card>
<h2>Card Activity Log <span style="font-size:.65em;color:#8f98a0;font-weight:400">(client-side, up to 2000 entries / 24h)</span></h2>
<div style="overflow-y:auto;max-height:300px;margin-top:8px">
<table id=mon-log-tbl style="width:100%;border-collapse:collapse;font-family:monospace;font-size:.78em">
<thead><tr style="color:#66c0f4;border-bottom:1px solid #2a475e;text-align:left"><th style="padding:4px 8px">Timestamp</th><th style="padding:4px 8px">Pulses</th><th style="padding:4px 8px">Idle (s)</th><th style="padding:4px 8px">Since Last</th></tr></thead>
<tbody id=mon-log-body><tr><td colspan=4 style="padding:8px;color:#8f98a0"><em>Start monitoring to collect data...</em></td></tr></tbody>
</table>
</div>
</div>
</div>


<!-- PROFILER WIZARD MODAL -->
<div id="prof-wiz" style="display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.85);z-index:9999;align-items:center;justify-content:center;overflow:auto;padding:16px">
<div style="background:#1b2838;border:1px solid #66c0f4;border-radius:12px;padding:20px;max-width:550px;width:100%;box-sizing:border-box;box-shadow:0 10px 40px rgba(102,192,244,0.2);margin:auto">
<h2 style="color:#fff;margin-bottom:10px;font-size:1.4em">&#9881; CPAP Profiler Wizard</h2>
<p style="font-size:0.9em;color:#c7d5e0;line-height:1.5;margin-bottom:20px">
This tool will measure your CPAP machine's specific SD card writing behavior to help you tune <strong style="color:#66c0f4">INACTIVITY_SECONDS</strong>.
</p>
<div style="background:#0f1923;padding:15px;border-radius:8px;margin-bottom:20px;border:1px solid #2a475e">
<ol style="font-size:0.85em;color:#8f98a0;padding-left:20px;line-height:1.6">
<li>Ensure the SD card is physically inserted into the CPAP.</li>
<li>Put on your CPAP mask, turn the machine <strong>ON</strong>, and <strong style="color:#ffcc44">start a therapy session</strong> &mdash; breathe in and out as you would during normal sleep. If your machine does not auto-start airflow on breath detection, press its start button.</li>
<li>Once the machine is actively blowing air and you are breathing normally, click <strong>Start Profiling</strong> below.</li>
<li><strong style="color:#ffcc44">Continue breathing steadily</strong> &mdash; do not pause or hold your breath. The profiler measures real SD write gaps during active therapy.</li>
<li>Wait 2&ndash;3 minutes. The wizard will record the longest continuous silence between SD writes and suggest an <strong>INACTIVITY_SECONDS</strong> value.</li>
</ol>
</div>
<div style="text-align:center;margin-bottom:20px">
<div style="font-size:1.1em;color:#8f98a0;margin-bottom:5px">Longest measured silence:</div>
<div id="prof-max-idle" style="font-size:2.8em;color:#44ff44;font-family:monospace;font-weight:bold">0.0s</div>
</div>
<div id="prof-rec-box" style="display:none;background:#1a3a1a;border:1px solid #2f8f57;padding:12px;border-radius:6px;margin-bottom:20px">
<div style="font-size:0.9em;color:#c7d5e0">
Recommended <strong style="color:#44ff44">INACTIVITY_SECONDS</strong>: <span id="prof-rec-val" style="font-weight:bold;font-size:1.2em;color:#fff">--</span>
</div>
<div style="font-size:0.75em;color:#8f98a0;margin-top:4px">(Longest silence + 4 second safety margin)</div>
</div>
<div style="display:flex;gap:10px;justify-content:flex-end">
<button id="btn-prof-cancel" class="btn bs" onclick="document.getElementById('prof-wiz').style.display='none'">Close</button>
<button id="btn-prof-start" class="btn bp" onclick="startProfiler()" style="background:#aa66ff;color:#fff">Start Profiling</button>
</div>
</div>
</div>

<!-- SYSTEM (was MEMORY) -->
<div id=mem class=page>
<div class=card style="margin-bottom:10px"><h2>Runtime Overview <span style="font-size:.7em;color:#8f98a0;font-weight:400">(live, 2s)</span></h2>
<div class=stats-grid>
<div class=stat-box><span class=sl>Free Heap</span><span class=sv id=hd-fh style="color:#66c0f4">—</span><span class=sl style="margin-top:6px">Min (2m)</span><span class=sv id=hd-fh-min style="color:#ddaa44">—</span></div>
<div class=stat-box><span class=sl>Max Contiguous</span><span class=sv id=hd-ma style="color:#aa66ff">—</span><span class=sl style="margin-top:6px">Min (2m)</span><span class=sv id=hd-ma-min style="color:#ddaa44">—</span></div>
<div class=stat-box><span class=sl>CPU Core 0</span><span class=sv id=hd-c0 style="color:#ff6b6b">—</span><span class=sl style="margin-top:6px">WiFi / System</span></div>
<div class=stat-box><span class=sl>CPU Core 1</span><span class=sv id=hd-c1 style="color:#ffd93d">—</span><span class=sl style="margin-top:6px">Application</span></div>
</div>
</div>
<div class=card style="margin-bottom:10px">
<h2>Heap History <span style="font-size:.65em;color:#8f98a0;font-weight:400">&nbsp;last ~2 min &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#5c9ade;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Free &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#aa66ff;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Max Alloc</span></h2>
<div style="background:#0f1923;border-radius:6px;padding:8px 4px 2px">
<svg id=heap-svg viewBox="0 0 600 200" preserveAspectRatio="none" style="width:100%;height:200px;display:block"></svg>
</div>
<div style="display:flex;justify-content:space-between;font-size:.72em;color:#3a5070;padding:2px 6px 0"><span>~2m ago</span><span>~1m ago</span><span>now</span></div>
</div>
<div class=card>
<h2>CPU Load <span style="font-size:.65em;color:#8f98a0;font-weight:400">&nbsp;last ~2 min &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#ff6b6b;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Core 0 &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#ffd93d;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Core 1</span></h2>
<div style="background:#0f1923;border-radius:6px;padding:8px 4px 2px">
<svg id=cpu-svg viewBox="0 0 600 150" preserveAspectRatio="none" style="width:100%;height:150px;display:block"></svg>
</div>
<div style="display:flex;justify-content:space-between;font-size:.72em;color:#3a5070;padding:2px 6px 0"><span>~2m ago</span><span>~1m ago</span><span>now</span></div>
</div>
</div>

<!-- OTA -->
<div id=ota class=page>
<div class=wb><h3>WARNING</h3><ul>
<li><strong>Do not power off</strong> during update</li>
<li><strong>Ensure stable WiFi</strong> before starting</li>
<li><strong>Do NOT remove SD card</strong> from CPAP during update</li>
<li>Takes 1-2 minutes; device restarts automatically</li>
</ul></div>
<div class=cards>
<div class=card><h2>Method 1: File Upload</h2>
<form id=f-up><div class=fg><label>Firmware file (.bin)</label>
<input type=file id=f-bin name=firmware accept=.bin required></div>
<button type=submit class="btn bp" style=width:100%>Upload &amp; Install</button>
<div id=s-up class=sm></div></form>
</div>
<div class=card><h2>Method 2: URL Download</h2>
<form id=f-url><div class=fg><label>Firmware URL</label>
<input type=url id=f-u name=url placeholder="https://github.com/.../firmware.bin" required></div>
<button type=submit class="btn bp" style=width:100%>Download &amp; Install</button>
<div id=s-url class=sm></div></form>
</div>
<div class=card><h2>System Actions</h2>
<div class=actions>
<button id=btn-srb class="btn bs" onclick=softReboot()>&#8635; Soft Reboot</button>
</div>
</div>
</div>
</div>

<div id=toast class=toast></div>
</div>

<script>
var cfg={},monPoll=null,logPoll=null,curTab='dash',monActive=false;
var heapHistory=[],MAX_HEAP_SAMPLES=60,currentFsmState='',prevHelpHtml='';
function tab(t){
  ['dash','logs','cfg','mon','mem','ota'].forEach(function(x){
    document.getElementById(x).classList.toggle('on',x===t);
    document.getElementById('t-'+x).classList.toggle('act',x===t);
  });
  curTab=t;
  if(t==='logs'){if(!backfillDone){fetchBackfill();}else if(!sseConnected){startLogPoll();}}else{stopLogPoll();stopSse();}
  if(t==='cfg'){loadCfg();}
  if(t==='mon'){checkMonUploadState();syncMonBtn();}
  if(t==='mem'){updateHeapChart();updateCpuChart();}
  _updateMonBanner();
}
function toast(msg,mode){
  var el=document.getElementById('toast');
  var cls=mode===true||mode==='ok'?'ok':mode==='warn'?'warn':'er';
  el.textContent=msg;el.className='toast on '+cls;
  setTimeout(function(){el.className='toast';},4000);
}
function fmt(ms){var s=Math.round(ms/1000);if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
function fmtUp(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';var h=Math.floor(s/3600);return h+'h '+Math.floor(s%3600/60)+'m';}
function sigClass(r){if(r>=-65)return 'sig-exc';if(r>=-75)return 'sig-good';if(r>=-85)return 'sig-fair';if(r>=-95)return 'sig-weak';return 'sig-vweak';}
function sigLabel(r){if(r>=-65)return 'Excellent';if(r>=-75)return 'Good';if(r>=-85)return 'Fair';if(r>=-95)return 'Weak';return 'Very Weak';}
function badgeHtml(st){var s=st.toLowerCase(),c='bi';
  if(s==='listening')c='bl';else if(s==='acquiring'||s==='uploading')c='bu';
  else if(s==='cooldown'||s==='releasing')c='bc';else if(s==='complete')c='bco';
  else if(s==='monitoring')c='bm';
  return '<span class="badge '+c+'">'+st+'</span>';
}
function set(id,html,inner){var el=document.getElementById(id);if(el){if(inner===false)el.innerHTML=html;else el.textContent=html;}}
function seti(id,html){set(id,html,false);}

var statusFailCount=0,rebootExpected=false;
function renderStatus(d){
  statusFailCount=0;
  document.getElementById('reboot-overlay').style.display='none';
  rebootExpected=false;
  var _newSt=d.state||'';
  if(_newSt!==currentFsmState){currentFsmState=_newSt;seti('d-st',badgeHtml(currentFsmState||'?'));}
  var ins=d.in_state_sec||0;set('d-ins',ins<60?ins+'s':Math.floor(ins/60)+'m '+ins%60+'s');
  var mode=(cfg.upload_mode||'—').toUpperCase();
  set('d-mode',mode);
  set('d-tsync',d.time_synced?'Yes':'No');
  var sh=cfg.upload_start_hour,eh=cfg.upload_end_hour;
  var ws=(sh!=null&&eh!=null)?sh+':00 - '+eh+':00':'—';
  var winAll=(sh!=null&&eh!=null&&sh===eh);
  set('d-win',winAll?'24/7 (always open)':ws);
  set('d-inact',cfg.inactivity_seconds!=null?cfg.inactivity_seconds+'s':'—');
  set('d-excl',cfg.exclusive_access_minutes!=null?cfg.exclusive_access_minutes+' min':'—');
  set('d-cool',cfg.cooldown_minutes!=null?cfg.cooldown_minutes+' min':'—');
  // Generate mode explanation helper — dynamic based on current state
  var help='';
  var iw=d.in_window;
  var rd=cfg.recent_folder_days||2;
  var cool=cfg.cooldown_minutes||10;
  var maxd=cfg.max_days||365;
  var mBadge='<span class="mode-badge">';
  var mEnd='</span>';
  var fBtn='<span style="color:#ffaa44">Force Upload</span> <span style="color:#aa7733;font-size:.9em">(not recommended)</span>';
  if(mode==='SMART'){
    if(winAll){
      help=mBadge+'\u25b6 Smart mode \u2014 24/7 window'+mEnd+'<br>'
        +'Uploads up to <b>'+maxd+' days</b> of data whenever CPAP is idle.<br>'
        +fBtn+' \u2192 scans all eligible folders and uploads any new/changed files.';
    }else if(iw){
      help=mBadge+'\u25b6 Smart mode \u2014 currently inside upload window'+mEnd+' <span style="color:#66c0f4;font-size:.9em">('+ws+')</span><br>'
        +'Uploads up to <b>'+maxd+' days</b> of data (recent + older backlog).<br>'
        +fBtn+' \u2192 uploads all eligible data now.<br>'
        +'<span style="color:#aaa">After the window closes at '+eh+':00, only the '+rd+' most recent day(s) will sync.</span>';
    }else{
      help=mBadge+'\u25b6 Smart mode \u2014 currently outside upload window'+mEnd+'<br>'
        +'Only the <b>'+rd+' most recent day(s)</b> of data will be uploaded until <b>'+sh+':00</b>.<br>'
        +'Older data (if any) will be uploaded during the regular window ('+ws+').<br>'
        +fBtn+' \u2192 uploads recent data only.';
    }
  }else if(mode==='SCHEDULED'){
    if(winAll){
      help=mBadge+'\u25b6 Scheduled mode \u2014 24/7 window'+mEnd+'<br>'
        +'Uploads up to <b>'+maxd+' days</b> of data whenever CPAP is idle.<br>'
        +fBtn+' \u2192 scans all folders and uploads new/changed files.';
    }else if(iw){
      help=mBadge+'\u25b6 Scheduled mode \u2014 currently inside upload window'+mEnd+' <span style="color:#66c0f4;font-size:.9em">('+ws+')</span><br>'
        +'Uploading up to <b>'+maxd+' days</b> of data until <b>'+eh+':00</b>.<br>'
        +fBtn+' \u2192 uploads all eligible data now.';
    }else{
      help=mBadge+'\u25b6 Scheduled mode \u2014 currently outside upload window'+mEnd+'<br>'
        +'No automatic uploads until <b>'+sh+':00</b>.<br>'
        +fBtn+' \u2192 forces an upload of recent data now.<br>'
        +'<span style="color:#aaa">Full upload resumes during the window ('+ws+').</span>';
    }
  }
  var helpEl=document.getElementById('d-mode-help');
  if(helpEl){helpEl.style.display=help?'':'none';if(help!==prevHelpHtml){prevHelpHtml=help;helpEl.innerHTML=help;}}
  var nx=d.next_upload;
  set('d-next',nx<0?'—':nx===0?'Now':fmtUp(nx));
  set('d-time',d.time||'—');
  set('d-fh',d.free_heap?Math.round(d.free_heap/1024)+' KB':'—');
  set('d-ma',d.max_alloc?Math.round(d.max_alloc/1024)+' KB':'—');
  var gmtOff=cfg.gmt_offset_hours;
  set('d-gmt',gmtOff!=null?(gmtOff>=0?'+':'')+gmtOff:'—');
  if(d.wifi){
    var rc=sigClass(d.rssi),rl=sigLabel(d.rssi);
    document.getElementById('d-wifi').innerHTML='<span class='+rc+'>'+rl+' ('+d.rssi+' dBm)</span>';
  }else{set('d-wifi','Disconnected');}
  set('d-ip',d.wifi_ip||'—');
  set('d-ep',cfg.endpoint_type||d.endpoint_type||'—');
  set('d-up',fmtUp(d.uptime||0));
  var ab=d.active_backend||'NONE';
  var abColor=ab==='SMB'?'#66c0f4':ab==='CLOUD'?'#aa66ff':ab==='DUAL'?'#44cc88':'#8f98a0';
  var abEl=document.getElementById('d-ab-name');abEl.textContent=ab;abEl.style.color=abColor;
  var done=d.folders_done||0,total=d.folders_total||0,pend=d.folders_pending||0;
  var pct=total>0?Math.round(done*100/total):0;
  document.getElementById('d-pf-active').style.width=pct+'%';
  var inc=Math.max(0,total-done);
  var abSt=total>0?(done+' / '+total+(pend>0?' ('+pend+' empty)':'')):'\u2014';
  if(total>0&&inc>0)abSt+=' &nbsp;<span style=color:#ffaa44>'+inc+' left</span>';
  else if(total>0&&inc===0&&done>0)abSt+=' &nbsp;<span style=color:#44ff44>&#10003;</span>';
  document.getElementById('d-ab-st').innerHTML=abSt;
  var liveDet=d.live_active?'File '+d.live_up+'/'+d.live_total+(d.live_folder?' &middot; '+d.live_folder:''):'';
  document.getElementById('d-ab-det').innerHTML=liveDet;
  var nb=d.next_backend||'NONE';
  var nbEl=document.getElementById('d-next-be');
  if(nb&&nb!=='NONE'){nbEl.style.display='';
    var nbDone=d.next_done||0,nbTotal=d.next_total||0,nbPct=nbTotal>0?Math.round(nbDone*100/nbTotal):0;
    document.getElementById('d-nb-name').textContent='Next: '+nb;
    document.getElementById('d-pf-next').style.width=nbPct+'%';
    var tsStr=d.next_ts>0?new Date(d.next_ts*1000).toLocaleDateString():'never';
    document.getElementById('d-nb-st').innerHTML=nbDone+'/'+nbTotal+' \u00b7 last '+tsStr+' <em style="color:#666">(stale)</em>';
    document.getElementById('d-nb-det').textContent=d.next_empty>0?d.next_empty+' empty folder(s)':'';
  }else{nbEl.style.display='none';}
  var fst=inc>0?'&#9888; '+inc+' folder(s) pending':(done>0?'&#10003; All synced':'Waiting for first scan');
  seti('d-fst',fst);
  set('sub','Firmware '+d.firmware+' \u00b7 '+fmtUp(d.uptime||0)+' uptime');
  checkMonUploadState();
}

var statusTimer=null,diagTimer=null;
function pollStatus(){
  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    renderStatus(d);
  }).catch(function(){
    statusFailCount++;
    if(statusFailCount>=2||rebootExpected){
      document.getElementById('reboot-overlay').style.display='block';
      seti('d-st','<span class="badge bc">REBOOTING</span>');
    } else {
      set('d-st','Offline');
    }
  });
}
function startStatusPoll(){if(!statusTimer){pollStatus();statusTimer=setInterval(pollStatus,3000);}}

var cpuHistory=[];
function pollDiag(){
  fetch('/api/diagnostics',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    var fhV=d.free_heap||0,maV=d.max_alloc||0;
    var fh=fhV?Math.round(fhV/1024)+' KB':'\u2014';
    var ma=maV?Math.round(maV/1024)+' KB':'\u2014';
    set('d-fh',fh);set('d-ma',ma);
    set('hd-fh',fh);set('hd-ma',ma);
    if(fhV){heapHistory.push({fh:fhV,ma:maV});if(heapHistory.length>MAX_HEAP_SAMPLES)heapHistory.shift();}
    var c0=d.cpu0||0,c1=d.cpu1||0;
    set('hd-c0',c0+'%');set('hd-c1',c1+'%');
    cpuHistory.push({c0:c0,c1:c1});if(cpuHistory.length>MAX_HEAP_SAMPLES)cpuHistory.shift();
    if(curTab==='mem'){updateHeapChart();updateCpuChart();}
  }).catch(function(){});
}
function updateHeapChart(){
  var svg=document.getElementById('heap-svg');
  if(!svg||heapHistory.length<2)return;
  var W=600,H=200,n=heapHistory.length;
  var maxVal=0;
  heapHistory.forEach(function(s){if(s.fh>maxVal)maxVal=s.fh;});
  if(maxVal<65536)maxVal=65536;
  var ptsFh='',ptsMa='';
  heapHistory.forEach(function(s,i){
    var x=((W-2)*i/(MAX_HEAP_SAMPLES-1)+1).toFixed(1);
    var yFh=(H-(s.fh/maxVal)*(H-14)-6).toFixed(1);
    var yMa=(H-(s.ma/maxVal)*(H-14)-6).toFixed(1);
    ptsFh+=(i===0?'M':'L')+x+' '+yFh;
    ptsMa+=(i===0?'M':'L')+x+' '+yMa;
  });
  var grid='';
  [0.25,0.5,0.75].forEach(function(f){
    var y=(H-f*(H-14)-6).toFixed(0);
    var kb=Math.round(maxVal*f/1024);
    grid+='<line x1="0" y1="'+y+'" x2="'+W+'" y2="'+y+'" stroke="#1a2a3a" stroke-width="1"/>';
    grid+='<text x="4" y="'+(parseInt(y)-2)+'" fill="#3a5070" font-size="9" font-family="monospace">'+kb+'K</text>';
  });
  svg.innerHTML=grid
    +'<path d="'+ptsFh+'" stroke="#5c9ade" stroke-width="1.5" fill="none"/>'
    +'<path d="'+ptsMa+'" stroke="#aa66ff" stroke-width="1.5" fill="none"/>';
  if(heapHistory.length>0){
    var minFh=heapHistory.reduce(function(m,s){return s.fh<m?s.fh:m;},heapHistory[0].fh);
    var minMa=heapHistory.reduce(function(m,s){return s.ma<m?s.ma:m;},heapHistory[0].ma);
    set('hd-fh-min',Math.round(minFh/1024)+' KB');
    set('hd-ma-min',Math.round(minMa/1024)+' KB');
  }
}
function updateCpuChart(){
  var svg=document.getElementById('cpu-svg');
  if(!svg||cpuHistory.length<2)return;
  var W=600,H=150,n=cpuHistory.length;
  var ptsC0='',ptsC1='';
  cpuHistory.forEach(function(s,i){
    var x=((W-2)*i/(MAX_HEAP_SAMPLES-1)+1).toFixed(1);
    var y0=(H-(s.c0/100)*(H-14)-6).toFixed(1);
    var y1=(H-(s.c1/100)*(H-14)-6).toFixed(1);
    ptsC0+=(i===0?'M':'L')+x+' '+y0;
    ptsC1+=(i===0?'M':'L')+x+' '+y1;
  });
  var grid='';
  [25,50,75].forEach(function(pct){
    var y=(H-(pct/100)*(H-14)-6).toFixed(0);
    grid+='<line x1="0" y1="'+y+'" x2="'+W+'" y2="'+y+'" stroke="#1a2a3a" stroke-width="1"/>';
    grid+='<text x="4" y="'+(parseInt(y)-2)+'" fill="#3a5070" font-size="9" font-family="monospace">'+pct+'%</text>';
  });
  svg.innerHTML=grid
    +'<path d="'+ptsC0+'" stroke="#ff6b6b" stroke-width="1.5" fill="none"/>'
    +'<path d="'+ptsC1+'" stroke="#ffd93d" stroke-width="1.5" fill="none"/>';
}
function syncMonBtn(){
  var busy=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  var btnSt=document.getElementById('btn-mst');
  if(!btnSt)return;
  if(monActive){
    btnSt.style.display='none';
  }else if(busy){
    btnSt.style.display='inline-flex';
    btnSt.disabled=true;
    btnSt.style.opacity='.4';
    btnSt.style.cursor='not-allowed';
    btnSt.textContent='Start Monitoring';
  }else{
    btnSt.style.display='inline-flex';
    btnSt.disabled=false;
    btnSt.style.opacity='1';
    btnSt.style.cursor='pointer';
    btnSt.textContent='Start Monitoring';
  }
}
function checkMonUploadState(){
  var busy=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  var w=document.getElementById('mon-upwarn');
  if(w)w.style.display=busy?'':'none';
  syncMonBtn();
}
function startDiagPoll(){if(!diagTimer){pollDiag();diagTimer=setInterval(pollDiag,2000);}}
startDiagPoll();

var cfgLocked=false;
function _setCfgLockUI(locked){
  cfgLocked=locked;
  document.getElementById('cfg-lock-banner').style.display=locked?'':'none';
  document.getElementById('cfg-lock-badge').style.display=locked?'':'none';
  var ta=document.getElementById('cfg-raw');
  ta.readOnly=!locked;
  ta.style.background=locked?'#1b2838':'#111820';
  ta.style.color=locked?'#c7d5e0':'#6a7a8a';
  ta.style.borderColor=locked?'#3d4450':'#2d3440';
  document.getElementById('btn-cfg-edit').style.display=locked?'none':'';
  document.getElementById('btn-cfg-reload').style.display=locked?'':'none';
  document.getElementById('btn-cfg-savereboot').style.display=locked?'':'none';
  document.getElementById('btn-cfg-cancel').style.display=locked?'':'none';
}
function acquireCfgLock(){
  document.getElementById('cfg-warn-modal').style.display='flex';
}
function _doCfgLock(){
  var active=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  if(active&&!confirm('An upload is currently in progress.\n\nThe upload will continue running. You can edit config and Save & Reboot when ready.\n\nContinue?'))return;
  _setCfgLockUI(true);
  loadRawCfg();
}
function releaseCfgLock(){
  _setCfgLockUI(false);
  document.getElementById('cfg-raw-msg').textContent='';
}
function loadCfg(){
  fetch('/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    cfg=d;
  }).catch(function(){});
}
function cfgRawCount(){
  var t=document.getElementById('cfg-raw');
  document.getElementById('cfg-raw-cnt').textContent=t.value.length+' / 4096 bytes';
}
function loadRawCfg(){
  var msg=document.getElementById('cfg-raw-msg');
  msg.textContent='Loading...';
  fetch('/api/config-raw',{cache:'no-store'}).then(function(r){
    if(!r.ok)return r.text().then(function(t){throw new Error(t);});
    return r.text();
  }).then(function(t){
    document.getElementById('cfg-raw').value=t;
    cfgRawCount();
    msg.textContent='';
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Load failed: '+e.message;});
}
function saveRawCfg(){
  var body=document.getElementById('cfg-raw').value;
  var msg=document.getElementById('cfg-raw-msg');
  msg.style.color='#8f98a0';msg.textContent='Saving...';
  fetch('/api/config-raw',{method:'POST',headers:{'Content-Type':'text/plain'},body:body,cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      msg.style.color='#57cbde';msg.innerHTML='\u2713 Saved \u2014 <strong>reboot required</strong> for changes to take effect.';
      releaseCfgLock();
    }else{msg.style.color='#ff6060';msg.textContent='Error: '+d.error;}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Failed: '+e.message;});
}
function saveAndReboot(){
  var body=document.getElementById('cfg-raw').value;
  var msg=document.getElementById('cfg-raw-msg');
  msg.style.color='#8f98a0';msg.textContent='Saving...';
  fetch('/api/config-raw',{method:'POST',headers:{'Content-Type':'text/plain'},body:body,cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      _setCfgLockUI(false);
      document.getElementById('cfg-lock-banner').style.display='none';
      msg.style.color='#57cbde';msg.textContent='Saved — rebooting… redirecting in 10s';rebootExpected=true;
      setTimeout(function(){fetch('/soft-reboot',{cache:'no-store'});},800);
      setTimeout(function(){window.location.href='/';},10000);
    }else{msg.style.color='#ff6060';msg.textContent='Error: '+d.error;}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Failed: '+e.message;});
}

// Client-side log buffer — persists across soft-reboots in browser memory
var logAtBottom=true,clientLogBuf=[],lastSeenLine='',LOG_BUF_MAX=2000,newBootDetected=false;
function _appendLogs(text){
  if(!text)return;
  newBootDetected=false;
  var lines=text.split('\n');
  // Detect reboot: find the LAST boot banner so that when /api/logs/full
  // contains old syslog content with ancient banners, we use the most recent one.
  var bootIdx=-1;
  for(var i=lines.length-1;i>=0;i--){
    if(lines[i].indexOf('=== CPAP Data Auto-Uploader ===')>=0){bootIdx=i;break;}
  }
  var newLines;
  if(bootIdx>=0){
    // Boot banner found. Determine if this is genuinely a NEW reboot or the
    // same boot we already buffered (banner is present in every server response).
    // A new reboot means lastSeenLine doesn't exist in this response at all, OR
    // it appears before/at the boot banner (i.e. it belongs to a prior boot).
    var lastSeenPos=-1;
    if(lastSeenLine){
      for(var i=lines.length-1;i>=0;i--){
        if(lines[i]===lastSeenLine){lastSeenPos=i;break;}
      }
    }
    if(lastSeenPos>bootIdx){
      // Same boot continuing — treat as normal poll, append only new tail
      newLines=lines.slice(lastSeenPos+1);
    } else if(clientLogBuf.length===0){
      // Fresh start (initial page load or post-reboot buffer clear).
      // Include ALL lines so full NAND history + pre-reboot context is shown.
      var startFrom=0;
      while(startFrom<lines.length&&!lines[startFrom].trim())startFrom++;
      newLines=lines.slice(startFrom);
    } else {
      // Genuinely new reboot — insert separator.
      // Search backwards from boot banner for the === BOOT separator
      // (written by enableLogSaving on boot) to capture pre-reboot context
      // like "Rebooting for clean state reset..." that's only in NAND syslog.
      newBootDetected=true;
      var ctxStart=bootIdx;
      for(var j=bootIdx-1;j>=Math.max(0,bootIdx-60);j--){
        if(lines[j].indexOf('=== BOOT ')>=0){ctxStart=Math.max(0,j-10);break;}
      }
      clientLogBuf.push('','\u2500\u2500\u2500 DEVICE REBOOTED \u2500\u2500\u2500','');
      newLines=lines.slice(ctxStart);
    }
  } else {
    // No boot banner in response. Two sub-cases:
    // (a) Same boot, buffer hasn't wrapped — lastSeenLine IS found → normal dedup
    // (b) Reboot happened, buffer wrapped, boot banner overwritten — lastSeenLine NOT found
    var startFrom=0;
    var lastSeenFound=false;
    if(lastSeenLine){
      for(var i=lines.length-1;i>=0;i--){
        if(lines[i]===lastSeenLine){startFrom=i+1;lastSeenFound=true;break;}
      }
    }
    if(lastSeenLine&&!lastSeenFound){
      // lastSeenLine not found and no boot banner — likely a reboot where the
      // 12KB circular buffer wrapped (boot banner overwritten by later boot logs).
      // Signal newBootDetected so fetchLogs triggers a /api/logs/full backfill
      // which has the complete NAND history including pre-reboot context.
      newBootDetected=true;
    }
    newLines=lines.slice(startFrom);
  }
  // Strip trailing empty lines — server responses end with blank lines that
  // would otherwise be appended as "new" on every poll.
  while(newLines.length>0&&!newLines[newLines.length-1].trim())newLines.pop();
  for(var i=0;i<newLines.length;i++){
    if(newLines[i]!==undefined)clientLogBuf.push(newLines[i]);
  }
  // Track the last non-empty line we put into the buffer for next dedup pass
  for(var i=clientLogBuf.length-1;i>=0;i--){
    if(clientLogBuf[i]){lastSeenLine=clientLogBuf[i];break;}
  }
  if(clientLogBuf.length>LOG_BUF_MAX)clientLogBuf=clientLogBuf.slice(clientLogBuf.length-LOG_BUF_MAX);
}
function _renderLogBuf(){
  var b=document.getElementById('log-box');
  logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
  b.textContent=clientLogBuf.join('\n');
  if(logAtBottom)b.scrollTop=b.scrollHeight;
}
var sseSource=null,sseConnected=false,backfillDone=false,backfillRetries=0;
function fetchLogs(){
  if(curTab!=='logs')return;
  fetch('/api/logs',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){
    _appendLogs(t);
    _renderLogBuf();
    // If polling detected a new boot (boot banner found, or lastSeenLine vanished
    // because buffer wrapped), re-fetch /api/logs/full to get NAND pre-reboot
    // context (reboot reason, state reset trigger, early boot lines).
    if(newBootDetected&&backfillDone){
      backfillDone=false;
      stopLogPoll();
      // Clear buffer so backfill rebuilds from scratch with full NAND context
      clientLogBuf=[];lastSeenLine='';
      fetchBackfill();
      return;
    }
    set('log-st',(sseConnected?'SSE Live':'Polling')+' \u2022 '+clientLogBuf.length+' lines');
  }).catch(function(){set('log-st','Disconnected');});
}
function fetchBackfill(){
  seti('log-st','<span class="log-spinner"></span>Loading history\u2026');
  fetch('/api/logs/full',{cache:'no-store'}).then(function(r){
    // Use streaming to show progressive loading feedback
    var reader=r.body?r.body.getReader():null;
    if(!reader) return r.text();
    var chunks=[],totalBytes=0,decoder=new TextDecoder();
    function pump(){
      return reader.read().then(function(result){
        if(result.done){return chunks.join('');}
        var chunk=decoder.decode(result.value,{stream:true});
        chunks.push(chunk);
        totalBytes+=result.value.length;
        var lines=0;for(var i=0;i<chunk.length;i++){if(chunk.charCodeAt(i)===10)lines++;}
        seti('log-st','<span class="log-spinner"></span>Loading\u2026 '+(totalBytes/1024).toFixed(0)+' KB received');
        return pump();
      });
    }
    return pump();
  }).then(function(t){
    _appendLogs(t);
    _renderLogBuf();
    backfillDone=true;
    backfillRetries=0;
    set('log-st','Loaded \u2022 '+clientLogBuf.length+' lines');
    startSse();
  }).catch(function(){
    // Device might still be rebooting — retry with exponential backoff
    backfillRetries++;
    if(backfillRetries<6){
      var delay=Math.min(3000*backfillRetries,15000);
      set('log-st','Device offline \u2014 retry '+backfillRetries+'/5 in '+(delay/1000)+'s\u2026');
      setTimeout(function(){if(curTab==='logs'){fetchBackfill();}},delay);
    } else {
      backfillDone=true;
      backfillRetries=0;
      startLogPoll();
    }
  });
}
function startSse(){
  if(sseSource)return;
  if(typeof EventSource==='undefined'){startLogPoll();return;}
  sseSource=new EventSource('/api/logs/stream');
  sseSource.onopen=function(){
    sseConnected=true;
    stopLogPoll();
    set('log-st','SSE Live \u2022 '+clientLogBuf.length+' lines');
  };
  sseSource.onmessage=function(e){
    if(!e.data)return;
    _appendLogs(e.data+'\n');
    _renderLogBuf();
    set('log-st','SSE Live \u2022 '+clientLogBuf.length+' lines');
  };
  sseSource.onerror=function(){
    sseConnected=false;
    if(sseSource){sseSource.close();sseSource=null;}
    backfillDone=false;
    set('log-st','SSE lost \u2014 reconnecting\u2026');
    setTimeout(function(){if(curTab==='logs'){fetchBackfill();}else{startLogPoll();}},3000);
  };
}
function stopSse(){
  if(sseSource){sseSource.close();sseSource=null;sseConnected=false;}
}
function clearLogBuf(){clientLogBuf=[];lastSeenLine='';document.getElementById('log-box').textContent='';}
function downloadSavedLogs(){
  var a=document.createElement('a');
  a.href='/api/logs/saved';
  a.download='cpap_logs.txt';
  document.body.appendChild(a);a.click();document.body.removeChild(a);
}
function copyLogBuf(){
  var txt=clientLogBuf.join('\n');
  if(!txt){return;}
  if(navigator.clipboard&&navigator.clipboard.writeText){
    navigator.clipboard.writeText(txt).then(function(){set('log-st','Copied! \u2022 '+clientLogBuf.length+' lines');setTimeout(function(){set('log-st',(sseConnected?'SSE Live':'Polling')+' \u2022 '+clientLogBuf.length+' lines buffered');},2000);});
  } else {
    var ta=document.createElement('textarea');ta.value=txt;ta.style.position='fixed';ta.style.opacity='0';
    document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);
    set('log-st','Copied! \u2022 '+clientLogBuf.length+' lines');setTimeout(function(){set('log-st',(sseConnected?'SSE Live':'Polling')+' \u2022 '+clientLogBuf.length+' lines buffered');},2000);
  }
}
document.getElementById('log-box').addEventListener('scroll',function(){
  var b=this;logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
});
function startLogPoll(){if(!logPoll){fetchLogs();logPoll=setInterval(fetchLogs,4000);}}
function stopLogPoll(){if(logPoll){clearInterval(logPoll);logPoll=null;}}

function startMon(){
  var busy=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  if(busy){toast('Cannot start monitoring while upload is in progress.','warn');return;}
  monActive=true;
  fetch('/api/monitor-start',{cache:'no-store'});
  document.getElementById('btn-mst').style.display='none';
  document.getElementById('btn-msp').style.display='inline-flex';
  if(!monPoll)monPoll=setInterval(fetchMon,2000);
  fetchMon();
  _updateMonBanner();
}
function stopMon(){
  if(monActive){
    monActive=false;
    fetch('/api/monitor-stop',{cache:'no-store'});
  }
  document.getElementById('btn-mst').style.display='inline-flex';
  document.getElementById('btn-msp').style.display='none';
  if(monPoll){clearInterval(monPoll);monPoll=null;}
  _updateMonBanner();
}
function _updateMonBanner(){
  var b=document.getElementById('mon-active-banner');
  if(b)b.style.display=(monActive&&curTab!=='mon')?'block':'none';
}
function fetchMon(){
  fetch('/api/sd-activity',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    set('m-p',d.last_pulse_count);
    set('m-i',(d.consecutive_idle_ms/1000).toFixed(1)+'s');
    set('m-l',(d.longest_idle_ms/1000).toFixed(1)+'s');
    set('m-r',d.total_active_samples+'/'+d.total_idle_samples);
    var dot=document.getElementById('mon-dot');
    if(dot)dot.className='dot '+(d.is_busy?'busy':'idle');
    if(d.samples&&d.samples.length){
      monHistory=[];
      d.samples.forEach(function(s){
        monHistory.push({p:s.p});
      });
      while(monHistory.length>MON_HIST_MAX)monHistory.shift();
      updateMonChart();
      var last=d.samples[d.samples.length-1];
      if(last){
        var now=new Date();
        var ts=String(now.getHours()).padStart(2,'0')+':'+String(now.getMinutes()).padStart(2,'0')+':'+String(now.getSeconds()).padStart(2,'0');
        var idleS=(d.consecutive_idle_ms/1000).toFixed(1);
        var sinceStr='—';
        if(last.a&&monLogLastWriteTs>0){
          var gap=Math.round((Date.now()-monLogLastWriteTs)/1000);
          sinceStr=gap+'s ago';
        }
        if(last.a)monLogLastWriteTs=Date.now();
        var shouldLog=true;
        if(monLog.length>0){
          var prev=monLog[monLog.length-1];
          if(prev.rawP===last.p&&prev.rawP===0)shouldLog=false;
        }
        if(shouldLog){
          monLog.push({ts:ts,p:last.p,idle:idleS+'s',since:sinceStr,rawP:last.p});
          if(monLog.length>MON_LOG_MAX)monLog.shift();
          var cutoff=Date.now()-24*3600*1000;
          while(monLog.length>0&&monLog[0].epoch&&monLog[0].epoch<cutoff)monLog.shift();
          renderMonLog();
        }
      }
    }
    if(profActive){
      var li=d.longest_idle_ms/1000;
      if(li>profMaxIdle)profMaxIdle=li;
      document.getElementById('prof-max-idle').textContent=profMaxIdle.toFixed(1)+'s';
      if(profMaxIdle>0){
        document.getElementById('prof-rec-box').style.display='block';
        document.getElementById('prof-rec-val').textContent=Math.ceil(profMaxIdle+4)+'';
      }
    }
  }).catch(function(){});
}

var monHistory=[],MON_HIST_MAX=120,monLog=[],MON_LOG_MAX=2000,monLogLastWriteTs=0;
function updateMonChart(){
  var svg=document.getElementById('mon-svg');
  if(!svg||monHistory.length<2)return;
  var W=600,H=120,n=monHistory.length;
  var maxP=1;
  monHistory.forEach(function(s){if(s.p>maxP)maxP=s.p;});
  maxP=Math.max(maxP,10);
  var pts='';
  monHistory.forEach(function(s,i){
    var x=((W-2)*i/(MON_HIST_MAX-1)+1).toFixed(1);
    var y=(H-(s.p/maxP)*(H-14)-6).toFixed(1);
    pts+=(i===0?'M':'L')+x+' '+y;
  });
  var fill=pts+'L'+((W-2)*(n-1)/(MON_HIST_MAX-1)+1).toFixed(1)+' '+(H-6)+'L1 '+(H-6)+'Z';
  var grid='';
  [0.25,0.5,0.75].forEach(function(f){
    var y=(H-f*(H-14)-6).toFixed(0);
    var v=Math.round(maxP*f);
    grid+='<line x1="0" y1="'+y+'" x2="'+W+'" y2="'+y+'" stroke="#1a2a3a" stroke-width="1"/>';
    grid+='<text x="4" y="'+(parseInt(y)-2)+'" fill="#3a5070" font-size="9" font-family="monospace">'+v+'</text>';
  });
  svg.innerHTML=grid
    +'<path d="'+fill+'" fill="rgba(255,68,68,0.15)" stroke="none"/>'
    +'<path d="'+pts+'" stroke="#ff4444" stroke-width="1.5" fill="none"/>';
}
function renderMonLog(){
  var body=document.getElementById('mon-log-body');
  if(!body||monLog.length===0)return;
  var h='';var show=Math.min(monLog.length,200);
  for(var i=monLog.length-1;i>=Math.max(0,monLog.length-show);i--){
    var e=monLog[i];
    var pc=e.p>0?'color:#ff6b6b':'color:#44ff44';
    h+='<tr style="border-bottom:1px solid #1a2a3a"><td style="padding:3px 8px;color:#8f98a0">'+e.ts+'</td><td style="padding:3px 8px;'+pc+'">'+e.p+'</td><td style="padding:3px 8px;color:#c7d5e0">'+e.idle+'</td><td style="padding:3px 8px;color:#8f98a0">'+e.since+'</td></tr>';
  }
  body.innerHTML=h;
}
var profActive=false,profMaxIdle=0;
function openProfilerWizard(){
  document.getElementById('prof-wiz').style.display='flex';
}
function startProfiler(){
  if(profActive){
    profActive=false;
    document.getElementById('btn-prof-start').textContent='Start Profiling';
    document.getElementById('btn-prof-start').style.background='#aa66ff';
    return;
  }
  profActive=true;profMaxIdle=0;
  document.getElementById('prof-max-idle').textContent='0.0s';
  document.getElementById('prof-rec-box').style.display='none';
  document.getElementById('btn-prof-start').textContent='Stop Profiling';
  document.getElementById('btn-prof-start').style.background='#e04030';
  startMon();
}

function triggerUpload(){
  var b=document.getElementById('btn-up');
  if(b._busy)return;b._busy=1;b.textContent='Triggering...';
  fetch('/trigger-upload',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    var mode=d.status==='success'?'ok':d.status==='scheduled'?'warn':'er';
    toast(d.message||'Upload triggered.',mode);
  }).catch(function(){toast('Failed to trigger upload.','er');
  }).finally(function(){setTimeout(function(){b._busy=0;b.textContent='\u25b2 Force Upload';},700);});
}
function softReboot(){
  var b=document.getElementById('btn-srb');
  if(b._busy)return;b._busy=1;b.textContent='Rebooting...';rebootExpected=true;
  fetch('/soft-reboot',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    toast(d.message||'Rebooting...',true);
  }).catch(function(){toast('Failed to reboot.',false);
  }).finally(function(){setTimeout(function(){b._busy=0;b.innerHTML='&#8635; Soft Reboot';},4000);});
}
function resetState(){
  if(!confirm('Reset all upload state? This cannot be undone.'))return;
  var b=document.getElementById('btn-rst');
  if(b._busy)return;b._busy=1;b.textContent='Resetting...';rebootExpected=true;
  fetch('/reset-state',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    toast(d.message||'State reset.',true);
  }).catch(function(){toast('Failed to reset state.',false);
  }).finally(function(){setTimeout(function(){b._busy=0;b.innerHTML='&#9762; Reset State';},1000);});
}

var otaBusy=false;
function setMsg(id,cls,msg){var e=document.getElementById(id);if(e){e.className='sm '+cls;e.textContent=msg;}}
document.getElementById('f-up').addEventListener('submit',function(e){
  e.preventDefault();if(otaBusy)return;
  var f=document.getElementById('f-bin').files[0];if(!f){alert('Select a file');return;}
  otaBusy=true;setMsg('s-up','info','Uploading 0%...');
  var fd=new FormData();fd.append('firmware',f);
  var x=new XMLHttpRequest();
  x.upload.addEventListener('progress',function(ev){if(ev.lengthComputable)setMsg('s-up','info','Uploading '+Math.round(ev.loaded/ev.total*100)+'%...');});
  x.addEventListener('load',function(){try{handleOtaResult(JSON.parse(x.responseText),'s-up');}catch(er){otaBusy=false;setMsg('s-up','er','Invalid response');}});
  x.addEventListener('error',function(){otaBusy=false;setMsg('s-up','er','Network error');});
  x.open('POST','/ota-upload');x.send(fd);
});
document.getElementById('f-url').addEventListener('submit',function(e){
  e.preventDefault();if(otaBusy)return;
  var u=document.getElementById('f-u').value;if(!u)return;
  otaBusy=true;setMsg('s-url','info','Downloading (may take ~1 min)...');
  var fd=new FormData();fd.append('url',u);
  fetch('/ota-url',{method:'POST',body:fd}).then(function(r){return r.json();}).then(function(d){handleOtaResult(d,'s-url');}).catch(function(){otaBusy=false;setMsg('s-url','er','Network error');});
});
function handleOtaResult(d,sid){
  otaBusy=false;
  if(d.success){setMsg(sid,'ok','Success! '+d.message);var t=30;var iv=setInterval(function(){t--;setMsg(sid,'ok','Redirecting in '+t+'s...');if(t<=0){clearInterval(iv);location.href='/';}},1000);}
  else setMsg(sid,'er','Failed: '+d.message);
}

loadCfg();
startStatusPoll();
</script>
</body></html>)HTMLEOF";

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
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1923;color:#c7d5e0;min-height:100vh;padding:16px}
.wrap{max-width:900px;margin:0 auto}
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
.bi{background:#2a475e;color:#8f98a0}.bl{background:#1a3a1a;color:#44ff44;animation:pu 2s infinite}
.ba,.bu{background:#1a2a4a;color:#66c0f4}.bc,.br{background:#3a2a1a;color:#ffaa44}
.bco{background:#1a3a1a;color:#44ff44}
@keyframes pu{0%,100%{opacity:1}50%{opacity:.6}}
.prog{background:#2a475e;border-radius:5px;height:8px;margin-top:5px;overflow:hidden}
.pf{background:linear-gradient(90deg,#66c0f4,#44aaff);height:100%;border-radius:5px;transition:width .5s}
.actions{display:flex;flex-wrap:wrap;gap:7px;margin-top:7px}
.btn{display:inline-flex;align-items:center;gap:5px;padding:8px 15px;border-radius:6px;font-size:.83em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all .2s}
.bp{background:#66c0f4;color:#0f1923}.bp:hover{background:#88d0ff}
.bs{background:#2a475e;color:#c7d5e0}.bs:hover{background:#3a5a7e}
.bd{background:#c0392b;color:#fff}.bd:hover{background:#e04030}
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
</style></head><body>
<div class=wrap>
<h1>CPAP Data Uploader</h1>
<p class=sub id=sub>Connecting...</p>
<nav>
<button id=t-dash onclick="tab('dash')" class=act>Dashboard</button>
<button id=t-logs onclick="tab('logs')">Logs</button>
<button id=t-cfg onclick="tab('cfg')">Config</button>
<button id=t-mon onclick="tab('mon')">Monitor</button>
<button id=t-mem onclick="tab('mem')">Memory</button>
<button id=t-ota onclick="tab('ota')">OTA</button>
</nav>

<!-- DASHBOARD -->
<div id=dash class="page on">
<div class=cards>
<div class=card><h2>Upload Engine</h2>
<div class=row><span class=k>State</span><span id=d-st class=v></span></div>
<div class=row><span class=k>In state</span><span id=d-ins class=v></span></div>
<div class=row><span class=k>Mode</span><span id=d-mode class=v></span></div>
<div class=row><span class=k>Time synced</span><span id=d-tsync class=v></span></div>
<div class=row><span class=k>Upload window</span><span id=d-win class=v></span></div>
<div class=row><span class=k>Next upload</span><span id=d-next class=v></span></div>
</div>
<div class=card><h2>System</h2>
<div class=row><span class=k>Time</span><span id=d-time class=v></span></div>
<div class=row><span class=k>Free heap</span><span id=d-fh class=v></span></div>
<div class=row><span class=k>Max alloc</span><span id=d-ma class=v></span></div>
<div class=row><span class=k>WiFi</span><span id=d-wifi class=v></span></div>
<div class=row><span class=k>IP</span><span id=d-ip class=v></span></div>
<div class=row><span class=k>Endpoint</span><span id=d-ep class=v></span></div>
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
<div class=card style="margin-bottom:14px"><h2>Actions</h2>
<div class=actions>
<button id=btn-up class="btn bp" onclick=triggerUpload()>&#9650; Trigger Upload</button>
<button id=btn-rst class="btn bd" onclick=resetState()>Reset State</button>
</div>
</div>
</div>

<!-- LOGS -->
<div id=logs class=page>
<div class=card style="margin-bottom:10px">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px">
<h2 style=margin:0>System Logs <span id=log-st style="font-size:.9em;color:#8f98a0;font-weight:400"></span></h2>
<div style="display:flex;gap:6px">
<button class="btn bs" onclick=downloadSavedLogs() style="padding:4px 10px;font-size:.8em" title="Download persisted log files from device flash">&#11015; Download Saved Logs</button>
<button class="btn bs" onclick=copyLogBuf() style="padding:4px 10px;font-size:.8em" title="Copy all buffered log lines to clipboard">&#128203; Copy to clipboard</button>
<button class="btn bs" onclick=clearLogBuf() style="padding:4px 10px;font-size:.8em">&#128465; Clear buffer</button>
</div>
</div>
<div id=log-box>Loading...</div>
</div>
</div>

<!-- CONFIG -->
<div id=cfg class=page>
<div id=cfg-lock-banner style="display:none;background:#2a2a00;border:1px solid #aa9900;border-radius:6px;padding:10px 14px;margin-bottom:10px;font-size:.85em;color:#ddcc88">
&#128274; <strong>Upload paused</strong> &mdash; Config editor is active. Press <em>Cancel</em> to resume without saving, or <em>Save &amp; Reboot</em> to apply changes. After reboot, <strong>always</strong> physically eject and reinsert the CPAP&rsquo;s SD card before powering it on.
</div>
<div class=card>
<h2>Edit config.txt
<span id=cfg-lock-badge style="display:none;margin-left:8px;background:#4a8a4a;color:#fff;font-size:.65em;padding:2px 7px;border-radius:10px;font-weight:600;vertical-align:middle">LOCKED</span>
</h2>
<p style="font-size:.82em;color:#8f98a0;margin-bottom:8px">Direct editor for the SD card config file. Passwords stored securely in flash appear as <code>***STORED_IN_FLASH***</code> &mdash; leave them unchanged to keep existing credentials. Max 4096 bytes. <strong>Changes take effect after reboot.</strong></p>
<p style="font-size:.82em;color:#ffcc44;margin-bottom:8px">&#9888; Click <strong>Edit</strong> to pause active uploads and start editing.</p>
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
<div class=card style="margin-bottom:10px"><h2>SD Activity Monitor <span id=mon-dot class="dot idle"></span></h2>
<p style="font-size:.85em;color:#c7d5e0;line-height:1.5;margin-bottom:10px">Monitors SD card bus activity. Use when CPAP machine is on. Red = CPAP writing, Green = safe to upload.</p>
<div id=mon-upwarn style="display:none;background:#2a2a1a;border:1px solid #665522;border-radius:6px;padding:9px 13px;margin-bottom:10px;font-size:.84em;color:#ddcc88">&#9889; Upload in progress — monitoring will start automatically when the upload finishes.</div>
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
<div class=card><h2>Activity Timeline (Last 60s)</h2>
<div class=chart id=m-ch><em>Waiting for data...</em></div>
</div>
</div>


<!-- PROFILER WIZARD MODAL -->
<div id="prof-wiz" style="display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.85);z-index:9999;display:none;align-items:center;justify-content:center">
<div style="background:#1b2838;border:1px solid #66c0f4;border-radius:12px;padding:25px;max-width:550px;width:90%;box-shadow:0 10px 40px rgba(102,192,244,0.2)">
<h2 style="color:#fff;margin-bottom:10px;font-size:1.4em">&#9881; CPAP Profiler Wizard</h2>
<p style="font-size:0.9em;color:#c7d5e0;line-height:1.5;margin-bottom:20px">
This tool will measure your CPAP machine's specific SD card writing behavior to help you tune <strong style="color:#66c0f4">INACTIVITY_SECONDS</strong> (SMART_WAIT).
</p>
<div style="background:#0f1923;padding:15px;border-radius:8px;margin-bottom:20px;border:1px solid #2a475e">
<ol style="font-size:0.85em;color:#8f98a0;padding-left:20px;line-height:1.6">
<li>Ensure the CPAP machine is <strong>turned ON</strong> and actively blowing air.</li>
<li>Ensure the SD card is physically inserted into the CPAP.</li>
<li>Click <strong>Start Profiling</strong>.</li>
<li><strong style="color:#ffcc44">Breathe in and out continuously as you would during normal therapy</strong> — do not pause or hold your breath. The profiler measures real SD write gaps during active therapy.</li>
<li>Wait 2–3 minutes. The wizard will record the longest continuous silence between SD writes and suggest an <strong>INACTIVITY_SECONDS</strong> value.</li>
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
<div style="font-size:0.75em;color:#8f98a0;margin-top:4px">(Longest silence + 5 second safety margin)</div>
</div>
<div style="display:flex;gap:10px;justify-content:flex-end">
<button id="btn-prof-cancel" class="btn bs" onclick="document.getElementById('prof-wiz').style.display='none'">Close</button>
<button id="btn-prof-start" class="btn bp" onclick="startProfiler()" style="background:#aa66ff;color:#fff">Start Profiling</button>
</div>
</div>
</div>

<!-- MEMORY -->
<div id=mem class=page>
<div class=card style="margin-bottom:10px"><h2>Runtime Memory <span style="font-size:.7em;color:#8f98a0;font-weight:400">(live, 2s)</span></h2>
<div class=stats-grid>
<div class=stat-box><span class=sl>Free Heap</span><span class=sv id=hd-fh style="color:#66c0f4">—</span><span class=sl style="margin-top:6px">Min (2m)</span><span class=sv id=hd-fh-min style="color:#ddaa44">—</span></div>
<div class=stat-box><span class=sl>Max Contiguous</span><span class=sv id=hd-ma style="color:#aa66ff">—</span><span class=sl style="margin-top:6px">Min (2m)</span><span class=sv id=hd-ma-min style="color:#ddaa44">—</span></div>
</div>
</div>
<div class=card>
<h2>Heap History <span style="font-size:.65em;color:#8f98a0;font-weight:400">&nbsp;last ~2 min &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#5c9ade;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Free &nbsp;<span style="display:inline-block;width:12px;height:3px;background:#aa66ff;vertical-align:middle;margin-right:3px;border-radius:2px"></span>Max Alloc</span></h2>
<div style="background:#0f1923;border-radius:6px;padding:8px 4px 2px">
<svg id=heap-svg viewBox="0 0 600 200" preserveAspectRatio="none" style="width:100%;height:200px;display:block"></svg>
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
var heapHistory=[],MAX_HEAP_SAMPLES=60,currentFsmState='';
function tab(t){
  ['dash','logs','cfg','mon','mem','ota'].forEach(function(x){
    document.getElementById(x).classList.toggle('on',x===t);
    document.getElementById('t-'+x).classList.toggle('act',x===t);
  });
  curTab=t;
  if(t==='logs'){startLogPoll();}else{stopLogPoll();}
  if(t!=='mon'){stopMon();}
  if(t==='cfg'){loadCfg();}
  if(t==='mon'){checkMonUploadState();}
  if(t==='mem'){updateHeapChart();}
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
  return '<span class="badge '+c+'">'+st+'</span>';
}
function set(id,html,inner){var el=document.getElementById(id);if(el){if(inner===false)el.innerHTML=html;else el.textContent=html;}}
function seti(id,html){set(id,html,false);}

function renderStatus(d){
  currentFsmState=d.state||'';
  seti('d-st',badgeHtml(currentFsmState||'?'));
  var ins=d.in_state_sec||0;set('d-ins',ins<60?ins+'s':Math.floor(ins/60)+'m '+ins%60+'s');
  set('d-mode',cfg.upload_mode||'—');
  set('d-tsync',d.time_synced?'Yes':'No');
  var ws=(cfg.upload_start_hour!=null&&cfg.upload_end_hour!=null)?cfg.upload_start_hour+':00 - '+cfg.upload_end_hour+':00':'—';
  set('d-win',ws);
  var nx=d.next_upload;
  set('d-next',nx<0?'—':nx===0?'Now':fmtUp(nx));
  set('d-time',d.time||'—');
  set('d-fh',d.free_heap?Math.round(d.free_heap/1024)+' KB':'—');
  set('d-ma',d.max_alloc?Math.round(d.max_alloc/1024)+' KB':'—');
  if(d.wifi){
    var rc=sigClass(d.rssi),rl=sigLabel(d.rssi);
    document.getElementById('d-wifi').innerHTML='<span class='+rc+'>'+rl+' ('+d.rssi+' dBm)</span>';
  }else{set('d-wifi','Disconnected');}
  set('d-ip',d.wifi_ip||'—');
  set('d-ep',cfg.endpoint_type||d.endpoint_type||'—');
  set('d-up',fmtUp(d.uptime||0));
  var ab=d.active_backend||'NONE';
  var abColor=ab==='SMB'?'#66c0f4':ab==='CLOUD'?'#aa66ff':'#8f98a0';
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
  if(curTab==='mon')checkMonUploadState();
}

var statusTimer=null,diagTimer=null;
function pollStatus(){
  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    renderStatus(d);
  }).catch(function(){set('d-st','Offline');});
}
function startStatusPoll(){if(!statusTimer){pollStatus();statusTimer=setInterval(pollStatus,3000);}}

function pollDiag(){
  fetch('/api/diagnostics',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    var fhV=d.free_heap||0,maV=d.max_alloc||0;
    var fh=fhV?Math.round(fhV/1024)+' KB':'\u2014';
    var ma=maV?Math.round(maV/1024)+' KB':'\u2014';
    set('d-fh',fh);set('d-ma',ma);
    set('hd-fh',fh);set('hd-ma',ma);
    if(fhV){heapHistory.push({fh:fhV,ma:maV});if(heapHistory.length>MAX_HEAP_SAMPLES)heapHistory.shift();}
    if(curTab==='mem')updateHeapChart();
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
function checkMonUploadState(){
  var busy=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  var w=document.getElementById('mon-upwarn');
  if(w)w.style.display=busy?'':'none';
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
  var active=currentFsmState==='UPLOADING'||currentFsmState==='ACQUIRING';
  if(active&&!confirm('An upload is currently in progress.\n\nThe upload will continue running. You can edit config and Save & Reboot when ready.\n\nContinue?'))return;
  var msg=document.getElementById('cfg-raw-msg');
  msg.style.color='#8f98a0';msg.textContent='Requesting lock...';
  fetch('/api/config-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"lock":true}',cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){_setCfgLockUI(true);loadRawCfg();msg.textContent='';}
    else{msg.style.color='#ff6060';msg.textContent='Lock failed: '+(d.error||'?');}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Lock error: '+e.message;});
}
function releaseCfgLock(){
  fetch('/api/config-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"lock":false}',cache:'no-store'});
  _setCfgLockUI(false);
  document.getElementById('cfg-raw-msg').textContent='';
}
function loadCfg(){
  fetch('/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    cfg=d;
  }).catch(function(){});
  if(!cfgLocked){_setCfgLockUI(false);loadRawCfg();}
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
      msg.style.color='#57cbde';msg.textContent='Saved. Rebooting...';
      fetch('/api/config-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"lock":false}',cache:'no-store'});
      setTimeout(function(){fetch('/soft-reboot',{cache:'no-store'});},800);
    }else{msg.style.color='#ff6060';msg.textContent='Error: '+d.error;}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Failed: '+e.message;});
}

// Client-side log buffer — persists across soft-reboots in browser memory
var logAtBottom=true,clientLogBuf=[],lastSeenLine='',LOG_BUF_MAX=2000;
function _appendLogs(text){
  if(!text)return;
  var lines=text.split('\n');
  // Detect reboot: scan ALL lines for the boot banner (not just line 0,
  // because the server prepends a space byte before streaming the buffer).
  var bootIdx=-1;
  for(var i=0;i<lines.length;i++){
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
    } else {
      // Genuinely new reboot — insert separator and start from boot banner
      if(clientLogBuf.length>0)clientLogBuf.push('','\u2500\u2500\u2500 DEVICE REBOOTED \u2500\u2500\u2500','');
      newLines=lines.slice(bootIdx);
    }
  } else {
    // Normal poll — find the last line we already buffered (search from end)
    // and only append what comes after it.
    var startFrom=0;
    if(lastSeenLine){
      for(var i=lines.length-1;i>=0;i--){
        if(lines[i]===lastSeenLine){startFrom=i+1;break;}
      }
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
function fetchLogs(){
  if(curTab!=='logs')return;
  fetch('/api/logs',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){
    _appendLogs(t);
    _renderLogBuf();
    set('log-st','Live \u2022 '+clientLogBuf.length+' lines buffered');
  }).catch(function(){set('log-st','Disconnected');});
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
    navigator.clipboard.writeText(txt).then(function(){set('log-st','Copied! \u2022 '+clientLogBuf.length+' lines');setTimeout(function(){set('log-st','Live \u2022 '+clientLogBuf.length+' lines buffered');},2000);});
  } else {
    var ta=document.createElement('textarea');ta.value=txt;ta.style.position='fixed';ta.style.opacity='0';
    document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);
    set('log-st','Copied! \u2022 '+clientLogBuf.length+' lines');setTimeout(function(){set('log-st','Live \u2022 '+clientLogBuf.length+' lines buffered');},2000);
  }
}
document.getElementById('log-box').addEventListener('scroll',function(){
  var b=this;logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
});
function startLogPoll(){if(!logPoll){fetchLogs();logPoll=setInterval(fetchLogs,4000);}}
function stopLogPoll(){if(logPoll){clearInterval(logPoll);logPoll=null;}}

function startMon(){
  monActive=true;
  fetch('/api/monitor-start',{cache:'no-store'});
  document.getElementById('btn-mst').style.display='none';
  document.getElementById('btn-msp').style.display='inline-flex';
  if(!monPoll)monPoll=setInterval(fetchMon,2000);
  fetchMon();
}
function stopMon(){
  if(monActive){
    monActive=false;
    fetch('/api/monitor-stop',{cache:'no-store'});
  }
  document.getElementById('btn-mst').style.display='inline-flex';
  document.getElementById('btn-msp').style.display='none';
  if(monPoll){clearInterval(monPoll);monPoll=null;}
}
function fetchMon(){
  if(curTab!=='mon')return;
  fetch('/api/sd-activity',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    set('m-p',d.last_pulse_count);
    set('m-i',(d.consecutive_idle_ms/1000).toFixed(1)+'s');
    set('m-l',(d.longest_idle_ms/1000).toFixed(1)+'s');
    set('m-r',d.total_active_samples+'/'+d.total_idle_samples);
    var dot=document.getElementById('mon-dot');
    dot.className='dot '+(d.is_busy?'busy':'idle');
    if(d.samples&&d.samples.length){
      var h='';
      d.samples.forEach(function(s){
        var w=Math.min(Math.max(s.p/10,1),100);
        var sec=s.t%3600,m=Math.floor(sec/60),ss=sec%60;
        var l=String(m).padStart(2,'0')+':'+String(ss).padStart(2,'0');
        h+='<div class=br2><span class=bl2>'+l+'</span><div class=bt><div class="bf '+(s.a?'a':'i')+'" style="width:'+w+'%"></div></div></div>';
      });
      document.getElementById('m-ch').innerHTML=h;
    }
    if(profActive){
      var li=d.longest_idle_ms/1000;
      if(li>profMaxIdle)profMaxIdle=li;
      document.getElementById('prof-max-idle').textContent=profMaxIdle.toFixed(1)+'s';
      if(profMaxIdle>0){
        document.getElementById('prof-rec-box').style.display='block';
        document.getElementById('prof-rec-val').textContent=Math.ceil(profMaxIdle+5)+'';
      }
    }
  }).catch(function(){});
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
  }).finally(function(){setTimeout(function(){b._busy=0;b.textContent='\u25b2 Trigger Upload';},700);});
}
function softReboot(){
  var b=document.getElementById('btn-srb');
  if(b._busy)return;b._busy=1;b.textContent='Rebooting...';
  fetch('/soft-reboot',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    toast(d.message||'Rebooting...',true);
  }).catch(function(){toast('Failed to reboot.',false);
  }).finally(function(){setTimeout(function(){b._busy=0;b.innerHTML='&#8635; Soft Reboot';},4000);});
}
function resetState(){
  if(!confirm('Reset all upload state? This cannot be undone.'))return;
  var b=document.getElementById('btn-rst');
  if(b._busy)return;b._busy=1;b.textContent='Resetting...';
  fetch('/reset-state',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    toast(d.message||'State reset.',true);
  }).catch(function(){toast('Failed to reset state.',false);
  }).finally(function(){setTimeout(function(){b._busy=0;b.textContent='Reset State';},1000);});
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

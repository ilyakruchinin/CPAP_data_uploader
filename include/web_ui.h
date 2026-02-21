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
<div class=prog><div id=d-ab-pb class=pf style=width:0></div></div>
<div id=d-ab-det class=bd-i>Waiting for next upload session.</div>
</div>
<div id=d-ib-wrap class=be style="display:none;opacity:0.6;margin-top:16px;border-top:1px solid #2a475e;padding-top:12px">
<div class=bh><span id=d-ib-name class=bt-c>—</span><span id=d-ib-st class=v>—</span></div>
<div class=prog><div id=d-ib-pb class="pf pfc" style=width:0></div></div>
<div id=d-ib-det class=bd-i>—</div>
</div>
</div>
<div class=card style="grid-column:1/-1"><h2>Actions</h2>
<div class=actions>
<button id=btn-up class="btn bp" onclick=triggerUpload()>&#9650; Trigger Upload</button>
<button id=btn-srb class="btn bs" onclick=softReboot()>&#8635; Soft Reboot</button>
<button id=btn-rst class="btn bd" onclick=resetState()>Reset State</button>
</div>
</div>
</div>
</div>

<!-- LOGS -->
<div id=logs class=page>
<div class=card style="margin-bottom:10px"><h2>System Logs</h2>
<div id=log-box><em>Loading...</em></div>
<div class=actions style="margin-top:10px">
<button class="btn bs" onclick="var b=document.getElementById('log-box');b.scrollTop=b.scrollHeight">&#8595; Scroll to Bottom</button>
</div>
</div>
</div>

<!-- CONFIG -->
<div id=cfg class=page>
<div class=wb id=cfg-lock-banner style=display:none><h3>Config Editing Paused FSM</h3><p>Uploads are suspended while you edit. Changes will trigger a reboot.</p></div>
<div class=card style="margin-bottom:10px">
<h2>Configuration <span id=cfg-lock-badge class="badge bc" style="display:none;float:right">EDITING</span></h2>
<div id=cfg-box><em>Loading...</em></div>
<textarea id=cfg-raw style="width:100%;height:300px;background:#111820;color:#6a7a8a;border:1px solid #2d3440;border-radius:6px;padding:12px;font-family:Consolas,monospace;font-size:12px;margin-top:10px;resize:vertical" readonly spellcheck=false onkeyup=cfgRawCount() onchange=cfgRawCount()></textarea>
<div class=row style="margin-top:5px;justify-content:flex-end"><span id=cfg-raw-cnt style="color:#8f98a0;font-size:11px"></span></div>
<div class=actions style="margin-top:10px;justify-content:space-between">
<div>
<button id=btn-cfg-edit class="btn bp" onclick=acquireCfgLock()>Edit config.txt</button>
<button id=btn-cfg-save class="btn bp" style=display:none onclick="saveRawCfg(false)">Save</button>
<button id=btn-cfg-savereboot class="btn bd" style="display:none;margin-left:5px" onclick="saveRawCfg(true)">Save &amp; Reboot</button>
<button id=btn-cfg-cancel class="btn bs" style="display:none;margin-left:5px" onclick=releaseCfgLock()>Cancel</button>
<button id=btn-cfg-reload class="btn bs" style="display:none;margin-left:5px" onclick=loadRawCfg()>Reload from SD</button>
</div>
<span id=cfg-raw-msg style="color:#8f98a0;font-size:12px;align-self:center"></span>
</div>
</div>
</div>

<!-- MONITOR -->
<div id=mon class=page>
<div class=card style="margin-bottom:10px"><h2>CPAP Profiler Wizard <span id=mon-dot class="dot idle"></span></h2>
<p style="font-size:.85em;color:#c7d5e0;line-height:1.5;margin-bottom:10px">Empirically measure your CPAP machine's SD card writing behavior to tune upload settings.</p>
<div class=actions>
<button id=btn-mst class="btn bp" onclick=startMon()>Start Profiling</button>
<button id=btn-msp class="btn bd" onclick=stopMon() style=display:none>Stop</button>
<button class="btn bs" onclick="tab('dash')">&#8592; Dashboard</button>
</div>
</div>
<div class=stats-grid>
<div class=stat-box><span class=sl>Phase</span><span class=sv id=m-phase>--</span></div>
<div class=stat-box><span class=sl>Consecutive Idle</span><span class=sv id=m-i>--</span></div>
<div class=stat-box><span class=sl>Longest Idle</span><span class=sv id=m-l>--</span></div>
<div class=stat-box><span class=sl>Total Active</span><span class=sv id=m-r>--</span></div>
</div>
<div class=card><h2>Activity Timeline</h2>
<div class=chart id=m-ch><em>Waiting for data...</em></div>
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
</div>
</div>

</div>
<script>
var curTab='dash',poll,logPoll,monPoll,cfg=null;
var renderStatus={_first:true,_cfgLoaded:false};
function set(id,v){document.getElementById(id).textContent=v;}
function tab(t){
  ['dash','logs','cfg','mon','ota'].forEach(function(x){
    document.getElementById(x).classList.toggle('on',x===t);
    document.getElementById('t-'+x).classList.toggle('act',x===t);
  });
  curTab=t;
  if(t==='logs'){startLogPoll();}else{stopLogPoll();}
  if(t==='mon'){startMon();}else{stopMon();}
  if(t==='cfg'){loadCfg();}
}
function toast(msg,mode){
  var el=document.getElementById('toast');
  if(!el){el=document.createElement('div');el.id='toast';document.body.appendChild(el);}
  var cls=mode===true||mode==='ok'?'ok':mode==='warn'?'warn':'er';
  el.textContent=msg;el.className='toast on '+cls;
  setTimeout(function(){el.className='toast';},8000);
}
function fmt(ms){var s=Math.round(ms/1000);if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
function fmtUp(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';var h=Math.floor(s/3600);return h+'h '+Math.floor(s%3600/60)+'m';}
function sigClass(r){if(r>=-65)return 'sig-exc';if(r>=-75)return 'sig-good';if(r>=-85)return 'sig-fair';if(r>=-95)return 'sig-weak';return 'sig-vweak';}
function sigLabel(r){if(r>=-65)return 'Excellent';if(r>=-75)return 'Good';if(r>=-85)return 'Fair';if(r>=-95)return 'Weak';return 'Very Weak';}
function badgeHtml(st){var s=st.toLowerCase(),c='bi';
  if(s==='idle')c='bi';else if(s==='listening')c='bl';
  else if(s==='acquiring'||s==='releasing')c='ba';else if(s==='uploading')c='bu';
  else if(s==='cooldown')c='bc';else if(s==='complete')c='bco';
  else if(s==='monitoring')c='bc';
  return '<span class="badge '+c+'">'+st+'</span>';}
function buildMode(m,w){if(m==='smart')return 'Smart (Inactivity trigger)';return 'Scheduled ('+w+')';}
function upText(a,b){
  if(a==='active')return 'Syncing '+b.done+'/'+b.tot+' folders ('+b.pend+' pending)...';
  if(a==='done')return 'Completed. Synced '+b.done+'/'+b.tot+' folders.';
  if(a==='waiting')return 'Waiting to sync ('+b.tot+' total folders).';
  return '—';
}
function upHtml(st){
  if(!st.val)return{n:'—',st:'—',p:0,t:'Not configured.'};
  var n=st.name,p=0,a='waiting',txt='';
  if(st.tot>0){
    if(st.tot===st.done)p=100;
    else p=Math.round((st.done/st.tot)*100);
  }
  if(st.upA){a='active';}else if(st.tot>0&&st.tot===st.done){a='done';}
  
  if(st.upA){
    txt='Syncing '+st.done+'/'+st.tot+' folders';
    if(st.livTot>0)txt+=' | File '+st.livUp+'/'+st.livTot+' in '+st.livF;
  }else if(st.tot>0&&st.tot===st.done){
    txt='Completed. Synced '+st.done+'/'+st.tot+' folders.';
  }else if(st.tot>0){
    txt='Waiting to sync ('+st.done+'/'+st.tot+' complete).';
  }else{
    txt='No folders scanned yet.';
  }
  
  return {n:n,st:(a==='active'?'SYNCING':(a==='done'?'COMPLETE':'WAITING')),p:p,t:txt};
}

function fetchStatus(){
  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    if(renderStatus._first){document.title=d.hostname+' - CPAP Uploader';set('sub','Connected | '+d.hostname+'.local');renderStatus._first=false;}
    set('d-time',d.time);set('d-fh',(d.fh/1024).toFixed(1)+' KB');set('d-ma',(d.ma/1024).toFixed(1)+' KB');
    set('d-up',fmtUp(d.up));
    var w=document.getElementById('d-wifi');w.textContent=d.wifi?'Connected ('+d.rssi+' dBm)':'Disconnected';w.className='v '+(d.wifi?sigClass(d.rssi):'sig-vweak');w.title=d.wifi?sigLabel(d.rssi):'';
    set('d-ip',d.ip||'—');
    document.getElementById('d-st').innerHTML=badgeHtml(d.fsm);
    set('d-ins',fmt(d.ins));
    
    if(renderStatus._cfgLoaded&&cfg){
      set('d-mode',buildMode(cfg.upload_mode,cfg.upload_start_hour+':00 - '+cfg.upload_end_hour+':00'));
      var eps=[];if(cfg.endpoint_type.indexOf('SMB')>=0)eps.push('SMB');if(cfg.cloud_configured)eps.push('Cloud');
      set('d-ep',eps.length?eps.join(' + '):'None');
    }
    set('d-tsync',d.tsync?'Yes':'No');set('d-win',d.inw?'Open':'Closed');
    set('d-next',d.nu>0?fmt(d.nu)+' from now':'—');

    var a=upHtml(d.ab);
    set('d-ab-name',a.n);set('d-ab-st',a.st);
    document.getElementById('d-ab-pb').style.width=a.p+'%';
    set('d-ab-det',a.t);
    
    var iw=document.getElementById('d-ib-wrap');
    if(d.ib&&d.ib.val){
      iw.style.display='';
      var i=upHtml(d.ib);
      set('d-ib-name',i.n);set('d-ib-st',i.st);
      document.getElementById('d-ib-pb').style.width=i.p+'%';
      set('d-ib-det',i.t);
    }else{iw.style.display='none';}

  }).catch(function(){if(curTab==='dash')set('sub','Disconnected');});
}
function startStatusPoll(){if(!poll)poll=setInterval(fetchStatus,3000);fetchStatus();}

function startLogPoll(){
  var b=document.getElementById('log-box');b.innerHTML='<em>Loading...</em>';
  if(!logPoll)logPoll=setInterval(fetchLogs,2000);
  fetchLogs();
}
function stopLogPoll(){if(logPoll){clearInterval(logPoll);logPoll=null;}}
function fetchLogs(){
  if(curTab!=='logs')return;
  fetch('/api/logs',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){
    var b=document.getElementById('log-box');
    var a=b.scrollHeight-b.scrollTop<=b.clientHeight+30;
    b.textContent=t||'No logs available.';
    if(a)b.scrollTop=b.scrollHeight;
  }).catch(function(){});
}

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
  document.getElementById('btn-cfg-save').style.display=locked?'':'none';
  document.getElementById('btn-cfg-savereboot').style.display=locked?'':'none';
  document.getElementById('btn-cfg-cancel').style.display=locked?'':'none';
}
function acquireCfgLock(){
  var msg=document.getElementById('cfg-raw-msg');
  msg.style.color='#8f98a0';msg.textContent='Pausing uploads...';
  fetch('/api/config-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"lock":true}',cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      _setCfgLockUI(true);
      loadRawCfg();
      msg.textContent='';
    }else{msg.style.color='#ff6060';msg.textContent='Cannot lock: '+(d.error||'?');}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Lock failed: '+e.message;});
}
function releaseCfgLock(){
  fetch('/api/config-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"lock":false}',cache:'no-store'});
  _setCfgLockUI(false);
  document.getElementById('cfg-raw-msg').textContent='';
}
function loadCfg(){
  fetch('/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    cfg=d;
    document.getElementById('cfg-box').textContent=JSON.stringify(d,null,2);
    renderStatus._cfgLoaded=true;
  }).catch(function(){document.getElementById('cfg-box').textContent='Failed to load config.';});
  if(!cfgLocked)_setCfgLockUI(false);
}
function cfgRawCount(){
  var t=document.getElementById('cfg-raw');
  document.getElementById('cfg-raw-cnt').textContent=t.value.length+' / 4096 bytes';
}
function loadRawCfg(){
  var msg=document.getElementById('cfg-raw-msg');
  msg.textContent='Loading...';
  fetch('/api/config-raw',{cache:'no-store'}).then(function(r){
    if(!r.ok)throw new Error('HTTP '+r.status);
    return r.text();
  }).then(function(t){
    var ta=document.getElementById('cfg-raw');
    ta.value=t;cfgRawCount();msg.textContent='';
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Load failed: '+e.message;});
}
function saveRawCfg(reboot){
  var ta=document.getElementById('cfg-raw');
  if(ta.value.length>4096){alert('Config too large (max 4096 bytes)');return;}
  var b1=document.getElementById('btn-cfg-save');var b2=document.getElementById('btn-cfg-savereboot');
  var ob1=b1.textContent,ob2=b2.textContent;
  b1.textContent='Saving...';b2.textContent='Saving...';b1.disabled=true;b2.disabled=true;
  var msg=document.getElementById('cfg-raw-msg');msg.style.color='#8f98a0';msg.textContent='Writing to SD...';
  
  fetch('/api/config-raw',{method:'POST',headers:{'Content-Type':'text/plain'},body:ta.value,cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      msg.style.color='#44ff44';msg.textContent='Saved!';
      if (d.requires_reinsert) {
          toast(d.message, 'er'); // Show the critical warning
      }
      setTimeout(function(){
        if(reboot)softReboot();
        else{releaseCfgLock();loadCfg();}
      },1000);
    }else{msg.style.color='#ff6060';msg.textContent='Error: '+(d.error||'?');}
  }).catch(function(e){msg.style.color='#ff6060';msg.textContent='Save failed: '+e.message;})
  .finally(function(){b1.textContent=ob1;b2.textContent=ob2;b1.disabled=false;b2.disabled=false;});
}

function startMon(){
  fetch('/api/monitor-start',{cache:'no-store'});
  document.getElementById('btn-mst').style.display='none';
  document.getElementById('btn-msp').style.display='inline-flex';
  if(!monPoll)monPoll=setInterval(fetchMon,2000);
  fetchMon();
}
function stopMon(){
  fetch('/api/monitor-stop',{cache:'no-store'});
  document.getElementById('btn-mst').style.display='inline-flex';
  document.getElementById('btn-msp').style.display='none';
  if(monPoll){clearInterval(monPoll);monPoll=null;}
}
function fetchMon(){
  if(curTab!=='mon')return;
  fetch('/api/sd-activity',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    set('m-phase',d.samples&&d.samples.length>0?'Profiling':'Waiting');
    set('m-i',(d.consecutive_idle_ms/1000).toFixed(1)+'s');
    set('m-l',(d.longest_idle_ms/1000).toFixed(1)+'s');
    set('m-r',d.total_active_samples+' samples');
    var dot=document.getElementById('mon-dot');
    dot.className='dot '+(d.is_busy?'busy':'idle');
    if(d.samples&&d.samples.length){
      var h='';
      d.samples.forEach(function(s){
        var w=Math.min(Math.max(s.pulseCount/10,1),100);
        var sec=s.timestamp%3600,m=Math.floor(sec/60),ss=sec%60;
        var l=String(m).padStart(2,'0')+':'+String(ss).padStart(2,'0');
        h+='<div class=br2><span class=bl2>'+l+'</span><div class=bt><div class="bf '+(s.active?'a':'i')+'" style="width:'+w+'%"></div></div></div>';
      });
      document.getElementById('m-ch').innerHTML=h;
    }
  }).catch(function(){});
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

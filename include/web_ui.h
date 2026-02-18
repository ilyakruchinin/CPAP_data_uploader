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
.toast.on{opacity:1;transform:translateY(0)}.toast.ok{border-color:#2f8f57}.toast.er{border-color:#c0392b}
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
<div class=bh><span class=bt-s>SMB</span><span id=d-smb-st class=v>—</span></div>
<div class=prog><div id=d-pf-smb class=pf style=width:0%></div></div>
<div id=d-smb-det class=bd-i></div>
</div>
<div class=be>
<div class=bh><span class=bt-c>Cloud</span><span id=d-cloud-st class=v>—</span></div>
<div class=prog><div id=d-pf-cloud class="pf pfc" style=width:0%></div></div>
<div id=d-cloud-det class=bd-i></div>
</div>
<div class=row style="border-top:1px solid #2a475e;padding-top:8px;margin-top:2px"><span class=k>Status</span><span id=d-fst class=v></span></div>
</div>
</div>
<div class=card style="margin-bottom:14px"><h2>Actions</h2>
<div class=actions>
<button id=btn-up class="btn bp" onclick=triggerUpload()>&#9650; Trigger Upload</button>
<button id=btn-srb class="btn bs" onclick=softReboot()>&#8635; Soft Reboot</button>
<button id=btn-rst class="btn bd" onclick=resetState()>Reset State</button>
</div>
</div>
</div>

<!-- LOGS -->
<div id=logs class=page>
<div class=card style="margin-bottom:10px"><h2>System Logs <span id=log-st style="font-size:.9em;color:#8f98a0;font-weight:400"></span></h2>
<div id=log-box>Loading...</div>
</div>
</div>

<!-- CONFIG -->
<div id=cfg class=page>
<div class=card><h2>Configuration</h2>
<pre id=cfg-box>Loading...</pre>
</div>
</div>

<!-- MONITOR -->
<div id=mon class=page>
<div class=card style="margin-bottom:10px"><h2>SD Activity Monitor <span id=mon-dot class="dot idle"></span></h2>
<p style="font-size:.85em;color:#c7d5e0;line-height:1.5;margin-bottom:10px">Monitors SD card bus activity. Use when CPAP machine is on. Red = CPAP writing, Green = safe to upload.</p>
<div class=actions>
<button id=btn-mst class="btn bp" onclick=startMon()>Start Monitoring</button>
<button id=btn-msp class="btn bd" onclick=stopMon() style=display:none>Stop</button>
<button class="btn bs" onclick="tab('dash')">&#8592; Dashboard</button>
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

<div id=toast class=toast></div>
</div>

<script>
var cfg={},monPoll=null,logPoll=null,curTab='dash';
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
function toast(msg,ok){
  var el=document.getElementById('toast');
  el.textContent=msg;el.className='toast on '+(ok?'ok':'er');
  setTimeout(function(){el.className='toast';},3000);
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
  seti('d-st',badgeHtml(d.state||'?'));
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
  var sc=d.smb_comp||0,si=d.smb_inc||0,st2=sc+si;
  var cc=d.cloud_comp||0,ci=d.cloud_inc||0,ct=cc+ci;
  document.getElementById('d-pf-smb').style.width=(st2>0?Math.round(sc*100/st2):0)+'%';
  document.getElementById('d-pf-cloud').style.width=(ct>0?Math.round(cc*100/ct):0)+'%';
  var sR=si>0?'<span style=color:#ffaa44>'+si+' left</span>':'<span style=color:#44ff44>&#10003; done</span>';
  var cR=ci>0?'<span style=color:#ffaa44>'+ci+' left</span>':'<span style=color:#44ff44>&#10003; done</span>';
  document.getElementById('d-smb-st').innerHTML=st2>0?sc+' / '+st2+' &nbsp;'+sR:'—';
  document.getElementById('d-cloud-st').innerHTML=ct>0?cc+' / '+ct+' &nbsp;'+cR:'—';
  var smbDet=d.smb_active?'Uploading '+d.smb_up+' / '+d.smb_total+' files'+(d.smb_folder?' &middot; '+d.smb_folder:''):'';
  var clDet=d.cloud_active?'Uploading '+d.cloud_up+' / '+d.cloud_total+' files'+(d.cloud_folder?' &middot; '+d.cloud_folder:''):'';
  document.getElementById('d-smb-det').innerHTML=smbDet;
  document.getElementById('d-cloud-det').innerHTML=clDet;
  var pend=(si||0)+(ci||0);
  var fst=pend>0?'&#9888; '+pend+' folder(s) pending':(sc+cc>0?'&#10003; All synced':'Waiting for first scan');
  seti('d-fst',fst);
  set('sub','Firmware '+d.firmware+' \u00b7 '+fmtUp(d.uptime||0)+' uptime');
}

var statusTimer=null;
function pollStatus(){
  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    renderStatus(d);
  }).catch(function(){set('d-st','Offline');});
}
function startStatusPoll(){if(!statusTimer){pollStatus();statusTimer=setInterval(pollStatus,3000);}}

function loadCfg(){
  fetch('/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    cfg=d;
    document.getElementById('cfg-box').textContent=JSON.stringify(d,null,2);
    renderStatus._cfgLoaded=true;
  }).catch(function(){document.getElementById('cfg-box').textContent='Failed to load config.';});
}

var logAtBottom=true;
function fetchLogs(){
  if(curTab!=='logs')return;
  fetch('/api/logs',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){
    var b=document.getElementById('log-box');
    logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
    b.textContent=t;
    if(logAtBottom)b.scrollTop=b.scrollHeight;
    set('log-st','Live');
  }).catch(function(){set('log-st','Disconnected');});
}
document.getElementById('log-box').addEventListener('scroll',function(){
  var b=this;logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
});
function startLogPoll(){if(!logPoll){fetchLogs();logPoll=setInterval(fetchLogs,4000);}}
function stopLogPoll(){if(logPoll){clearInterval(logPoll);logPoll=null;}}

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
  }).catch(function(){});
}

function triggerUpload(){
  var b=document.getElementById('btn-up');
  if(b._busy)return;b._busy=1;b.textContent='Triggering...';
  fetch('/trigger-upload',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    toast(d.message||'Upload triggered.',true);
  }).catch(function(){toast('Failed to trigger upload.',false);
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

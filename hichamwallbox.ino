/******************************************************
 * HichamWallbox.ino  —  PARTIE 1/2
 * ESP32 + RS-485 (MAX485) + ModbusMaster
 * Dashboard Web (PWA) + Provisioning Wi-Fi (AP+STA)
 * TFT 1.8" ST7735 (statut, barre 0→7 kW, IP/AP)
 ******************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>
#include <EEPROM.h>
#include <esp_system.h>  // ESP.restart()

// ===== AJOUT TFT =====
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
// Pins TFT (ST7735 128x160)
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  15
#define TFT_SCK  18
#define TFT_MOSI 23
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
unsigned long lastTftMs = 0;
// =====================

#define MAX485_DE 4
#define MAX485_RE 4
#define RX2 16
#define TX2 17

// ----- PLUS DE SSID/PASS EN DUR -----
/* rien en dur : provisioning via AP */

// --- Modbus / registres ---
ModbusMaster node;
uint16_t r104=0, r107=0, r108=0, r109=0, r126=1, r141=0, r151=0;
unsigned long lastRead=0;

// ---- Anti-retour sur consigne 109 ----
uint16_t        target109       = 0;
unsigned long   targetSetMs     = 0;
uint8_t         targetSyncTries = 0;
const unsigned long TARGET_HOLD_MS  = 10000; // 10 s
const uint8_t       TARGET_MAX_TRIES= 3;

// === EEPROM (persistance) ===
#define EEPROM_SIZE     512
#define EE_MAGIC_ADDR   0
#define EE_W_ADDR       2           // sauvegarde r109 (puissance)
#define EE_MAGIC        0xBEEFCAFE
#define EE_SSID_LEN     6
#define EE_PASS_LEN     7
#define EE_SSID_START   8           // SSID max ~32
#define EE_PASS_START   40          // PASS max ~64

WebServer server(80);

// ------------------ RS-485 ------------------
static const uint16_t RS485_SETTLE_US=300;
void preTX(){ digitalWrite(MAX485_DE,HIGH); digitalWrite(MAX485_RE,HIGH); delayMicroseconds(RS485_SETTLE_US); }
void postTX(){ delayMicroseconds(RS485_SETTLE_US); digitalWrite(MAX485_DE,LOW); digitalWrite(MAX485_RE,LOW); }

// ------------------ Modbus helpers ------------------
bool readU16(uint16_t addr,uint16_t&out){
  for(uint8_t i=0;i<3;i++){
    node.clearResponseBuffer();
    uint8_t r=node.readHoldingRegisters(addr,1);
    if(r==node.ku8MBSuccess){ out=node.getResponseBuffer(0); return true; }
    delay(15);
  }
  return false;
}
bool writeU16(uint16_t addr,uint16_t val){
  for(uint8_t i=0;i<3;i++){
    uint8_t r=node.writeSingleRegister(addr,val);
    if(r==node.ku8MBSuccess) return true;
    delay(15);
  }
  return false;
}

// Lock inversé confirmé : 0 = ON (autorise) ; 1 = OFF (coupe)
bool forceLockValue(uint16_t wanted){
  uint16_t addrs[2]={112,111};
  for(auto addr:addrs){
    if(writeU16(addr,wanted)){
      delay(100);
      uint16_t back=9999;
      if(readU16(addr,back)&&back==wanted) return true;
    }
  }
  return false;
}
bool lockON(){return forceLockValue(0);}   // 0 = ON (autorise la charge)
bool lockOFF(){return forceLockValue(1);}  // 1 = OFF (stoppe la charge)

// ================== HTML DASHBOARD ==================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hicham Wallbox</title>
<link rel="manifest" href="/manifest.json">
<meta name="theme-color" content="#009688">
<link rel="icon" href="/icon-192.png">
<script>
if("serviceWorker" in navigator){
  window.addEventListener("load",()=>navigator.serviceWorker.register("/sw.js").catch(()=>{}));
}
</script>
<style>
:root{--brand:#00796b;--brand2:#009688;--bg1:#e0f7fa;--bg2:#fff;--border:#e7e7e7}
*{box-sizing:border-box}
body{margin:0;font-family:Arial,Helvetica,sans-serif;background:linear-gradient(135deg,var(--bg1),var(--bg2))}
header{background:var(--brand);color:#fff;padding:14px 16px;display:flex;align-items:center;gap:12px;position:sticky;top:0;z-index:10}
header h2{margin:0;font-size:20px;flex:1}
.hamb{width:36px;height:36px;border-radius:8px;border:1px solid rgba(255,255,255,.3);display:flex;align-items:center;justify-content:center;cursor:pointer}
.hamb span,.hamb span::before,.hamb span::after{content:"";display:block;width:18px;height:2px;background:#fff;position:relative}
.hamb span::before{top:-6px;position:absolute}
.hamb span::after{top:6px;position:absolute}
.wrap{max-width:1000px;margin:20px auto;padding:0 16px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}
.card{background:#fff;border:1px solid var(--border);border-radius:14px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.06)}
.statcard{position:relative;display:flex;align-items:center;justify-content:space-between;gap:14px;padding:16px;border:1px solid var(--border);
border-radius:14px;background:#fff;box-shadow:0 10px 30px rgba(0,0,0,.06);overflow:hidden}
.statcard::before{content:"";position:absolute;left:0;top:0;bottom:0;width:10px;background:var(--stat-color,#636e72);transition:.3s}
.stat-emoji{font-size:34px}.stat-title{font-size:20px;font-weight:700}
.stopbtn{width:34px;height:34px;min-width:34px;min-height:34px;border-radius:8px;border:0;background:#d63031;color:#fff;
font-weight:900;display:none;align-items:center;justify-content:center;cursor:pointer}
.stopbtn:active{transform:scale(0.96)}
.gauge{width:260px;height:260px;margin:8px auto;position:relative}
.gauge svg{width:100%;height:100%;transform:rotate(-90deg)}
.track{stroke:#e9ecef;stroke-width:18;fill:none}
.bar{stroke:#009688;stroke-width:18;fill:none;stroke-linecap:round;transition:stroke-dashoffset .4s;
stroke-dasharray:722;stroke-dashoffset:722;}
.center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.val{font-size:30px;font-weight:700}
.sub{color:#667}
button{padding:9px 14px;border:0;border-radius:10px;background:#f7f7f7;color:#0a0;font-weight:700;cursor:pointer}
input{padding:8px 10px;border:1px solid var(--border);border-radius:10px;min-width:120px}
.row{display:flex;gap:8px;align-items:center;justify-content:center;margin-top:8px;flex-wrap:wrap}
.pill{background:#eef;border-radius:999px;padding:6px 10px;font-size:13px;display:inline-block;margin-top:6px;cursor:pointer}
.canvas-wrap{padding:6px;border:1px solid var(--border);border-radius:14px}
#chart{width:100%;height:260px}
.drawer{position:fixed;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:100}
.drawer .shade{position:absolute;inset:0;background:rgba(0,0,0,.25);opacity:0;transition:.2s}
.drawer .panel{position:absolute;top:0;left:-280px;width:250px;height:100%;background:#fff;
box-shadow:8px 0 25px rgba(0,0,0,.1);padding:14px;transition:.3s}
.drawer.open{pointer-events:auto}.drawer.open .shade{opacity:1}.drawer.open .panel{left:0}
.mbtn{display:block;width:100%;padding:10px 12px;margin-bottom:10px;background:#f7f7f7;border:1px solid var(--border);
border-radius:10px;cursor:pointer;text-align:left;color:#0a0}
.info{font-size:12px;color:#666;margin-top:6px;text-align:center}
</style>
<script>
let seriesKw=[],MAX_KW=7, MAX_POINTS=900; // 30 min @ 2s
function $(id){return document.getElementById(id);}
function toast(m,c){let t=$("msgErr");t.textContent=m;t.className=c||'';setTimeout(()=>t.textContent='',3000);}
function openDrawer(){$("drawer").classList.add('open');}
function closeDrawer(){$("drawer").classList.remove('open');}

function statusInfo(c){
 switch(c){
  case 1:return{t:'Chargeur prêt',c:'#00b894',i:'✅'};
  case 3:return{t:'Connecté',c:'#0984e3',i:'🔌'};
  case 5:return{t:'Chargement en cours',c:'#00cec9',i:'⚡'};
  case 6:return{t:'Attention chauffe',c:'#fd9644',i:'🌡️'};
  case 7:return{t:'Court-circuit',c:'#d63031',i:'🚫'};
  default:return{t:'Inconnu',c:'#636e72',i:'❔'};
 }}
function setStatusCard(c){
  let i=statusInfo(c);
  $('statCard').style.setProperty('--stat-color',i.c);
  $('sicon').textContent=i.i;
  $('slabel').textContent=i.t;
  $('stopInline').style.display = (c===5 ? 'flex' : 'none');
}
function setPwrGauge(w){
 let c=Math.max(0,Math.min(MAX_KW*1000,w||0)),r=115,C=2*Math.PI*r,p=c/(MAX_KW*1000);
 let b=$('gbar');b.setAttribute('stroke-dasharray',C);b.setAttribute('stroke-dashoffset',(1-p)*C);
 $('gVal').textContent=(c/1000).toFixed(1)+' kW';$('gSub').textContent=Math.round(c)+' W';
}
function refresh(){
 fetch('/data').then(r=>r.json()).then(j=>{
  setStatusCard(j.status_code);setPwrGauge(j.puissance_max_W);
  let kw=(j.puissance_max_W||0)/1000;
  seriesKw.push(kw); if(seriesKw.length>MAX_POINTS) seriesKw.shift();
  drawChart();
 });}
setInterval(refresh,2000);
window.onload=function(){setPwrGauge(0);refresh();}

// Graphe 0..7 kW / 30 min
function drawChart(){
 const c=$('chart'),ctx=c.getContext('2d'),dpr=Math.max(1,window.devicePixelRatio||1);
 const W=c.clientWidth,H=c.clientHeight;c.width=W*dpr;c.height=H*dpr;ctx.scale(dpr,dpr);
 const p=40;ctx.clearRect(0,0,W,H);ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);

 // Axes
 ctx.strokeStyle='#c9c9c9';ctx.lineWidth=1;
 ctx.beginPath();ctx.moveTo(p,p);ctx.lineTo(p,H-p);ctx.lineTo(W-p,H-p);ctx.stroke();

 // Y ticks (kW 0..7)
 ctx.fillStyle='#444';ctx.font='12px Arial';
 for(let i=0;i<=MAX_KW;i++){
   const y=H-p-(i/MAX_KW)*(H-2*p);
   ctx.fillText(i+" kW",8,y+4);
   ctx.strokeStyle='#e9e9e9';ctx.beginPath();ctx.moveTo(p,y);ctx.lineTo(W-p,y);ctx.stroke();
 }

 // X ticks (30 min, marque toutes 5 min)
 const pxPerPoint=(W-2*p)/Math.max(1,seriesKw.length-1);
 const stepPts=Math.round((5*60)/2); // 5min / 2s
 ctx.fillStyle='#666';
 for(let idx=0; idx<seriesKw.length; idx+=stepPts){
   const x=p+idx*pxPerPoint;
   const min= Math.round((seriesKw.length-idx)*2/60);
   ctx.fillText("-"+min+" min", x-14, H-p+14);
   ctx.strokeStyle='#f1f1f1';ctx.beginPath();ctx.moveTo(x,p);ctx.lineTo(x,H-p);ctx.stroke();
 }

 if(seriesKw.length<2) return;

 // Courbe
 ctx.strokeStyle='#009688';ctx.lineWidth=2;ctx.beginPath();
 const ys=(H-2*p)/MAX_KW;
 for(let i=0;i<seriesKw.length;i++){
   const x=p+i*pxPerPoint;
   const y=H-p-(seriesKw[i]*ys);
   if(i==0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
 }
 ctx.stroke();
}

function wifiReset(){fetch('/wifiReset');closeDrawer();}
function stopCharge(){fetch('/stopCharge',{method:'POST'}).then(()=>toast('⛔ Charge stoppée','ok'));}
function forceLock(){fetch('/forceLock',{method:'POST'}).then(()=>toast('🔒 Lock ON','ok'));}

// ---- Contrôle puissance (reg 109) ----
function applyPower(){
  const kw=parseFloat(($('kw')?.value)||'0');
  if(isNaN(kw) || kw<0 || kw>7){toast('Plage 0–7 kW','');return;}
  const w=Math.round(kw*1000);
  fetch('/setPmax?w='+w).then(r=>r.json()).then(j=>{
    if(j.ok){toast('✅ '+(j.w/1000)+' kW','');} else {toast('❌ '+(j.err||'Erreur'),'');}
  }).catch(()=>toast('Erreur réseau',''));
}
</script>
</head>
<body>
<header><div class="hamb" onclick="openDrawer()"><span></span></div><h2>Hicham Wallbox</h2></header>
<div id="drawer" class="drawer"><div class="shade" onclick="closeDrawer()"></div>
 <div class="panel">
   <button class="mbtn" onclick="wifiReset()">🔄 Réinitialiser le Wi-Fi</button>
   <button class="mbtn" onclick="stopCharge()">⛔ Stopper la charge</button>
   <button class="mbtn" onclick="forceLock()">🔒 Forcer Lock ON</button>
 </div></div>

<div class="wrap">
 <div id="msgErr"></div>
 <div class="cards">
   <!-- Statut + bouton STOP inline -->
   <div class="card statcard" id="statCard">
     <div style="display:flex;align-items:center;gap:14px;flex:1">
       <div class="stat-emoji" id="sicon">—</div>
       <div class="stat-title" id="slabel">—</div>
     </div>
     <button id="stopInline" class="stopbtn" title="Stop" onclick="stopCharge()">■</button>
   </div>

   <!-- Puissance max + sélecteurs -->
   <div class="card">
     <h3>Puissance max véhicule</h3>
     <div class="gauge">
       <svg viewBox="0 0 260 260"><circle class="track" cx="130" cy="130" r="115"></circle>
       <circle id="gbar" class="bar" cx="130" cy="130" r="115"></circle></svg>
       <div class="center"><div id="gVal" class="val">-- kW</div><div id="gSub" class="sub">-- W</div></div>
     </div>
     <div class="row">
       <input id="kw" type="number" step="0.1" min="0" max="7" placeholder="ex : 3.7">
       <button onclick="applyPower()">Appliquer</button>
     </div>
     <div class="row">
       <span class="pill" onclick="$('kw').value='3.7';applyPower()">3.7 kW</span>
       <span class="pill" onclick="$('kw').value='5.0';applyPower()">5.0 kW</span>
       <span class="pill" onclick="$('kw').value='7.0';applyPower()">7.0 kW</span>
     </div>
     <div class="info">Réglage écrit dans le registre 109 (et conservé en EEPROM).</div>
   </div>

   <!-- Graphe 30 minutes (kW) -->
   <div class="card">
     <h3>Puissance – 30 min</h3>
     <div class="canvas-wrap"><canvas id="chart"></canvas></div>
   </div>
 </div>
</div>
</body></html>
)HTML";

// ================== PAGE Wi-Fi (scan + saisie clé) ==================
const char WIFI_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Configurer le Wi-Fi</title>
<link rel="manifest" href="/manifest.json">
<meta name="theme-color" content="#009688">
<link rel="icon" href="/icon-192.png">
<script>
if("serviceWorker" in navigator){
  window.addEventListener("load",()=>navigator.serviceWorker.register("/sw.js").catch(()=>{}));
}
</script>
<style>
body{font-family:Arial,Helvetica,sans-serif;background:#f4f7fb;margin:0}
.wrap{max-width:420px;margin:40px auto;background:#fff;border:1px solid #e7e7e7;border-radius:14px;padding:18px;box-shadow:0 10px 30px rgba(0,0,0,.06)}
h2{margin:0 0 12px}button,input,select{width:100%;padding:10px;border-radius:10px;border:1px solid #ddd;margin-top:8px}
.btn{background:#009688;color:#fff;border:0;cursor:pointer}
.small{font-size:12px;color:#666;margin-top:8px}
</style>
<script>
async function scan(){
  const r=await fetch('/wifiScan'); const j=await r.json();
  const sel=document.getElementById('ssid'); sel.innerHTML='';
  j.list.forEach(s=>{const o=document.createElement('option');o.value=s; o.textContent=s; sel.appendChild(o);});
  if(!j.list.length){const o=document.createElement('option');o.textContent='Aucun réseau détecté'; sel.appendChild(o);}
}
async function save(){
  const ssid=document.getElementById('ssid').value;
  const pass=document.getElementById('pass').value;
  if(!ssid){alert('Choisis un SSID');return;}
  try{
    const r=await fetch('/wifiSave',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)
    });
    const j=await r.json();
    if(j.ok && j.ip){
      // Redirection automatique vers l'IP LAN
      location.href = 'http://' + j.ip + '/';
    }else{
      alert(j.msg || 'Erreur');
    }
  }catch(e){
    alert('Erreur réseau: '+e);
  }
}
window.onload=scan;
</script>
</head>
<body>
<div class="wrap">
  <h2>Connexion Wi-Fi</h2>
  <label>SSID</label>
  <select id="ssid"></select>
  <button class="btn" onclick="scan()">🔄 Re-scanner</button>
  <label>Mot de passe</label>
  <input id="pass" type="password" placeholder="Clé Wi-Fi">
  <button class="btn" onclick="save()">✅ Enregistrer & se connecter</button>
  <div class="small">L’ESP reste en AP pour rediriger la page, puis bascule en STA.</div>
</div>
</body></html>
)HTML";

// ==== PWA: manifest.json, sw.js, icônes (PNG 1×1 “placeholder”) ====
const char MANIFEST_JSON[] PROGMEM = R"JSON(
{
  "name": "Hicham Wallbox",
  "short_name": "Wallbox",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "background_color": "#ffffff",
  "theme_color": "#009688",
  "icons": [
    { "src": "/icon-192.png", "sizes": "192x192", "type": "image/png" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png" }
  ]
}
)JSON";

const char SW_JS[] PROGMEM = R"JS(
const CACHE="wallbox-v1";
const CORE=["/","/manifest.json","/sw.js","/icon-192.png","/icon-512.png"];
self.addEventListener("install",e=>{
  e.waitUntil(caches.open(CACHE).then(c=>c.addAll(CORE)));
});
self.addEventListener("activate",e=>{
  e.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k!==CACHE).map(k=>caches.delete(k)))));
});
self.addEventListener("fetch",e=>{
  const url=new URL(e.request.url);
  if(url.pathname.startsWith("/data")||url.pathname.startsWith("/setPmax")||url.pathname.startsWith("/stopCharge")||url.pathname.startsWith("/forceLock")||url.pathname.startsWith("/wifi")){
    return;
  }
  e.respondWith(
    caches.match(e.request).then(r=>r||fetch(e.request).then(resp=>{
      if(resp.ok && e.request.method==="GET"){
        const clone=resp.clone();
        caches.open(CACHE).then(c=>c.put(e.request,clone));
      }
      return resp;
    }).catch(()=>r))
  );
});
)JS";

// PNG 1x1 (blanc) – valide, ultra léger
const uint8_t ICON_192_PNG[] PROGMEM = {
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
  0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0x00,0x01,0x00,0x00,
  0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
  0x42,0x60,0x82
}; // 1x1 blanc

const uint8_t ICON_512_PNG[] PROGMEM = {
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
  0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0x00,0x01,0x00,0x00,
  0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
  0x42,0x60,0x82
};

// ================== Wi-Fi creds en EEPROM & modes ==================
bool wifiIsAP=false;

void eepromClearCreds(){
  EEPROM.writeUInt(EE_MAGIC_ADDR, 0);
  EEPROM.writeUChar(EE_SSID_LEN, 0);
  EEPROM.writeUChar(EE_PASS_LEN, 0);
  for(int i=EE_SSID_START;i<EE_PASS_START+96;i++) EEPROM.writeUChar(i,0);
  EEPROM.commit();
}
bool eepromLoadCreds(String &ssid, String &pass){
  uint32_t m=EEPROM.readUInt(EE_MAGIC_ADDR);
  if(m!=EE_MAGIC) return false;
  uint8_t sl=EEPROM.readUChar(EE_SSID_LEN);
  uint8_t pl=EEPROM.readUChar(EE_PASS_LEN);
  if(sl==0 || sl>32 || pl>64) return false;
  char sbuf[33]={0}, pbuf[65]={0};
  for(uint8_t i=0;i<sl;i++) sbuf[i]=EEPROM.readUChar(EE_SSID_START+i);
  for(uint8_t i=0;i<pl;i++) pbuf[i]=EEPROM.readUChar(EE_PASS_START+i);
  ssid = String(sbuf);
  pass = String(pbuf);
  return true;
}
void eepromSaveCreds(const String &ssid, const String &pass){
  EEPROM.writeUInt(EE_MAGIC_ADDR, EE_MAGIC);
  EEPROM.writeUChar(EE_SSID_LEN, (uint8_t)ssid.length());
  EEPROM.writeUChar(EE_PASS_LEN, (uint8_t)pass.length());
  for(uint8_t i=0;i<ssid.length();i++) EEPROM.writeUChar(EE_SSID_START+i, ssid[i]);
  for(uint8_t i=0;i<pass.length();i++) EEPROM.writeUChar(EE_PASS_START+i, pass[i]);
  EEPROM.commit();
}

String makeApSsid(){
  uint64_t mac=ESP.getEfuseMac();
  uint16_t last=(uint16_t)(mac&0xFFFF);
  char suf[5]; sprintf(suf,"%04X", last);
  String s="HichamWallbox-"; s+=suf; return s;
}
void startAP(){
  // AP ou AP+STA dès le début (on choisit AP+STA pour simplifier)
  WiFi.mode(WIFI_AP_STA);
  String apSsid=makeApSsid();
  const char* apPass="12345678";
  bool ok=WiFi.softAP(apSsid.c_str(), apPass);
  wifiIsAP=ok;
  Serial.println(ok? String("AP: ")+apSsid+" pass:"+apPass : "AP échec");
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}
bool trySTA(const String &ssid,const String &pass, uint32_t timeoutMs=15000){
  // On garde l’AP actif pendant la tentative STA (AP+STA)
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("WiFi STA => "); Serial.print(ssid); Serial.print(" ...");
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && (millis()-t0)<timeoutMs){
    delay(500); Serial.print(".");
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.println("\nConnecté, IP: "+WiFi.localIP().toString());
    wifiIsAP=false; // n’affichera plus la page Wi-Fi
    return true;
  }
  Serial.println("\nEchec STA");
  return false;
}
void startWiFi(){
  String s,p;
  if(eepromLoadCreds(s,p)){
    if(trySTA(s,p)) return;
  }
  startAP();
}

// ================== ROUTES ==================
void handleRoot(){
  // Si AP actif ou pas de STA connectée => page Wi-Fi
  if(wifiIsAP || WiFi.status()!=WL_CONNECTED){
    server.send_P(200,"text/html; charset=utf-8",WIFI_HTML);
  }else{
    server.send_P(200,"text/html; charset=utf-8",INDEX_HTML);
  }
}
void handleWifiScan(){
  int n=WiFi.scanNetworks();
  String j="{\"list\":[";
  for(int i=0;i<n;i++){
    if(i) j+=",";
    j+="\""+WiFi.SSID(i)+"\"";
  }
  j+="]}";
  server.send(200,"application/json; charset=utf-8",j);
}
void handleWifiSave(){
  if(!server.hasArg("ssid")){ server.send(400,"application/json","{\"ok\":false,\"msg\":\"ssid manquant\"}"); return; }
  String s=server.arg("ssid");
  String p=server.hasArg("pass")?server.arg("pass"):"";
  if(s.length()==0 || s.length()>32 || p.length()>64){
    server.send(400,"application/json","{\"ok\":false,\"msg\":\"tailles invalides\"}"); return;
  }
  eepromSaveCreds(s,p);
  // Connexion en gardant l’AP vivant (AP+STA), et renvoyer l’IP
  if(trySTA(s,p)){
    String ip = WiFi.localIP().toString();
    String j = String("{\"ok\":true,\"msg\":\"Connecté.\",\"ip\":\"")+ip+"\"}";
    server.send(200,"application/json", j);
    // Optionnel: couper l’AP après quelques secondes
    // delay(3000); WiFi.softAPdisconnect(true);
  }else{
    startAP();
    server.send(200,"application/json","{\"ok\":false,\"msg\":\"Connexion échouée. AP actif, réessayez.\"}");
  }
}
void sendData(){
  float tension_V=r107/10.0f;int phases=(r126==3?3:1);
  uint16_t ampA=(r151==1667)?10:(r151==2667)?16:(r151==3333)?20:(r151==4167)?25:(r151==5333)?32:0;
  float pwr_est=(tension_V>0&&ampA>0)?tension_V*ampA*phases:0;
  float pwr_W=(r109>0)?r109:((r108>0)?r108:pwr_est);
  String j="{\"status_code\":"+String(r141)+",\"puissance_max_W\":"+String(pwr_W,0)+"}";
  server.send(200,"application/json; charset=utf-8",j);
}
void handleStopCharge(){target109=0;bool ok=lockOFF();
  server.send(200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");}
void handleForceLock(){bool ok=lockON();
  server.send(200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");}
void handleSetPmax(){
  if(!server.hasArg("w")){ server.send(400,"application/json","{\"ok\":false,\"err\":\"w manquant\"}"); return; }
  long wL = server.arg("w").toInt(); if(wL<0) wL=0; if(wL>7000) wL=7000;
  uint16_t w=(uint16_t)wL;
  bool ok=false;
  for(uint8_t i=0;i<3;i++){
    if(writeU16(109,w)){
      delay(120);
      uint16_t back=0;
      if(readU16(109,back) && back==w){ ok=true; break; }
    }
    delay(15);
  }
  if(!ok){ server.send(500,"application/json","{\"ok\":false,\"err\":\"échec écriture/validation 109\"}"); return; }
  r109=w;
  EEPROM.writeUInt(EE_MAGIC_ADDR,EE_MAGIC);
  EEPROM.writeUShort(EE_W_ADDR,w);
  EEPROM.commit();
  target109=w; targetSetMs=millis(); targetSyncTries=0;
  String okj=String("{\"ok\":true,\"w\":")+String((int)w)+"}";
  server.send(200,"application/json",okj);
}
// /wifiReset : efface creds + bascule AP (sans reboot)
void handleWifiReset(){
  server.send(200,"application/json","{\"ok\":true}");
  delay(100);
  WiFi.disconnect(true, true);
  eepromClearCreds();
  delay(200);
  startAP();
}

// ==== ROUTES PWA (manifest, SW, icônes) ====
void handleManifest(){
  server.send_P(200,"application/manifest+json",MANIFEST_JSON);
}
void handleSW(){
  server.sendHeader("Cache-Control","no-cache");
  server.send_P(200,"application/javascript",SW_JS);
}
void handleIcon192(){
  server.send_P(200,"image/png",(const char*)ICON_192_PNG,sizeof(ICON_192_PNG));
}
void handleIcon512(){
  server.send_P(200,"image/png",(const char*)ICON_512_PNG,sizeof(ICON_512_PNG));
}

// ===== TFT : helpers d’affichage =====
const char* statusLabel(uint16_t s){
  switch(s){
    case 1: return "Pret";
    case 3: return "Connecte";
    case 5: return "Charge";
    case 6: return "Chauffe!";
    case 7: return "Court-circ!";
    default: return "Inconnu";
  }
}
void drawTftSimple(){
  float tension_V = r107 / 10.0f;
  int   phases    = (r126==3?3:1);
  uint16_t ampA   = (r151==1667)?10:(r151==2667)?16:(r151==3333)?20:(r151==4167)?25:(r151==5333)?32:0;
  float pwr_est   = (tension_V>0 && ampA>0) ? tension_V*ampA*phases : 0;
  float pwr_W     = (r109>0) ? (float)r109 : ((r108>0) ? (float)r108 : pwr_est);
  float pwr_kW    = pwr_W / 1000.0f;
  if(pwr_kW > 7.0f) pwr_kW = 7.0f;
  if(pwr_kW < 0.0f) pwr_kW = 0.0f;

  tft.fillScreen(ST77XX_BLACK);

  // Statut (couleur)
  tft.setTextWrap(false);
  tft.setCursor(4, 6);
  uint16_t color = ST77XX_WHITE;
  switch(r141){
    case 1: color=ST77XX_GREEN; break;
    case 3: color=ST77XX_CYAN; break;
    case 5: color=ST77XX_GREEN; break;
    case 6: color=ST77XX_YELLOW; break;
    case 7: color=ST77XX_RED; break;
    default: color=ST77XX_WHITE; break;
  }
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.print(statusLabel(r141));

  // Barre 0..7 kW
  int barX=10, barY=50, barW=108, barH=14;
  tft.drawRect(barX, barY, barW, barH, ST77XX_WHITE);
  int fill = (int)((barW - 2) * (pwr_kW / 7.0f));
  if(fill>0) tft.fillRect(barX+1, barY+1, fill, barH-2, ST77XX_GREEN);

  // Valeur numérique
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 70);
  tft.print(pwr_kW,1);
  tft.print(" kW");

  // IP/AP
  tft.setCursor(10, 100);
  if(WiFi.status()==WL_CONNECTED){
    tft.print("IP: ");
    tft.print(WiFi.localIP());
  }else{
    tft.print("AP: ");
    tft.print(WiFi.softAPIP());
  }
}

/***************  FIN PARTIE 1/2  ***************/
/******************************************************
 * HichamWallbox.ino  —  PARTIE 2/2
 * (suite : setup() + loop() + enregistrements routes)
 ******************************************************/

void setup(){
  Serial.begin(115200);

  // RS-485 direction pins
  pinMode(MAX485_DE,OUTPUT);
  pinMode(MAX485_RE,OUTPUT);
  digitalWrite(MAX485_DE,LOW);
  digitalWrite(MAX485_RE,LOW);

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // --- TFT init ---
  SPI.begin(TFT_SCK, -1, TFT_MOSI);        // SCK=18, MOSI=23
  tft.initR(INITR_BLACKTAB);               // si couleurs bizarres, essaie INITR_REDTAB
  tft.setRotation(1);                      // paysage
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 10);
  tft.print("Boot...");
  // -----------------

  // --- Wi-Fi provisioning (AP+STA) ---
  startWiFi();

  // --- Modbus (UART2 sur GPIO16/17) ---
  Serial2.begin(9600, SERIAL_8N1, RX2, TX2);
  Serial2.setTimeout(1000);
  node.begin(1, Serial2);
  node.preTransmission(preTX);
  node.postTransmission(postTX);

  // Autoriser la charge au boot (lock ON = 0)
  lockON();
  delay(100);

  // Restaurer 109 depuis EEPROM si present (independant du Wi-Fi)
  uint32_t m = EEPROM.readUInt(EE_MAGIC_ADDR);
  if(m == EE_MAGIC){
    uint16_t w = EEPROM.readUShort(EE_W_ADDR);
    if(w <= 7000){
      writeU16(109, w);
      delay(120);
    }
  }

  // --- Routes HTTP ---
  server.on("/",            handleRoot);
  server.on("/wifi",        handleRoot);          // alias
  server.on("/wifiScan",    handleWifiScan);
  server.on("/wifiSave",    HTTP_POST, handleWifiSave);

  server.on("/data",        sendData);
  server.on("/stopCharge",  HTTP_POST, handleStopCharge);
  server.on("/forceLock",   HTTP_POST, handleForceLock);
  server.on("/wifiReset",   handleWifiReset);
  server.on("/setPmax",     handleSetPmax);

  // PWA assets
  server.on("/manifest.json", handleManifest);
  server.on("/sw.js",         handleSW);
  server.on("/icon-192.png",  handleIcon192);
  server.on("/icon-512.png",  handleIcon512);

  server.begin();
  Serial.println("Serveur web demarre !");
}

void loop(){
  server.handleClient();

  // Lecture Modbus toutes les 2s
  if(millis() - lastRead > 2000){
    uint16_t v;
    readU16(104, v); r104 = v;
    readU16(107, v); r107 = v;
    readU16(108, v); r108 = v;
    readU16(109, v); r109 = v;
    readU16(126, v); r126 = v;
    readU16(141, v); r141 = v;
    readU16(151, v); r151 = v;

    // Anti-retour 109 : maintient pendant 10 s (max 3 coups) si le controleur modifie la consigne
    if(target109 > 0 && (millis() - targetSetMs) <= TARGET_HOLD_MS && targetSyncTries < TARGET_MAX_TRIES){
      if(r109 != target109){
        if(writeU16(109, target109)){
          delay(120);
          readU16(109, r109);
        }
        targetSyncTries++;
      }
    }

    lastRead = millis();
  }

  // MAJ TFT toutes les 2s
  if(millis() - lastTftMs > 2000){
    drawTftSimple();
    lastTftMs = millis();
  }
}

/***************  FIN PARTIE 2/2  ***************/

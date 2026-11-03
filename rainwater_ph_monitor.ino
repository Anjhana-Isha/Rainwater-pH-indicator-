/*
  Rainwater Harvesting pH Monitor v2  (ESP32 + analog pH module, e.g. PH-4502C / DFRobot SEN0161)
  ---------------------------------------------------------------------------------------------
  - Reads the pH probe, filters noise, converts to pH with 2-point calibration (pH 7 + pH 4)
  - Serves a phone app at http://<ESP32-IP>/  or  http://rainwater.local/
    -> In Chrome/Safari use "Add to Home screen" to install it like an app
    -> Or use the companion Android app (RainwaterPH-Android project)
  - App features: live pH, safe/unsafe alerts, 30-min chart, calibration,
    adjustable safe range, CSV download
  - Optional relay output: diverts water away from the tank when pH is out of range
  - Calibration and settings are saved in flash (survive power cuts)

  WIRING
    pH module V+  -> 5V (VIN)          pH module G -> GND
    pH module Po  -> 10k --+-- 20k -> GND     (voltage divider: protects the 3.3V ESP32 pin)
                           |
                         GPIO34
    Relay IN      -> GPIO26 (optional: diverter valve or first-flush solenoid)

  Board: "ESP32 Dev Module" in Arduino IDE (ESP32 core 2.x or 3.x). No extra libraries needed.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>

// ---------- USER SETTINGS ----------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME  = "rainwater";          // -> http://rainwater.local/

const int   PH_PIN     = 34;     // ADC1 pin (ADC2 pins stop working when Wi-Fi is on)
const int   RELAY_PIN  = 26;     // set to -1 if you don't use a relay
const bool  RELAY_ACTIVE_HIGH = true;

const float HYSTERESIS = 0.2;    // prevents relay chattering near the limits
const unsigned long READ_INTERVAL_MS    = 2000;   // live reading every 2 s
const unsigned long HISTORY_INTERVAL_MS = 10000;  // chart point every 10 s
const int HISTORY_LEN = 180;                      // 180 x 10 s = last 30 minutes
// -----------------------------------

WebServer server(80);
Preferences prefs;

float safeMin = 6.0;   // editable in the app
float safeMax = 8.5;
float calV7 = 2.50;    // pin voltage at pH 7 (default guess, calibrate!)
float calV4 = 3.03;    // pin voltage at pH 4
float lastVoltage = 0, lastPH = 7.0;
float phMin24 = NAN, phMax24 = NAN;
bool  diverting = false;
float history[HISTORY_LEN];
int   histCount = 0, histHead = 0;
unsigned long lastRead = 0, lastHist = 0;

// ---- Read voltage with median filtering (removes spikes from pumps/relays) ----
float readVoltage() {
  const int N = 40;
  int mv[N];
  for (int i = 0; i < N; i++) {
    mv[i] = analogReadMilliVolts(PH_PIN);
    delay(4);
  }
  std::sort(mv, mv + N);
  long sum = 0;
  for (int i = 10; i < 30; i++) sum += mv[i];   // average the middle 20 values
  return (sum / 20.0) / 1000.0;
}

// ---- Linear 2-point conversion: voltage -> pH ----
float voltageToPH(float v) {
  if (fabs(calV7 - calV4) < 0.01) return NAN;    // bad calibration
  float slope = (7.0 - 4.0) / (calV7 - calV4);
  float ph = 7.0 + slope * (v - calV7);
  if (ph < -1 || ph > 15) return NAN;            // probe unplugged / wiring fault
  return ph;
}

// level: 0 = unknown, 1 = safe, 2 = warning, 3 = danger
int statusLevel(float ph) {
  if (isnan(ph)) return 0;
  if (ph < 5.0 || ph > 9.5) return 3;
  if (ph < safeMin || ph > safeMax) return 2;
  return 1;
}

String statusText(float ph) {
  if (isnan(ph))     return "Check probe / calibrate";
  if (ph < 5.0)      return "Strongly acidic";
  if (ph < safeMin)  return "Acidic - diverting";
  if (ph <= safeMax) return "Safe to store";
  return "Alkaline - check tank";
}

void setRelay(bool on) {
  if (RELAY_PIN < 0) return;
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

void updateDiverter(float ph) {
  if (isnan(ph)) return;
  if (!diverting && (ph < safeMin || ph > safeMax)) diverting = true;
  else if (diverting && ph > safeMin + HYSTERESIS && ph < safeMax - HYSTERESIS) diverting = false;
  setRelay(diverting);
}

void takeReading() {
  lastVoltage = readVoltage();
  lastPH = voltageToPH(lastVoltage);
  updateDiverter(lastPH);
  if (!isnan(lastPH)) {
    if (isnan(phMin24) || lastPH < phMin24) phMin24 = lastPH;
    if (isnan(phMax24) || lastPH > phMax24) phMax24 = lastPH;
  }
  Serial.printf("V=%.3f  pH=%.2f  %s\n", lastVoltage, lastPH, statusText(lastPH).c_str());
}

void pushHistory() {
  history[histHead] = lastPH;
  histHead = (histHead + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

String fmt(float v, int d) { return isnan(v) ? String("null") : String(v, d); }

// ---------- App icon (SVG) ----------
const char ICON_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">
<rect width="512" height="512" rx="110" fill="#0f172a"/>
<path d="M256 80C256 80 120 240 120 320a136 136 0 0 0 272 0C392 240 256 80 256 80z" fill="#38bdf8"/>
<text x="256" y="370" font-family="Arial,sans-serif" font-size="120" font-weight="700" fill="#0f172a" text-anchor="middle">pH</text>
</svg>)rawliteral";

const char MANIFEST_JSON[] PROGMEM = R"rawliteral({
 "name":"Rainwater pH Monitor","short_name":"Rain pH","start_url":"/","display":"standalone",
 "background_color":"#0f172a","theme_color":"#0f172a",
 "icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]
})rawliteral";

// ---------- The app (HTML/CSS/JS served by the ESP32) ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f172a">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Rain pH">
<link rel="manifest" href="/manifest.json">
<link rel="icon" href="/icon.svg"><link rel="apple-touch-icon" href="/icon.svg">
<title>Rainwater pH</title>
<style>
 *{box-sizing:border-box}
 body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;
   padding:calc(12px + env(safe-area-inset-top)) 14px 90px}
 header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}
 header h1{font-size:20px;margin:0}
 #conn{font-size:12px;padding:4px 10px;border-radius:20px;background:#334155}
 #conn.on{background:#14532d;color:#86efac}#conn.off{background:#7f1d1d;color:#fecaca}
 .card{background:#1e293b;border-radius:18px;padding:16px;margin-bottom:12px}
 h2{font-size:15px;margin:0 0 10px;color:#93c5fd}
 #ph{font-size:80px;font-weight:800;text-align:center;line-height:1;margin-top:6px}
 #status{text-align:center;font-size:18px;margin-top:6px;font-weight:700}
 #alert{display:none;background:#7f1d1d;color:#fecaca;padding:12px;border-radius:12px;margin-bottom:12px;font-weight:600;text-align:center}
 .row{display:flex;justify-content:space-between;font-size:13px;color:#94a3b8;margin-top:8px}
 .stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:12px}
 .stat{background:#0f172a;border-radius:12px;padding:10px;text-align:center}
 .stat b{display:block;font-size:18px;color:#e2e8f0}.stat span{font-size:11px;color:#94a3b8}
 .bar{height:14px;border-radius:7px;margin-top:16px;position:relative;
   background:linear-gradient(90deg,#ef4444,#f97316,#eab308,#22c55e,#3b82f6,#6366f1,#a855f7)}
 #marker{position:absolute;top:-6px;width:6px;height:26px;background:#fff;border-radius:3px;
   box-shadow:0 0 6px #000;transition:left .6s;transform:translateX(-50%)}
 canvas{width:100%;height:180px;display:block}
 button{flex:1;padding:14px;border:0;border-radius:12px;font-size:15px;font-weight:600;background:#334155;color:#fff}
 button.primary{background:#2563eb}
 .btns{display:flex;gap:8px;margin-top:10px}
 small{color:#94a3b8;line-height:1.4}
 input{width:100%;padding:12px;border-radius:10px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:16px}
 label{font-size:13px;color:#94a3b8}
 .grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
 nav{position:fixed;left:0;right:0;bottom:0;display:flex;background:#1e293b;border-top:1px solid #334155;
   padding-bottom:env(safe-area-inset-bottom)}
 nav a{flex:1;text-align:center;padding:12px 0;color:#94a3b8;text-decoration:none;font-size:12px}
 nav a.act{color:#38bdf8}nav a i{display:block;font-style:normal;font-size:20px}
 .page{display:none}.page.act{display:block}
</style></head><body>
<header><h1>🌧️ Rainwater pH</h1><span id="conn">…</span></header>
<div id="alert"></div>

<div class="page act" id="p-home">
 <div class="card">
  <div id="ph">--</div><div id="status">Connecting…</div>
  <div class="bar"><div id="marker" style="left:50%"></div></div>
  <div class="row"><span>0 acidic</span><span>7 neutral</span><span>14 alkaline</span></div>
  <div class="stats">
   <div class="stat"><b id="min">--</b><span>Lowest</span></div>
   <div class="stat"><b id="max">--</b><span>Highest</span></div>
   <div class="stat"><b id="div">--</b><span>Diverter</span></div>
  </div>
 </div>
 <div class="card"><h2>Last 30 minutes</h2><canvas id="chart"></canvas>
  <div class="row"><span>Green band = safe range</span><a href="/csv" style="color:#38bdf8">Download CSV</a></div>
 </div>
</div>

<div class="page" id="p-cal">
 <div class="card"><h2>Calibrate probe</h2>
  <small>1. Rinse probe with clean water.<br>2. Dip in pH 7.00 buffer, wait ~1 minute until the voltage below stops changing, tap <b>Set pH 7</b>.<br>3. Rinse, dip in pH 4.00 buffer, wait, tap <b>Set pH 4</b>.<br>Recalibrate about once a month.</small>
  <div class="stats"><div class="stat"><b id="volt">--</b><span>Live volts</span></div>
   <div class="stat"><b id="c7">--</b><span>V at pH 7</span></div>
   <div class="stat"><b id="c4">--</b><span>V at pH 4</span></div></div>
  <div class="btns"><button class="primary" onclick="cal(7)">Set pH 7</button><button class="primary" onclick="cal(4)">Set pH 4</button></div>
 </div>
</div>

<div class="page" id="p-set">
 <div class="card"><h2>Safe range for storage</h2>
  <div class="grid2"><div><label>Minimum pH</label><input id="smin" type="number" step="0.1" inputmode="decimal"></div>
   <div><label>Maximum pH</label><input id="smax" type="number" step="0.1" inputmode="decimal"></div></div>
  <div class="btns"><button class="primary" onclick="saveSet()">Save</button></div>
  <small>Outside this range the diverter turns on and the app alerts you. Clean rain is about pH 5.6; 6.0–8.5 is a common target for stored water.</small>
 </div>
 <div class="card"><h2>Alerts</h2>
  <div class="btns"><button id="vibBtn" onclick="toggleVib()">Vibrate on alert: ON</button></div>
 </div>
 <div class="card"><h2>Device</h2><small id="dev">--</small></div>
</div>

<nav>
 <a href="#" class="act" data-p="home"><i>💧</i>Monitor</a>
 <a href="#" data-p="cal"><i>🧪</i>Calibrate</a>
 <a href="#" data-p="set"><i>⚙️</i>Settings</a>
</nav>

<script>
const $=id=>document.getElementById(id);
let vib=true,lastLevel=1,setLoaded=false;
try{vib=localStorage.getItem('vib')!=='0'}catch(e){}
$('vibBtn').textContent='Vibrate on alert: '+(vib?'ON':'OFF');
document.querySelectorAll('nav a').forEach(a=>a.onclick=e=>{e.preventDefault();
 document.querySelectorAll('nav a,.page').forEach(x=>x.classList.remove('act'));
 a.classList.add('act');$('p-'+a.dataset.p).classList.add('act');});
function color(l){return ['#94a3b8','#22c55e','#f97316','#ef4444'][l];}
function buzz(){try{if(window.Android&&Android.vibrate)Android.vibrate();else if(navigator.vibrate)navigator.vibrate([300,150,300]);}catch(e){}}
async function refresh(){
 try{
  const d=await (await fetch('/data',{cache:'no-store'})).json();
  $('conn').textContent='Live';$('conn').className='on';
  const ok=d.ph!==null,c=color(d.level);
  $('ph').textContent=ok?d.ph.toFixed(2):'--';$('ph').style.color=c;
  $('status').textContent=d.status;$('status').style.color=c;
  $('volt').textContent=d.voltage.toFixed(3);
  $('div').textContent=d.diverting?'ON':'OFF';
  $('min').textContent=d.min===null?'--':d.min.toFixed(2);
  $('max').textContent=d.max===null?'--':d.max.toFixed(2);
  $('c7').textContent=d.calV7.toFixed(3);$('c4').textContent=d.calV4.toFixed(3);
  $('dev').textContent='IP '+d.ip+' · Wi-Fi '+d.rssi+' dBm · up '+Math.floor(d.uptime/60)+' min';
  if(!setLoaded){$('smin').value=d.safeMin;$('smax').value=d.safeMax;setLoaded=true;}
  if(ok)$('marker').style.left=Math.min(100,Math.max(0,d.ph/14*100))+'%';
  if(d.level>=2){$('alert').style.display='block';$('alert').textContent='⚠️ '+d.status+' (pH '+(ok?d.ph.toFixed(2):'?')+')';
   if(lastLevel<2&&vib)buzz();}
  else $('alert').style.display='none';
  lastLevel=d.level;
  draw(d.history,d.safeMin,d.safeMax);
 }catch(e){$('conn').textContent='Offline';$('conn').className='off';}
}
function draw(h,lo,hi){
 const c=$('chart'),x=c.getContext('2d'),r=window.devicePixelRatio||1;
 c.width=c.clientWidth*r;c.height=c.clientHeight*r;x.setTransform(r,0,0,r,0,0);
 const W=c.clientWidth,H=c.clientHeight,y=p=>H-(p-3)/(10-3)*H;
 x.clearRect(0,0,W,H);
 x.fillStyle='rgba(34,197,94,.13)';x.fillRect(0,y(hi),W,y(lo)-y(hi));
 x.strokeStyle='#475569';x.fillStyle='#94a3b8';x.font='10px sans-serif';x.lineWidth=1;
 [4,5,6,7,8,9].forEach(v=>{x.globalAlpha=.3;x.beginPath();x.moveTo(0,y(v));x.lineTo(W,y(v));x.stroke();x.globalAlpha=1;x.fillText(v,2,y(v)-2);});
 const p=h.filter(v=>v!==null);if(p.length<2){x.fillText('Collecting data…',W/2-40,H/2);return;}
 x.strokeStyle='#38bdf8';x.lineWidth=2;x.beginPath();
 p.forEach((v,i)=>{const px=i/(p.length-1)*W;i?x.lineTo(px,y(v)):x.moveTo(px,y(v));});x.stroke();
}
async function cal(pt){
 if(!confirm('Probe is in pH '+pt+' buffer and the voltage is stable?'))return;
 const r=await fetch('/cal?point='+pt);alert(await r.text());refresh();
}
async function saveSet(){
 const a=parseFloat($('smin').value),b=parseFloat($('smax').value);
 if(!(a>=0&&b<=14&&a<b)){alert('Enter a minimum lower than the maximum (0–14).');return;}
 const r=await fetch('/settings?min='+a+'&max='+b);alert(await r.text());refresh();
}
function toggleVib(){vib=!vib;try{localStorage.setItem('vib',vib?'1':'0')}catch(e){}
 $('vibBtn').textContent='Vibrate on alert: '+(vib?'ON':'OFF');if(vib)buzz();}
refresh();setInterval(refresh,2000);
</script></body></html>
)rawliteral";

void handleRoot()     { server.send_P(200, "text/html", INDEX_HTML); }
void handleIcon()     { server.sendHeader("Cache-Control", "max-age=86400"); server.send_P(200, "image/svg+xml", ICON_SVG); }
void handleManifest() { server.send_P(200, "application/manifest+json", MANIFEST_JSON); }

void handleData() {
  String ip = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String j = "{";
  j += "\"ph\":" + fmt(lastPH, 2);
  j += ",\"voltage\":" + String(lastVoltage, 3);
  j += ",\"status\":\"" + statusText(lastPH) + "\"";
  j += ",\"level\":" + String(statusLevel(lastPH));
  j += ",\"diverting\":" + String(diverting ? "true" : "false");
  j += ",\"min\":" + fmt(phMin24, 2) + ",\"max\":" + fmt(phMax24, 2);
  j += ",\"safeMin\":" + String(safeMin, 1) + ",\"safeMax\":" + String(safeMax, 1);
  j += ",\"calV7\":" + String(calV7, 3) + ",\"calV4\":" + String(calV4, 3);
  j += ",\"ip\":\"" + ip + "\",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"history\":[";
  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - histCount + i + HISTORY_LEN) % HISTORY_LEN;
    if (i) j += ",";
    j += fmt(history[idx], 2);
  }
  j += "]}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

void handleCal() {
  String pt = server.arg("point");
  float v = readVoltage();
  if (pt == "7")      { calV7 = v; prefs.putFloat("v7", v); }
  else if (pt == "4") { calV4 = v; prefs.putFloat("v4", v); }
  else { server.send(400, "text/plain", "Use point=7 or point=4"); return; }
  phMin24 = phMax24 = NAN;   // old min/max are no longer meaningful
  server.send(200, "text/plain", "Saved pH " + pt + " at " + String(v, 3) + " V");
}

void handleSettings() {
  float a = server.arg("min").toFloat(), b = server.arg("max").toFloat();
  if (!(a >= 0 && b <= 14 && a < b)) { server.send(400, "text/plain", "Invalid range"); return; }
  safeMin = a; safeMax = b;
  prefs.putFloat("smin", a); prefs.putFloat("smax", b);
  server.send(200, "text/plain", "Safe range saved: " + String(a, 1) + " - " + String(b, 1));
}

void handleCSV() {
  String csv = "seconds_ago,ph\n";
  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - histCount + i + HISTORY_LEN) % HISTORY_LEN;
    csv += String((histCount - 1 - i) * (HISTORY_INTERVAL_MS / 1000)) + "," + fmt(history[idx], 2) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=rainwater_ph.csv");
  server.send(200, "text/csv", csv);
}

void setup() {
  Serial.begin(115200);
  analogSetPinAttenuation(PH_PIN, ADC_11db);   // full 0–3.3 V range
  if (RELAY_PIN >= 0) { pinMode(RELAY_PIN, OUTPUT); setRelay(false); }

  prefs.begin("phcal", false);
  calV7   = prefs.getFloat("v7", calV7);
  calV4   = prefs.getFloat("v4", calV4);
  safeMin = prefs.getFloat("smin", safeMin);
  safeMax = prefs.getFloat("smax", safeMax);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nOpen on your phone: http://%s/  or  http://%s.local/\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
  } else {
    // No router? ESP32 makes its own hotspot. Connect phone to it, open http://192.168.4.1/
    WiFi.mode(WIFI_AP);
    WiFi.softAP("RainwaterPH", "12345678");
    Serial.println("\nHotspot 'RainwaterPH' (password 12345678) -> http://192.168.4.1/");
  }
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", handleRoot);
  server.on("/icon.svg", handleIcon);
  server.on("/manifest.json", handleManifest);
  server.on("/data", handleData);
  server.on("/cal", handleCal);
  server.on("/settings", handleSettings);
  server.on("/csv", handleCSV);
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });
  server.begin();

  takeReading();
  pushHistory();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL_MS)    { lastRead = now; takeReading(); }
  if (now - lastHist >= HISTORY_INTERVAL_MS) { lastHist = now; pushHistory(); }
}

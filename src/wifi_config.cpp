#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include "ui.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>

static WebServer server(80);
static WebSocketsServer wsServer(81);

static constexpr size_t WS_AUDIO_BUF_SAMPLES = 512;
static int16_t ws_audio_buf[WS_AUDIO_BUF_SAMPLES];
static size_t ws_audio_len = 0;
static volatile bool ws_client_connected = false;
static uint64_t ws_stream_start_ms = 0;
static bool timeConfigured = false;
static bool timeSynced = false;
static bool serverStarted = false;
static uint32_t lastReconnectMs = 0;
static bool staConnecting = false;
static uint32_t staStartMs = 0;
static bool apActive = false;
static wl_status_t lastStaStatus = WL_IDLE_STATUS;

static String savedSsid;
static String savedPass;

static String pageHeader(const char* title, const char* active) {
  String page;
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>");
  page += F("<title>"); page += title; page += F("</title>");
  page += F("<style>");
  page += F("body{font-family:Arial,Helvetica,sans-serif;background:#f5f6f7;margin:0;color:#222;}");
  page += F(".top{background:#fff;border-bottom:1px solid #e5e5e5;padding:12px 20px;display:flex;align-items:center;gap:20px;}");
  page += F(".brand{font-weight:700;}");
  page += F(".nav a{margin-right:12px;text-decoration:none;color:#555;padding:6px 10px;border-radius:4px;}");
  page += F(".nav a.active{background:#e9f5ff;color:#1a73e8;}");
  page += F(".container{max-width:900px;margin:20px auto;padding:0 16px;}");
  page += F(".section{background:#fff;border:1px solid #e5e5e5;border-radius:8px;padding:16px 20px;margin-bottom:16px;}");
  page += F(".section h3{margin:0 0 12px 0;font-size:18px;}");
  page += F(".grid{display:grid;grid-template-columns:1fr 2fr;gap:12px;align-items:center;}");
  page += F("label{font-size:13px;color:#444;}");
  page += F("input{width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:6px;font-size:14px;}");
  page += F(".row{display:grid;grid-template-columns:140px 1fr;gap:12px;align-items:center;margin:6px 0;}");
  page += F(".row .ctrl{display:flex;gap:8px;align-items:center;}");
  page += F("input[type=range]{width:100%;}");
  page += F(".switch{position:relative;display:inline-block;width:44px;height:24px;}");
  page += F(".switch input{opacity:0;width:0;height:0;}");
  page += F(".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.2s;border-radius:24px;}");
  page += F(".slider:before{position:absolute;content:'';height:18px;width:18px;left:3px;bottom:3px;background:white;transition:.2s;border-radius:50%;}");
  page += F(".switch input:checked+.slider{background:#1a7f37;}");
  page += F(".switch input:checked+.slider:before{transform:translateX(20px);}");
  page += F(".actions{display:flex;gap:10px;justify-content:flex-end;margin-top:12px;}");
  page += F(".btn{background:#1a7f37;color:#fff;border:none;padding:8px 14px;border-radius:6px;cursor:pointer;}");
  page += F(".btn.secondary{background:#e9e9e9;color:#333;}");
  page += F(".small{font-size:12px;color:#666;}");
  page += F(".muted{font-size:13px;color:#666;margin:6px 0 12px;}");
  page += F(".badge{display:inline-block;padding:4px 8px;border-radius:999px;background:#eee;color:#333;font-size:12px;}");
  page += F("</style></head><body>");
  page += F("<div class='top'><div class='brand'>uSDX</div><div class='nav'>");
  page += F("<a href='/' class='"); page += (strcmp(active, "wifi") == 0) ? "active" : ""; page += F("'>WiFi Config</a>");
  page += F("<a href='/ui' class='"); page += (strcmp(active, "ui") == 0) ? "active" : ""; page += F("'>UI Settings</a>");
  page += F("</div></div>");
  page += F("<div class='container'>");
  return page;
}

static String pageFooter() {
  String page;
  page += F("</div></body></html>");
  return page;
}

static void loadCredentials() {
  Preferences prefs;
  if (!prefs.begin("net", true)) return;
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
}

static void setup_time_once() {
  if (!timeConfigured) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    timeConfigured = true;
  }
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 2000)) {
    timeSynced = true;
  }
}

static uint64_t utc_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length)
  {
    (void)payload;
    (void)length;
    if (type == WStype_CONNECTED) {
      ws_client_connected = true;
      ws_stream_start_ms = utc_ms();
    } else if (type == WStype_DISCONNECTED) {
      ws_client_connected = false;
    }else if (type == WStype_TEXT) {

      Serial.printf("[WS] RX spot (%u bytes)\n", length);
      // handle_decoded_spot((const char *)payload, length);
    }
  }

static void saveCredentials(const String& ssid, const String& pass) {
  Preferences prefs;
  if (!prefs.begin("net", false)) return;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static void handleRoot() {
  Serial.printf("[HTTP] %s %s\n", (server.method() == HTTP_GET) ? "GET" : "POST", server.uri().c_str());
  String page = pageHeader("uSDX WiFi", "wifi");
  page += F("<div class='section'><h3>WiFi Access</h3>");
  page += F("<div class='small'>Enter SSID and password. Device will reboot after saving.</div>");
  page += F("<form method='POST' action='/save'>");
  page += F("<div class='grid'>");
  page += F("<label>SSID</label><input name='ssid' maxlength='32' value=''>");
  page += F("<label>Password</label><input name='pass' type='password' maxlength='64' value=''>");
  page += F("</div>");
  page += F("<div class='actions'><button class='btn' type='submit'>Save & Reboot</button></div>");
  page += F("</form></div>");
  page += pageFooter();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(page);
  server.sendContent("");
  Serial.printf("[HTTP] Done %s\n", server.uri().c_str());
}

static void handleUi() {
  Serial.printf("[HTTP] %s %s\n", (server.method() == HTTP_GET) ? "GET" : "POST", server.uri().c_str());
  UiSettings s;
  if (!ui_get_settings(&s)) {
    server.send(500, "text/plain", "Failed to read settings");
    Serial.printf("[HTTP] Done %s (500)\n", server.uri().c_str());
    return;
  }
  String page = pageHeader("uSDX UI", "ui");
  page += F("<div class='section'><h3>uSDX UI Settings</h3>");
  page += F("<form method='POST' action='/ui/save'>");
  page += F("<div class='row'><label>VFO A (Hz)</label><div class='ctrl'><input name='vfoA' value='"); page += s.vfoA; page += F("'></div></div>");
  page += F("<div class='row'><label>VFO B (Hz)</label><div class='ctrl'><input name='vfoB' value='"); page += s.vfoB; page += F("'></div></div>");
  page += F("<div class='row'><label>VFO Sel</label><div class='ctrl'><select name='vfoSel'>");
  page += F("<option value='0' "); if (s.vfoSel == 0) page += F("selected"); page += F(">A</option>");
  page += F("<option value='1' "); if (s.vfoSel == 1) page += F("selected"); page += F(">B</option>");
  page += F("</select></div></div>");
  page += F("<div class='row'><label>Mode</label><div class='ctrl'><select name='mode'>");
  page += F("<option value='0' "); if (s.mode == 0) page += F("selected"); page += F(">LSB</option>");
  page += F("<option value='1' "); if (s.mode == 1) page += F("selected"); page += F(">USB</option>");
  page += F("<option value='2' "); if (s.mode == 2) page += F("selected"); page += F(">CW</option>");
  page += F("<option value='3' "); if (s.mode == 3) page += F("selected"); page += F(">AM</option>");
  page += F("</select></div></div>");
  page += F("<div class='row'><label>Band (0-10)</label><div class='ctrl'><input type='range' min='0' max='10' name='bandval' value='"); page += s.bandval; page += F("'><input type='number' min='0' max='10' name='bandval' value='"); page += s.bandval; page += F("'></div></div>");
  page += F("<div class='row'><label>Step (0-9)</label><div class='ctrl'><input type='range' min='0' max='9' name='stepsize' value='"); page += s.stepsize; page += F("'><input type='number' min='0' max='9' name='stepsize' value='"); page += s.stepsize; page += F("'></div></div>");
  page += F("<div class='row'><label>RIT (Hz)</label><div class='ctrl'><input type='range' min='-9999' max='9999' name='rit' value='"); page += s.rit; page += F("'><input name='rit' value='"); page += s.rit; page += F("'></div></div>");
  page += F("<div class='row'><label>RIT Active</label><div class='ctrl'><input type='hidden' name='ritActive' value='0'><label class='switch'><input type='checkbox' name='ritActive' value='1' "); if (s.ritActive) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>Volume (0-10)</label><div class='ctrl'><input type='range' min='0' max='10' name='volume' value='"); page += s.volume; page += F("'><input type='number' min='0' max='10' name='volume' value='"); page += s.volume; page += F("'></div></div>");
  page += F("<div class='row'><label>Filter</label><div class='ctrl'><select name='filt'>");
  page += F("<option value='0' "); if (s.filt == 0) page += F("selected"); page += F(">0 None</option>");
  page += F("<option value='1' "); if (s.filt == 1) page += F("selected"); page += F(">1 (SSB/AM): 300–2900 Hz</option>");
  page += F("<option value='2' "); if (s.filt == 2) page += F("selected"); page += F(">2 (SSB/AM): 300–2400 Hz</option>");
  page += F("<option value='3' "); if (s.filt == 3) page += F("selected"); page += F(">3 (SSB/AM): 300–1800 Hz</option>");
  page += F("<option value='4' "); if (s.filt == 4) page += F("selected"); page += F(">4 (CW): 500–1000 Hz</option>");
  page += F("<option value='5' "); if (s.filt == 5) page += F("selected"); page += F(">5 (CW): 650–840 Hz</option>");
  page += F("<option value='6' "); if (s.filt == 6) page += F("selected"); page += F(">6 (CW): 650–750 Hz</option>");
  page += F("<option value='7' "); if (s.filt == 7) page += F("selected"); page += F(">7 (AM): 50–2900 Hz</option>");
  page += F("</select></div></div>");
  page += F("<div class='row'><label>AGC</label><div class='ctrl'><input type='hidden' name='agc' value='0'><label class='switch'><input type='checkbox' name='agc' value='1' "); if (s.agc) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>NR</label><div class='ctrl'><input type='hidden' name='nr' value='0'><label class='switch'><input type='checkbox' name='nr' value='1' "); if (s.nr) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>ATT (0-2)</label><div class='ctrl'><input type='range' min='0' max='2' name='att' value='"); page += s.att; page += F("'><input type='number' min='0' max='2' name='att' value='"); page += s.att; page += F("'></div></div>");
  page += F("<div class='row'><label>S-Meter (0-6)</label><div class='ctrl'><input type='range' min='0' max='6' name='smode' value='"); page += s.smode; page += F("'><input type='number' min='0' max='6' name='smode' value='"); page += s.smode; page += F("'></div></div>");
  page += F("<div class='row'><label>CW Tone (0-5)</label><div class='ctrl'><input type='range' min='0' max='5' name='cw_tone' value='"); page += s.cw_tone; page += F("'><input type='number' min='0' max='5' name='cw_tone' value='"); page += s.cw_tone; page += F("'></div></div>");
  page += F("<div class='row'><label>CW Offset (300-1200)</label><div class='ctrl'><input type='range' min='300' max='1200' name='cw_offset' value='"); page += s.cw_offset; page += F("'><input type='number' min='300' max='1200' name='cw_offset' value='"); page += s.cw_offset; page += F("'></div></div>");
  page += F("<div class='row'><label>VOX</label><div class='ctrl'><input type='hidden' name='vox' value='0'><label class='switch'><input type='checkbox' name='vox' value='1' "); if (s.vox) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>VOX Gain (0-100)</label><div class='ctrl'><input type='range' min='0' max='100' name='vox_gain' value='"); page += s.vox_gain; page += F("'><input type='number' min='0' max='100' name='vox_gain' value='"); page += s.vox_gain; page += F("'></div></div>");
  page += F("<div class='row'><label>Drive (0-10)</label><div class='ctrl'><input type='range' min='0' max='10' name='drive' value='"); page += s.drive; page += F("'><input type='number' min='0' max='10' name='drive' value='"); page += s.drive; page += F("'></div></div>");
  page += F("<div class='row'><label>TX Delay (0-500)</label><div class='ctrl'><input type='range' min='0' max='500' name='txdelay' value='"); page += s.txdelay; page += F("'><input type='number' min='0' max='500' name='txdelay' value='"); page += s.txdelay; page += F("'></div></div>");
  page += F("<div class='row'><label>MOX</label><div class='ctrl'><input type='hidden' name='mox' value='0'><label class='switch'><input type='checkbox' name='mox' value='1' "); if (s.mox) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>Backlight</label><div class='ctrl'><input type='hidden' name='backlight' value='0'><label class='switch'><input type='checkbox' name='backlight' value='1' "); if (s.backlight) page += F("checked"); page += F("><span class='slider'></span></label></div></div>");
  page += F("<div class='row'><label>SI Fxtal (Hz)</label><div class='ctrl'><input name='sifxtal' value='"); page += s.sifxtal; page += F("'></div></div>");
  page += F("<div class='row'><label>IQ Phase (30-150)</label><div class='ctrl'><input type='range' min='30' max='150' name='iq_phase' value='"); page += s.iq_phase; page += F("'><input type='number' min='30' max='150' name='iq_phase' value='"); page += s.iq_phase; page += F("'></div></div>");
  page += F("<div class='row'><label>IQ Bal (50-150)</label><div class='ctrl'><input type='range' min='50' max='150' name='iq_balance' value='"); page += s.iq_balance; page += F("'><input type='number' min='50' max='150' name='iq_balance' value='"); page += s.iq_balance; page += F("'></div></div>");
  page += F("<div class='row'><label>IQ Delay (-50..50)</label><div class='ctrl'><input type='range' min='-50' max='50' name='iq_delay' value='"); page += s.iq_delay; page += F("'><input type='number' min='-50' max='50' name='iq_delay' value='"); page += s.iq_delay; page += F("'></div></div>");
  page += F("<div class='row'><label>WF Thresh (0-100)</label><div class='ctrl'><input type='range' min='0' max='100' name='wf_thresh' value='"); page += s.wf_thresh; page += F("'><input type='number' min='0' max='100' name='wf_thresh' value='"); page += s.wf_thresh; page += F("'></div></div>");
  page += F("<div class='actions'><button class='btn' type='submit'>Apply</button></div>");
  page += F("</form></div>");

  page += F("<div class='section'><h3>Audio Monitor</h3>");
  page += F("<div class='muted'>Stream 8 kHz PCM audio to this browser.</div>");
  page += F("<div class='actions'>");
  page += F("<button class='btn' type='button' id='audioStart'>Start Audio</button>");
  page += F("<button class='btn secondary' type='button' id='audioStop'>Stop Audio</button>");
  page += F("<span id='audioStatus' class='badge'>Stopped</span>");
  page += F("</div></div>");

  page += F("<script>");
  page += F("(function(){\n");
  page += F("let ws=null,audioCtx=null,node=null,queue=[],cur=null,curIdx=0;\n");
  page += F("const statusEl=document.getElementById('audioStatus');\n");
  page += F("function setStatus(t){if(statusEl) statusEl.textContent=t;}\n");
  page += F("function onAudio(e){\n");
  page += F("  const out=e.outputBuffer.getChannelData(0);\n");
  page += F("  for(let i=0;i<out.length;i++){\n");
  page += F("    if(!cur || curIdx>=cur.length){\n");
  page += F("      cur=queue.shift(); curIdx=0;\n");
  page += F("      if(!cur){out[i]=0; continue;}\n");
  page += F("    }\n");
  page += F("    out[i]=cur[curIdx++];\n");
  page += F("  }\n");
  page += F("}\n");
  page += F("function startAudio(){\n");
  page += F("  if(ws){return;}\n");
  page += F("  audioCtx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:8000});\n");
  page += F("  node=audioCtx.createScriptProcessor(1024,0,1);\n");
  page += F("  node.onaudioprocess=onAudio;\n");
  page += F("  node.connect(audioCtx.destination);\n");
  page += F("  ws=new WebSocket('ws://'+location.hostname+':81');\n");
  page += F("  ws.binaryType='arraybuffer';\n");
  page += F("  ws.onopen=()=>setStatus('Streaming');\n");
  page += F("  ws.onclose=()=>{setStatus('Stopped'); ws=null;};\n");
  page += F("  ws.onerror=()=>setStatus('Error');\n");
  page += F("  ws.onmessage=(evt)=>{\n");
  page += F("    const i16=new Int16Array(evt.data);\n");
  page += F("    const f32=new Float32Array(i16.length);\n");
  page += F("    for(let i=0;i<i16.length;i++){f32[i]=i16[i]/32768;}\n");
  page += F("    queue.push(f32);\n");
  page += F("  };\n");
  page += F("}\n");
  page += F("function stopAudio(){\n");
  page += F("  if(ws){ws.close(); ws=null;}\n");
  page += F("  if(node){node.disconnect(); node=null;}\n");
  page += F("  if(audioCtx){audioCtx.close(); audioCtx=null;}\n");
  page += F("  queue=[]; cur=null; curIdx=0; setStatus('Stopped');\n");
  page += F("}\n");
  page += F("const s=document.getElementById('audioStart');\n");
  page += F("const t=document.getElementById('audioStop');\n");
  page += F("if(s) s.addEventListener('click', startAudio);\n");
  page += F("if(t) t.addEventListener('click', stopAudio);\n");
  page += F("})();\n");
  page += F("</script>");
  page += pageFooter();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(page);
  server.sendContent("");
  Serial.printf("[HTTP] Done %s\n", server.uri().c_str());
}

static int32_t argToInt(const String& arg, int32_t defVal) {
  if (arg.length() == 0) return defVal;
  return arg.toInt();
}

static bool argHasValue(const String& name, const String& value) {
  const int count = server.args();
  for (int i = 0; i < count; i++) {
    if (server.argName(i) == name && server.arg(i) == value) {
      return true;
    }
  }
  return false;
}

static void handleUiSave() {
  Serial.printf("[HTTP] %s %s\n", (server.method() == HTTP_GET) ? "GET" : "POST", server.uri().c_str());
  UiSettings s;
  if (!ui_get_settings(&s)) {
    server.send(500, "text/plain", "Failed to read settings");
    Serial.printf("[HTTP] Done %s (500)\n", server.uri().c_str());
    return;
  }

  s.vfoA = argToInt(server.arg("vfoA"), s.vfoA);
  s.vfoB = argToInt(server.arg("vfoB"), s.vfoB);
  s.vfoSel = (uint8_t)argToInt(server.arg("vfoSel"), s.vfoSel);
  s.mode = (uint8_t)argToInt(server.arg("mode"), s.mode);
  s.bandval = argToInt(server.arg("bandval"), s.bandval);
  s.stepsize = (uint8_t)argToInt(server.arg("stepsize"), s.stepsize);
  s.rit = (int16_t)argToInt(server.arg("rit"), s.rit);
  s.ritActive = argHasValue("ritActive", "1") ? 1 : 0;
  s.volume = (int8_t)argToInt(server.arg("volume"), s.volume);
  s.filt = (int8_t)argToInt(server.arg("filt"), s.filt);
  s.agc = argHasValue("agc", "1") ? 1 : 0;
  s.nr = argHasValue("nr", "1") ? 1 : 0;
  s.att = (int8_t)argToInt(server.arg("att"), s.att);
  s.smode = (int8_t)argToInt(server.arg("smode"), s.smode);
  s.cw_tone = (int8_t)argToInt(server.arg("cw_tone"), s.cw_tone);
  s.cw_offset = (int16_t)argToInt(server.arg("cw_offset"), s.cw_offset);
  s.vox = argHasValue("vox", "1") ? 1 : 0;
  s.vox_gain = (int8_t)argToInt(server.arg("vox_gain"), s.vox_gain);
  s.drive = (int8_t)argToInt(server.arg("drive"), s.drive);
  s.txdelay = (int16_t)argToInt(server.arg("txdelay"), s.txdelay);
  s.mox = argHasValue("mox", "1") ? 1 : 0;
  s.backlight = argHasValue("backlight", "1") ? 1 : 0;
  s.sifxtal = argToInt(server.arg("sifxtal"), s.sifxtal);
  s.iq_phase = (int16_t)argToInt(server.arg("iq_phase"), s.iq_phase);
  s.iq_balance = (int16_t)argToInt(server.arg("iq_balance"), s.iq_balance);
  s.iq_delay = (int16_t)argToInt(server.arg("iq_delay"), s.iq_delay);
  s.wf_thresh = (int8_t)argToInt(server.arg("wf_thresh"), s.wf_thresh);

  ui_apply_settings(s);
  server.sendHeader("Location", "/ui", true);
  server.send(303, "text/plain", "UI settings applied.");
  Serial.printf("[HTTP] Done %s\n", server.uri().c_str());
}

static void handleSave() {
  Serial.printf("[HTTP] %s %s\n", (server.method() == HTTP_GET) ? "GET" : "POST", server.uri().c_str());
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID is required.");
    Serial.printf("[HTTP] Done %s (400)\n", server.uri().c_str());
    return;
  }

  saveCredentials(ssid, pass);
  server.send(200, "text/plain", "Saved. Rebooting...");
  Serial.printf("[HTTP] Done %s\n", server.uri().c_str());
  delay(500);
  ESP.restart();
}

static void startServer() {
  if (serverStarted) return;
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ui", handleUi);
  server.on("/ui/save", HTTP_POST, handleUiSave);
  server.begin();
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  serverStarted = true;
}

static void startSTA() {
  if (savedSsid.length() == 0) return;
  WiFi.mode(WIFI_STA);
  Serial.printf("[WiFi] Connecting to SSID '%s'...\n", savedSsid.c_str());
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  staConnecting = true;
  staStartMs = millis();
}

static void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("uSDX-Setup", "usdx1234");
  Serial.printf("[WiFi] AP started. SSID=uSDX-Setup IP=%s\n", WiFi.softAPIP().toString().c_str());
  apActive = true;
}

void wifi_config_setup() {
  loadCredentials();
  if (savedSsid.length() > 0) {
    startSTA();
  } else {
    startAP();
  }
  startServer();
}

void wifi_config_loop() {
  if (serverStarted) {
    server.handleClient();
  }

  if (staConnecting && WiFi.status() == WL_CONNECTED) {
    staConnecting = false;
    Serial.printf("[WiFi] STA connected. IP=%s\n", WiFi.localIP().toString().c_str());
    setup_time_once();
  }

  if (!timeSynced && (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    setup_time_once();
  }

  if (staConnecting) {
    const uint32_t now = millis();
    if ((uint32_t)(now - staStartMs) > 8000U) {
      Serial.printf("[WiFi] STA connect timeout.\n");
      staConnecting = false;
      if (!apActive) {
        startAP();
      }
    }
  }

  if (WiFi.getMode() & WIFI_STA) {
    wl_status_t st = WiFi.status();
    if (st != lastStaStatus) {
      if (st != WL_CONNECTED) {
        Serial.printf("[WiFi] STA disconnected (status=%d).\n", (int)st);
      }
      lastStaStatus = st;
    }
    if (st != WL_CONNECTED) {
      const uint32_t now = millis();
      if ((uint32_t)(now - lastReconnectMs) > 10000U) {
        lastReconnectMs = now;
        Serial.printf("[WiFi] STA disconnected. Reconnecting...\n");
        WiFi.disconnect();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
        staConnecting = true;
        staStartMs = now;
      }
    }
  }
  wsServer.loop();
}

void print_utc(uint64_t ts_ms) {
  time_t sec = ts_ms / 1000;
  uint16_t ms = ts_ms % 1000;

  struct tm tm_utc;
  gmtime_r(&sec, &tm_utc);

  char buf[32];
  snprintf(buf, sizeof(buf),
           "%02d:%02d:%02d.%03u",
           tm_utc.tm_hour,
           tm_utc.tm_min,
           tm_utc.tm_sec,
           ms);

  Serial.println(buf);
}


void wifi_config_audio_push(int32_t freq_hz, int16_t sample)
{
  if (!ws_client_connected) {
    return;
  }

  ws_audio_buf[ws_audio_len++] = sample;
  if (ws_audio_len >= WS_AUDIO_BUF_SAMPLES) {
        uint8_t packet[8 + 4 + 4 + (WS_AUDIO_BUF_SAMPLES * sizeof(int16_t))];
    const uint64_t ts = utc_ms();
    struct tm tm_utc;
    time_t sec = ts / 1000;
    uint16_t ms = ts % 1000;
    gmtime_r(&sec, &tm_utc);
    int sec15 = tm_utc.tm_sec % 15;
    if( sec15 > 14 || sec15 < 2  ) {
    //printf("%.2d:%.2d:%.2d.%.3u ", tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ms);
      printf("[WS] Sending audio packet, ts=%llu, samples=%u\n", ts, (unsigned)ws_audio_len);
    }

    // load timestamp (little endian)
    for (uint8_t i = 0; i < 8; i++) {
      packet[7-i] = (uint8_t)((ts >> (8 * i)) & 0xFF); 
    }
    // load sample count (little endian)
    for (uint8_t i = 0; i < 4; i++) {
      packet[8 + 3 - i] = (uint8_t)((ws_audio_len >> (8 * i)) & 0xFF); 
    }
    
    // load frequency in Hz (little endian)
    for (uint8_t i = 0; i < 4; i++) {
      packet[12 + 3 - i] = (uint8_t)((freq_hz >> (8 * i)) & 0xFF); 
    }
    
    // load samples
    memcpy(packet + 8 + 4 + 4, ws_audio_buf, ws_audio_len * sizeof(int16_t));
    wsServer.broadcastBIN(packet, 8 + 4 + 4 + ws_audio_len * sizeof(int16_t));
    ws_audio_len = 0;
  }
}

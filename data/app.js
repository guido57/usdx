let allQSOs = [];
let selectedQsoId = null;

(async function(){

// location, flag and nation functions for FT8 QSOs
// === CONFIG: set your locator here ===
const MY_LOCATOR = "JN53qh"; // <-- change to yours

let ctyData = [];
let prefixMap = [];

async function loadCtyDat() {
  const resp = await fetch('/cty.dat');
  const text = await resp.text();
  parseCtyDat(text);
}

await loadCtyDat();

function parseCtyDat(text) {
  prefixMap = [];

  const blocks = text.split(';');

  for (let block of blocks) {
    block = block.trim();
    if (!block) continue;

    // normalize spaces
    block = block.replace(/\r?\n/g, ' ').replace(/\s+/g, ' ');

    // find FIRST colon (after country)
    const firstColon = block.indexOf(':');
    if (firstColon === -1) continue;

    const country = block.substring(0, firstColon).trim();

    // find SECOND colon (after dxcc)
    const secondColon = block.indexOf(':', firstColon + 1);
    if (secondColon === -1) continue;

    const dxccStr = block.substring(firstColon + 1, secondColon).trim();
    const dxcc = parseInt(dxccStr, 10);
    if (isNaN(dxcc)) continue;

    // 🔥 KEY PART: prefixes are AFTER THE LAST COLON
    const lastColon = block.lastIndexOf(':');
    if (lastColon === -1 || lastColon <= secondColon) continue;

    const prefixPart = block.substring(lastColon + 1);

    const prefixes = prefixPart.split(',')
      .map(cleanPrefix)
      .filter(p => p.length > 0);

    prefixMap.push({
      country,
      dxcc,
      prefixes
    });
  }

  console.log("CTY loaded:", prefixMap.length);
}

function cleanPrefix(p) {
  return p
    .replace(/[=()]/g, '')
    .replace(/\//g, '')
    .replace(/\s+/g, '')
    .toUpperCase()
    .trim();
}

function normalizeCallsign(cs) {
  if (!cs) return '';

  cs = cs.toUpperCase();

  // remove portable prefixes like F/IW5ALZ/P
  if (cs.includes('/')) {
    const parts = cs.split('/');

    // heuristic: longest part is usually the real call
    cs = parts.sort((a,b)=>b.length-a.length)[0];
  }

  return cs;
}

function lookupCallsign(ctyCall) {
  const cs = normalizeCallsign(ctyCall);

  let bestMatch = null;
  let bestLength = 0;

  for (const entry of prefixMap) {
    for (const prefix of entry.prefixes) {

      if (cs.startsWith(prefix) && prefix.length > bestLength) {
        bestMatch = entry;
        bestLength = prefix.length;
      }
    }
  }

  return bestMatch;
}

// --- Maidenhead → lat/lon ---
function maidenheadToLatLon(locator) {
  if (!locator || locator.length < 4) return null;

  locator = locator.toUpperCase();
  const A = 'A'.charCodeAt(0);

  let lon = (locator.charCodeAt(0) - A) * 20 - 180;
  let lat = (locator.charCodeAt(1) - A) * 10 - 90;

  lon += parseInt(locator[2]) * 2;
  lat += parseInt(locator[3]) * 1;

  if (locator.length >= 6) {
    lon += (locator.charCodeAt(4) - A) * (5 / 60);
    lat += (locator.charCodeAt(5) - A) * (2.5 / 60);
  }

  lon += 1;
  lat += 0.5;

  return { lat, lon };
}

// --- Distance ---
function haversine(lat1, lon1, lat2, lon2) {
  const R = 6371;
  const toRad = d => d * Math.PI / 180;

  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);

  const a =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(toRad(lat1)) *
    Math.cos(toRad(lat2)) *
    Math.sin(dLon / 2) ** 2;

  return 2 * R * Math.asin(Math.sqrt(a));
}

const countryToISO = {
  // ===== EUROPE =====
  "Italy": "it",
  "France": "fr",
  "Germany": "de",
  "Spain": "es",
  "Portugal": "pt",
  "United Kingdom": "gb",
  "England": "gb",
  "Scotland": "gb",
  "Wales": "gb",
  "Ireland": "ie",
  "Switzerland": "ch",
  "Austria": "at",
  "Belgium": "be",
  "Netherlands": "nl",
  "Luxembourg": "lu",
  "Denmark": "dk",
  "Norway": "no",
  "Sweden": "se",
  "Finland": "fi",
  "Poland": "pl",
  "Czech Republic": "cz",
  "Slovakia": "sk",
  "Hungary": "hu",
  "Romania": "ro",
  "Bulgaria": "bg",
  "Greece": "gr",
  "Ukraine": "ua",
  "Belarus": "by",
  "Russia": "ru",
  "European Russia": "ru",
  "Estonia": "ee",
  "Latvia": "lv",
  "Lithuania": "lt",
  "Iceland": "is",
  "Malta": "mt",
  "Cyprus": "cy",
  "Slovenia": "si",
  "Croatia": "hr",
  "Serbia": "rs",
  "Bosnia and Herzegovina": "ba",
  "Albania": "al",
  "North Macedonia": "mk",
  "Moldova": "md",
  "San Marino": "sm",
  "Monaco": "mc",
  "Andorra": "ad",
  "Vatican City": "va",
  "Liechtenstein": "li",

  // Canary / special EU territories
  "Canary Islands": "es",
  "Balearic Islands": "es",

  // ===== NORTH AMERICA =====
  "United States": "us",
  "USA": "us",
  "Canada": "ca",
  "Mexico": "mx",
  "Cuba": "cu",
  "Jamaica": "jm",
  "Puerto Rico": "pr",
  "Bahamas": "bs",
  "Dominican Republic": "do",
  "Haiti": "ht",

  // ===== SOUTH AMERICA =====
  "Brazil": "br",
  "Argentina": "ar",
  "Chile": "cl",
  "Peru": "pe",
  "Colombia": "co",
  "Venezuela": "ve",
  "Uruguay": "uy",
  "Paraguay": "py",
  "Bolivia": "bo",
  "Ecuador": "ec",

  // ===== ASIA =====
  "China": "cn",
  "Japan": "jp",
  "South Korea": "kr",
  "North Korea": "kp",
  "Taiwan": "tw",
  "India": "in",
  "Pakistan": "pk",
  "Indonesia": "id",
  "Malaysia": "my",
  "Philippines": "ph",
  "Thailand": "th",
  "Vietnam": "vn",
  "Singapore": "sg",
  "Sri Lanka": "lk",
  "Israel": "il",
  "Saudi Arabia": "sa",
  "United Arab Emirates": "ae",
  "Iran": "ir",
  "Iraq": "iq",
  "Turkey": "tr",
  "Kazakhstan": "kz",
  "Mongolia": "mn",

  // Russia already included above

  // ===== AFRICA =====
  "South Africa": "za",
  "Egypt": "eg",
  "Morocco": "ma",
  "Algeria": "dz",
  "Tunisia": "tn",
  "Nigeria": "ng",
  "Kenya": "ke",
  "Ethiopia": "et",
  "Sudan": "sd",
  "Libya": "ly",
  "Ghana": "gh",
  "Uganda": "ug",
  "Tanzania": "tz",

  // ===== OCEANIA =====
  "Australia": "au",
  "New Zealand": "nz",
  "Papua New Guinea": "pg",
  "Fiji": "fj",
  "Solomon Islands": "sb",
  "Vanuatu": "vu",
  "Samoa": "ws",

  // ===== SPECIAL / HAM RADIO COMMON =====
  "European Russia": "ru",
  "Asiatic Russia": "ru",
  "EU Russia": "ru",
  "AS Russia": "ru",

  // PATCH

  "Lebanon": "lb",
  "Syria": "sy",
  "Palestine": "ps",
  "Iraq": "iq",
  "Iran": "ir",
  "Yemen": "ye",
  "Jordan": "jo",

  "Hong Kong": "hk",
  "Macau": "mo",

  "Canary Islands": "es",
  "Balearic Islands": "es",

  "Madeira Islands": "pt",
  "Azores": "pt",
  
  "United Kingdom": "gb",
  "England": "gb",
  "Scotland": "gb",
  "Wales": "gb",
  "Northern Ireland": "gb",

  "USA": "us",
  "United States": "us",

  "Kaliningrad": "ru",
  "Kaliningrad (European Russia)": "ru",

  "Federal Republic of Germany": "de",
  "Fed. Rep. of Germany": "de",
  "Germany": "de",

  "Djibouti": "dj",

  "Asiatic Turkey": "tr",
  "European Turkey": "tr",
  "Turkey": "tr",

"Republic of the Congo": "cg",
"Congo": "cg",
"Congo (Brazzaville)": "cg",

"Benin": "bj",

"Bahrain": "bh",

"Oman": "om",

"Bangladesh": "bd",

"Bosnia and Herzegovina": "ba",
"Bosnia-Herzegovina": "ba",

"Slovak Republic": "sk",
"Slovakia": "sk",

"Sicily": "it",          // ISO Italy, DXCC IT9 special case
"Guantanamo Bay": "cu",  // territory mapping

"Namibia": "na",
"Kosovo": "xk",
"Georgia": "ge",

"Macedonia": "mk",
"North Macedonia": "mk",
"Kyrgyzstan": "kg",
"Central African Republic": "cf",
"Croatia": "hr",
"Malawi": "mw",
"Guadeloupe": "gp",
"Cayman Islands": "ky",
"Qatar": "qa",
"Crete": "gr",
"Corsica": "fr",
"Sardinia": "it",
"Réunion": "re",
"Martinique": "mq",
"Guam": "gu",
"Northern Mariana Islands": "mp",
"Aruba": "aw",
"Armenia": "am",
"Liberia": "lr",

};

function getFlagFromCountry(country) {
  const iso = countryToISO[country];
  if (!iso) return '';

  return `<img src="https://flagcdn.com/w20/${iso.toLowerCase()}.png">`;
}

// --- Callsign → country (basic, extend later) ---
const callsignCache = {};

function getCountryFromCallsign(cs) {
  if (callsignCache[cs]) return callsignCache[cs];

  const entry = lookupCallsign(cs);

  const result = entry
    ? {
        country: entry.country,
        dxcc: entry.dxcc
      }
    : {
        country: 'Unknown',
        dxcc: null
      };

  callsignCache[cs] = result;
  return result;
}

// --- Flag ---
function getFlagHtml(iso) {
  if (!iso) return '';
  return `<img src="https://flagcdn.com/w20/${iso.toLowerCase()}.png" title="${iso}">`;
}

// --- Combined helper ---
function computeGeo(call, grid) {
  const my = maidenheadToLatLon(MY_LOCATOR);
  const his = maidenheadToLatLon(grid);

  let distance = '';
  if (my && his) {
    distance = Math.round(haversine(my.lat, my.lon, his.lat, his.lon));
  }

  const { country, dxcc } = getCountryFromCallsign(call);

  return {
    distance,
    country,
    dxcc,
    flag: getFlagFromCountry(country) 
  };
}


// ------------------------------------------
// Audio streaming
// ------------------------------------------   
let ws=null,audioCtx=null,node=null,queue=[],cur=null,curIdx=0;
const statusEl=document.getElementById('audioStatus');

function setStatus(t){if(statusEl) statusEl.textContent=t;}

function onAudio(e){
  const out=e.outputBuffer.getChannelData(0);
  for(let i=0;i<out.length;i++){
    if(!cur || curIdx>=cur.length){cur=queue.shift(); curIdx=0;if(!cur){out[i]=0;continue;}}
    out[i]=cur[curIdx++];
  }
}

function startAudio(){
  if(ws) return;
  audioCtx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:8000});
  node=audioCtx.createScriptProcessor(1024,0,1);
  node.onaudioprocess=onAudio;
  node.connect(audioCtx.destination);
  ws=new WebSocket('ws://'+location.hostname+':81');
  ws.binaryType='arraybuffer';
  ws.onopen=()=>setStatus('Streaming');
  ws.onclose=()=>{setStatus('Stopped'); ws=null;};
  ws.onerror=()=>setStatus('Error');
  ws.onmessage=(evt)=>{
    const i16=new Int16Array(evt.data);
    const f32=new Float32Array(i16.length);
    for(let i=0;i<i16.length;i++){f32[i]=i16[i]/32768;}
    queue.push(f32);
  };
}

function stopAudio(){
  if(ws){ws.close(); ws=null;}
  if(node){node.disconnect(); node=null;}
  if(audioCtx){audioCtx.close(); audioCtx=null;}
  queue=[]; cur=null; curIdx=0; setStatus('Stopped');
}

// Tabs
document.querySelectorAll('.tab-link').forEach(a=>{
  a.addEventListener('click', e=>{
    e.preventDefault();
    document.querySelectorAll('.tab-link').forEach(x=>x.classList.remove('active'));
    a.classList.add('active');
    const tab=document.querySelectorAll('.tab-section');
    tab.forEach(x=>x.classList.remove('active'));
    const sel=document.getElementById(a.dataset.tab);
    if(sel) sel.classList.add('active');
  });
});

// Audio buttons
document.getElementById('audioStart')?.addEventListener('click', startAudio);
document.getElementById('audioStop')?.addEventListener('click', stopAudio);

// ------------------------------------------
// UI Controls
// ------------------------------------------
function syncFt8FromUi() {
    const bandInput = document.querySelector('input[name="bandval"]');
    if (bandInput && ft8Freq) {
        ft8Freq.selectedIndex = parseInt(bandInput.value) || 0;
    }

    const autoOffsetInput = document.querySelector('input[name="ft8_offset_enabled"]');
    if (autoOffsetInput && ft8AutoOffset) {
        ft8AutoOffset.checked = autoOffsetInput.value == '1';
    }

    // only sync the offset value if auto offset is enabled, 
    // to avoid overwriting the user input while they are editing it
    const offsetInput = document.querySelector('input[name="ft8_offset"]');
    if (offsetInput && ft8Offset && ft8AutoOffset.checked) {
        ft8Offset.value = offsetInput.value || 0;
    }

    const testMsgInput = document.querySelector('input[name="ft8_testmsg"]');
    if (testMsgInput && ft8TestMsg) {
        ft8TestMsg.value = testMsgInput.value || '';
    }
}

// Fetch UI
async function fetchUi(){
  try{
    const resp = await fetch('/api/ui');
    const data = await resp.json();
    const container = document.getElementById('uiControls');
    if(!container) return;
    container.innerHTML='';
    for(const key in data){
      const div = document.createElement('div');
      div.className='row';
      const label = document.createElement('label');
      label.textContent = key;
      const ctrl = document.createElement('div');
      ctrl.className='ctrl';
      const input = document.createElement('input');
      input.name = key;
      input.value = data[key];
      ctrl.appendChild(input);
      div.appendChild(label);
      div.appendChild(ctrl);
      container.appendChild(div);
    }
  }catch(e){console.error(e);}
}

// FT8 Controls
const ft8Status = document.getElementById('ft8-status');
const ft8Freq = document.getElementById('ft8-frequency');
const ft8Offset = document.getElementById('ft8-offset');
const ft8AutoOffset = document.getElementById("ft8-auto-offset");
const ft8TestMsg = document.getElementById('ft8-testmsg');
const mycall = document.getElementById('mycall');

async function init() {
    await fetchUi(); // Wait for the UI to be built from the API
    syncFt8FromUi(); // Sync FT8 controls with the main UI form values    
}
init();

// periodically refresh UI values from the backend and sync FT8 controls, 
// but only if the Settings tab is not active to avoid overwriting user 
// changes while they are editing.
function isSettingsTabActive() {
    const settingsPanel = document.getElementById('ui'); // <-- your data-tab id
    return settingsPanel?.classList.contains('active');
}

setInterval(async () => {
    if (!isSettingsTabActive()) {
        await fetchUi();   // refresh backend values
        syncFt8FromUi();   // update FT8 controls
    }
}, 5000);


// UI Form
document.getElementById('uiForm')?.addEventListener('submit', async e=>{
  e.preventDefault();
  const formData = new FormData(e.target);
  const obj = Object.fromEntries(formData.entries());
  await fetch('/api/ui/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  alert('Settings applied');
});

// FT8 Controls
document.getElementById('ft8-start')?.addEventListener('click', () => {
    fetch('/api/ft8/start', { method: 'POST',headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ freq: parseInt(ft8Freq.value)+parseInt(ft8Offset.value), msg: ft8TestMsg.value })
     })
     .then(r => r.json())
     .then(data => ft8Status.innerText = 'Status: ' + data.message);
});

document.getElementById('ft8-stop')?.addEventListener('click', () => {
    fetch('/api/ft8/stop', { method: 'POST',headers:{'Content-Type':'application/json'} })
        .then(r => r.json())
        .then(data => ft8Status.innerText = 'Status: ' + data.message);
});

document.getElementById('ft8-send-test')?.addEventListener('click', () => {
    fetch('/api/ft8/send', {
        method: 'POST',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ freq: parseInt(ft8Freq.value)+parseInt(ft8Offset.value), msg: ft8TestMsg.value })
    })
    .then(r => r.json())
    .then(data => ft8Status.innerText = 'Status: ' + data.message);
});


// generic function to sync a control with the master input and save
async function syncAndSave(controlEl, targetName) {
  const masterInput = document.querySelector(`input[name="${targetName}"]`);
  if (!masterInput) return;

  // Update the master input value
  // If it's a select, we use selectedIndex; otherwise, we use the value.
  // masterInput.value = (controlEl.tagName === 'SELECT') ? controlEl.selectedIndex : controlEl.value;
  switch(controlEl.tagName){
    case 'SELECT':
        masterInput.value = controlEl.selectedIndex;
        break;
    case 'INPUT':
        if(controlEl.type === 'checkbox'){
            masterInput.value = controlEl.checked ? '1' : '0';
        } else {
            masterInput.value = controlEl.value;
        }
        break;
    default:
        masterInput.value = controlEl.value;
  } 
    
  // Collect all data from the main form and save
  const uiForm = document.getElementById('uiForm');
  const formData = new FormData(uiForm);
  const data = Object.fromEntries(formData.entries());

  try {
    await fetch('/api/ui/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    console.log(`Saved ${targetName}: ${masterInput.value}`);
  } catch (e) {
    console.error("Auto-save failed:", e);
  }
}

// For the Frequency dropdown
ft8Freq?.addEventListener('change', (e) => {
    // Change also the tuned frequency in the main UI form
    const frequencyInput = document.querySelector('input[name="vfoA"]');
    if (frequencyInput) {
        frequencyInput.value = ft8Freq.value;
    }
    syncAndSave(e.target, 'bandval');
    // also sync the main VFO frequency
    // syncAndSave(document.querySelector(`input[name="vfoA"]`),'bandval')
    
});

// For the FT8 Offset input
ft8Offset?.addEventListener('change', (e) => {
    syncAndSave(e.target, 'ft8_offset');
});

// For the FT8 Auto Offset check box
ft8AutoOffset?.addEventListener('change', (e) => {
    syncAndSave(e.target, 'ft8_offset_enabled');
    ft8Offset.disabled = e.target.checked; // disable offset input if auto offset is enabled
});

// For the FT8 Test Message input
ft8TestMsg?.addEventListener('change', (e) => {
    syncAndSave(e.target, 'ft8_testmsg');
});

// FT8 QSOs
const qsoDiv = document.getElementById('ft8-qso');
let currentQso = null;

// convert epoch seconds to DD/MM/YY HH:MM:SS
function formatEpoch(epochSeconds) {
    const d = new Date(epochSeconds * 1000); // JS wants ms

    const pad = n => n.toString().padStart(2, '0');

    const day  = pad(d.getDate());
    const mon  = pad(d.getMonth() + 1);
    const year = pad(d.getFullYear() % 100); // 2 digits
    const h    = pad(d.getHours());
    const m    = pad(d.getMinutes());
    const s    = pad(d.getSeconds());

    return `${day}/${mon}/${year} ${h}:${m}:${s}`;
}

function getFt8Slot(epochSeconds) {
    const d = new Date(epochSeconds * 1000);
    const sec = d.getSeconds();
    return (sec < 30) ? 'EVEN' : 'ODD';
}

function formatTime(ts) {
    const d = new Date(ts*1000); // JS wants ms

    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    const ss = String(d.getSeconds()).padStart(2, '0');

    return `${hh}:${mm}:${ss}`;
}

function formatDate(ts) {
    const d = new Date(ts*1000); // JS wants ms

    const dd = String(d.getDate()).padStart(2, '0');
    const mm = String(d.getMonth() + 1).padStart(2, '0'); // month is 0–11
    const yy = String(d.getFullYear()).slice(-2);

    return `${dd}/${mm}/${yy}`;
}

// function to add a QSO row
const qsosTableBody = document.querySelector('#ft8-qsos tbody');

qsosTableBody.addEventListener('dblclick', async (event) => {
    const tr = event.target.closest('tr');
    if (!tr) return;

    const cells = tr.querySelectorAll('td');

    const qso = tr.qso;
    // Adjust indexes based on your table structure!
    const isMine = qso.isMine;
    const qsoId = qso.qso_id;
    
    // const iscq = (qso.state === 'CQ');
    // if(!iscq){ 
    //     alert('You can only answer/clear a CQ spot!')
    //     return
    // }    
    
    const url = isMine ? '/api/ft8/clear' : '/api/ft8/answer';

    try {
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ freq: parseInt(ft8Freq.value)+parseInt(ft8Offset.value), qso_id: qsoId })
        });

        if (!response.ok) {
            console.error('Request failed:', response.status);
        }

    } catch (err) {
        console.error('Error:', err);
    }
});

function showQsoLog(qsoId) {
    const q = allQSOs.find(x => x.qso_id == qsoId);
    if (!q || !q.log) return;

    const panel = document.getElementById('qso-log-panel');
    const logEl = document.getElementById('qso-log');

    panel.style.display = 'block';

    let out = '';

    for (const e of q.log) {

        const d = new Date(e.ts * 1000);

        const yy = String(d.getFullYear()).slice(-2);
        const mm = String(d.getMonth()+1).padStart(2,'0');
        const dd = String(d.getDate()).padStart(2,'0');
        const hh = String(d.getHours()).padStart(2,'0');
        const mi = String(d.getMinutes()).padStart(2,'0');
        const ss = String(d.getSeconds()).padStart(2,'0');

        const time = `${yy}${mm}${dd} ${hh}:${mi}:${ss}`;

        const rtx = e.rtx == 84 ? 'TX' : (e.rtx == 82 ? 'RX' : '??');

        out += `${time} ${rtx.padEnd(3)} ${e.state.padEnd(18)} ${e.msg}\n`;
    }

    logEl.textContent = out;
    logEl.scrollTop = logEl.scrollHeight;
}

function getLastTx(qso) {
  if (!qso.log) return null;

  let best = null;

  for (const l of qso.log) {
    if (!l.rtx) continue;                 // only TX
    if (l.msg.startsWith("CQ ")) continue; // ignore CQ

    if (!best || l.ts > best.ts) {
      best = l;
    }
  }

  if (!best) return null;

  return {
    time: formatTime(best.ts),
    msg: simplifyMsg(best.msg)
  };
}

function getNextTx(qso) {
  if (!qso.tx_queue || qso.tx_queue.length === 0) return null;

  // take earliest (or just first if already ordered)
  let next = qso.tx_queue[0];

  for (const t of qso.tx_queue) {
    if (t.ts < next.ts) next = t;
  }

  return {
    time: formatTime(next.ts * 15), // convert slot number to seconds (15s slots)
    msg: simplifyMsg(next.msg),
    retries: next.retries ?? qso.retryCount ?? 0
  };
}

function formatTime(ts) {
  const d = new Date(ts * 1000);
  return d.toISOString().substr(11, 8);
}

function simplifyMsg(msg) {
  msg = msg.trim();
  const parts = msg.split(" ");
  return parts[parts.length - 1]; // only "-08", "RR73", etc.
}

function renderTxCell(qso) {
  const last = getLastTx(qso);
  const next = getNextTx(qso);

  let html = "";

  if (next) {
    html += `<div style="color:#aa8800">⏳ ${next.time} ${next.msg}</div>`;
  }

  if (last) {
    html += `<div style="color:#008800">✔ ${last.time} ${last.msg}</div>`;
  }

  return html;
}

function isTransmitting(qso) {
  if (!qso.tx_queue) return false;

  return qso.tx_queue.some(j => j.transmitting === true);
}

function hasQueued(qso) {
  return qso.tx_queue && qso.tx_queue.length > 0;
}

function hasRecentTx(qso) {
  if (!qso.log) return false;

  const now = Date.now() / 1000;

  return qso.log.some(l =>
    l.rtx === 84 &&                // TX
    (now - l.ts) < 60              // within last 60 seconds
  );
}


function addQsoRow(qso){
    const tr = document.createElement('tr');

    // compute duration in seconds and format as MM:SS
    const durationSec = qso.lastHeard - qso.firstSeen;
    const minutes = Math.floor(durationSec / 60);
    const seconds = durationSec % 60;
    const durationStr = `${minutes}m ${seconds}s`;
    const stateText = qso.state || '';
    const geo1 = computeGeo(qso.call1, qso.grid1);
    const geo2 = computeGeo(qso.call2, qso.grid2);
    const geogeo = qso.call2 ? geo2 : geo1; // for single-call QSOs, use the available one for flag/dxcc    

    tr.innerHTML = `
      <td>${formatTime(qso.firstSeen)}</td>
      <td>${formatTime(qso.lastHeard)}</td>
      <td>${qso.call1}</td>
      <td>${qso.grid1 || ''}</td>
      <td>${geo1.distance || ''}</td>
      <td>${geo1.country || ''}</td>
      <td>${geo1.flag}</td>
      <td>${qso.snr1 === -128 ? '' : qso.snr1}</td>
      <td>${qso.call2}</td>
      <td>${qso.grid2 || ''}</td>
      <td>${geo2.distance || ''}</td>
      <td>${geo2.country || ''}</td>
      <td>${geo2.flag}</td>
      <td>${qso.snr2 === -128 ? '' : qso.snr2}</td>
      <td>${qso.report1 || ''}</td>
      <td>${qso.report2 || ''}</td>
      <td>${qso.state === 'DONE' ? '✅' : qso.state}</td>
      <td>${renderTxCell(qso)}</td>
      <td>${durationStr}</td>
      <td>${qso.cared || ''}</td>
      <td>${qso.isMine ? '👤' : ''}</td>
      <td>${qso.qso_id}</td>
      <td>${qso.retryCount > 0 ? qso.retryCount : ''}</td>
      
    `;

    tr.qso = qso; // attach the whole qso object for later use

    //Highlight CQs
    switch(qso.state){
        case 'CQ':
            tr.style.backgroundColor = "#fff3a0";
            break;
        case 'CALLING':
            tr.style.backgroundColor = "#d0f0ff";
            break;
        case 'REPORT_RCVD':
            tr.style.backgroundColor = "#d0ffd0";
            break;
        case 'RRR_SENT':
            tr.style.backgroundColor = "#e0d0ff";
            break;
        case 'REPORT_EXCHANGED':
            tr.style.backgroundColor = "#e0d0ff";
            break;
        case 'DONE':
            tr.style.opacity = "0.5";
            break;
    }

    if (qso.isMine) {
        tr.style.fontWeight = "bold";
        tr.style.boxShadow = "inset 8px 0 0 #ffd700";
    }

    tr.style.cursor = "pointer";
 
    // --- TX STATE OVERRIDE (priority order) ---
    if (isTransmitting(qso)) {

        // 🔴 ON AIR
        tr.classList.add("tx-active");

    } else if (hasQueued(qso)) {

        // 🟡 QUEUED
        tr.style.backgroundColor = "#ffe680";

    } else if (hasRecentTx(qso)) {

        // 🟢 JUST SENT
        tr.style.backgroundColor = "#b6fcb6";

    }

    qsosTableBody.appendChild(tr);

    tr.addEventListener('click', () => {
        selectedQsoId = qso.qso_id;
        showQsoLog(qso.qso_id);

        // highlight selected row
        document.querySelectorAll('#ft8-qsos tr').forEach(r => r.classList.remove('selected'));
        tr.classList.add('selected');
    });
}

// Poll server for QSOs every 3 seconds
async function fetchFt8QSOs(){
    try{
        const resp = await fetch('/api/ft8/qsos');
        const data = await resp.json();
        if(!Array.isArray(data)) return;
        allQSOs = data; // keep a master list of QSOs for reference
        document.getElementById('qsos-title').innerText = formatDate(data[0]?.firstSeen) + ' Active / Recent QSOs '+`(${data.length})`  ;

        // <-- CLEAR TABLE BEFORE ADDING NEW QSOs
        qsosTableBody.innerHTML = '';

        function statePriority(s){
            switch(s){
                case 'CQ': return 0;
                case 'CALLING': return 1;
                case 'REPORT_RCVD': return 2;
                case 'REPORT_EXCHANGED': return 3;
                case 'RRR_SENT': return 4;
                case 'DONE': return 5;
                default: return 6;
            }
        }

        data.sort((a, b) => {
            const pa = statePriority(a.state);
            const pb = statePriority(b.state);

            // prima per stato
            if(pa !== pb) return pa - pb;

            // poi per tempo (più recenti sopra)
            return Number(b.lastHeard) - Number(a.lastHeard);
        });
        showAll = document.getElementById('showAllQsos').checked;
        // add rows
        for(const qso of data) {
            
            if(qso.state === 'CQ' || qso.isMine || showAll)
                addQsoRow(qso);
        }
        
        // show log of selected QSO if still present
        if (selectedQsoId !== null) {
           showQsoLog(selectedQsoId);
        }

    }catch(e){console.error(e);}
}
setInterval(fetchFt8QSOs,3000);

// WiFi Form
document.getElementById('wifiForm')?.addEventListener('submit', async e=>{
  e.preventDefault();
  const formData = new FormData(e.target);
  const obj = Object.fromEntries(formData.entries());
  await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  alert('WiFi settings saved. Rebooting...');
});

})();

let allQSOs = [];
let selectedQsoId = null;

(async function(){

// location, flag and nation functions for FT8 QSOs
// === CONFIG: set your locator here ===
const MY_LOCATOR = "JN53qh"; // <-- change to yours

let ctyData = [];
let prefixMap = [];

async function loadCtyDat() {
  const resp = await fetch('/cty_extended.dat');
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

    // Prefixes are after the last colon in each CTY block.
    const lastColon = block.lastIndexOf(':');
    if (lastColon === -1) continue;

    const headerPart = block.substring(0, lastColon);
    const headerFields = headerPart.split(':').map(s => s.trim());
    if (headerFields.length < 2) continue;

    const country = headerFields[0];

    // Extended format: Country:CQ:ITU:Cont:Lon:Lat:Offset:Prefix:DXCC
    // Legacy fallback (if needed): Country:CQ:...
    const dxccStr = headerFields.length >= 9 ? headerFields[8] : headerFields[1];
    const dxcc = parseInt(dxccStr, 10);
    if (isNaN(dxcc)) continue;

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
  "Asiatic Russia": "ru",
  "EU Russia": "ru",
  "AS Russia": "ru",

  // PATCH

  "Lebanon": "lb",
  "Syria": "sy",
  "Palestine": "ps",
  "Yemen": "ye",
  "Jordan": "jo",

  "Hong Kong": "hk",
  "Macau": "mo",

  "Madeira Islands": "pt",
  "Azores": "pt",
  
  "Northern Ireland": "gb",

  "Kaliningrad": "ru",
  "Kaliningrad (European Russia)": "ru",

  "Federal Republic of Germany": "de",
  "Fed. Rep. of Germany": "de",

  "Djibouti": "dj",

  "Asiatic Turkey": "tr",
  "European Turkey": "tr",

"Republic of the Congo": "cg",
"Congo": "cg",
"Congo (Brazzaville)": "cg",

"Benin": "bj",

"Bahrain": "bh",

"Oman": "om",

"Bangladesh": "bd",

"Bosnia-Herzegovina": "ba",

"Slovak Republic": "sk",

"Sicily": "it",          // ISO Italy, DXCC IT9 special case
"Guantanamo Bay": "cu",  // territory mapping

"Namibia": "na",
"Kosovo": "xk",
"Georgia": "ge",

"Macedonia": "mk",
"Kyrgyzstan": "kg",
"Central African Republic": "cf",
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

  // CTY extended coverage additions
"Afghanistan": "af",
"African Italy": "it",
"Agalega & St. Brandon": "mu",
"Aland Islands": "ax",
"Alaska": "us",
"American Samoa": "as",
"Amsterdam & St. Paul Is.": "tf",
"Andaman & Nicobar Is.": "in",
"Angola": "ao",
"Anguilla": "ai",
"Annobon Island": "gq",
"Antarctica": "aq",
"Antigua & Barbuda": "ag",
"Ascension Island": "sh",
"Auckland & Campbell Is.": "nz",
"Austral Islands": "pf",
"Aves Island": "ve",
"Azerbaijan": "az",
"Baker & Howland Islands": "um",
"Banaba Island": "ki",
"Barbados": "bb",
"Bear Island": "sj",
"Belize": "bz",
"Bermuda": "bm",
"Bhutan": "bt",
"Bonaire": "bq",
"Botswana": "bw",
"Bouvet": "bv",
"British Virgin Islands": "vg",
"Brunei Darussalam": "bn",
"Burkina Faso": "bf",
"Burundi": "bi",
"Cambodia": "kh",
"Cameroon": "cm",
"Cape Verde": "cv",
"Central Kiribati": "ki",
"Ceuta & Melilla": "es",
"Chad": "td",
"Chagos Islands": "io",
"Chatham Islands": "nz",
"Chesterfield Islands": "nc",
"Christmas Island": "cx",
"Clipperton Island": "fr",
"Cocos (Keeling) Islands": "cc",
"Cocos Island": "cr",
"Comoros": "km",
"Conway Reef": "fj",
"Costa Rica": "cr",
"Cote d'Ivoire": "ci",
"Crozet Island": "tf",
"Curacao": "cw",
"DPR of Korea": "kp",
"Dem. Rep. of the Congo": "cd",
"Desecheo Island": "pr",
"Dodecanese": "gr",
"Dominica": "dm",
"Ducie Island": "pn",
"East Malaysia": "my",
"Easter Island": "cl",
"Eastern Kiribati": "ki",
"El Salvador": "sv",
"Equatorial Guinea": "gq",
"Eritrea": "er",
"Falkland Islands": "fk",
"Faroe Islands": "fo",
"Fernando de Noronha": "br",
"Franz Josef Land": "ru",
"French Guiana": "gf",
"French Polynesia": "pf",
"Gabon": "ga",
"Galapagos Islands": "ec",
"Gibraltar": "gi",
"Glorioso Islands": "tf",
"Greenland": "gl",
"Grenada": "gd",
"Guatemala": "gt",
"Guernsey": "gg",
"Guinea": "gn",
"Guinea-Bissau": "gw",
"Guyana": "gy",
"Hawaii": "us",
"Heard Island": "hm",
"Honduras": "hn",
"ITU HQ": "ch",
"Isle of Man": "im",
"Jan Mayen": "sj",
"Jersey": "je",
"Johnston Island": "um",
"Juan Fernandez Islands": "cl",
"Juan de Nova, Europa": "tf",
"Kerguelen Islands": "tf",
"Kermadec Islands": "nz",
"Kingman Reef": "um",
"Kure Island": "um",
"Kuwait": "kw",
"Lakshadweep Islands": "in",
"Laos": "la",
"Lesotho": "ls",
"Lord Howe Island": "au",
"Macao": "mo",
"Macquarie Island": "au",
"Madagascar": "mg",
"Maldives": "mv",
"Mali": "ml",
"Malpelo Island": "co",
"Mariana Islands": "mp",
"Market Reef": "fi",
"Marquesas Islands": "pf",
"Marshall Islands": "mh",
"Mauritania": "mr",
"Mauritius": "mu",
"Mayotte": "yt",
"Mellish Reef": "au",
"Micronesia": "fm",
"Midway Island": "um",
"Minami Torishima": "jp",
"Montenegro": "me",
"Montserrat": "ms",
"Mount Athos": "gr",
"Mozambique": "mz",
"Myanmar": "mm",
"Nauru": "nr",
"Navassa Island": "um",
"Nepal": "np",
"New Caledonia": "nc",
"Nicaragua": "ni",
"Niger": "ne",
"Niue": "nu",
"Norfolk Island": "nf",
"North Cook Islands": "ck",
"Ogasawara": "jp",
"Palau": "pw",
"Palmyra & Jarvis Islands": "um",
"Panama": "pa",
"Peter 1 Island": "aq",
"Pitcairn Island": "pn",
"Pr. Edward & Marion Is.": "za",
"Pratas Island": "tw",
"Republic of Korea": "kr",
"Republic of South Sudan": "ss",
"Reunion Island": "re",
"Revillagigedo": "mx",
"Rodriguez Island": "mu",
"Rotuma Island": "fj",
"Rwanda": "rw",
"Saba & St. Eustatius": "bq",
"Sable Island": "ca",
"San Andres & Providencia": "co",
"San Felix & San Ambrosio": "cl",
"Sao Tome & Principe": "st",
"Scarborough Reef": "ph",
"Senegal": "sn",
"Seychelles": "sc",
"Shetland and Fair Isle": "gb",
"Sierra Leone": "sl",
"Sint Maarten": "sx",
"Somalia": "so",
"South Cook Islands": "ck",
"South Georgia Island": "gs",
"South Orkney Islands": "aq",
"South Sandwich Islands": "gs",
"South Shetland Islands": "aq",
"Sov Mil Order of Malta": "mt",
"Spratly Islands": "ph",
"St. Barthelemy": "bl",
"St. Helena": "sh",
"St. Kitts & Nevis": "kn",
"St. Lucia": "lc",
"St. Martin": "mf",
"St. Paul Island": "ca",
"St. Peter & St. Paul": "br",
"St. Pierre & Miquelon": "pm",
"St. Vincent": "vc",
"Suriname": "sr",
"Svalbard": "sj",
"Swains Island": "as",
"Swaziland": "sz",
"Tajikistan": "tj",
"Temotu Province": "sb",
"The Gambia": "gm",
"Timor - Leste": "tl",
"Togo": "tg",
"Tokelau Islands": "tk",
"Tonga": "to",
"Trindade & Martim Vaz": "br",
"Trinidad & Tobago": "tt",
"Tristan da Cunha & Gough": "sh",
"Tromelin Island": "tf",
"Turkmenistan": "tm",
"Turks & Caicos Islands": "tc",
"Tuvalu": "tv",
"UK Base Areas on Cyprus": "gb",
"US Virgin Islands": "vi",
"United Nations HQ": "us",
"Uzbekistan": "uz",
"Vienna Intl Ctr": "at",
"Wake Island": "um",
"Wallis & Futuna Islands": "wf",
"West Malaysia": "my",
"Western Kiribati": "ki",
"Western Sahara": "eh",
"Willis Island": "au",
"Zambia": "zm",
"Zimbabwe": "zw",

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
        country: '',
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
let queuedSamples=0;
let audioPrimed=false;
let concealSamples=0;
let lastSample=0;
const AUDIO_PRIME_SAMPLES=1024;
const AUDIO_MAX_CONCEAL_SAMPLES=8000;
const statusEl=document.getElementById('audioStatus');

function setStatus(t){
  if(statusEl && statusEl.textContent!==t) statusEl.textContent=t;
}

function onAudio(e){
  const out=e.outputBuffer.getChannelData(0);

  // Keep a small jitter buffer to avoid underruns and audible glitches.
  if(!audioPrimed){
    if(queuedSamples < AUDIO_PRIME_SAMPLES){
      out.fill(0);
      return;
    }
    audioPrimed=true;
    setStatus('Streaming');
  }

  for(let i=0;i<out.length;i++){
    if(!cur || curIdx>=cur.length){
      cur=queue.shift();
      curIdx=0;
      if(!cur){
        // Conceal short network stalls instead of hard muting.
        if(concealSamples < AUDIO_MAX_CONCEAL_SAMPLES){
          lastSample *= 0.998;
          out[i]=lastSample;
          concealSamples++;
          continue;
        }
        audioPrimed=false;
        concealSamples=0;
        out[i]=0;
        continue;
      }
    }
    out[i]=cur[curIdx++];
    lastSample=out[i];
    concealSamples=0;
    queuedSamples--;
  }
}

function startAudio(){
  if(ws) return;
  queue=[];
  cur=null;
  curIdx=0;
  queuedSamples=0;
  audioPrimed=false;
  concealSamples=0;
  lastSample=0;
  audioCtx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:8000});
  audioCtx.resume();
  node=audioCtx.createScriptProcessor(512,0,1);
  node.onaudioprocess=onAudio;
  node.connect(audioCtx.destination);
  ws=new WebSocket('ws://ft8-esp32.local:8765');
  ws.binaryType='arraybuffer';
  ws.onopen=()=>setStatus('Streaming (buffering)');
  ws.onclose=()=>{setStatus('Stopped'); ws=null;};
  ws.onerror=()=>setStatus('Error');
  ws.onmessage=(evt)=>{
    if(!(evt.data instanceof ArrayBuffer)) return;
    const dv = new DataView(evt.data);
    if(dv.byteLength < 16) return;

    const announcedSamples = dv.getUint32(8, false); // big-endian header field
    const maxSamples = Math.floor((dv.byteLength - 16) / 2);
    const samples = Math.min(announcedSamples, maxSamples);
    if(samples <= 0) return;

    const f32=new Float32Array(samples);
    for(let i=0;i<samples;i++){
      // ESP32 sends int16 PCM in native little-endian.
      f32[i]=dv.getInt16(16 + i*2, true)/32768;
    }

    queue.push(f32);
    queuedSamples += samples;

    // Limit backlog growth if browser tab stalls.
    while(queue.length > 24){
      const dropped = queue.shift();
      if(dropped){
        queuedSamples -= dropped.length;
      }
    }
  };
}

function stopAudio(){
  if(ws){ws.close(); ws=null;}
  if(node){node.disconnect(); node=null;}
  if(audioCtx){audioCtx.close(); audioCtx=null;}
  queue=[];
  cur=null;
  curIdx=0;
  queuedSamples=0;
  audioPrimed=false;
  concealSamples=0;
  lastSample=0;
  setStatus('Stopped');
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

    const modeInput = document.querySelector('input[name="ft8_mode"]');
    if (modeInput && ft8Mode)
        ft8Mode.selectedIndex = parseInt(modeInput.value) || 0;

    const retriesInput = document.querySelector('input[name="ft8_max_retries"]');
    if (retriesInput && ft8MaxRetries)
        ft8MaxRetries.selectedIndex = parseInt(retriesInput.value) || 0;

    const parityInput = document.querySelector('input[name="ft8_parity"]');
    if (parityInput && ft8SendParity)
        ft8SendParity.selectedIndex = parseInt(parityInput.value) || 0;
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
const ft8Mode = document.getElementById('ft8-mode');
const ft8MaxRetries = document.getElementById('ft8-max-retries');
const ft8SendParity = document.getElementById('ft8-parity');

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
  // Always poll UI, even during audio streaming.
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

ft8Mode?.addEventListener('change', e => {
    syncAndSave(e.target, 'ft8_mode');
});

ft8MaxRetries?.addEventListener('change', e => {
    syncAndSave(e.target, 'ft8_max_retries');
});

ft8SendParity?.addEventListener('change', e => {
    syncAndSave(e.target, 'ft8_send_parity');
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
    // double click on a QSO row to answer a CQ, clear thr QSO or call the call2
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
    
    let url = ''; // isMine ? '/api/ft8/clear' : '/api/ft8/answer';
    if(isMine)  url = '/api/ft8/clear';
    else if(qso.state === 'CQ') 
      url = '/api/ft8/answer';
    else  // we want to call the call2 of the current QSO, if it's not a CQ
      url = '/api/ft8/call';

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

function formatTime(ts) {
  const d = new Date(ts * 1000);
  return d.toISOString().substr(11, 8);
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function getQsoMessageHistory(qso) {
  const history = [];

  if (Array.isArray(qso.log)) {
    for (const entry of qso.log) {
      history.push({
        kind: entry.rtx === 84 ? 'tx' : (entry.rtx === 82 ? 'rx' : 'other'),
        time: formatTime(entry.ts),
        msg: entry.msg || ''
      });
    }
  }

  if (Array.isArray(qso.tx_queue)) {
    const queued = [...qso.tx_queue].sort((left, right) => left.ts - right.ts);
    for (const entry of queued) {
      history.push({
        kind: entry.transmitting === true ? 'onair' : 'queued',
        time: formatTime(entry.ts * 15),
        msg: entry.msg || ''
      });
    }
  }

  return history;
}

function renderTxCell(qso) {
  const history = getQsoMessageHistory(qso);
  if (history.length === 0) return '';

  return history.map(entry => {
    let color = '#666';
    let prefix = '•';

    if (entry.kind === 'queued') {
      color = '#aa8800';
      prefix = '⏳';
    } else if (entry.kind === 'onair') {
      color = '#cc0000';
      prefix = '●';
    } else if (entry.kind === 'tx') {
      color = '#008800';
      prefix = 'TX';
    } else if (entry.kind === 'rx') {
      color = '#0055aa';
      prefix = 'RX';
    }

    return `<div style="color:${color}; white-space:nowrap; font-size:11px; line-height:1.25">${prefix} ${escapeHtml(entry.time)} ${escapeHtml(entry.msg)}</div>`;
  }).join('');
}

function formatQsoState(state) {
  switch (state) {
    case 'CQ':
      return '📢';
    case 'CALLING':
      return '📞';
    case 'REPORT_RECEIVED':
    case 'REPORT_RCVD':
      return '📥';
    case 'REPORT_EXCHANGED':
      return '🔄';
    case 'QSO_DONE':
    case 'DONE':
      return '✅';
    default:
      return state || '';
  }
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
      <td>${qso.report1 || ''}</td>
      <td>${qso.score1 ? qso.score1.toFixed(1) : ''}</td>      
      <td>${qso.call2}</td>
      <td>${qso.grid2 || ''}</td>
      <td>${geo2.distance || ''}</td>
      <td>${geo2.country || ''}</td>
      <td>${geo2.flag}</td>
      <td>${qso.snr2 === -128 ? '' : qso.snr2}</td>
      <td>${qso.report2 || ''}</td>
      <td>${qso.score2 ? qso.score2.toFixed(1) : ''}</td>      
      <td>${formatQsoState(qso.state)}</td>
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

// create a sorting stateobject to keep track of current sort field and direction
const currentSort = {
  field: null,
  dir: 'asc' // or 'desc'
};

function updateSortIndicators() {
  document.querySelectorAll('#ft8-qsos th.sortable').forEach(th => {
    th.classList.remove('sort-asc', 'sort-desc');

    if (th.dataset.sort === currentSort.field) {
      th.classList.add(
        currentSort.dir === 'asc' ? 'sort-asc' : 'sort-desc'
      );
    }
  });
}
updateSortIndicators();

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


function compareQsos(a, b) {
  let va = a[currentSort.field];
  let vb = b[currentSort.field];

  // normalize undefined / null
  if (va == null) va = '';
  if (vb == null) vb = '';

  // special case: state
  if (currentSort.field === 'state') {
    const diff = statePriority(va) - statePriority(vb);
    return currentSort.dir === 'asc' ? diff : -diff;
  }

  // treat -128 (empty SNR) as null
  if (va === -128) va = null;
  if (vb === -128) vb = null;

  // detect numeric
  const na = Number(va);
  const nb = Number(vb);
  const bothNumeric = !isNaN(na) && !isNaN(nb);

  let result;

  if (bothNumeric) {
    result = na - nb;
  } else {
    // 🔥 STRING COMPARISON (correct way)
    result = String(va).localeCompare(String(vb), undefined, {
      numeric: true,      // "Z2" < "Z10"
      sensitivity: 'base' // case-insensitive
    });
  }

  return currentSort.dir === 'asc' ? result : -result;
}

// click on header to sort by that field, toggle asc/desc if already sorting by it
document.querySelectorAll('#ft8-qsos th').forEach(th => {
  th.addEventListener('click', () => {
    const field = th.dataset.sort;
    if (!field) return;

    if (currentSort.field === field) {
      currentSort.dir = currentSort.dir === 'asc' ? 'desc' : 'asc';
    } else {
      currentSort.field = field;
      currentSort.dir = 'asc';
    }

     // update arrows
    updateSortIndicators();

    autoSort = true;

    fetchFt8QSOs(); // refresh with new sorting
  });
});

// Poll server for QSOs with near real-time cadence while avoiding background-tab load.
let qsosFetchBusy = false;
async function fetchFt8QSOs(){
  // While audio websocket is active, avoid expensive QSO polling/rendering
  // that can starve streaming on both browser and ESP32.
  //if (ws) return;

    if (qsosFetchBusy) return;
    qsosFetchBusy = true;

    try{
        const resp = await fetch('/api/ft8/qsos');
        const data = await resp.json();
        if(!Array.isArray(data)) return;
        allQSOs = data; // keep a master list of QSOs for reference
        document.getElementById('qsos-title').innerText = formatDate(data[0]?.firstSeen) + ' Active / Recent QSOs '+`(${data.length})`  ;

        // <-- CLEAR TABLE BEFORE ADDING NEW QSOs
        qsosTableBody.innerHTML = '';

        // data.sort((a, b) => {
        //     const pa = statePriority(a.state);
        //     const pb = statePriority(b.state);

        //     // prima per stato
        //     if(pa !== pb) return pa - pb;

        //     // poi per tempo (più recenti sopra)
        //     return Number(b.lastHeard) - Number(a.lastHeard);
        // });

        data.sort(compareQsos); // sort based on current sort state
        
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
    finally{qsosFetchBusy = false;}
}
setInterval(() => {
  if (document.hidden) return;
  fetchFt8QSOs();
}, 3000);

// WiFi Form
document.getElementById('wifiForm')?.addEventListener('submit', async e=>{
  e.preventDefault();
  const formData = new FormData(e.target);
  const obj = Object.fromEntries(formData.entries());
  await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  alert('WiFi settings saved. Rebooting...');
});

})();

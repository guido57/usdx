(function(){
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
const ft8TestMsg = document.getElementById('ft8-testmsg');

async function init() {
    await fetchUi(); // Wait for the UI to be built from the API
    
    // Now that the elements exist, set the initial FT8 settings 
    const bandInput = document.querySelector('input[name="bandval"]');
    if (bandInput && ft8Freq) {
        ft8Freq.selectedIndex = parseInt(bandInput.value) || 0;
    }
    
    const offsetInput = document.querySelector('input[name="ft8_offset"]');
    if (offsetInput && ft8Offset) {
        ft8Offset.value = offsetInput.value || 0;   
    }

    const testMsgInput = document.querySelector('input[name="ft8_testmsg"]');
    if (testMsgInput && ft8TestMsg) {
        ft8TestMsg.value = testMsgInput.value || '';   
    }
}
init();

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
    fetch('/api/ft8/start', { method: 'POST',
        body: JSON.stringify({ freq: parseInt(ft8Freq.value)+parseInt(ft8Offset.value), msg: ft8TestMsg.value })
     })
     .then(r => r.json())
     .then(data => ft8Status.innerText = 'Status: ' + data.message);
});

document.getElementById('ft8-stop')?.addEventListener('click', () => {
    fetch('/api/ft8/stop', { method: 'POST' })
        .then(r => r.json())
        .then(data => ft8Status.innerText = 'Status: ' + data.message);
});

document.getElementById('ft8-send-test')?.addEventListener('click', () => {
    fetch('/api/ft8/send', {
        method: 'POST',
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
  masterInput.value = (controlEl.tagName === 'SELECT') ? controlEl.selectedIndex : controlEl.value;

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
    frequencyInput = document.querySelector('input[name="vfoA"]');
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

// For the FT8 Test Message input
ft8TestMsg?.addEventListener('change', (e) => {
    syncAndSave(e.target, 'ft8_testmsg');
});

// FT8 Spots & QSO
const spotsTableBody = document.querySelector('#ft8-spots tbody');
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

// Function to add a new spot to table
function addSpot(spot){
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${formatEpoch(spot.time)}</td>
      <td>${spot.callsign}</td>
      <td>${spot.grid}</td>
      <td>${spot.receiver_callsign}</td>
      <td>${spot.receiver_grid}</td>
      <td>${spot.freq}</td>
      <td>${spot.report}</td>
    `;
     if(spot.cq){
        tr.innerHTML += `<td><button class="answer-btn">Answer CQ</button></td>`;
        tr.querySelector('.answer-btn').addEventListener('click', ()=>{
          currentQso = spot;
          qsoDiv.textContent = `QSO with ${spot.callsign} on ${spot.freq} Hz, report ${spot.report}`;
        });
     }else{
        tr.innerHTML += `<td></td>`;
    } 


    spotsTableBody.prepend(tr);
    if(spotsTableBody.children.length>50) spotsTableBody.removeChild(spotsTableBody.lastChild);
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
function addQsoRow(qso){
    const tr = document.createElement('tr');

    // compute duration in seconds and format as MM:SS
    const durationSec = qso.lastHeard - qso.firstSeen;
    const minutes = Math.floor(durationSec / 60);
    const seconds = durationSec % 60;
    const durationStr = `${minutes}m ${seconds}s`;

    tr.innerHTML = `
      <td>${formatTime(qso.firstSeen)}</td>
      <td>${formatTime(qso.lastHeard)}</td>
      <td>${qso.call1}</td>
      <td>${qso.grid1 || ''}</td>
      <td>${qso.snr1 ?? ''}</td>
      <td>${qso.call2}</td>
      <td>${qso.grid2 || ''}</td>
      <td>${qso.snr2 ?? ''}</td>
      <td>${qso.report1 || ''}</td>
      <td>${qso.report2 || ''}</td>
      <td>${qso.completed ? '✅' : '…'}</td>
      <td>${durationStr}</td>
    `;

    //Highlight CQs
    if(qso.cq){
         tr.style.backgroundColor = "#ffff00"; // yellow
    }

    // Dim completed QSOs
    if(qso.completed) tr.style.opacity = "0.6";

    // Click on row to auto-fill current QSO
    tr.style.cursor = "pointer";
    tr.addEventListener('click', () => {
        currentQso = {
            call: qso.call1,
            freq: ft8Freq.value
        };
        qsoDiv.textContent = `Selected QSO: ${qso.call1} ⇄ ${qso.call2}, last report ${qso.report2}`;
    });

    qsosTableBody.appendChild(tr);
}


// Poll server for spots every 2 seconds
async function fetchFt8Spots(){
    try{
        const resp = await fetch('/api/ft8/spots');
        const data = await resp.json();
        spotsTableBody.replaceChildren(); // clears all rows
        data.forEach(addSpot);
    }catch(e){console.error(e);}
}
setInterval(fetchFt8Spots,2000);

// Poll server for QSOs every 3 seconds
async function fetchFt8QSOs(){
    try{
        const resp = await fetch('/api/ft8/qsos');
        const data = await resp.json();
        if(!Array.isArray(data)) return;
        document.getElementById('activeRecentQSOs').textContent = formatDate(data[0]?.firstSeen) + ' Active / Recent QSOs '+`(${data.length})`  ;

        // <-- CLEAR TABLE BEFORE ADDING NEW QSOs
        qsosTableBody.innerHTML = '';

        // Sort by lastHeard descending (newest first)
        data.sort((a, b) => {
            // ensure numeric comparison
            return Number(b.lastHeard) - Number(a.lastHeard);
        });

        // add rows
        data.forEach(addQsoRow);

    }catch(e){console.error(e);}
}
setInterval(fetchFt8QSOs,3000);


// Answer CQ button
document.getElementById('ft8-answer-cq')?.addEventListener('click', ()=>{
    if(!currentQso) return alert('No spot selected!');
    fetch('/api/ft8/answer', {
        method:'POST',
        body: JSON.stringify({ call: currentQso.call, freq: currentQso.freq })
    }).then(r=>r.json()).then(d=>{
        qsoDiv.textContent = `Answered CQ to ${currentQso.call}: ${d.message}`;
    });
});

// Clear QSO
document.getElementById('ft8-clear-qso')?.addEventListener('click', ()=>{
    currentQso=null;
    qsoDiv.textContent='No active QSO';
});




// WiFi Form
document.getElementById('wifiForm')?.addEventListener('submit', async e=>{
  e.preventDefault();
  const formData = new FormData(e.target);
  const obj = Object.fromEntries(formData.entries());
  await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  alert('WiFi settings saved. Rebooting...');
});

})();

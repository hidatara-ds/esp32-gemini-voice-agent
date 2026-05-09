// static/app.js  — Gabriel AI Frontend
// State machine: idle → connecting → live → listening → thinking → speaking → error
// v3: Fixed connection storm, proper reconnect, threading-mode compatible

// ==================== STATE ====================
const state = {
  status: 'idle',
  socket: null,
  sessionId: null,
  msgSent: 0,
  msgRecv: 0,
  mediaRecorder: null,
  audioChunks: [],
  isRecording: false,
  retryCount: 0,
  maxRetries: 5,
  retryTimer: null,
  processingStart: null,
  latencyHistory: [],
  _connecting: false,

  // VAD (Voice Activity Detection)
  vad: {
    active: false,           // VAD mode enabled
    audioCtx: null,
    analyser: null,
    micStream: null,
    animFrame: null,
    isSpeaking: false,       // currently above threshold
    silenceTimer: null,      // timer to trigger send after silence
    recorder: null,          // MediaRecorder during speech capture
    chunks: [],
    // ── Tunable ────────────────────────────────────────────────
    speechThreshold: 18,     // RMS 0-128; above = speech detected
    silenceMs: 1200,         // ms of silence before sending (1200ms = natural word gap)
    minSpeechMs: 500,        // ignore sounds shorter than this (noise/cough)
    maxRecordMs: 15000,      // auto-send after 15s (prevent infinite recording)
    // ── Runtime ────────────────────────────────────────────────
    speechStart: null,       // timestamp when speech started
    activeSpeechMs: 0,       // cumulative ms above threshold (for adaptive silence)
  },
};

// ==================== DOM REFS ====================
const dom = {
  statusBadge:     document.getElementById('statusBadge'),
  statusDot:       document.getElementById('statusDot'),
  statusText:      document.getElementById('statusText'),
  orb:             document.getElementById('orb'),
  orbInner:        document.getElementById('orbInner'),
  orbLabel:        document.getElementById('orbLabel'),
  btnConnect:      document.getElementById('btnConnect'),
  btnDisconnect:   document.getElementById('btnDisconnect'),
  btnPing:         document.getElementById('btnPing'),
  btnRecord:       document.getElementById('btnRecord'),
  btnVAD:          document.getElementById('btnVAD'),
  audioLevelWrap:  document.getElementById('audioLevelWrap'),
  audioLevelBar:   document.getElementById('audioLevelBar'),
  vadStatus:       document.getElementById('vadStatus'),
  transcriptBody:  document.getElementById('transcriptBody'),
  logConsole:      document.getElementById('logConsole'),
  loadingOverlay:  document.getElementById('loadingOverlay'),
  loadingText:     document.getElementById('loadingText'),
  sessionId:       document.getElementById('sessionId'),
  msgSentEl:       document.getElementById('msgSent'),
  msgRecvEl:       document.getElementById('msgRecv'),
  dashLatency:     document.getElementById('dashLatency'),
  dashVad:         document.getElementById('dashVad'),
  dashMsgCount:    document.getElementById('dashMsgCount'),
  dashUptime:      document.getElementById('dashUptime'),
  audioPlayerWrap: document.getElementById('audioPlayerWrap'),
  audioPlayer:     document.getElementById('audioPlayer'),
};

let uptimeInterval = null;
let connectTime    = null;

// ==================== LOGGING ====================
function log(type, msg) {
  const el = dom.logConsole;
  if (!el) return;
  const time = new Date().toLocaleTimeString();
  const div  = document.createElement('div');
  div.className = 'log-entry';
  div.innerHTML = `<span class="log-time">[${time}]</span><span class="log-tag log-${type}">${type.toUpperCase()}</span><span class="log-msg"> ${escapeHtml(msg)}</span>`;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}
function escapeHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// ==================== UI STATE ====================
function setStatus(s) {
  state.status = s;
  renderStatus(); renderOrb(); renderControls();
}

function renderStatus() {
  const live = ['live','listening','thinking','speaking'].includes(state.status);
  dom.statusBadge.className = 'status-badge ' +
    (live ? 'connected' : state.status === 'connecting' ? 'connecting' : 'disconnected');
  dom.statusText.textContent = {
    idle:'Disconnected', connecting:'Connecting…', live:'Live',
    listening:'Listening…', thinking:'Processing…', speaking:'Speaking', error:'Error'
  }[state.status] || state.status;
}

function renderOrb() {
  const o = {
    idle:'idle', connecting:'idle', live:'listening',
    listening:'listening', thinking:'thinking', speaking:'speaking', error:'error'
  }[state.status] || 'idle';
  dom.orb.className = `orb ${o}`;
  dom.orbLabel.textContent = {
    idle:'IDLE', listening:'LISTENING', thinking:'THINKING', speaking:'SPEAKING', error:'ERROR'
  }[o] || 'IDLE';
}

function renderControls() {
  const live = ['live','listening','thinking','speaking'].includes(state.status);
  const conn = state.status === 'connecting';
  const busy = state.status === 'thinking';
  if (dom.btnConnect)    dom.btnConnect.disabled    = live || conn;
  if (dom.btnDisconnect) dom.btnDisconnect.disabled  = !live && !conn;
  if (dom.btnPing)       dom.btnPing.disabled        = !live;
  if (dom.btnRecord)     dom.btnRecord.disabled      = !live || busy || state.vad.active;
  if (dom.btnVAD)        dom.btnVAD.disabled         = !live;
}

function showLoading(show, text = 'Connecting…') {
  dom.loadingOverlay?.classList.toggle('show', show);
  if (dom.loadingText) dom.loadingText.textContent = text;
}

// ==================== TRANSCRIPT ====================
function addTranscript(role, text) {
  if (!dom.transcriptBody) return;
  dom.transcriptBody.querySelector('.transcript-empty')?.remove();
  const div = document.createElement('div');
  div.className = `msg-bubble msg-${role}`;
  div.textContent = text;
  dom.transcriptBody.appendChild(div);
  dom.transcriptBody.scrollTop = dom.transcriptBody.scrollHeight;
}

function addSystemMsg(text) {
  if (!dom.transcriptBody) return;
  dom.transcriptBody.querySelector('.transcript-empty')?.remove();
  const div = document.createElement('div');
  div.className = 'msg-bubble msg-system';
  div.textContent = text;
  dom.transcriptBody.appendChild(div);
  dom.transcriptBody.scrollTop = dom.transcriptBody.scrollHeight;
}

function clearTranscript() {
  if (dom.transcriptBody)
    dom.transcriptBody.innerHTML = '<div class="transcript-empty">Start a conversation…</div>';
}

// ==================== DASHBOARD ====================
function updateDashboard() {
  if (dom.msgSentEl)   dom.msgSentEl.textContent   = state.msgSent;
  if (dom.msgRecvEl)   dom.msgRecvEl.textContent   = state.msgRecv;
  if (dom.dashMsgCount) dom.dashMsgCount.textContent = state.msgRecv;
}

function updateLatency(ms) {
  state.latencyHistory.push(ms);
  if (state.latencyHistory.length > 10) state.latencyHistory.shift();
  const avg = Math.round(state.latencyHistory.reduce((a,b)=>a+b,0) / state.latencyHistory.length);
  if (dom.dashLatency) {
    dom.dashLatency.textContent = `${avg}ms`;
    dom.dashLatency.className = 'dash-value ' + (avg>800?'error':avg>400?'warn':'good');
  }
}

function startUptime() {
  connectTime = Date.now();
  if (uptimeInterval) clearInterval(uptimeInterval);
  uptimeInterval = setInterval(() => {
    if (!connectTime) return;
    const s = Math.floor((Date.now()-connectTime)/1000);
    if (dom.dashUptime) dom.dashUptime.textContent = `${Math.floor(s/60)}:${String(s%60).padStart(2,'0')}`;
  }, 1000);
}

function stopUptime() {
  if (uptimeInterval) { clearInterval(uptimeInterval); uptimeInterval = null; }
  connectTime = null;
  if (dom.dashUptime) dom.dashUptime.textContent = '—';
}

// ==================== WEBSOCKET ====================
function _destroySocket() {
  if (state.socket) {
    state.socket.removeAllListeners();
    state.socket.disconnect();
    state.socket = null;
  }
  state._connecting = false;
}

function connect() {
  // ── Guard: prevent multiple parallel connections ──────────────────────────
  if (state._connecting) { log('info', 'Already connecting…'); return; }
  if (state.socket && state.socket.connected) { log('info', 'Already connected'); return; }

  // Cleanly destroy any stale socket first
  _destroySocket();

  state._connecting = true;
  log('info', 'Connecting to WebSocket…');
  setStatus('connecting');
  showLoading(true);

  const wsUrl = window.location.origin;

  state.socket = io(wsUrl, {
    transports: ['websocket'],   // WebSocket only — no polling fallback (cleaner)
    timeout: 15000,
    reconnection: false,         // WE handle reconnection manually (prevents storm)
    forceNew: true,
  });

  state.socket.on('connect', () => {
    state._connecting = false;
    log('info', `Socket connected (id: ${state.socket.id})`);
  });

  state.socket.on('disconnect', (reason) => {
    log('info', `Disconnected: ${reason}`);
    state._connecting = false;
    setStatus('idle');
    showLoading(false);
    stopUptime();
    state.sessionId = null;
    if (dom.sessionId) dom.sessionId.textContent = '—';
    if (dom.dashVad) { dom.dashVad.textContent = '—'; dom.dashVad.className = 'dash-value muted'; }
    addSystemMsg(`🔌 Disconnected (${reason})`);

    // Auto-reconnect only for unexpected drops — NOT for manual disconnect
    if (reason !== 'io client disconnect') {
      scheduleReconnect();
    }
  });

  state.socket.on('connect_error', (err) => {
    log('error', `Connection error: ${err.message}`);
    state._connecting = false;
    setStatus('error');
    showLoading(false);
    scheduleReconnect();
  });

  state.socket.on('message', handleServerMessage);
}

function disconnect() {
  if (state.retryTimer) { clearTimeout(state.retryTimer); state.retryTimer = null; }
  state.retryCount = 0;
  _destroySocket();
  setStatus('idle');
  showLoading(false);
  stopUptime();
  addSystemMsg('👋 Disconnected manually');
}

// ── Reconnect with exponential back-off — hard stop after maxRetries ──────
function scheduleReconnect() {
  if (state.retryTimer) return;   // already scheduled
  if (state.retryCount >= state.maxRetries) {
    addSystemMsg(`❌ Max retries (${state.maxRetries}) reached. Click Connect to retry.`);
    setStatus('error');
    return;
  }
  const delay = Math.min(Math.pow(2, state.retryCount) * 1500, 30000);
  state.retryCount++;
  addSystemMsg(`🔄 Reconnecting in ${(delay/1000).toFixed(1)}s… (${state.retryCount}/${state.maxRetries})`);
  log('info', `Retry ${state.retryCount} in ${delay}ms`);
  state.retryTimer = setTimeout(() => {
    state.retryTimer = null;
    connect();
  }, delay);
}

// ==================== MESSAGE HANDLER ====================
function handleServerMessage(data) {
  state.msgRecv++;
  updateDashboard();

  let msg;
  try { msg = typeof data === 'string' ? JSON.parse(data) : data; }
  catch (e) { log('error', `Parse error: ${e.message}`); return; }

  log('recv', JSON.stringify(msg));

  switch (msg.type) {
    case 'connected':
      state.sessionId  = msg.session_id;
      state.retryCount = 0;    // reset retry counter on successful connect
      if (dom.sessionId) dom.sessionId.textContent = msg.session_id.slice(0,8) + '…';
      setStatus('live');
      showLoading(false);
      startUptime();
      addSystemMsg('✅ Connected — Session: ' + msg.session_id.slice(0,8) + '…');
      log('info', `Session: ${msg.session_id}`);
      break;

    case 'pong': {
      const rtt = state.processingStart ? Date.now() - state.processingStart : null;
      state.processingStart = null;
      if (rtt !== null) { updateLatency(rtt); log('recv', `Pong — RTT: ${rtt}ms`); }
      else log('recv', 'Pong');
      break;
    }

    case 'processing':
      setStatus('thinking');
      if (dom.dashVad) { dom.dashVad.textContent = 'Active'; dom.dashVad.className = 'dash-value good'; }
      addSystemMsg('⚙️ AI is processing…');
      break;

    case 'stt_result':
      if (msg.text) addTranscript('user', msg.text);
      setStatus('thinking');
      break;

    case 'result':
      if (msg.question && !dom.transcriptBody.querySelector(`[data-q="${msg.question}"]`)) {
        const b = addTranscriptRaw('user', msg.question);
        if (b) b.dataset.q = msg.question;
      }
      if (msg.answer) addTranscript('ai', msg.answer);
      setStatus('speaking');
      if (msg.audio_url) playAudio(msg.audio_url);
      if (state.processingStart) {
        updateLatency(Date.now() - state.processingStart);
        state.processingStart = null;
      }
      setTimeout(() => { if (state.status === 'speaking') setStatus('live'); }, 4000);
      break;

    case 'error':
      log('error', msg.message || 'Unknown error');
      addSystemMsg(`❌ ${msg.message || 'Error'}`);
      if (['thinking','speaking'].includes(state.status)) setStatus('live');
      break;

    default:
      log('info', `Unknown: ${msg.type}`);
  }
}

// Helper that returns the created bubble element
function addTranscriptRaw(role, text) {
  if (!dom.transcriptBody) return null;
  dom.transcriptBody.querySelector('.transcript-empty')?.remove();
  const div = document.createElement('div');
  div.className = `msg-bubble msg-${role}`;
  div.textContent = text;
  dom.transcriptBody.appendChild(div);
  dom.transcriptBody.scrollTop = dom.transcriptBody.scrollHeight;
  return div;
}

// ==================== PING ====================
function sendPing() {
  if (!state.socket?.connected) { log('error', 'Not connected'); return; }
  state.processingStart = Date.now();
  state.socket.emit('message', JSON.stringify({ type: 'ping' }));
  state.msgSent++;
  updateDashboard();
  log('send', 'Ping →');
}

// ==================== AUDIO RECORDING ====================
async function toggleRecord() {
  state.isRecording ? stopRecording() : await startRecording();
}

async function startRecording() {
  if (!state.socket?.connected) { log('error', 'Not connected'); return; }

  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { sampleRate: 16000, channelCount: 1, echoCancellation: true, noiseSuppression: true }
    });

    // Choose best supported mimeType
    const mimeType = ['audio/webm;codecs=opus','audio/webm','audio/ogg'].find(t => MediaRecorder.isTypeSupported(t)) || '';
    state.audioChunks  = [];
    state.mediaRecorder = new MediaRecorder(stream, mimeType ? { mimeType } : {});

    state.mediaRecorder.ondataavailable = (e) => { if (e.data.size > 0) state.audioChunks.push(e.data); };

    state.mediaRecorder.onstop = async () => {
      stream.getTracks().forEach(t => t.stop());
      if (!state.socket?.connected) { log('error', 'Disconnected before send'); return; }
      const blob = new Blob(state.audioChunks, { type: state.mediaRecorder.mimeType || 'audio/webm' });
      await sendAudioBlob(blob);
    };

    state.mediaRecorder.start(250);   // 250ms chunks for smoother buffer
    state.isRecording = true;
    setStatus('listening');
    if (dom.dashVad) { dom.dashVad.textContent = 'Mic Active'; dom.dashVad.className = 'dash-value good'; }
    if (dom.btnRecord) { dom.btnRecord.textContent = '⏹ Stop'; dom.btnRecord.classList.add('recording'); }
    log('info', 'Recording started…');
    addSystemMsg('🎙️ Recording…');
  } catch (err) {
    log('error', `Mic: ${err.message}`);
    addSystemMsg(`❌ Mic denied: ${err.message}`);
    setStatus('live');
  }
}

function stopRecording() {
  state.mediaRecorder?.stop();
  state.isRecording = false;
  if (dom.btnRecord) { dom.btnRecord.textContent = '🎙 Record & Send'; dom.btnRecord.classList.remove('recording'); }
  if (dom.dashVad) { dom.dashVad.textContent = 'Sending…'; dom.dashVad.className = 'dash-value warn'; }
  log('info', 'Recording stopped, sending…');
}

async function sendAudioBlob(blob) {
  const arrayBuffer = await blob.arrayBuffer();
  const uint8       = new Uint8Array(arrayBuffer);
  const chunkSize   = 16384;   // 16KB chunks

  log('info', `Sending ${uint8.length} bytes…`);
  for (let i = 0; i < uint8.length; i += chunkSize) {
    state.socket.emit('message', uint8.slice(i, i + chunkSize));
    state.msgSent++;
    // Yield to event loop every 4 chunks to keep UI responsive
    if ((i / chunkSize) % 4 === 0) await new Promise(r => setTimeout(r, 0));
  }
  state.socket.emit('message', JSON.stringify({ type: 'audio_end' }));
  state.msgSent++;
  state.processingStart = Date.now();
  setStatus('thinking');
  updateDashboard();
  log('send', `Audio sent (${uint8.length} bytes) + audio_end`);
}

// ==================== AUDIO PLAYBACK ====================
function playAudio(url) {
  if (!dom.audioPlayer || !dom.audioPlayerWrap) return;
  dom.audioPlayer.src = url;
  dom.audioPlayerWrap.classList.add('show');
  dom.audioPlayer.play().catch(e => log('error', `Play failed: ${e.message}`));
  dom.audioPlayer.onended = () => {
    if (state.status === 'speaking') setStatus('live');
    // Resume VAD after audio finishes playing
    if (state.vad.active) _vadResume();
  };
}

// ==================== VAD ENGINE ====================
// Pure Web Audio API — no external libraries needed
// Flow: Start → analyser loop → speech detected → start MediaRecorder
//       → silence detected → stop recorder → send → await response → resume

async function toggleVAD() {
  if (state.vad.active) {
    stopVAD();
  } else {
    await startVAD();
  }
}

async function startVAD() {
  if (!state.socket?.connected) { log('error', 'Not connected'); return; }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { sampleRate: 16000, channelCount: 1, echoCancellation: true, noiseSuppression: true }
    });

    const audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
    const source   = audioCtx.createMediaStreamSource(stream);
    const analyser = audioCtx.createAnalyser();
    analyser.fftSize = 512;
    analyser.smoothingTimeConstant = 0.3;
    source.connect(analyser);

    state.vad.audioCtx  = audioCtx;
    state.vad.analyser  = analyser;
    state.vad.micStream = stream;
    state.vad.active    = true;
    state.vad.isSpeaking = false;
    state.vad.chunks    = [];

    if (dom.btnVAD) { dom.btnVAD.textContent = '🔴 Listening…'; dom.btnVAD.classList.add('active'); }
    if (dom.audioLevelWrap) dom.audioLevelWrap.style.display = 'block';
    if (dom.dashVad) { dom.dashVad.textContent = 'VAD Active'; dom.dashVad.className = 'dash-value good'; }
    log('info', 'VAD started — always listening mode');
    addSystemMsg('🎧 Always Listen activated. Start speaking!');

    _vadLoop();
  } catch (err) {
    log('error', `VAD mic: ${err.message}`);
    addSystemMsg(`❌ Mic access denied: ${err.message}`);
  }
}

function stopVAD() {
  const v = state.vad;
  v.active = false;

  if (v.animFrame)    { cancelAnimationFrame(v.animFrame); v.animFrame = null; }
  if (v.silenceTimer) { clearTimeout(v.silenceTimer); v.silenceTimer = null; }
  if (v.recorder && v.recorder.state !== 'inactive') v.recorder.stop();
  if (v.micStream)    { v.micStream.getTracks().forEach(t => t.stop()); v.micStream = null; }
  if (v.audioCtx)     { v.audioCtx.close(); v.audioCtx = null; }

  v.analyser   = null;
  v.isSpeaking = false;
  v.recorder   = null;
  v.chunks     = [];

  if (dom.btnVAD) { dom.btnVAD.textContent = '🎧 Always Listen'; dom.btnVAD.classList.remove('active','speaking-vad'); }
  if (dom.audioLevelWrap) dom.audioLevelWrap.style.display = 'none';
  if (dom.dashVad) { dom.dashVad.textContent = '—'; dom.dashVad.className = 'dash-value muted'; }
  if (dom.vadStatus) dom.vadStatus.textContent = 'VAD stopped';
  renderControls();
  log('info', 'VAD stopped');
  addSystemMsg('🔇 Always Listen deactivated');
}

function _vadLoop() {
  const v = state.vad;
  if (!v.active || !v.analyser) return;

  const buf = new Uint8Array(v.analyser.fftSize);
  v.analyser.getByteTimeDomainData(buf);

  // Compute RMS (0-128 scale)
  let sum = 0;
  for (let i = 0; i < buf.length; i++) {
    const s = (buf[i] - 128) / 128;
    sum += s * s;
  }
  const rms    = Math.sqrt(sum / buf.length) * 128;
  const pct    = Math.min(100, (rms / v.speechThreshold) * 60);
  const isSpeech = rms > v.speechThreshold;

  // Update level bar
  if (dom.audioLevelBar) dom.audioLevelBar.style.width = pct.toFixed(1) + '%';

  // Don't pick up new speech while server is thinking
  const busy = state.status === 'thinking';

  if (isSpeech && !v.isSpeaking && !busy) {
    // ── Speech START ───────────────────────────────────────────
    v.isSpeaking    = true;
    v.speechStart   = Date.now();
    v.activeSpeechMs = 0;
    if (v.silenceTimer) { clearTimeout(v.silenceTimer); v.silenceTimer = null; }
    _vadStartRecording();
    if (dom.vadStatus) dom.vadStatus.textContent = '🎙️ Listening…';
    if (dom.btnVAD)    dom.btnVAD.classList.add('speaking-vad');

  } else if (isSpeech && v.isSpeaking) {
    // ── Still speaking: accumulate active speech time ──────────
    v.activeSpeechMs += 16;  // ~16ms per frame @ 60fps
    // Cancel pending silence timer when speech resumes
    if (v.silenceTimer) { clearTimeout(v.silenceTimer); v.silenceTimer = null; }

    // ── Max duration guard (auto-send after 15s) ───────────────
    const elapsed = Date.now() - (v.speechStart || 0);
    if (elapsed >= v.maxRecordMs) {
      if (dom.vadStatus) dom.vadStatus.textContent = '⏱️ Max duration — sending…';
      if (dom.btnVAD) dom.btnVAD.classList.remove('speaking-vad');
      v.isSpeaking = false;
      _vadStopAndSend();
    }

  } else if (!isSpeech && v.isSpeaking) {
    // ── Silence after speech: start countdown ─────────────────
    // Adaptive: if the user spoke a lot, give more time before cutting
    // Base: 1800ms; bonus: +100ms per second of active speech (max +1200ms)
    const bonusMs = Math.min(1200, Math.floor(v.activeSpeechMs / 1000) * 100);
    const silenceWait = v.silenceMs + bonusMs;

    if (!v.silenceTimer) {
      // Show countdown hint in the UI
      const secStr = (silenceWait / 1000).toFixed(1);
      if (dom.vadStatus) dom.vadStatus.textContent = `⏸️ Pause detected — sending in ${secStr}s…`;

      v.silenceTimer = setTimeout(() => {
        v.silenceTimer = null;
        if (v.isSpeaking) {
          v.isSpeaking = false;
          if (dom.vadStatus) dom.vadStatus.textContent = '⚙️ Processing…';
          if (dom.btnVAD) dom.btnVAD.classList.remove('speaking-vad');
          _vadStopAndSend();
        }
      }, silenceWait);
    }

  } else if (!isSpeech && !v.isSpeaking && !v.silenceTimer) {
    if (dom.vadStatus) dom.vadStatus.textContent = 'Waiting for speech…';
  }

  v.animFrame = requestAnimationFrame(_vadLoop);
}

function _vadStartRecording() {
  const v = state.vad;
  if (!v.micStream) return;

  v.chunks  = [];
  const mimeType = ['audio/webm;codecs=opus','audio/webm','audio/ogg']
    .find(t => MediaRecorder.isTypeSupported(t)) || '';
  v.recorder = new MediaRecorder(v.micStream, mimeType ? { mimeType } : {});
  v.recorder.ondataavailable = (e) => { if (e.data.size > 0) v.chunks.push(e.data); };
  v.recorder.start(100);
}

async function _vadStopAndSend() {
  const v = state.vad;
  if (!v.recorder || v.recorder.state === 'inactive') return;
  if (!state.socket?.connected) return;

  // Wait for final data chunk from recorder
  await new Promise(res => {
    v.recorder.onstop = res;
    v.recorder.stop();
  });

  const speechDuration = Date.now() - (v.speechStart || 0);
  const activeSpeech   = v.activeSpeechMs;
  v.activeSpeechMs     = 0;  // reset for next utterance

  if (speechDuration < v.minSpeechMs || v.chunks.length === 0) {
    log('info', `VAD: too short (${speechDuration}ms active=${activeSpeech}ms), ignored`);
    if (dom.vadStatus) dom.vadStatus.textContent = 'Waiting for speech…';
    return;
  }

  const blob = new Blob(v.chunks, { type: v.recorder.mimeType || 'audio/webm' });
  v.chunks   = [];
  log('info', `VAD: captured ${(speechDuration/1000).toFixed(1)}s / ${blob.size} bytes`);
  await sendAudioBlob(blob);
}

// Resume VAD listening after AI finishes (called from playAudio.onended)
function _vadResume() {
  if (!state.vad.active) return;
  if (dom.vadStatus) dom.vadStatus.textContent = 'Waiting for speech…';
  if (dom.btnVAD) dom.btnVAD.classList.remove('speaking-vad');
  setStatus('live');
  log('info', 'VAD: resumed listening');
}

// ==================== TAB NAVIGATION ====================
function switchTab(tabId) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === tabId));
  document.querySelectorAll('.tab-panel').forEach(p => p.classList.toggle('active', p.id === tabId));
}

// ==================== API TEST ====================
async function testEndpoint(id, url, options = {}) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = 'Loading…'; el.className = 'api-response';
  try {
    const resp = await fetch(url, options);
    const text = await resp.text();
    let pretty; try { pretty = JSON.stringify(JSON.parse(text), null, 2); } catch { pretty = text; }
    el.textContent = `HTTP ${resp.status}\n\n${pretty}`;
    el.className = resp.ok ? 'api-response' : 'api-response error';
  } catch (err) {
    el.textContent = `Error: ${err.message}`; el.className = 'api-response error';
  }
}
async function testHealth()    { await testEndpoint('res-health', '/api/health'); }
async function testIndex()     { await testEndpoint('res-index', '/'); }
async function testDebugMeta() { await testEndpoint('res-debug', '/api/debug/latest-input-meta'); }
async function testTextPrompt() {
  const text = document.getElementById('textPromptInput')?.value || 'Halo, siapa kamu?';
  await testEndpoint('res-text', '/api/test-text', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ text }),
  });
}

// ==================== COPY ====================
function copyCode(id) {
  const el = document.getElementById(id);
  if (!el) return;
  navigator.clipboard.writeText(el.innerText).then(() => {
    const btn = el.parentElement?.querySelector('.copy-btn');
    if (btn) { btn.textContent = 'Copied!'; setTimeout(() => btn.textContent = 'Copy', 1500); }
  });
}

// ==================== INIT ====================
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('.tab-btn').forEach(b => b.addEventListener('click', () => switchTab(b.dataset.tab)));
  document.getElementById('btnConnect')?.addEventListener('click', connect);
  document.getElementById('btnDisconnect')?.addEventListener('click', disconnect);
  document.getElementById('btnPing')?.addEventListener('click', sendPing);
  document.getElementById('btnRecord')?.addEventListener('click', toggleRecord);
  document.getElementById('btnVAD')?.addEventListener('click', toggleVAD);
  document.getElementById('btnClearTranscript')?.addEventListener('click', clearTranscript);
  document.getElementById('btnTestHealth')?.addEventListener('click', testHealth);
  document.getElementById('btnTestIndex')?.addEventListener('click', testIndex);
  document.getElementById('btnTestDebug')?.addEventListener('click', testDebugMeta);
  document.getElementById('btnTestText')?.addEventListener('click', testTextPrompt);
  document.querySelectorAll('.copy-btn').forEach(b => b.addEventListener('click', () => copyCode(b.dataset.target)));

  setStatus('idle');
  log('info', 'Gabriel AI ready. Click Connect to start.');
});

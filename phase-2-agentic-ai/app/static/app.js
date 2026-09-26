async function refreshStatus() {
  try {
    const response = await fetch('/api/system/status', { headers: { Accept: 'application/json' } });
    if (!response.ok) throw new Error('status unavailable');
    const data = await response.json();
    document.querySelector('#service-state').textContent = 'Read-only telemetry online';
    const aiState = document.querySelector('#ai-state');
    if (aiState) aiState.textContent = data.ai.configured ? data.ai.name : 'Not set';
    const healthResponse = await fetch('/api/farm/health', { headers: { Accept: 'application/json' } });
    if (healthResponse.ok) {
      const health = await healthResponse.json();
      document.querySelector('#health-value').innerHTML = `${health.score}<br /><span>${escapeHtml(health.status.toUpperCase())}</span>`;
      document.querySelector('#health-copy').textContent = `${health.finding_count} evidence-linked finding${health.finding_count === 1 ? '' : 's'} · ${health.offline_devices.length} offline device${health.offline_devices.length === 1 ? '' : 's'}.`;
    }
  } catch (error) {
    document.querySelector('#service-state').textContent = 'Shell needs attention';
  }
}

function formatValue(item) {
  if (String(item.value).toLowerCase() === 'true') return 'ON';
  if (String(item.value).toLowerCase() === 'false') return 'OFF';
  const number = Number(item.value);
  if (!Number.isNaN(number)) return number.toFixed(number % 1 === 0 ? 0 : 1);
  return item.value ?? '—';
}

function labelFor(field) {
  return { temperature: 'Air temperature', humidity: 'Humidity', soil_moisture: 'Soil moisture', soil_temperature: 'Soil temperature', light_level: 'Light level', ldr: 'LDR reading', ph: 'Water pH', tds_ppm: 'TDS' }[field] || field.replaceAll('_', ' ');
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }[character]));
}

function renderCropSources(sources) {
  return (sources || []).map((source) => {
    const url = typeof source === 'string' ? source : source.url;
    const title = typeof source === 'string' ? new URL(source).hostname : (source.title || source.url);
    return `<a href="${escapeHtml(url)}" target="_blank" rel="noreferrer">${escapeHtml(title)}</a>`;
  }).join(' · ');
}

async function loadCropCatalog() {
  try {
    const response = await fetch('/api/crops', { headers: { Accept: 'application/json' } });
    if (!response.ok) return;
    const data = await response.json();
    const list = document.querySelector('#crop-options');
    const existing = new Set([...list.options].map((option) => option.value.toLowerCase()));
    (data.crops || []).forEach((crop) => {
      if (!existing.has(String(crop).toLowerCase())) {
        const option = document.createElement('option');
        option.value = String(crop).replaceAll('_', ' ').replace(/\b\w/g, (letter) => letter.toUpperCase());
        list.appendChild(option);
      }
    });
  } catch { /* the picker keeps its starter catalog when the service is unavailable */ }
}

function renderAnswer(value) {
  let html = escapeHtml(String(value ?? ''));
  html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
  html = html.replace(/^\s*[-•]\s+(.+)$/gm, '<span class="answer-bullet">$1</span>');
  return html.replaceAll('\n', '<br />');
}

function readFarmProfile() {
  try { return JSON.parse(localStorage.getItem('farmwiz-farm-profile') || '{}'); } catch { return {}; }
}

function saveFarmProfile(profile) {
  localStorage.setItem('farmwiz-farm-profile', JSON.stringify(profile));
  document.querySelector('#profile-note').textContent = profile.location_name
    ? `${profile.crop} · ${profile.stage} · ${profile.location_name} (${Number(profile.latitude).toFixed(2)}, ${Number(profile.longitude).toFixed(2)})`
    : 'Choose a crop and location so future weather and crop advice can be specific. Nothing is sent to ThingsBoard.';
}

function selectedCropName() {
  return document.querySelector('#profile-crop-search').value.trim();
}

function cropKey(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '');
}

function selectedCrops() {
  const profile = readFarmProfile();
  if (Array.isArray(profile.crops)) return profile.crops.filter(Boolean);
  const legacy = profile.custom_crop || (profile.crop && profile.crop !== 'custom' ? profile.crop : '');
  return legacy ? [legacy] : [];
}

function loadFarmProfile() {
  const profile = readFarmProfile();
  const crops = selectedCrops();
  document.querySelector('#profile-crop-search').value = '';
  if (profile.stage) document.querySelector('#profile-stage').value = profile.stage;
  if (profile.location_name) document.querySelector('#profile-location').value = profile.location_name;
  saveFarmProfile({ ...profile, crops, crop: crops[0] ? cropKey(crops[0]) : '', stage: document.querySelector('#profile-stage').value });
}

function weatherDescription(code) {
  if (code === 0) return 'Clear sky';
  if ([1, 2, 3].includes(code)) return 'Partly cloudy';
  if ([45, 48].includes(code)) return 'Foggy';
  if ([51, 53, 55, 56, 57].includes(code)) return 'Drizzle';
  if ([61, 63, 65, 66, 67].includes(code)) return 'Rain';
  if ([80, 81, 82].includes(code)) return 'Rain showers';
  if ([95, 96, 99].includes(code)) return 'Storm risk';
  return 'Mixed conditions';
}

async function refreshWeather() {
  const profile = readFarmProfile();
  const grid = document.querySelector('#weather-grid');
  if (profile.latitude == null || profile.longitude == null) {
    document.querySelector('#weather-source').textContent = 'LOCATION NEEDED';
    grid.innerHTML = '<div class="sensor-empty">Choose a farm location to load current weather.</div>';
    return;
  }
  try {
    const response = await fetch(`/api/weather?latitude=${encodeURIComponent(profile.latitude)}&longitude=${encodeURIComponent(profile.longitude)}`, { headers: { Accept: 'application/json' } });
    if (!response.ok) throw new Error('weather unavailable');
    const data = await response.json();
    const current = data.current || {}; const daily = data.daily || {};
    document.querySelector('#weather-source').textContent = `${data.source.toUpperCase()} · ${data.timezone || 'LOCAL TIME'}`;
    const days = (daily.time || []).map((date, index) => `<div class="forecast-day"><strong>${new Date(`${date}T12:00:00`).toLocaleDateString([], { weekday: 'short' })}</strong><span>${Number(daily.temperature_2m_max?.[index] ?? 0).toFixed(0)}° / ${Number(daily.temperature_2m_min?.[index] ?? 0).toFixed(0)}°C</span><small>${daily.precipitation_probability_max?.[index] ?? 0}% rain · ${Number(daily.precipitation_sum?.[index] ?? 0).toFixed(1)} mm</small></div>`).join('');
    grid.innerHTML = `<article class="weather-current"><span class="sensor-source">${profile.location_name || 'Selected farm'}</span><strong>${Number(current.temperature_2m ?? 0).toFixed(1)}°C</strong><span>${weatherDescription(Number(current.weather_code))}</span><small>Humidity ${current.relative_humidity_2m ?? '—'}% · Wind ${Number(current.wind_speed_10m ?? 0).toFixed(0)} km/h · Rain now ${current.precipitation ?? 0} mm</small></article><div class="forecast-strip">${days}</div>`;
  } catch {
    document.querySelector('#weather-source').textContent = 'UNAVAILABLE';
    grid.innerHTML = '<div class="sensor-empty">Weather could not be loaded; telemetry remains available.</div>';
  }
}

async function refreshCropProfileLegacy() {
  const crop = cropKey(selectedCropName());
  const guide = document.querySelector('#crop-guide');
  const customName = selectedCropName();
  if (!customName) { document.querySelector('#crop-source').textContent = 'SEARCH OR ENTER'; guide.innerHTML = '<div class="sensor-empty">Search the crop list or type any crop name. FarmWiz will use a sourced guide when available.</div>'; return; }
  if (!['tomato', 'pepper', 'lettuce', 'cucumber'].includes(crop)) {
    document.querySelector('#crop-source').textContent = 'YOUR CROP';
    guide.innerHTML = `<div class="crop-facts"><strong>${escapeHtml(customName)}</strong><span>Farm profile</span></div><div class="sensor-empty">FarmWiz will connect this crop to its knowledge agent and add a sourced guide when available. It will not invent thresholds.</div>`;
    return;
  }
  try {
    const response = await fetch(`/api/crops/${encodeURIComponent(crop)}`, { headers: { Accept: 'application/json' } });
    if (!response.ok) throw new Error('profile unavailable');
    const data = await response.json();
    document.querySelector('#crop-source').textContent = data.status.toUpperCase();
    const ph = data.soil_ph ? `pH ${data.soil_ph.min}–${data.soil_ph.max}` : 'pH not specified';
    const temp = data.growing_temperature ? `${data.growing_temperature.min}–${data.growing_temperature.max}°C` : 'temperature not specified';
    guide.innerHTML = `<div class="crop-facts"><strong>${escapeHtml(data.display_name)}</strong><span>${ph}</span><span>${temp}</span></div><ul>${(data.requirements || []).map((item) => `<li>${escapeHtml(item)}</li>`).join('')}</ul><div class="crop-sources">Sources: ${(data.sources || []).map((source) => `<a href="${escapeHtml(source.url)}" target="_blank" rel="noreferrer">${escapeHtml(source.title)}</a>`).join(' · ')}</div>`;
  } catch {
    document.querySelector('#crop-source').textContent = 'NOT SOURCED YET';
    guide.innerHTML = '<div class="sensor-empty">No sourced profile is available for this crop yet. FarmWiz will not invent thresholds.</div>';
  }
}

async function refreshCropProfile() {
  const guide = document.querySelector('#crop-guide');
  const crops = selectedCrops();
  const selection = document.querySelector('#crop-selection');
  selection.innerHTML = crops.map((name) => `<button type="button" class="crop-chip" data-edit-crop="${escapeHtml(cropKey(name))}">${escapeHtml(name)} · edit</button>`).join('');
  if (!crops.length) { document.querySelector('#crop-source').textContent = 'SEARCH OR ADD'; guide.innerHTML = '<div class="sensor-empty">Search for one or more crops, then choose Add crop.</div>'; return; }
  document.querySelector('#crop-source').textContent = `${crops.length} CROP${crops.length === 1 ? '' : 'S'}`;
  const cards = await Promise.all(crops.map(async (name) => {
    const crop = cropKey(name);
    try {
      const response = await fetch(`/api/crops/${encodeURIComponent(crop)}`, { headers: { Accept: 'application/json' } });
      if (!response.ok) throw new Error('profile unavailable');
      const data = await response.json();
      const ph = data.soil_ph ? `pH ${data.soil_ph.min}-${data.soil_ph.max}` : 'pH not specified';
      const temp = data.growing_temperature ? `${data.growing_temperature.min}-${data.growing_temperature.max}°C` : 'temperature not specified';
      return `<article class="crop-card"><div class="crop-facts"><strong>${escapeHtml(data.display_name || name)}</strong><span>${ph}</span><span>${temp}</span></div><ul>${(data.requirements || []).map((item) => `<li>${escapeHtml(item)}</li>`).join('')}</ul><div class="crop-sources">${escapeHtml(data.status || 'Profile')} · ${renderCropSources(data.sources)}</div><button type="button" class="crop-delete" data-delete-crop="${escapeHtml(crop)}">Delete saved profile</button></article>`;
    } catch { return `<article class="crop-card"><div class="crop-facts"><strong>${escapeHtml(name)}</strong><span>Profile missing</span></div><p class="sensor-empty">No saved guide yet. Ask FarmWiz to research this crop and store a sourced profile.</p><button type="button" class="crop-research" data-research-crop="${escapeHtml(crop)}">Ask FarmWiz to create profile</button></article>`; }
  }));
  guide.innerHTML = cards.join('');
}

async function searchFarmLocation() {
  const query = document.querySelector('#profile-location').value.trim();
  const results = document.querySelector('#profile-results');
  if (query.length < 2) { results.innerHTML = '<span>Enter a city or postal code.</span>'; return; }
  results.innerHTML = '<span>Searching locations…</span>';
  try {
    const response = await fetch(`/api/geocode?query=${encodeURIComponent(query)}`, { headers: { Accept: 'application/json' } });
    const data = await response.json();
    const places = data.results || [];
    results.innerHTML = places.length ? places.map((place, index) => `<button type="button" data-place-index="${index}">${escapeHtml([place.name, place.admin1, place.country].filter(Boolean).join(', '))}</button>`).join('') : '<span>No matching locations found.</span>';
    results.querySelectorAll('[data-place-index]').forEach((button) => button.addEventListener('click', () => {
      const place = places[Number(button.dataset.placeIndex)];
      const crops = selectedCrops();
      const profile = { ...readFarmProfile(), crops, crop: crops[0] ? cropKey(crops[0]) : '', stage: document.querySelector('#profile-stage').value, location_name: [place.name, place.admin1, place.country].filter(Boolean).join(', '), latitude: place.latitude, longitude: place.longitude, timezone: place.timezone };
      document.querySelector('#profile-location').value = profile.location_name;
      results.innerHTML = '';
      saveFarmProfile(profile);
      refreshWeather();
    }));
  } catch { results.innerHTML = '<span>Location search is unavailable right now.</span>'; }
}

let latestVoiceDraft = null;
let speechRecognition = null;
function voiceSummary(draft) { const p = draft.parameters || {}; const s = draft.schedule || {}; return draft.status === 'ready_for_farmer_review' ? `I heard: set the ${p.output || 'unknown'} ${p.value ? 'on' : 'off'}${s.duration_seconds ? ` for ${s.duration_seconds} seconds` : ''}. This is a draft and still needs your approval.` : `I need clarification before this is safe: ${(draft.clarification || []).join(', ')}.`; }
const FEMALE_VOICE_NAMES = /samantha|victoria|zira|hazel|susan|karen|moira|tessa|ava|allison|fiona|serena|kate|olivia|aria|jenny|libby|siri voice 1|siri voice 2/i;
function preferredFarmWizVoice() {
  const voices = window.speechSynthesis?.getVoices?.() || [];
  const english = voices.filter((voice) => /^en([-_]|$)/i.test(voice.lang));
  return english.find((voice) => FEMALE_VOICE_NAMES.test(voice.name)) || voices.find((voice) => FEMALE_VOICE_NAMES.test(voice.name)) || null;
}
function waitForFemaleVoice() {
  const selected = preferredFarmWizVoice();
  if (selected || !('speechSynthesis' in window)) return Promise.resolve(selected);
  return new Promise((resolve) => {
    const finish = () => resolve(preferredFarmWizVoice());
    window.speechSynthesis.addEventListener('voiceschanged', finish, { once: true });
    window.setTimeout(finish, 450);
  });
}
async function speakText(text) {
  if (!('speechSynthesis' in window) || !text) return;
  window.speechSynthesis.cancel();
  const utterance = new SpeechSynthesisUtterance(text);
  const voice = await waitForFemaleVoice();
  if (voice) utterance.voice = voice;
  utterance.lang = 'en-US';
  // iPhone may not expose a named voice until after voiceschanged. If it still
  // cannot, this avoids the low male-sounding default without blocking speech.
  utterance.pitch = voice ? 1.16 : 1.42;
  utterance.rate = 0.96;
  window.speechSynthesis.speak(utterance);
}
if ('speechSynthesis' in window) window.speechSynthesis.getVoices();
async function interpretVoiceCommand() { const transcript = document.querySelector('#voice-transcript').value.trim(); if (!transcript) return; const status = document.querySelector('#voice-status'); status.textContent = 'INTERPRETING'; try { const response = await fetch('/api/edge/parse-intent', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ transcript }) }); latestVoiceDraft = await response.json(); document.querySelector('#voice-preview').textContent = JSON.stringify(latestVoiceDraft, null, 2); const send = document.querySelector('#voice-send'); if (send) send.hidden = !(latestVoiceDraft.status === 'ready_for_farmer_review' && latestVoiceDraft.action === 'set_output' && latestVoiceDraft.parameters?.output === 'light'); status.textContent = latestVoiceDraft.status === 'ready_for_farmer_review' ? 'READY FOR APPROVAL' : 'NEEDS CLARIFICATION'; speakText(voiceSummary(latestVoiceDraft)); } catch { status.textContent = 'ERROR'; } }
async function sendApprovedLightTest() { if (!latestVoiceDraft || latestVoiceDraft.action !== 'set_output' || latestVoiceDraft.parameters?.output !== 'light') return; const button = document.querySelector('#voice-send'); button.disabled = true; document.querySelector('#voice-status').textContent = 'SENDING TEST RPC'; try { const response = await fetch('/api/edge/test-light-rpc', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ enabled: Boolean(latestVoiceDraft.parameters.value), approval: 'farmer-approved' }) }); const result = await response.json(); if (!response.ok) throw new Error(result.detail || 'RPC failed'); document.querySelector('#voice-preview').textContent = JSON.stringify(result, null, 2); document.querySelector('#voice-status').textContent = 'RPC SENT'; } catch (error) { document.querySelector('#voice-status').textContent = `RPC ERROR: ${error.message}`; } finally { button.disabled = false; } }
function startVoiceInput(targetSelector = '#voice-transcript') { const status = document.querySelector('#voice-status') || document.querySelector('#agent-voice-status') || document.querySelector('#telemetry-updated'); const target = document.querySelector(targetSelector); const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition; if (!window.isSecureContext && location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') { status.textContent = 'MICROPHONE NEEDS HTTPS'; return; } if (!SpeechRecognition) { status.textContent = 'VOICE INPUT NOT SUPPORTED'; return; } if (!speechRecognition) { speechRecognition = new SpeechRecognition(); speechRecognition.lang = 'en-US'; speechRecognition.interimResults = true; speechRecognition.onstart = () => { status.textContent = 'LISTENING'; }; speechRecognition.onresult = (event) => { if (target) target.value = Array.from(event.results).map((result) => result[0].transcript).join(''); }; speechRecognition.onerror = (event) => { status.textContent = event.error === 'not-allowed' ? 'MICROPHONE DENIED' : `VOICE ERROR: ${event.error}`; }; speechRecognition.onend = () => { if (status.textContent === 'LISTENING') status.textContent = 'TRANSCRIPT READY'; }; } status.textContent = 'LISTENING'; try { speechRecognition.start(); } catch { status.textContent = 'VOICE INPUT BUSY'; } }

let latestTelemetryData = null;
let selectedDevice = null;
let latestHistoryData = null;

function openReadingChart(device, sourceKey, label) {
  const series = (latestHistoryData?.series || []).find((item) => item.device === device && item.source_key === sourceKey);
  const dialog = document.querySelector('#history-dialog');
  if (!dialog) return;
  document.querySelector('#dialog-title').textContent = `${device} · ${label}`;
  if (!series || !series.samples?.length) {
    document.querySelector('#dialog-chart').innerHTML = '<div class="chart-empty">No historical samples are available for this reading yet. The device may be offline or ThingsBoard may not have stored this key.</div>';
    document.querySelector('#dialog-summary').textContent = 'History unavailable';
    dialog.showModal();
    return;
  }
  const raw = series.samples.map((sample) => String(sample.value).toLowerCase());
  const categorical = raw.some((value) => ['true', 'false', 'on', 'off'].includes(value));
  const values = categorical ? raw.map((value) => ['true', 'on', '1'].includes(value) ? 1 : 0) : raw.map(Number).filter((value) => Number.isFinite(value));
  if (!values.length) return;
  const min = Math.min(...values); const max = Math.max(...values); const range = max - min || 1;
  const points = values.map((value, index) => `${(index / Math.max(values.length - 1, 1)) * 620},${categorical ? (value ? 45 : 165) : 185 - ((value - min) / range) * 155}`).join(' ');
  document.querySelector('#dialog-title').textContent = `${device} · ${label}`;
  document.querySelector('#dialog-chart').innerHTML = `<svg viewBox="0 0 620 210" role="img" aria-label="${escapeHtml(label)} history chart"><line x1="0" y1="185" x2="620" y2="185" /><polyline points="${points}" /></svg>`;
  document.querySelector('#dialog-summary').textContent = categorical ? `${values.length} samples · OFF / ON timeline · latest ${values[values.length - 1] ? 'ON' : 'OFF'}` : `${values.length} samples · range ${min.toFixed(min % 1 === 0 ? 0 : 1)}–${max.toFixed(max % 1 === 0 ? 0 : 1)} · latest ${values[values.length - 1].toFixed(values[values.length - 1] % 1 === 0 ? 0 : 1)}`;
  dialog.showModal();
}

function showDeviceDetail(deviceName) {
  if (!latestTelemetryData) return;
  selectedDevice = deviceName;
  const device = (latestTelemetryData.devices || []).find((item) => item.device === deviceName);
  const readings = (latestTelemetryData.readings || []).filter((item) => item.device === deviceName);
  const grid = document.querySelector('#sensor-grid');
  if (!grid) return;
  grid.innerHTML = readings.length ? readings.map((item) => { const output = ['light', 'pump', 'water_valve', 'air_pump'].includes(item.field); const on = String(item.value).toLowerCase() === 'true'; return `<article class="sensor-card device-${device?.state || 'offline'} ${output ? `output-reading ${on ? 'output-on' : 'output-off'}` : ''}" data-history-device="${deviceName}" data-history-key="${item.source_key}" data-history-label="${labelFor(item.field)}" tabindex="0" role="button"><div class="sensor-top"><span class="sensor-label">${labelFor(item.field)}</span><span class="sensor-source">${deviceName} · ${device?.state || 'offline'} · VIEW TREND</span></div><div class="sensor-value">${formatValue(item)}<small>${item.field === 'temperature' || item.field === 'soil_temperature' ? '°C' : item.field === 'humidity' || item.field === 'soil_moisture' ? '%' : item.field === 'ph' ? ' pH' : item.field === 'tds_ppm' ? ' ppm' : item.field === 'light_level' || item.field === 'ldr' ? ' ADC' : ''}</small></div><div class="sensor-key">source / ${item.source_key}</div></article>`; }).join('') : '<div class="sensor-empty">No recent readings for this device.</div>';
  grid.querySelectorAll('[data-history-device]').forEach((card) => { const open = () => openReadingChart(card.dataset.historyDevice, card.dataset.historyKey, card.dataset.historyLabel); card.addEventListener('click', open); card.addEventListener('keydown', (event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); open(); } }); });
  document.querySelectorAll('.device-card').forEach((card) => card.classList.toggle('selected', card.dataset.device === deviceName));
  document.querySelector('#telemetry-updated').textContent = `${deviceName.toUpperCase()} · UPDATED ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
}

async function refreshTelemetry() {
  const response = await fetch('/api/farm/telemetry/latest', { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error('telemetry unavailable');
  const data = await response.json();
  latestTelemetryData = data;
  const grid = document.querySelector('#sensor-grid');
  const readings = data.readings || [];
  const deviceStates = Object.fromEntries((data.devices || []).map((device) => [device.device, device.state]));
  document.querySelector('#reading-count').textContent = readings.length;
  document.querySelector('#reading-copy').textContent = `${data.sources.length} connected field sources · GET-only`;
  document.querySelector('#health-copy').textContent = `${data.sources.length} FarmWiz devices reporting without control access.`;
  document.querySelector('#telemetry-updated').textContent = `UPDATED ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
  grid.innerHTML = readings.map((item) => { const output = ['light', 'pump', 'water_valve', 'air_pump'].includes(item.field); const on = String(item.value).toLowerCase() === 'true'; const state = deviceStates[item.device] || 'offline'; return `<article class="sensor-card device-${state} ${output ? `output-reading ${on ? 'output-on' : 'output-off'}` : ''}"><div class="sensor-top"><span class="sensor-label">${labelFor(item.field)}</span><span class="sensor-source">${item.device} · ${state}</span></div><div class="sensor-value">${formatValue(item)}<small>${item.field === 'temperature' || item.field === 'soil_temperature' ? '°C' : item.field === 'humidity' || item.field === 'soil_moisture' ? '%' : item.field === 'ph' ? ' pH' : item.field === 'tds_ppm' ? ' ppm' : item.field === 'light_level' || item.field === 'ldr' ? ' ADC' : ''}</small></div><div class="sensor-key">source / ${item.source_key}</div></article>`; }).join('');
  document.querySelector('#device-grid').innerHTML = (data.devices || []).map((device) => {
    const outputs = Object.entries(device.outputs || {}).filter(([, value]) => value !== null && value !== undefined).map(([key, value]) => `<span class="output-pill ${String(value).toLowerCase() === 'true' ? 'on' : 'off'}"><i></i>${key.replaceAll('_', ' ')}: ${String(value).toLowerCase() === 'true' ? 'ON' : 'OFF'}</span>`).join('');
    return `<article class="device-card ${device.state}" data-device="${device.device}" tabindex="0" role="button" aria-label="Inspect ${device.device}"><div class="device-top"><span class="device-name">${device.device}</span><span class="device-state"><i></i>${device.state}</span></div><p>${device.state === 'online' ? 'Latest signal received' : 'No recent signal'}</p>${outputs ? `<div class="output-row">${outputs}</div>` : '<div class="output-row"><span class="output-pill muted">sensor-only / no output keys</span></div>'}</article>`;
  }).join('');
  document.querySelectorAll('.device-card').forEach((card) => { card.addEventListener('click', () => showDeviceDetail(card.dataset.device)); card.addEventListener('keydown', (event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); showDeviceDetail(card.dataset.device); } }); });
  showDeviceDetail(selectedDevice || (data.devices || [])[0]?.device);
}

async function refreshInsights() {
  const response = await fetch('/api/farm/insights', { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error('insights unavailable');
  const data = await response.json();
  document.querySelector('#insight-mode').textContent = `${data.count} FINDINGS · ${data.mode}`;
  const findings = data.findings.length ? data.findings.map((item) => { const label = item.type === 'connectivity' ? 'OFFLINE' : item.type === 'soil_moisture' ? 'LOW SOIL' : item.type === 'water_ph' ? 'PH WATCH' : item.type === 'heat' ? 'HEAT' : item.severity; return `<article class="insight-card ${item.severity} ${item.type}"><span class="insight-severity">${label}</span><div><strong>${item.device}</strong><p>${item.message}</p></div></article>`; }).join('') : '<div class="sensor-empty">No rule-based findings right now.</div>';
  const recommendations = (data.recommendations || []).map((item) => `<article class="recommendation-card"><span class="insight-severity">${escapeHtml(item.priority)}</span><div><strong>Next check · ${escapeHtml(item.device)}</strong><p>${escapeHtml(item.recommendation)}</p></div></article>`).join('');
  const pulse = document.querySelector('#pulse-findings');
  if (pulse) pulse.innerHTML = data.findings.length ? `<div class="pulse-heading"><span class="card-kicker">NEEDS A CLOSER LOOK</span><span class="tiny-label">${data.count} FINDINGS</span></div>${findings}` : '';
  const legacy = document.querySelector('#insight-list');
  if (legacy) legacy.innerHTML = findings + recommendations;
}

function historyLabel(key) {
  return { temperature: 'Air temperature', humidity: 'Humidity', soilMoisture: 'Soil moisture', temp: 'Temperature', hum: 'Humidity', ldrADC: 'LDR', pH: 'Water pH', tds_ppm: 'TDS' }[key] || key.replaceAll('_', ' ');
}

async function refreshHistory(days = Number(document.querySelector('#history-days')?.value || 7)) {
  const response = await fetch(`/api/farm/history?days=${days}&limit=200`, { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error('history unavailable');
  const data = await response.json();
  latestHistoryData = data;
  const series = (data.series || []).filter((item) => item.samples && item.samples.length >= 2).slice(0, 6);
  document.querySelector('#history-mode').textContent = `${data.days} DAYS · ${series.length} SERIES`;
  document.querySelector('#history-grid').innerHTML = series.length ? series.map((item) => {
    const values = item.samples.map((sample) => Number(sample.value)).filter((value) => Number.isFinite(value));
    const min = Math.min(...values); const max = Math.max(...values); const range = max - min || 1;
    const points = values.map((value, index) => `${(index / Math.max(values.length - 1, 1)) * 180},${42 - ((value - min) / range) * 34}`).join(' ');
    const first = values[0]; const last = values[values.length - 1]; const delta = last - first;
    const direction = Math.abs(delta) < 0.05 ? 'steady' : delta > 0 ? 'rising' : 'falling';
    return `<article class="history-card"><div class="history-card-top"><div><span class="sensor-label">${historyLabel(item.source_key)}</span><span class="sensor-source">${item.device}</span></div><span class="trend-state ${direction}">${direction}</span></div><svg class="sparkline" viewBox="0 0 180 48" role="img" aria-label="${historyLabel(item.source_key)} trend"><polyline points="${points}" /></svg><div class="history-values"><strong>${last.toFixed(last % 1 === 0 ? 0 : 1)}</strong><span>range ${min.toFixed(min % 1 === 0 ? 0 : 1)}–${max.toFixed(max % 1 === 0 ? 0 : 1)}</span></div></article>`;
  }).join('') : '<div class="sensor-empty">No populated historical series are available yet.</div>';
}

async function refreshAgentUsage() {
  const response = await fetch('/api/agent/usage', { headers: { Accept: 'application/json' } });
  if (!response.ok) return;
  const data = await response.json();
  document.querySelector('#agent-usage').textContent = `${data.provider.toUpperCase()} · ${data.calls_today}/${data.daily_limit} TODAY`;
}

async function refreshDebug() {
  const [statusResponse, providersResponse, usageResponse] = await Promise.all([
    fetch('/api/system/status', { headers: { Accept: 'application/json' } }),
    fetch('/api/agent/providers', { headers: { Accept: 'application/json' } }),
    fetch('/api/agent/usage', { headers: { Accept: 'application/json' } }),
  ]);
  if (!statusResponse.ok) return;
  const status = await statusResponse.json();
  const providers = providersResponse.ok ? await providersResponse.json() : { providers: [] };
  const usage = usageResponse.ok ? await usageResponse.json() : {};
  const providerCards = (providers.providers || []).map((provider) => `<div class="debug-item"><span>${escapeHtml(provider.name)}</span><strong class="debug-${provider.configured ? 'ok' : 'muted'}">${provider.configured ? 'READY' : 'NOT CONFIGURED'}</strong></div>`).join('');
  document.querySelector('#debug-grid').innerHTML = `<div class="debug-item"><span>ThingsBoard</span><strong class="debug-ok">${escapeHtml(status.thingsboard)}</strong></div><div class="debug-item"><span>Telemetry</span><strong class="debug-ok">${escapeHtml(status.telemetry)}</strong></div><div class="debug-item"><span>Monitoring</span><strong class="debug-ok">${escapeHtml(status.monitoring)}</strong></div><div class="debug-item"><span>Edge access</span><strong class="debug-guard">${escapeHtml(status.edge)}</strong></div><div class="debug-item"><span>Local ICM relay</span><strong class="debug-ok">${escapeHtml(status.edge_agent?.local_icm || 'unknown')}</strong></div>${providerCards}<div class="debug-item"><span>AI guard</span><strong class="debug-guard">${usage.calls_today ?? 0}/${usage.daily_limit ?? '—'} today · ${usage.cooldown_seconds ?? '—'}s cooldown</strong></div>`;
  document.querySelector('#debug-updated').textContent = `UPDATED ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
}

let mockActions = [];

function renderMockActions() {
  const list = document.querySelector('#action-list');
  if (!mockActions.length) { list.innerHTML = '<div class="sensor-empty">No mock proposals yet.</div>'; return; }
  list.innerHTML = mockActions.map((action) => `<article class="action-card"><div><strong>${escapeHtml(action.parameters.output)} → ${action.parameters.value ? 'ON' : 'OFF'}</strong><span>${escapeHtml(action.plan_id)} · ${escapeHtml(action.approval)} · ${escapeHtml(action.execution)}${action.scheduled_for ? ` · scheduled ${new Date(action.scheduled_for).toLocaleString()}` : ''}</span></div><div class="action-buttons">${action.approval === 'pending' ? `<button type="button" data-action="approve" data-id="${action.plan_id}">Approve</button>` : ''}${action.approval === 'approved' && action.execution === 'not_run' ? `<button type="button" data-action="execute" data-id="${action.plan_id}">Simulate</button>` : ''}</div></article>`).join('');
  list.querySelectorAll('[data-action]').forEach((button) => button.addEventListener('click', () => updateMockAction(button.dataset.id, button.dataset.action)));
}

async function createMockAction(event) {
  event.preventDefault();
  const schedule = document.querySelector('#action-schedule').value;
  const payload = { device: 'switchbox_01', output: document.querySelector('#action-output').value, value: document.querySelector('#action-value').value === 'true', expires_in_seconds: Number(document.querySelector('#action-expiry').value), scheduled_for: schedule ? new Date(schedule).toISOString() : null };
  const response = await fetch('/api/mock/actions', { method: 'POST', headers: { 'Content-Type': 'application/json', Accept: 'application/json' }, body: JSON.stringify(payload) });
  if (response.ok) { mockActions.unshift(await response.json()); renderMockActions(); }
}

async function updateMockAction(planId, action) {
  const response = await fetch(`/api/mock/actions/${encodeURIComponent(planId)}/${action}`, { method: 'POST', headers: { Accept: 'application/json' } });
  if (response.ok) { const updated = await response.json(); mockActions = mockActions.map((item) => item.plan_id === planId ? updated : item); renderMockActions(); }
}

async function loadAgentProviders() {
  const response = await fetch('/api/agent/providers', { headers: { Accept: 'application/json' } });
  if (!response.ok) return;
  const data = await response.json();
  const select = document.querySelector('#agent-provider');
  const configured = data.providers.filter((provider) => provider.configured);
  select.innerHTML = configured.map((provider) => `<option value="${provider.name}">${provider.name} · ${provider.model}</option>`).join('');
  if (!select.options.length) select.innerHTML = '<option>none configured</option>';
  const local = configured.find((provider) => provider.name === 'local_icm');
  if (local && !sessionStorage.getItem('farmwiz-proactive-brief')) {
    sessionStorage.setItem('farmwiz-proactive-brief', 'requested');
    document.querySelector('#agent-message').value = 'What should I investigate first today?';
    askAgent(null, true);
  }
}

async function askAgent(event, automatic = false) {
  if (event) event.preventDefault();
  const input = document.querySelector('#agent-message');
  const answer = document.querySelector('#agent-answer');
  const message = input.value.trim();
  const provider = automatic ? 'local_icm' : document.querySelector('#agent-provider').value;
  if (!message) return;
  answer.innerHTML = `<span class="answer-label">${escapeHtml(provider.toUpperCase())} IS READING</span><p>Gathering the latest read-only evidence…</p>`;
  try {
    const response = await fetch('/api/agent/chat', { method: 'POST', headers: { 'Content-Type': 'application/json', Accept: 'application/json' }, body: JSON.stringify({ message, provider, profile: readFarmProfile() }) });
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail || 'Agent unavailable');
    answer.innerHTML = `<span class="answer-label">LAST AI ANALYSIS · ${escapeHtml(data.provider)}</span><p>${escapeHtml(data.answer).replaceAll('\n', '<br />')}</p>`;
    answer.innerHTML = `<span class="answer-label">${automatic ? 'PROACTIVE FARM BRIEF' : 'LAST AI ANALYSIS'} · ${escapeHtml(data.provider)}</span><p>${renderAnswer(data.answer)}</p>`;
    window.latestFarmAnswer = data.answer;
    await refreshAgentUsage();
  } catch (error) {
    answer.innerHTML = `<span class="answer-label">AI GUARD</span><p>${error.message}</p>`;
  }
}

async function analyzePlantImage(event) {
  event.preventDefault();
  const file = document.querySelector('#plant-image').files[0];
  const question = document.querySelector('#image-question').value.trim();
  const status = document.querySelector('#image-status');
  const answer = document.querySelector('#agent-answer');
  if (!file || !question) return;
  let upload = file;
  try {
    const bitmap = await createImageBitmap(file);
    const localVision = document.querySelector('#image-provider').value === 'local_icm';
    const maxDimension = localVision ? 640 : 1400;
    const scale = Math.min(1, maxDimension / Math.max(bitmap.width, bitmap.height));
    const canvas = document.createElement('canvas'); canvas.width = Math.round(bitmap.width * scale); canvas.height = Math.round(bitmap.height * scale);
    canvas.getContext('2d').drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    let blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', localVision ? 0.45 : 0.78));
    if (localVision) {
      for (const quality of [0.3, 0.18, 0.1]) {
        if (!blob || blob.size <= 12000) break;
        blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', quality));
      }
      if (blob && blob.size > 6000) {
        const tiny = document.createElement('canvas');
        const tinyScale = Math.min(1, 320 / Math.max(bitmap.width, bitmap.height));
        tiny.width = Math.max(1, Math.round(bitmap.width * tinyScale)); tiny.height = Math.max(1, Math.round(bitmap.height * tinyScale));
        tiny.getContext('2d').drawImage(bitmap, 0, 0, tiny.width, tiny.height);
        blob = await new Promise((resolve) => tiny.toBlob(resolve, 'image/jpeg', 0.25));
      }
    }
    if (blob && (localVision || blob.size < file.size)) upload = new File([blob], 'farmwiz-image.jpg', { type: 'image/jpeg' });
  } catch { /* send the original if browser image compression is unavailable */ }
  const form = new FormData(); form.append('image', upload); form.append('question', question); form.append('provider', document.querySelector('#image-provider').value);
  status.textContent = 'ANALYSING IMAGE…';
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 75000);
    const response = await fetch('/api/agent/image', { method: 'POST', body: form, signal: controller.signal });
    clearTimeout(timer);
    const raw = await response.text();
    let data = {};
    try { data = JSON.parse(raw); } catch { data = { detail: raw.slice(0, 240) || 'Provider returned an empty response' }; }
    if (!response.ok) throw new Error(data.detail || 'Image analysis unavailable');
    answer.innerHTML = `<span class="answer-label">PLANT IMAGE ANALYSIS · ${escapeHtml(data.provider)}</span><p>${renderAnswer(data.answer)}</p>`;
    window.latestFarmAnswer = data.answer;
    status.textContent = 'IMAGE ANALYSIS COMPLETE · NO HARDWARE ACTIONS';
  } catch (error) { status.textContent = error.name === 'AbortError' ? 'Image analysis timed out. Try Gemini Vision or a smaller image.' : error.message; }
}

async function refresh() {
  await refreshStatus();
  try { await refreshTelemetry(); await refreshInsights(); } catch (error) {
    document.querySelector('#telemetry-updated').textContent = 'SIGNAL UNAVAILABLE';
    document.querySelector('#sensor-grid').innerHTML = '<div class="sensor-empty">The read-only telemetry feed needs attention.</div>';
    document.querySelector('#device-grid').innerHTML = '<div class="sensor-empty">Device states unavailable.</div>';
    document.querySelector('#insight-list').innerHTML = '<div class="sensor-empty">Rule-based findings unavailable.</div>';
  }
}

refresh();
document.querySelector('#agent-form').addEventListener('submit', askAgent);
document.querySelector('#image-agent-form')?.addEventListener('submit', analyzePlantImage);
document.querySelector('#action-form')?.addEventListener('submit', createMockAction);
document.querySelector('#add-crop').addEventListener('click', () => { const name = selectedCropName(); if (!name) return; const crops = [...new Set([...selectedCrops(), name])]; saveFarmProfile({ ...readFarmProfile(), crops, crop: cropKey(crops[0]), stage: document.querySelector('#profile-stage').value }); document.querySelector('#profile-crop-search').value = ''; refreshCropProfile(); });
document.querySelector('#crop-guide').addEventListener('click', async (event) => {
  const research = event.target.closest('[data-research-crop]');
  if (research) { research.disabled = true; research.textContent = 'FarmWiz is researching…'; try { const response = await fetch('/api/crops/research', { method: 'POST', headers: {'Content-Type': 'application/json', Accept: 'application/json'}, body: JSON.stringify({ crop: research.dataset.researchCrop, provider: document.querySelector('#agent-provider')?.value || null }) }); if (!response.ok) throw new Error((await response.json()).detail || 'Research unavailable'); await loadCropCatalog(); await refreshCropProfile(); } catch (error) { research.disabled = false; research.textContent = error.message; } return; }
  const remove = event.target.closest('[data-delete-crop]');
  if (remove) { await fetch(`/api/crops/${encodeURIComponent(remove.dataset.deleteCrop)}`, { method: 'DELETE' }); const crops = selectedCrops().filter((name) => cropKey(name) !== remove.dataset.deleteCrop); saveFarmProfile({ ...readFarmProfile(), crops, crop: crops[0] ? cropKey(crops[0]) : '' }); refreshCropProfile(); }
});
document.querySelector('#crop-selection').addEventListener('click', (event) => { const chip = event.target.closest('[data-edit-crop]'); if (!chip) return; const crops = selectedCrops().filter((name) => cropKey(name) !== chip.dataset.editCrop); const old = selectedCrops().find((name) => cropKey(name) === chip.dataset.editCrop) || ''; document.querySelector('#profile-crop-search').value = old; saveFarmProfile({ ...readFarmProfile(), crops, crop: crops[0] ? cropKey(crops[0]) : '' }); refreshCropProfile(); document.querySelector('#profile-crop-search').focus(); });
document.querySelector('#profile-stage').addEventListener('change', () => saveFarmProfile({ ...readFarmProfile(), stage: document.querySelector('#profile-stage').value }));
document.querySelector('#profile-search').addEventListener('click', searchFarmLocation);
document.querySelector('#profile-location').addEventListener('keydown', (event) => { if (event.key === 'Enter') { event.preventDefault(); searchFarmLocation(); } });
loadFarmProfile();
loadCropCatalog();
refreshWeather();
refreshCropProfile();
document.querySelector('#dialog-close').addEventListener('click', () => document.querySelector('#history-dialog').close());
document.querySelector('#voice-listen')?.addEventListener('click', () => startVoiceInput('#voice-transcript'));
document.querySelector('#voice-parse')?.addEventListener('click', interpretVoiceCommand);
document.querySelector('#voice-speak')?.addEventListener('click', () => { const text = document.querySelector('#voice-transcript')?.value.trim(); if (text) speakText(text); });
document.querySelector('#voice-convert')?.addEventListener('click', interpretVoiceCommand);
document.querySelector('#voice-send')?.addEventListener('click', sendApprovedLightTest);
document.querySelector('#voice-clear')?.addEventListener('click', () => { document.querySelector('#voice-transcript').value = ''; document.querySelector('#voice-preview').textContent = 'No command converted yet.'; document.querySelector('#voice-status').textContent = 'READY'; document.querySelector('#voice-send').hidden = true; latestVoiceDraft = null; });
document.querySelector('#agent-listen')?.addEventListener('click', () => startVoiceInput('#agent-message'));
document.querySelector('#agent-speak')?.addEventListener('click', () => { if (window.latestFarmAnswer) speakText(window.latestFarmAnswer); });
const historyDays = document.querySelector('#history-days');
if (historyDays) {
  historyDays.addEventListener('input', () => { document.querySelector('#history-days-value').textContent = `${historyDays.value} ${historyDays.value === '1' ? 'DAY' : 'DAYS'}`; });
  historyDays.addEventListener('change', () => refreshHistory(Number(historyDays.value)));
}
document.querySelectorAll('[data-question]').forEach((button) => {
  button.addEventListener('click', () => {
    const input = document.querySelector('#agent-message');
    input.value = button.dataset.question;
    input.focus();
  });
});
refreshAgentUsage();
loadAgentProviders();
refreshDebug();
setInterval(() => { refreshTelemetry(); refreshInsights(); }, 60000);
refreshHistory();
setInterval(refreshHistory, 300000);

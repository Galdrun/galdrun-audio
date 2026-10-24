// The client's settings page (client-settings.html), one script for every
// client build. Two backends serve it:
//
//   * control: the desktop and Linux clients, on their control port. Every
//     setting is its own /api line command and applies at once; cards appear
//     as the client reports them supported.
//   * board: the ESP32, on port 80. /api/settings holds wifi.json and
//     client.json together, and saving them restarts the board. It also works
//     in setup mode, when the client is not running.
//
// The page asks for /api/settings first: only the board has that route, and
// the control port answers it with 404.
'use strict';
const $ = id => document.getElementById(id);
createUsageCharts($('usageCharts'), $('wifiChart'));

let backend = null;
let saved = {}; // what settings() returned when last loaded or saved
let savedClientId = '';
let ready = false, saving = false, scanning = false, restarting = false;
let lastUptime = 0;
let scanned = [], storedSsid = '', savedBssid = '', connectedBssid = '';

// ---------------------------------------------------------------------------
// Shared

function msg(text, bad) {
  $('msg').textContent = text;
  $('msg').className = bad ? 'message bad' : 'message';
}

function show(id, visible) {
  $(id).classList.toggle('hidden', !visible);
}

function setEnabled(id, enabled) {
  for (const control of $(id).querySelectorAll('input,select,button')) control.disabled = !enabled;
}

const fmt = f => f ? (f.sampleRate / 1000) + ' kHz \u00b7 ' + f.channels + ' ch \u00b7 ' + f.sampleType : '\u2013';
const up = s => (s >= 86400 ? Math.floor(s / 86400) + 'd ' : '') + Math.floor(s % 86400 / 3600) + 'h ' + Math.floor(s % 3600 / 60) + 'm';

function list(element, rows) {
  element.replaceChildren(...rows.flatMap(([key, value]) => {
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = key;
    dd.textContent = value;
    return [dt, dd];
  }));
}

function settingsChanged() {
  if (!ready) return false;
  const current = backend.settings();
  return Object.keys(current).some(key => current[key] !== saved[key]);
}

function updateSave() {
  const changed = settingsChanged();
  $('save').textContent = restarting ? 'Restarting...' : changed && backend?.restarts ? 'Save and restart' : 'Save';
  $('save').disabled = !ready || saving || scanning || restarting ||
    (!changed && !$('hardwareInputs').inputEdits?.dirty);
}
for (const event of ['input', 'change']) $('form').addEventListener(event, updateSave);
$('hardwareInputs').addEventListener('bindingschange', updateSave);

// The running client's status, which both backends pass through unchanged.
function showStatus(s) {
  lastUptime = s.uptimeSeconds;
  const a = s.audio;
  $('headerDevice').textContent = s.name || s.clientId;
  $('connection').textContent = s.server.state;
  $('connection').classList.toggle('online', s.server.state === 'connected');
  const rows = [
    ['Version', s.version + ' \u00b7 up ' + up(s.uptimeSeconds)], ['Room', s.name || '\u2013'], ['Client id', s.clientId],
    ['Server', (s.server.address || 'not configured') + ' \u00b7 ' + s.server.state], ['Session', s.session.state],
    ['Output', a.deviceSelected ? a.deviceName : 'none'], ['Format', a.format ? fmt(a.format) : 'device closed']];
  if (s.power) rows.push(['Power', s.power.state]);
  list($('status'), rows);
  show('statusBox', true);
}

function statusUnavailable() {
  $('connection').textContent = 'Unavailable';
  $('connection').classList.remove('online');
}

async function showRestart(message) {
  const previousUptime = lastUptime;
  restarting = true;
  $('restart').disabled = true;
  document.querySelector('main').inert = true;
  updateSave();
  msg(message + ' Waiting for the device to reconnect...');
  let offline = false;
  for (let attempt = 0; attempt < 120; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 1000));
    try {
      const board = backend === boardBackend;
      const response = await fetch(board ? (setup ? '/api/settings' : '/api/status') : '/api', {
        method: board ? 'GET' : 'POST', cache: 'no-store', signal: AbortSignal.timeout(3000),
        ...(!board && {headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: 'status'})})});
      const result = await response.json();
      if (!response.ok || (!setup && !result.status)) throw new Error('Device unavailable');
      if (offline || result.status?.uptimeSeconds < previousUptime) {
        // The listener can return before every startup request is ready.
        sessionStorage.setItem('galdrunRestartUntil', String(Date.now() + 60000));
        location.reload();
        return;
      }
    } catch { offline = true; }
  }
  document.querySelector('main').inert = false;
  restarting = false;
  $('restart').disabled = false;
  updateSave();
  msg('The device has not reconnected. Check its power and network address, then refresh this page.', true);
}
$('restart').onclick = async () => {
  if (saving || scanning || restarting || !confirm('Restart this device? Playback will stop and unsaved settings will be lost.')) return;
  $('restart').disabled = true;
  try {
    if (backend === boardBackend)
      await boardJson(await fetch('/api/restart', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'}));
    else await api('restart');
    void showRestart('Restarting the device.');
  } catch (e) { $('restart').disabled = false; msg(e.message, true); }
};

async function refreshInputs(initial = false) {
  try {
    renderHardwareInputs(await backend.inputs(), $('hardwareInputs'), backend.saveBindings);
  } catch (e) {
    if (initial) throw e;
    if (!$('hardwareInputs').querySelector('article'))
      $('hardwareInputs').textContent = 'Cannot read hardware inputs: ' + e.message;
  }
}

// Static or DHCP address rows, for Ethernet and Wi-Fi alike.
function addressRows() {
  for (const p of ['eth', 'wifi']) {
    const fixed = $(p + 'Mode').value === 'static';
    show(p + 'Static', fixed);
    for (const k of ['Ip', 'Mask', 'Gateway', 'Dns']) $(p + k).disabled = !fixed;
    $(p + 'Ip').required = fixed;
    $(p + 'Mask').required = fixed;
  }
}
for (const p of ['eth', 'wifi']) $(p + 'Mode').addEventListener('change', addressRows);

// Automatic, then the access points of the network in the field, strongest first.
function fillAps(want) {
  const ssid = $('wifiSsid').value, aps = scanned.filter(x => x.ssid === ssid);
  const label = x => x.bssid + ' \u00b7 ch ' + x.channel + ' \u00b7 ' + x.rssi + ' dBm' +
    (x.bssid === connectedBssid ? ' \u00b7 connected now' : '');
  const options = [new Option('Automatic (strongest signal)', ''), ...aps.map(x => new Option(label(x), x.bssid))];
  if (want && !aps.some(x => x.bssid === want)) {
    if (ssid === storedSsid && want === savedBssid)
      options.push(new Option(want + (want === connectedBssid ? ' \u00b7 connected now' : ' \u00b7 not in the last scan'), want));
    else
      want = '';
  }
  $('wifiAp').replaceChildren(...options);
  $('wifiAp').value = want;
  updateSave();
}
$('wifiSsid').oninput = () => fillAps($('wifiAp').value);

// The eye shows the password only while it is held.
const reveal = visible => {
  $('wifiPassword').type = visible ? 'text' : 'password';
  $('wifiEye').setAttribute('aria-pressed', visible);
};
$('wifiEye').onpointerdown = e => { e.preventDefault(); reveal(true); };
for (const event of ['pointerup', 'pointerleave', 'pointercancel', 'blur']) $('wifiEye').addEventListener(event, () => reveal(false));
$('wifiEye').oncontextmenu = e => e.preventDefault();
$('wifiEye').onkeydown = e => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); reveal(true); } };
$('wifiEye').onkeyup = () => reveal(false);

$('wifiScan').onclick = async () => {
  if (scanning || saving) return;
  scanning = true;
  const button = $('wifiScan');
  button.disabled = true;
  button.replaceChildren(Object.assign(document.createElement('span'), {className: 'spin'}));
  updateSave();
  msg('Scanning\u2026');
  try {
    // Each network once, at its strongest access point; all of them go to the
    // access point list. A tappable list, not a datalist: most phone browsers
    // never show those.
    scanned = await backend.scan();
    const seen = new Set(), nets = scanned.filter(x => !seen.has(x.ssid) && seen.add(x.ssid));
    fillAps($('wifiAp').value);
    $('wifiNetworks').replaceChildren(...nets.map(x => {
      const choice = document.createElement('button');
      choice.type = 'button';
      const name = document.createElement('span');
      name.textContent = x.ssid;
      const signal = document.createElement('span');
      signal.className = 'sig';
      signal.textContent = (x.channel > 14 ? '5' : '2.4') + ' GHz \u00b7 ' + x.rssi + ' dBm' + (x.secure ? '' : ' \u00b7 open');
      choice.append(name, signal);
      choice.onclick = () => {
        $('wifiSsid').value = x.ssid;
        fillAps($('wifiAp').value);
        if (x.secure) $('wifiPassword').focus();
      };
      return choice;
    }));
    msg(nets.length + ' network(s) found' + (nets.length ? '; tap one to use it.' : '.'));
  } catch (e) {
    msg('Scan failed: ' + e.message, true);
  } finally {
    scanning = false;
    button.disabled = false;
    button.textContent = 'Scan';
    updateSave();
  }
};

$('editClientId').onclick = () => {
  const input = $('clientId');
  input.disabled = false;
  input.focus();
  $('editClientId').disabled = true;
};

$('form').onsubmit = async e => {
  e.preventDefault();
  if (!ready || saving || restarting || $('save').disabled) return;
  const changed = settingsChanged(), edits = $('hardwareInputs').inputEdits;
  if ((changed && !$('form').reportValidity()) || (edits && !edits.validate())) return;
  if ($('clientId').value.trim() !== savedClientId &&
      !confirm('Change the client id? The server will see this as a new room and forget its zone, volume and settings.'))
    return;
  const controls = [...$('form').querySelectorAll('input,select,button')], disabled = controls.map(c => c.disabled);
  saving = true;
  updateSave();
  controls.forEach(c => c.disabled = true);
  try {
    await edits?.save();
    await backend.save(changed);
  } catch (err) {
    msg(err.message, true);
  } finally {
    controls.forEach((c, i) => c.disabled = disabled[i]);
    saving = false;
    updateSave();
  }
};

// ---------------------------------------------------------------------------
// Control backend: desktop and Linux clients, /api line commands.

async function api(command, params) {
  const r = await fetch('/api', {method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({command, params: params || {}})});
  if (!r.ok) throw new Error(await r.text());
  const j = await r.json();
  if (!j.ok) throw new Error(j.message || j.error);
  return j;
}

let networkEnabled = false, cpuEnabled = false, ledsEnabled = false, powerEnabled = false, driftEnabled = false;
let sshEnabled = false, ssh = null;
let dacEnabled = false, dacControls = [], dacRoomVolume = false, audioDevices = [], selectedAudioId = '', explicitAudioDevice = false;

// SSH has its own buttons, not the Save bar: its passwords are never kept as "saved" state.
async function loadSsh() {
  const r = await api('getSsh');
  sshEnabled = r.supported;
  show('sshBox', sshEnabled);
  if (sshEnabled) renderSsh(r.ssh);
}

function renderSshState(s) {
  ssh = s;
  $('sshState').textContent = !s.enabled ? 'SSH is off.' : s.running ?
    'SSH is on: ssh root@' + location.hostname :
    'SSH is on and starting. The first start creates its keys and takes up to a minute.';
  $('sshToggle').textContent = s.enabled ? 'Turn off SSH' : 'Turn on SSH';
}

function renderSsh(s) {
  renderSshState(s);
  show('sshCurrentField', s.passwordSet);
  show('sshPasswordSave', s.passwordSet);
  $('sshPasswordLabel').textContent = s.passwordSet ? 'New password (optional)' : 'New password';
  for (const id of ['sshCurrent', 'sshPassword', 'sshRepeat']) $(id).value = '';
}

async function changeSsh(enabled, done) {
  const password = $('sshPassword').value;
  if (password !== $('sshRepeat').value) return msg('The new passwords do not match.', true);
  if (enabled && !ssh.passwordSet && !password) return msg('Choose a password to turn SSH on.', true);
  const params = {enabled};
  if (ssh.passwordSet) params.currentPassword = $('sshCurrent').value;
  if (password) params.password = password;
  try {
    renderSsh((await api('setSsh', params)).ssh);
    msg(done);
  } catch (e) {
    msg('SSH: ' + e.message, true);
  }
}

$('sshToggle').onclick = () => changeSsh(!ssh.enabled, ssh.enabled ? 'SSH is off.' : 'SSH is on.');
$('sshPasswordSave').onclick = () => $('sshPassword').value ?
  changeSsh(ssh.enabled, 'Password changed.') : msg('Enter the new password.', true);

async function loadCpu() {
  const r = await api('getCpu');
  cpuEnabled = r.supported;
  show('cpuBox', cpuEnabled);
  $('cpuMhz').disabled = !cpuEnabled;
  if (cpuEnabled) {
    const values = r.cpu.frequenciesMhz;
    $('cpuMhz').replaceChildren(...values.slice().reverse().map(v =>
      new Option(v + ' MHz' + (v === r.cpu.defaultMhz ? ' (default)' : ''), v)));
    if (r.cpu.configured === false) {
      const automatic = new Option('Firmware policy (' + r.cpu.governor + ')', 'auto');
      automatic.disabled = true;
      $('cpuMhz').prepend(automatic);
    }
    $('cpuMhz').value = r.cpu.configured === false ? 'auto' : r.cpu.mhz;
    $('cpuNow').textContent = 'Now running at ' + r.cpu.currentMhz + ' MHz. ' +
      (r.cpu.configured === false ? 'The firmware controls frequency until you save a MHz value.' : 'Requested: ' + r.cpu.mhz + ' MHz.');
  }
  saved.cpu = cpuEnabled ? $('cpuMhz').value : '';
}

// Minutes on the page, seconds on the device.
let deepSleepEnabled = false;
function powerSettings() {
  const minutes = id => Math.round(Number($(id).value) * 60);
  return JSON.stringify(deepSleepEnabled
    ? {standbyAfterSeconds: minutes('standbyMinutes'), deepSleepAfterSeconds: minutes('deepSleepMinutes')}
    : {standbyAfterSeconds: minutes('standbyMinutes')});
}
// One button: Standby while on, Wake while parked (manual or automatic).
function showPowerState(state) {
  $('powerNow').textContent = 'State: ' + state;
  show('standbyOn', state === 'on');
  show('standbyOff', state !== 'on');
}
function renderPower(r) {
  $('standbyMinutes').value = String(Math.round(r.standbyAfterSeconds / 60));
  if (deepSleepEnabled) $('deepSleepMinutes').value = String(Math.round(r.deepSleepAfterSeconds / 60));
  showPowerState(r.state);
}
async function loadPower() {
  const r = await api('getPower');
  show('restart', r.restart === true);
  powerEnabled = r.supported;
  deepSleepEnabled = powerEnabled && r.deepSleep === true;
  show('powerBox', powerEnabled);
  show('deepSleepFields', deepSleepEnabled);
  show('deepSleepNow', deepSleepEnabled);
  $('standbyMinutes').disabled = !powerEnabled;
  $('deepSleepMinutes').disabled = !deepSleepEnabled;
  if (powerEnabled) renderPower(r);
  saved.power = powerEnabled ? powerSettings() : '';
}
$('deepSleepNow').onclick = async () => {
  if (!confirm('Put this device into deep sleep now? It leaves the network until POWER or the server wakes it.')) return;
  try {
    await api('deepSleep');
    $('powerNow').textContent = 'State: going to deep sleep';
  } catch (e) {
    msg(e.message, true);
  }
};
for (const command of ['standbyOn', 'standbyOff']) $(command).onclick = async () => {
  const button = $(command);
  button.disabled = true;
  try {
    const r = await api(command);
    showPowerState(r.state);
    controlRefresh();
  } catch (e) {
    msg(e.message, true);
  } finally {
    button.disabled = false;
  }
};

async function loadLeds() {
  const r = await api('getLeds');
  ledsEnabled = r.supported;
  show('ledBox', ledsEnabled);
  $('ledOff').disabled = !ledsEnabled;
  if (ledsEnabled) $('ledOff').value = String(r.off);
  saved.leds = ledsEnabled ? $('ledOff').value : '';
}

const dacHints = {
  'Master Playback Volume': 'Left/right attenuation: 127 = 0 dB, 0 = -127 dB.',
  'Master Playback Switch': 'Enable the left/right analog outputs.',
  'Master Capture Switch': 'Enable the left/right analog inputs.',
  'Digital Loopback Switch': 'Route the ADC directly to the DAC instead of server playback.',
  'Auto-Mute Switch': 'Mute when the DAC detects digital silence.',
  'De-emphasis Switch': 'Apply de-emphasis for pre-emphasized recordings; the driver follows the sample rate.',
  'Dither 16-Bit Data Switch': 'Dither the ADC output for 16-bit capture.',
  'High Pass Filter Switch': 'Remove DC from the left/right ADC inputs.',
  'Left Channel Inversion Switch': 'Invert the left analog output polarity.',
  'Right Channel Inversion Switch': 'Invert the right analog output polarity.',
  'Slow Roll Off Filter Switch': 'Use the slow roll-off DAC interpolation filter.',
  'Soft Ramp Switch': 'Ramp digital volume changes.',
  'Soft Ramp-Down Switch': 'Ramp down on mute or power-down.',
  'Soft Volume Ramp-Up Switch': 'Ramp up on unmute or power-up.',
  'Zero Cross Switch': 'Apply volume changes near signal zero crossings.',
  'Left Output Source': 'Choose what the left analog output plays.',
  'Right Output Source': 'Choose what the right analog output plays.',
  'Link Playback Volume Switch': 'Make both analog outputs use the left volume. The stored right attenuation applies again when unlinked.'};
let dacRoomControls = ['Master Playback Volume', 'Master Playback Switch'];
let dacResetSource = 'firmware defaults';
let dacCard = '';

function dacRoomOwned(control) {
  return dacRoomVolume && dacRoomControls.includes(control.name);
}

function dacSettings() {
  return Object.fromEntries(dacControls.flatMap((c, i) => dacRoomOwned(c) ? [] :
    [[c.name, c.values.map((_, ch) => Number($('dac-' + i + '-' + ch).value))]]));
}

function renderDac(r) {
  const dac = r.dac;
  dacCard = dac.card || '';
  dacRoomVolume = !!r.roomVolume;
  dacRoomControls = r.roomVolumeControls || ['Master Playback Volume', 'Master Playback Switch'];
  dacResetSource = dac.resetSource || 'firmware defaults';
  $('dacReset').textContent = dacResetSource === 'initial card settings' ? 'Restore initial card settings' : 'Reset Default DAC settings';
  dacControls = dac.controls;
  $('dacName').textContent = dac.codec + ' DAC settings';
  $('dacControls').replaceChildren();
  dacControls.forEach((c, i) => {
    c.values.forEach((v, ch) => {
      const id = 'dac-' + i + '-' + ch, label = document.createElement('label');
      label.htmlFor = id;
      label.textContent = c.name + (c.values.length === 2 ? (ch === 0 ? ' \u2014 left' : ' \u2014 right') : '');
      let input;
      if (c.type === 'boolean' || c.type === 'enum') {
        input = document.createElement('select');
        input.replaceChildren(...(c.type === 'boolean' ? ['Off', 'On'] : c.items).map((text, n) => new Option(text, n)));
      } else {
        input = document.createElement('input');
        input.type = 'number';
        input.required = true;
        input.min = c.min;
        input.max = c.max;
        input.step = c.step;
      }
      input.id = id;
      input.value = v;
      input.disabled = dacRoomOwned(c);
      $('dacControls').append(label, input);
    });
    const hint = document.createElement('p');
    hint.className = 'hint';
    hint.textContent = dacRoomOwned(c)
      ? 'Set by the room volume: follows it with hardware volume, stays at 0 dB and on otherwise.'
      : dacHints[c.name] || '';
    $('dacControls').append(hint);
  });
  saved.dac = JSON.stringify(dacSettings());
}

let audioProfiles = [];
function audioProfileHint() {
  const p = audioProfiles.find(p => p.id === $('audioProfile').value);
  $('audioProfileHint').textContent = p ? [p.pins, p.clock, p.notes].filter(Boolean).join('. ') : '';
}
async function loadAudioProfile() {
  const r = await api('getAudioProfile');
  show('audioProfileBox', r.supported);
  if (!r.supported) return;
  audioProfiles = r.profiles.slice().sort((a, b) =>
    Number(b.id === 'usb') - Number(a.id === 'usb') || a.name.localeCompare(b.name, 'en', {sensitivity: 'base'}));
  $('audioProfile').replaceChildren(...audioProfiles.map(p => new Option(p.name, p.id)));
  $('audioProfile').value = r.profile;
  audioProfileHint();
  if (r.rebootRequired) $('audioProfileHint').textContent += ' Reboot required.';
}
$('audioProfile').onchange = audioProfileHint;
for (const [button, profile] of [['audioProfileApply', null], ['audioProfileRollback', 'rollback']]) {
  $(button).onclick = async () => {
    if (saving || scanning || restarting || !confirm('Apply this audio boot profile? Check the card wiring first. A changed profile restarts the device automatically; unsaved settings will be lost.')) return;
    $(button).disabled = true;
    try {
      const result = await api('setAudioProfile', {profile: profile || $('audioProfile').value});
      if (result.restarting) void showRestart('Audio profile saved. Restarting and scanning audio outputs.');
      else {
        await loadAudioProfile();
        msg('Audio profile saved. The active profile is unchanged.');
      }
    } catch (e) { msg(e.message, true); }
    finally { $(button).disabled = false; }
  };
}

async function loadDac() {
  const r = await api('getDac');
  dacEnabled = r.supported;
  show('dacBox', dacEnabled);
  if (dacEnabled) renderDac(r);
  else saved.dac = '';
}
$('dacReload').onclick = async () => {
  try {
    await loadDac();
    updateSave();
    msg('Current DAC settings loaded.');
  } catch (e) {
    msg(e.message, true);
  }
};
$('dacReset').onclick = async () => {
  if (saving || !confirm('Restore ' + dacResetSource + ' now? Routing or mute may change.' +
    (dacRoomVolume ? ' The level and mute stay with the room volume.' : '')))
    return;
  const button = $('dacReset');
  button.disabled = true;
  try {
    renderDac(await api('resetDac', dacCard ? {card: dacCard} : {}));
    updateSave();
    msg('Default DAC settings restored and saved.');
  } catch (e) {
    msg(e.message, true);
  } finally {
    button.disabled = false;
  }
};

function renderFormats() {
  // Automatic ('') is the default device, as the client picks it: the marked one, else the first.
  // A missing explicit ID shows the device the client fell back to.
  const want = $('device').value;
  let d = (want && (audioDevices.find(x => x.id === want) || audioDevices.find(x => x.id === selectedAudioId)))
    || audioDevices.find(x => x.isDefault) || audioDevices[0];
  if (explicitAudioDevice) d = want ? audioDevices.find(x => x.id === want) : null;
  // ALSA's "default" plugin can convert formats. Use the actual hardware list for DAC capabilities.
  if (d?.id === 'default') d = audioDevices.find(x => x.id.startsWith('hw:')) || d;
  $('formatDevice').textContent = d ? d.name + ' \u00b7 direct hardware formats' : 'No output device found.';
  const formats = d?.exclusiveFormats || [];
  const groups = new Map();
  for (const f of formats) {
    const key = f.sampleRate + ' / ' + f.channels;
    if (!groups.has(key)) groups.set(key, {rate: f.sampleRate, channels: f.channels, types: []});
    groups.get(key).types.push(f.sampleType);
  }
  const names = {int16: '16-bit', int24: '24-bit packed', int24in32: '24-bit in 32-bit', int32: '32-bit', float32: '32-bit float'};
  list($('audioFormats'), [...groups.values()].sort((a, b) => a.rate - b.rate || a.channels - b.channels)
    .map(g => [(g.rate / 1000) + ' kHz \u00b7 ' + g.channels + ' ch', g.types.map(t => names[t] || t).join(', ')]));
  show('audioFormats', formats.length > 0);
  show('audioFormatsEmpty', !formats.length);
}
$('device').addEventListener('change', () => {
  if (backend === controlBackend) {
    renderFormats();
    show('dacBox', dacEnabled && $('device').value === saved.device);
  }
});

async function loadDevices() {
  const j = await api('audioDevices'), want = j.configuredDeviceId || '';
  explicitAudioDevice = !!j.requiresConfiguredDevice;
  const options = [new Option(explicitAudioDevice ? 'Select output (silent until selected)' : 'Automatic (the default device)', ''),
    ...j.cache.devices.filter(d => !explicitAudioDevice || d.id.startsWith('hw:CARD='))
      .map(d => new Option(d.name + (d.isDefault ? ' (default)' : ''), d.id))];
  if (want && !j.cache.devices.some(d => d.id === want)) options.push(new Option(want + ' (not found)', want));
  $('device').replaceChildren(...options);
  $('device').value = want;
  saved.device = want;
  audioDevices = j.cache.devices;
  selectedAudioId = j.selectedDeviceId || '';
  renderFormats();
}
$('rescan').onclick = async () => {
  const button = $('rescan'), keep = $('device').value;
  button.disabled = true;
  msg('Scanning the audio devices\u2026');
  try {
    const j = await api('rescanAudio');
    await loadDevices();
    if ([...$('device').options].some(o => o.value === keep)) { $('device').value = keep; renderFormats(); }
    msg(j.deviceCount + ' device(s) found.');
    controlRefresh();
  } catch (e) {
    msg('Rescan failed: ' + e.message, true);
  } finally {
    button.disabled = false;
    updateSave();
  }
};

function networkSettings() {
  const address = p => ({ipMode: $(p + 'Mode').value, ip: $(p + 'Ip').value.trim(), netmask: $(p + 'Mask').value.trim(),
    gateway: $(p + 'Gateway').value.trim(), dns: $(p + 'Dns').value.trim()});
  return {
    device: {name: $('deviceName').value.trim()},
    ethernet: {...address('eth'), speedMbps: $('ethSpeed').value},
    wifi: {...address('wifi'), radio: $('wifiRadio').value, ssid: $('wifiSsid').value, bssid: $('wifiAp').value, password: $('wifiPassword').value,
      clearPassword: $('wifiClearPassword').checked, country: $('wifiCountry').value.trim()}};
}

async function loadNetwork() {
  const n = await api('getNetwork');
  networkEnabled = n.supported;
  show('wifiRadioBox', networkEnabled);
  for (const id of ['networkBox', 'ethAddressBox', 'wifiAddressBox', 'deviceNameBox']) {
    show(id, networkEnabled);
    setEnabled(id, networkEnabled);
  }
  $('deviceName').value = n.network?.device?.name || '';
  $('deviceName').placeholder = n.network?.device?.defaultName || '';
  if (networkEnabled) {
    for (const [p, g] of [['eth', n.network.ethernet], ['wifi', n.network.wifi]]) {
      for (const [suffix, key] of [['Mode', 'ipMode'], ['Ip', 'ip'], ['Mask', 'netmask'], ['Gateway', 'gateway'], ['Dns', 'dns']])
        $(p + suffix).value = g[key] || '';
      $(p + 'Now').textContent = (g.currentIp ? 'Now on ' + g.currentIp + '. ' : '') + 'A wrong static address leaves the board unreachable.';
    }
    const w = n.network.wifi;
    $('wifiRadio').replaceChildren(new Option('Automatic', ''));
    for (const radio of w.radios || [])
      $('wifiRadio').add(new Option(radio.interface + ' (' + radio.id + ')', radio.id));
    if (w.radio && !(w.radios || []).some(r => r.id === w.radio))
      $('wifiRadio').add(new Option(w.radio + ' (unplugged; using fallback)', w.radio));
    $('wifiRadio').value = w.radio || '';
    $('wifiRadioHint').textContent = (w.activeInterface ? 'Active: ' + w.activeInterface + '. ' : '') +
      (w.apSupported === false ? 'This radio supports station mode only. Use Ethernet for setup.' :
        'If the selected radio is unplugged, another available radio is used.');
    $('wifiSsid').value = storedSsid = w.ssid;
    $('wifiCountry').value = w.country;
    $('wifiPassword').value = '';
    $('wifiPassword').placeholder = w.passwordSet ? 'unchanged' : '';
    $('wifiClearPassword').checked = false;
    savedBssid = w.bssid || '';
    connectedBssid = w.connectedBssid || '';
    fillAps(savedBssid);
    addressRows();
    $('ethSpeed').value = n.network.ethernet.speedMbps || 'auto';
  }
  saved.network = networkEnabled ? JSON.stringify(networkSettings()) : '';
}

function renderDrift(d) {
  driftEnabled = !!d && (!!d.hardwareClock || d.configured === 'hardwareClock');
  show('driftBox', driftEnabled);
  if (!driftEnabled) return;
  const c = d.hardwareClock;
  $('driftNow').textContent = 'Now: ' +
    (d.effective === 'hardwareClock' ? 'hardware clock, ' + c.appliedPpm + ' ppm' + (c.saturated ? ' (at its limit)' : '') : 'server resampling') +
    (d.reason ? ' \u00b7 ' + d.reason : '') +
    (d.serverDriftPpm !== undefined ? ' \u00b7 measured ' + d.serverDriftPpm.toFixed(1) + ' ppm' : '');
}

function controlSettings() {
  return {
    // Hidden when the output loses its clock, not changed: kept as saved.
    drift: driftEnabled ? $('driftCorrection').value : saved.drift,
    room: $('room').value.trim(), clientId: $('clientId').value.trim(), server: $('server').value.trim(),
    device: $('device').value,
    network: networkEnabled ? JSON.stringify(networkSettings()) : '',
    cpu: cpuEnabled ? $('cpuMhz').value : '',
    dac: dacEnabled ? JSON.stringify(dacSettings()) : '',
    leds: ledsEnabled ? $('ledOff').value : '',
    power: powerEnabled ? powerSettings() : ''};
}

async function controlRefresh(initial = false) {
  try {
    const s = (await api('status')).status;
    showStatus(s);
    $('about').textContent = s.platform + ' \u00b7 version ' + s.version + ' \u00b7 up ' + up(s.uptimeSeconds);
    renderDrift(s.audio.driftCorrection);
    if (s.power) showPowerState(s.power.state);
    if (cpuEnabled) {
      const r = await api('getCpu');
      $('cpuNow').textContent = 'Now running at ' + r.cpu.currentMhz + ' MHz.';
    }
    if (sshEnabled && ssh.enabled && !ssh.running) renderSshState((await api('getSsh')).ssh);
    connectHardwareInputEvents($('hardwareInputs'), '/api/input-events');
    if (s.platform === 'linux') {
      show('hardwareBox', true);
      await refreshInputs(initial);
    }
  } catch (e) {
    if (initial) throw e;
    statusUnavailable();
    msg('Cannot read the status: ' + e.message, true);
  }
}

async function controlLoad() {
  $('room').required = true;
  show('rescan', true);
  show('formatsBox', true);
  show('wifiAdvanced', true);
  const st = (await api('status')).status;
  saved.room = st.name || '';
  saved.clientId = savedClientId = st.clientId;
  saved.server = (await api('getServer')).server;
  renderDrift(st.audio.driftCorrection);
  // Also while hidden, so an output that gains a clock shows what is configured.
  $('driftCorrection').value = saved.drift = st.audio.driftCorrection?.configured || '';
  $('room').value = saved.room;
  $('clientId').value = saved.clientId;
  $('server').value = saved.server;
  await Promise.all([loadDevices(), loadNetwork(), loadCpu(), loadDac(), loadLeds(), loadPower(), loadSsh(), loadAudioProfile()]);
  ready = true;
  updateSave();
}

async function controlSave() {
  const {drift, room, clientId, server, device, network, cpu, dac, leds, power} = controlSettings();
  const deviceChanged = device !== saved.device;
  const networkChanged = network !== saved.network;
  if (room !== saved.room) { await api('setName', {name: room}); saved.room = room; }
  if (clientId !== saved.clientId) { await api('setClientId', {clientId}); saved.clientId = savedClientId = clientId; }
  if (server !== saved.server) { await api('setServer', {server}); saved.server = server; }
  if (power !== saved.power) {
    renderPower(await api('setPower', JSON.parse(power)));
    saved.power = powerSettings();
  }
  if (leds !== saved.leds) {
    const r = await api('setLeds', {off: leds === 'true'});
    saved.leds = String(r.off);
  }
  if (deviceChanged) { await api('setAudioDevice', {deviceId: device}); saved.device = device; await loadDac(); }
  if (drift !== saved.drift) { await api('setDriftCorrection', {driftCorrection: drift}); saved.drift = drift; }
  if (cpu !== saved.cpu) {
    const r = await api('setCpu', {mhz: Number(cpu)});
    saved.cpu = String(r.cpu.mhz);
    $('cpuNow').textContent = 'Now running at ' + r.cpu.currentMhz + ' MHz.';
  }
  if (!deviceChanged && dac !== saved.dac) {
    const before = JSON.parse(saved.dac), after = JSON.parse(dac);
    const changes = Object.fromEntries(Object.entries(after).filter(([name, values]) =>
      JSON.stringify(values) !== JSON.stringify(before[name])));
    renderDac(await api('setDac', dacCard ? {card: dacCard, controls: changes} : changes));
  }
  if (networkChanged) {
    const n = await api('setNetwork', JSON.parse(network));
    storedSsid = $('wifiSsid').value;
    savedBssid = $('wifiAp').value;
    $('wifiPassword').placeholder = n.network?.wifi.passwordSet ? 'unchanged' : '';
    $('wifiPassword').value = '';
    $('wifiClearPassword').checked = false;
    saved.network = JSON.stringify(networkSettings());
  }
  msg(networkChanged ? 'Saved. Network reconnects shortly; reopen this page at the configured address.' : 'Saved.');
  if (!networkChanged) controlRefresh();
}

const controlBackend = {
  restarts: false,
  settings: controlSettings,
  save: () => controlSave(),
  inputs: () => api('inputs'),
  saveBindings: (id, bindings) => api('setInputBindings', {id, bindings}),
  async scan() {
    let r = (await api('scanNetworks')).scan;
    for (let i = 0; r.pending && i < 30; i++) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      r = (await api('getNetworks')).scan;
    }
    if (r.pending) throw new Error('scan timed out');
    if (r.error) throw new Error(r.error);
    return r.networks;
  },
  async start() {
    await Promise.all([controlLoad(), controlRefresh(true)]);
    await $('hardwareInputs').inputUsageReady;
    setInterval(() => { if (!document.hidden && !restarting) controlRefresh(); }, 2000);
  }};

// ---------------------------------------------------------------------------
// Board backend: the ESP32's port-80 routes. Settings apply on restart.

let setup = false, codecText = '';
const kHz = r => (r / 1000) + ' kHz';
const bits = t => ({int16: '16-bit', int24in32: '24-bit'})[t] || '32-bit';

async function boardJson(response) {
  const j = await response.json();
  if (!response.ok) throw new Error(j.error || j.message || response.statusText);
  return j;
}

function syncRate() {
  const slave = $('clock').value === 'slave';
  show('rateRow', slave);
  $('codecFormats').textContent = slave ? '16, 24 and 32-bit at the external clock rate, when that is 8 to 48 kHz.' : codecText;
}
$('clock').addEventListener('change', syncRate);

function renderI2sFormats(rates, formats) {
  const on = new Set(formats.map(f => f.sampleRate + '/' + f.sampleType));
  $('i2sFormats').replaceChildren(...rates.map(r => {
    const row = document.createElement('tr'), rate = document.createElement('td');
    rate.textContent = kHz(r);
    row.append(rate);
    for (const t of ['int16', 'int32']) {
      const cell = document.createElement('td'), box = document.createElement('input');
      box.type = 'checkbox';
      box.dataset.rate = r;
      box.dataset.type = t;
      box.checked = on.has(r + '/' + t);
      box.setAttribute('aria-label', kHz(r) + ' ' + bits(t));
      cell.append(box);
      row.append(cell);
    }
    return row;
  }));
}

function describeCodec(formats) {
  const rates = [...new Set(formats.map(f => f.sampleRate))], types = [...new Set(formats.map(f => bits(f.sampleType)))];
  return rates.map(kHz).join(', ') + '; ' + types.join(', ') + '.';
}

function boardBody() {
  const body = {
    ssid: $('wifiSsid').value, bssid: $('wifiAp').value, server: $('server').value.trim(), audioDeviceId: $('device').value,
    name: $('room').value.trim(), clientId: $('clientId').value.trim(), deviceName: $('deviceName').value.trim(),
    ipMode: $('wifiMode').value, ip: $('wifiIp').value.trim(), netmask: $('wifiMask').value.trim(),
    gateway: $('wifiGateway').value.trim(), dns: $('wifiDns').value.trim(),
    i2sClockMode: $('clock').value, i2sSlaveSampleRate: Number($('rate').value), bufferMs: Number($('buffer').value),
    i2sExternalFormats: [...document.querySelectorAll('#i2sFormats input:checked')]
      .map(c => ({sampleRate: Number(c.dataset.rate), sampleType: c.dataset.type}))};
  // An empty field keeps the stored password, unless the network changed: then it is an open one.
  if ($('wifiPassword').value || body.ssid !== storedSsid) body.password = $('wifiPassword').value;
  return body;
}

// What the running client reports, from its control API; there is none in setup mode.
async function boardRefresh(initial = false) {
  try {
    showStatus((await boardJson(await fetch('/api/status'))).status);
  } catch (e) {
    if (initial) throw e;
    statusUnavailable();
    list($('status'), [['Client', e.message]]);
    show('statusBox', true);
  }
}

async function boardLoad(response) {
  const s = await boardJson(response);
  setup = s.setupMode;
  show('restart', true);
  for (const id of ['networkBox', 'wifiAddressBox', 'deviceNameBox', 'i2sClockBox', 'i2sAudioBox', 'hardwareBox']) show(id, true);
  $('wifiSsid').required = true;
  $('deviceName').disabled = false;
  $('buffer').disabled = false;
  $('device').replaceChildren(new Option('Automatic (on-board codec if present)', ''),
    new Option('On-board codec (ES8388)', 'es8388'), new Option('External I2S DAC', 'i2s'));

  storedSsid = s.ssid;
  $('headerDevice').textContent = s.name || s.clientId || s.board;
  $('connection').textContent = setup ? 'Setup mode' : 'Connecting';
  $('about').textContent = s.board + ' \u00b7 ' + (setup
    ? 'Setup mode: this board is not on a network yet. Pick one and save.'
    : 'Saving device settings restarts the board. Button actions apply immediately.');
  $('wifiSsid').value = s.ssid;
  $('wifiPassword').placeholder = s.passwordSet ? 'unchanged' : '';
  savedBssid = s.bssid || '';
  connectedBssid = s.connectedBssid || '';
  fillAps(savedBssid);
  $('room').value = s.name;
  $('clientId').value = savedClientId = s.clientId;
  $('deviceName').value = s.deviceName;
  $('deviceName').placeholder = s.defaultDeviceName;
  $('wifiMode').value = s.ipMode;
  for (const [suffix, key] of [['Ip', 'ip'], ['Mask', 'netmask'], ['Gateway', 'gateway'], ['Dns', 'dns']])
    $('wifi' + suffix).value = s[key];
  addressRows();
  $('wifiNow').textContent = s.currentIp ? 'Now on ' + s.currentIp + '. A wrong static address leaves the board unreachable.' : '';
  $('server').value = s.server;
  $('device').value = s.audioDeviceId;
  $('clock').value = s.i2sClockMode;
  if (![...$('rate').options].some(o => o.value === String(s.i2sSlaveSampleRate)))
    $('rate').add(new Option(s.i2sSlaveSampleRate));
  $('rate').value = String(s.i2sSlaveSampleRate);
  $('buffer').value = s.bufferMs;
  codecText = describeCodec(s.es8388Formats);
  renderI2sFormats(s.i2sRates, s.i2sExternalFormats);
  syncRate();
  saved = boardBackend.settings();
  ready = true;
  updateSave();
  if (!setup && s.inputEventsPort) {
    const url = new URL('/api/input-events', location.href);
    url.port = String(s.inputEventsPort);
    connectHardwareInputEvents($('hardwareInputs'), url.href);
  }
}

const boardBackend = {
  restarts: true,
  settings: () => ({board: JSON.stringify(boardBody())}),
  async save(changed) {
    if (!changed) {
      msg('Saved. Button actions apply immediately.');
      return;
    }
    await boardJson(await fetch('/api/settings', {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(boardBody())}));
    restarting = true;
    msg('Saved. The board restarts now' + (setup ? '; reconnect to your own network.' : '.'));
  },
  inputs: async () => {
    const data = await boardJson(await fetch('/api/inputs'));
    if (!data.ok) throw new Error(data.error || data.message || 'Inputs unavailable');
    return data;
  },
  saveBindings: async (id, bindings) => {
    const result = await boardJson(await fetch('/api/inputs', {method: 'POST',
      headers: {'Content-Type': 'application/json'}, body: JSON.stringify({id, bindings})}));
    if (!result.ok) throw new Error(result.message || result.error || 'Save failed');
    return result;
  },
  scan: async () => boardJson(await fetch('/api/networks')),
  async start(response) {
    await boardLoad(response);
    await Promise.all([refreshInputs(true), setup ? Promise.resolve() : boardRefresh(true)]);
    if (setup) return;
    await $('hardwareInputs').inputUsageReady;
    setInterval(() => { if (!document.hidden && !restarting) { boardRefresh(); refreshInputs(); } }, 2000);
  }};

// ---------------------------------------------------------------------------

const restartLoadRetry = Number(sessionStorage.getItem('galdrunRestartUntil')) > Date.now()
  ? setTimeout(() => location.reload(), 10000) : null;
finishPageLoading((async () => {
  const response = await fetch('/api/settings');
  backend = response.status === 404 ? controlBackend : boardBackend;
  await backend.start(response);
  clearTimeout(restartLoadRetry);
  sessionStorage.removeItem('galdrunRestartUntil');
})());

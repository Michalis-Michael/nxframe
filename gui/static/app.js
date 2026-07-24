'use strict';

const state = {
  config: null,
  interfaces: [],
  cpuProfiles: [],
  cpuProfileWarning: '',
  templates: [],
  senderChannels: [null, null, null, null],
  receiverChannels: [null, null, null, null],
  activeTab: 'admin',
  runtimeStatusTimer: null,
  receiverBitrateSamples: [null, null, null, null],
  dirty: false,
  saving: false
};

const elements = {
  serviceStatus: document.getElementById('service-status'),
  statusContainer: document.querySelector('.topbar-status'),
  configurationState: document.getElementById('configuration-state'),
  notice: document.getElementById('notice'),
  deviceName: document.getElementById('device-name'),
  cpuProfile: document.getElementById('cpu-profile'),
  cpuProfileNote: document.getElementById('cpu-profile-note'),
  networkList: document.getElementById('network-list'),
  sdiList: document.getElementById('sdi-admin-list'),
  saveButton: document.getElementById('save-button'),
  saveSummary: document.getElementById('save-summary'),
  saveDetail: document.getElementById('save-detail'),
  toast: document.getElementById('toast')
};

function clone(value) {
  return value == null ? value : structuredClone(value);
}

function escapeHtml(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

function formatReceiverPid(item) {
  const numeric = Number(item?.pid);
  if (Number.isInteger(numeric) && numeric >= 0) return `0x${numeric.toString(16).toUpperCase().padStart(4, '0')}`;
  const raw = String(item?.pid_hex || '').replace(/^PID\s*/i, '');
  const match = raw.match(/^0x([0-9a-f]+)$/i);
  if (match) return `0x${match[1].toUpperCase().padStart(4, '0')}`;
  return '—';
}

function friendlyCodecName(codec) {
  const key = String(codec || '').toLowerCase();
  const names = {
    h264: 'H.264/AVC', hevc: 'H.265/HEVC', mpeg2video: 'MPEG-2 Video',
    aac: 'AAC', mp2: 'MPEG-1 Layer II', mp3: 'MPEG Audio Layer III',
    s302m: 'SMPTE 302M', ac3: 'Dolby Digital', eac3: 'Dolby Digital Plus',
    pcm_s16le: 'PCM', pcm_s24le: 'PCM', pcm_s32le: 'PCM'
  };
  return names[key] || (codec ? String(codec).toUpperCase() : 'Unknown');
}

function formatStreamBitrate(bitsPerSecond) {
  const value = Number(bitsPerSecond);
  if (!Number.isFinite(value) || value <= 0) return '';
  if (value >= 1000000) return `${(value / 1000000).toFixed(value >= 10000000 ? 1 : 2).replace(/\.0+$/, '')} Mbps`;
  return `${Math.round(value / 1000)} kbps`;
}

function receiverFrameRate(video) {
  const numerator = Number(video?.frame_rate_num);
  const denominator = Number(video?.frame_rate_den);
  return numerator > 0 && denominator > 0 ? numerator / denominator : 0;
}

function formatRateValue(value) {
  if (!Number.isFinite(value) || value <= 0) return '';
  const rounded = Math.abs(value - Math.round(value)) < 0.01 ? Math.round(value) : Number(value.toFixed(2));
  return String(rounded);
}

function receiverVideoFormat(video) {
  const width = Number(video?.width || 0);
  const height = Number(video?.height || 0);
  const frameRate = receiverFrameRate(video);
  const interlaced = Boolean(video?.interlaced);
  const rate = interlaced ? frameRate * 2 : frameRate;
  if (!height) return '';
  const raster = (width === 1280 && height === 720) || (width === 1920 && height === 1080)
    ? `${height}${interlaced ? 'i' : 'p'}${formatRateValue(rate)}`
    : `${width}×${height}${interlaced ? 'i' : 'p'}${formatRateValue(rate)}`;
  return raster;
}


function showToast(message, type = 'success') {
  elements.toast.textContent = message;
  elements.toast.className = `toast visible${type === 'error' ? ' error' : ''}`;
  window.clearTimeout(showToast.timeout);
  showToast.timeout = window.setTimeout(() => {
    elements.toast.className = 'toast';
  }, 4200);
}

function setOnline(online) {
  elements.statusContainer.classList.toggle('online', online);
  elements.statusContainer.classList.toggle('offline', !online);
  elements.serviceStatus.textContent = online ? 'Control plane online' : 'Control plane unavailable';
}

function markDirty() {
  if (!state.config || state.saving) return;
  state.dirty = true;
  elements.saveButton.disabled = false;
  elements.saveSummary.textContent = 'Unsaved configuration';
  elements.saveDetail.textContent = 'Save to persist the current admin settings.';
  elements.configurationState.textContent = 'Modified';
}

function markClean(message = 'Configuration loaded') {
  state.dirty = false;
  elements.saveButton.disabled = true;
  elements.saveSummary.textContent = 'No unsaved changes';
  elements.saveDetail.textContent = message;
  elements.configurationState.textContent = 'Saved';
}

function anyChannelActive() {
  return state.senderChannels.some(channel => channel?.running || channel?.stopping) ||
    state.receiverChannels.some(channel => channel?.running || channel?.stopping);
}

function selectedCpuProfile() {
  return elements.cpuProfile?.value || state.config?.cpu?.profile || 'system_default';
}

function cpuProfileDescription(profile) {
  if (!profile) return 'Selected CPU profile is unavailable.';
  const details = [];
  if (Number(profile.max_frequency_mhz) > 0) details.push(`maximum ${Number(profile.max_frequency_mhz) / 1000} GHz`);
  if (Number(profile.min_frequency_mhz) > 0) details.push(`minimum ${Number(profile.min_frequency_mhz) / 1000} GHz`);
  if (profile.governor) details.push(`governor ${profile.governor}`);
  return [profile.description, details.join(' • ')].filter(Boolean).join(' ');
}

function updateCpuProfileDescription() {
  const profile = state.cpuProfiles.find(item => item.id === selectedCpuProfile());
  if (state.cpuProfileWarning && state.cpuProfiles.length <= 1) {
    elements.cpuProfileNote.textContent = state.cpuProfileWarning;
    return;
  }
  elements.cpuProfileNote.textContent = anyChannelActive()
    ? 'Stop all active SDI channels before changing the CPU profile.'
    : cpuProfileDescription(profile);
}

function updateCpuProfileAvailability() {
  if (!elements.cpuProfile) return;
  elements.cpuProfile.disabled = anyChannelActive();
  updateCpuProfileDescription();
}

function renderCpuProfiles() {
  const current = state.config?.cpu?.profile || 'system_default';
  const profiles = [...state.cpuProfiles];
  if (!profiles.some(item => item.id === current)) {
    profiles.push({ id: current, name: `${current} (unavailable)`, description: 'This saved profile is not available.' });
  }
  elements.cpuProfile.innerHTML = profiles
    .map(profile => selectOption(profile.id, current, profile.name || profile.id))
    .join('');
  updateCpuProfileAvailability();
}

function netmaskToPrefix(netmask) {
  if (!netmask) return 24;
  const octets = netmask.split('.').map(Number);
  if (octets.length !== 4 || octets.some(value => !Number.isInteger(value) || value < 0 || value > 255)) return 24;
  const bits = octets.map(value => value.toString(2).padStart(8, '0')).join('');
  if (!/^1*0*$/.test(bits)) return 24;
  return bits.indexOf('0') === -1 ? 32 : bits.indexOf('0');
}

function prefixToNetmask(prefixValue) {
  const prefix = Number(prefixValue);
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return '';
  const bits = `${'1'.repeat(prefix)}${'0'.repeat(32 - prefix)}`;
  return [0, 8, 16, 24].map(offset => parseInt(bits.slice(offset, offset + 8), 2)).join('.');
}

function roleForInterface(name) {
  const control = state.config?.network?.control?.interface === name;
  const streaming = state.config?.network?.streaming?.interface === name;
  if (control && streaming) return 'both';
  if (control) return 'control';
  if (streaming) return 'streaming';
  return 'unassigned';
}

function settingsForInterface(name) {
  const role = roleForInterface(name);
  if (role === 'control' || role === 'both') return state.config.network.control;
  if (role === 'streaming') return state.config.network.streaming;
  const detected = state.interfaces.find(item => item.name === name) || {};
  return {
    mode: detected.address ? 'static' : 'dhcp',
    address: detected.address || '',
    netmask: detected.netmask || '255.255.255.0',
    gateway: '',
    dns: []
  };
}

function interfaceMeta(item) {
  const parts = [];
  if (item.mac) parts.push(item.mac);
  if (item.speed_mbps) parts.push(item.speed_mbps >= 1000 ? `${item.speed_mbps / 1000} Gb/s` : `${item.speed_mbps} Mb/s`);
  if (item.address) parts.push(item.address);
  return parts.join('  •  ') || 'No link information reported';
}

function renderNetworks() {
  if (!state.interfaces.length) {
    elements.networkList.innerHTML = '<div class="empty-state">No non-loopback network interfaces were detected.</div>';
    return;
  }

  elements.networkList.innerHTML = state.interfaces.map((item, index) => {
    const role = roleForInterface(item.name);
    const settings = settingsForInterface(item.name);
    const mode = settings.mode === 'static' ? 'static' : 'dhcp';
    const dns = Array.isArray(settings.dns) ? settings.dns.join(', ') : '';
    const linkUp = Boolean(item.up && item.running);
    return `
      <article class="network-interface" data-interface="${escapeHtml(item.name)}">
        <div class="interface-head">
          <div class="interface-identity">
            <div class="interface-icon">N${index + 1}</div>
            <div>
              <div class="interface-name">${escapeHtml(item.name)}</div>
              <div class="interface-meta">${escapeHtml(interfaceMeta(item))}</div>
            </div>
          </div>
          <span class="link-badge ${linkUp ? 'up' : ''}">${linkUp ? 'Link up' : 'Link down'}</span>
        </div>
        <div class="interface-body">
          <div class="interface-grid">
            <label class="field wide">
              <span>Interface role</span>
              <select data-field="role">
                <option value="control" ${role === 'control' ? 'selected' : ''}>Management / control</option>
                <option value="streaming" ${role === 'streaming' ? 'selected' : ''}>Streaming network</option>
                <option value="both" ${role === 'both' ? 'selected' : ''}>Management + streaming</option>
                <option value="unassigned" ${role === 'unassigned' ? 'selected' : ''}>Unassigned</option>
              </select>
            </label>
            <label class="field wide">
              <span>Address method</span>
              <select data-field="mode" ${role === 'unassigned' ? 'disabled' : ''}>
                <option value="dhcp" ${mode === 'dhcp' ? 'selected' : ''}>Automatic (DHCP)</option>
                <option value="static" ${mode === 'static' ? 'selected' : ''}>Static IPv4</option>
              </select>
            </label>
            <div class="static-fields ${mode === 'static' && role !== 'unassigned' ? '' : 'hidden-fields'}">
              <label class="field">
                <span>IPv4 address</span>
                <input data-field="address" value="${escapeHtml(settings.address || '')}" placeholder="192.168.10.25">
              </label>
              <label class="field">
                <span>Prefix</span>
                <input data-field="prefix" type="number" min="0" max="32" value="${netmaskToPrefix(settings.netmask)}">
              </label>
              <label class="field">
                <span>Gateway</span>
                <input data-field="gateway" value="${escapeHtml(settings.gateway || '')}" placeholder="Optional">
              </label>
              <label class="field">
                <span>DNS servers</span>
                <input data-field="dns" value="${escapeHtml(dns)}" placeholder="1.1.1.1, 8.8.8.8">
              </label>
            </div>
          </div>
        </div>
      </article>`;
  }).join('');

  elements.networkList.querySelectorAll('select, input').forEach(control => {
    control.addEventListener('change', event => {
      const card = event.target.closest('.network-interface');
      if (event.target.dataset.field === 'role' || event.target.dataset.field === 'mode') updateInterfaceVisibility(card);
      markDirty();
    });
    control.addEventListener('input', markDirty);
  });
}

function updateInterfaceVisibility(card) {
  const role = card.querySelector('[data-field="role"]').value;
  const modeControl = card.querySelector('[data-field="mode"]');
  const staticFields = card.querySelector('.static-fields');
  modeControl.disabled = role === 'unassigned';
  staticFields.classList.toggle('hidden-fields', role === 'unassigned' || modeControl.value !== 'static');
}

function selectedSdiRole(index) {
  const row = elements.sdiList.querySelector(`[data-sdi-index="${index}"]`);
  return row?.querySelector('.segmented button.active')?.dataset.value || state.config?.sdi_ports?.[index]?.role || 'disabled';
}

function renderSdiAssignments() {
  const ports = Array.isArray(state.config.sdi_ports) ? state.config.sdi_ports : [];
  elements.sdiList.innerHTML = ports.map((port, index) => {
    const role = ['sender', 'receiver', 'disabled'].includes(port.role) ? port.role : 'disabled';
    return `
      <div class="sdi-row" data-sdi-index="${index}">
        <div class="sdi-name">${escapeHtml(port.name || `SDI ${index + 1}`)}<small>${escapeHtml(port.id || `sdi${index + 1}`)}</small></div>
        <div class="segmented" role="group" aria-label="${escapeHtml(port.name || `SDI ${index + 1}`)} role">
          <button type="button" data-value="sender" class="${role === 'sender' ? 'active' : ''}">Sender</button>
          <button type="button" data-value="receiver" class="${role === 'receiver' ? 'active' : ''}">Receiver</button>
          <button type="button" data-value="disabled" class="${role === 'disabled' ? 'active' : ''}">Disabled</button>
        </div>
        <label class="field">
          <span>DeckLink device index</span>
          <input data-field="decklink-device" type="number" min="0" max="128" value="${Number.isInteger(port.decklink_device) ? port.decklink_device : index}">
        </label>
      </div>`;
  }).join('');

  elements.sdiList.querySelectorAll('.segmented button').forEach(button => {
    button.addEventListener('click', () => {
      const row = button.closest('.sdi-row');
      const index = Number(row.dataset.sdiIndex);
      button.parentElement.querySelectorAll('button').forEach(item => item.classList.remove('active'));
      button.classList.add('active');
      markDirty();
      updateChannelTabsFromForm();
      renderChannelPanel(index);
    });
  });
  elements.sdiList.querySelectorAll('input').forEach(input => input.addEventListener('input', markDirty));
  updateChannelTabsFromForm();
}

function updateChannelTabsFromForm() {
  document.querySelectorAll('.channel-tab[data-tab^="sdi"]').forEach((tab, index) => {
    const role = selectedSdiRole(index);
    tab.dataset.role = role;
    const senderState = state.senderChannels[index];
    const receiverState = state.receiverChannels[index];
    if (role === 'sender' && senderState?.exists) tab.querySelector('small').textContent = 'sender • configured';
    else if (role === 'receiver' && receiverState?.exists) tab.querySelector('small').textContent = 'receiver • configured';
    else tab.querySelector('small').textContent = role === 'disabled' ? 'Not configured' : role;
  });
}

function templateById(id) {
  return state.templates.find(item => item.id === id) || null;
}

function normalizePairs(settings) {
  const audio = settings.audio || {};
  const count = Math.max(1, Math.floor(Number(audio.input_channels || 2) / 2));
  const existing = Array.isArray(audio.pairs) ? audio.pairs : [];
  const pairs = [];
  for (let index = 0; index < count; index += 1) {
    const current = existing[index] || {};
    const channels = Array.isArray(current.channels) && current.channels.length === 2
      ? current.channels
      : [index * 2 + 1, index * 2 + 2];
    pairs.push({
      name: current.name || `Pair ${index + 1}`,
      channels,
      codec: current.codec || 'disabled',
      bitrate: Number(current.bitrate || audio.bitrate || 192000),
      profile: current.profile || audio.profile || 'aac_low',
      transport: current.transport || audio.transport || 'adts'
    });
  }
  audio.stereo_bitrate = Number(audio.stereo_bitrate || audio.bitrate || 192000);
  audio.stereo_profile = audio.stereo_profile || audio.profile || 'aac_low';
  audio.stereo_transport = audio.stereo_transport || audio.transport || 'adts';
  audio.pairs = pairs;
  settings.audio = audio;
  return settings;
}

function defaultSenderState(index) {
  const template = state.templates[0];
  if (!template) return null;
  const settings = normalizePairs(clone(template.editable));
  return {
    exists: false,
    templateId: template.id,
    configurationName: `SDI ${index + 1} - ${template.name}`,
    settings,
    dirty: false,
    busy: false,
    autosaveState: 'saved',
    autosaveError: '',
    lastSavedSignature: null,
    pendingSave: null,
    savePromise: null,
    running: false,
    stopping: false
  };
}

function ensureSenderState(index) {
  if (!state.senderChannels[index]) state.senderChannels[index] = defaultSenderState(index);
  return state.senderChannels[index];
}

async function loadTemplates() {
  const response = await fetch('/api/sender/templates', { cache: 'no-store' });
  const result = await response.json();
  if (!response.ok || !result.ok) throw new Error(result.error || 'Unable to load GUI encoder templates.');
  state.templates = Array.isArray(result.templates) ? result.templates : [];
  if (!state.templates.length) throw new Error('No valid GUI encoder templates were found.');
  if (result.warning) showToast(result.warning, 'error');
}

async function loadSenderChannel(index) {
  const channel = `sdi${index + 1}`;
  let runtime = { running: false, stopping: false, available: false };
  try {
    const [channelResponse, statusResponse] = await Promise.all([
      fetch(`/api/sender/channels/${channel}`, { cache: 'no-store' }),
      fetch(`/api/sender/status/${channel}`, { cache: 'no-store' })
    ]);
    const result = await channelResponse.json();
    if (!channelResponse.ok || !result.ok) throw new Error(result.error || `Unable to load ${channel}.`);
    if (statusResponse.ok) runtime = await statusResponse.json();
    if (result.exists) {
      const template = templateById(result.template_id) || state.templates[0];
      state.senderChannels[index] = {
        exists: true,
        templateId: template.id,
        configurationName: result.configuration_name || `SDI ${index + 1}`,
        settings: normalizePairs(clone(result.settings || template.editable)),
        dirty: false,
        busy: false,
        autosaveState: 'saved',
        autosaveError: '',
        lastSavedSignature: null,
        pendingSave: null,
        savePromise: null,
        running: Boolean(runtime.running && runtime.role === 'sender'),
        stopping: Boolean(runtime.stopping && runtime.role === 'sender'),
        serviceAvailable: Boolean(runtime.available)
      };
    } else {
      state.senderChannels[index] = defaultSenderState(index);
      state.senderChannels[index].running = Boolean(runtime.running && runtime.role === 'sender');
      state.senderChannels[index].stopping = Boolean(runtime.stopping && runtime.role === 'sender');
      state.senderChannels[index].serviceAvailable = Boolean(runtime.available);
    }
  } catch (error) {
    state.senderChannels[index] = defaultSenderState(index);
    state.senderChannels[index].loadError = error.message;
  }
}

function formatMbps(value) {
  const number = Number(value || 0) / 1000000;
  if (!Number.isFinite(number)) return '0';
  return String(Number(number.toFixed(2)));
}

function formatKbps(value) {
  return Math.round(Number(value || 0) / 1000);
}

function selectOption(value, current, label) {
  return `<option value="${escapeHtml(value)}" ${String(value) === String(current) ? 'selected' : ''}>${escapeHtml(label ?? value)}</option>`;
}

function isAacCodec(codec) {
  return codec === 'aac_lc_mpeg4' || codec === 'aac_lc_mpeg2';
}

function aacBitrateOptions(current) {
  return [64, 96, 128, 160, 192, 224, 256, 320, 384]
    .map(value => selectOption(value, formatKbps(current), `${value} kbps`)).join('');
}

function pairRowsHtml(settings) {
  const pairs = normalizePairs(settings).audio.pairs;
  return pairs.map((pair, index) => `
    <div class="audio-pair-row" data-pair-index="${index}">
      <div class="audio-pair-identity">
        <strong>Pair ${index + 1}</strong>
      </div>
      <label class="field pair-codec"><span>Codec</span><select data-pair-field="codec">
        ${selectOption('disabled', pair.codec, 'Disabled')}
        ${selectOption('aac_lc_mpeg4', pair.codec, 'AAC-LC MPEG-4')}
        ${selectOption('aac_lc_mpeg2', pair.codec, 'AAC-LC MPEG-2')}
        ${selectOption('s302m', pair.codec, 'SMPTE 302M / PCM')}
        ${selectOption('dolby_e', pair.codec, 'Dolby E passthrough')}
      </select></label>
      <div class="audio-pair-aac-settings ${isAacCodec(pair.codec) ? '' : 'hidden'}" data-pair-aac-settings>
        <label class="field"><span>AAC bitrate</span><select data-pair-field="bitrate-kbps">${aacBitrateOptions(pair.bitrate)}</select></label>
        <label class="field"><span>AAC profile</span><select data-pair-field="profile">${selectOption('aac_low', pair.profile, 'AAC-LC')}</select></label>
        <label class="field"><span>AAC transport</span><select data-pair-field="transport">${selectOption('adts', pair.transport, 'ADTS')}</select></label>
      </div>
    </div>`).join('');
}

function interfaceOptions(current) {
  const names = state.interfaces.map(item => item.name);
  return `<option value="" ${current ? '' : 'selected'}>Automatic / routing table</option>` +
    names.map(name => selectOption(name, current, name)).join('');
}

function renderSenderPanel(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureSenderState(index);
  if (!channelState) {
    panel.innerHTML = '<div class="empty-state">No valid GUI encoder templates are available.</div>';
    return;
  }
  const settings = normalizePairs(channelState.settings);
  const video = settings.video;
  const ts = settings.mpegts;
  const audio = settings.audio;
  const streaming = settings.streaming;
  const template = templateById(channelState.templateId) || state.templates[0];

  panel.innerHTML = `
    <div class="page-heading channel-heading">
      <div>
        <p class="eyebrow">CONTRIBUTION SENDER</p>
        <h1>SDI ${index + 1}</h1>
        <p class="page-description">Choose the operating settings for this SDI sender. Changes are saved automatically.</p>
      </div>
      <div class="heading-badge">
        <span class="heading-badge-label">Streaming</span>
        <strong data-sender-status>${channelState.stopping ? 'Stopping' : (channelState.running ? 'On air' : 'Stopped')}</strong>
      </div>
    </div>

    ${channelState.loadError ? `<div class="notice">${escapeHtml(channelState.loadError)}</div>` : ''}

    <section class="card sender-template-card">
      <div class="card-header">
        <div><p class="section-kicker">PROTECTED FOUNDATION</p><h2>Encoder template</h2><p>Advanced x264, GOP, lookahead, colorimetry, and socket values remain controlled by the template.</p></div>
        <span class="card-index">01</span>
      </div>
      <div class="form-grid sender-template-grid">
        <label class="field"><span>Template category</span><select data-sender-field="template">
          ${state.templates.map(item => selectOption(item.id, channelState.templateId, item.name)).join('')}
        </select><small>${escapeHtml(template.description || '')}</small></label>
        <label class="field"><span>Configuration name</span><input data-sender-field="configuration-name" maxlength="96" value="${escapeHtml(channelState.configurationName)}"></label>
      </div>
    </section>

    <section class="card">
      <div class="card-header">
        <div><p class="section-kicker">VIDEO</p><h2>Format and rate control</h2><p>Select the SDI video format, bitrate, bit depth, and chroma.</p></div>
        <span class="card-index">02</span>
      </div>
      <div class="form-grid sender-grid">
        <label class="field"><span>Video format</span><select data-sender-field="video-format">
          ${selectOption('720p50', video.format, '720p50')}
          ${selectOption('720p60', video.format, '720p60')}
          ${selectOption('1080i50', video.format, '1080i50')}
          ${selectOption('1080i60', video.format, '1080i60')}
          ${selectOption('1080p25', video.format, '1080p25')}
          ${selectOption('1080p30', video.format, '1080p30')}
          ${selectOption('1080p50', video.format, '1080p50')}
          ${selectOption('1080p60', video.format, '1080p60')}
        </select></label>
        <label class="field bitrate-field"><span>Video bitrate</span><div class="bitrate-control">
          <div class="unit-input bitrate-manual"><input data-sender-field="bitrate-mbps" type="number" min="0.5" max="300" step="0.1" value="${formatMbps(video.bitrate)}"><span>Mbps</span></div>
          <input class="bitrate-slider" data-sender-field="bitrate-slider" type="range" min="0.5" max="300" step="0.1" value="${formatMbps(video.bitrate)}" aria-label="Video bitrate slider">
        </div></label>
        <label class="field"><span>Rate control</span><select data-sender-field="rate-control">
          ${selectOption('cbr', video.rate_control, 'CBR')}${selectOption('vbr', video.rate_control, 'VBR')}
        </select></label>
        <label class="field"><span>Chroma</span><select data-sender-field="chroma">
          ${selectOption('422', video.chroma, '4:2:2')}${selectOption('420', video.chroma, '4:2:0')}
        </select></label>
        <label class="field"><span>Bit depth</span><select data-sender-field="bit-depth">
          ${selectOption(10, video.bit_depth, '10-bit')}${selectOption(8, video.bit_depth, '8-bit')}
        </select></label>
        <label class="field"><span>H.264 level</span><select data-sender-field="h264-level">
          ${selectOption('auto', video.level || 'auto', 'Auto')}
          ${selectOption('3.0', video.level, '3.0')}${selectOption('3.1', video.level, '3.1')}
          ${selectOption('3.2', video.level, '3.2')}${selectOption('4.0', video.level, '4.0')}
          ${selectOption('4.1', video.level, '4.1')}${selectOption('4.2', video.level, '4.2')}
          ${selectOption('5.0', video.level, '5.0')}${selectOption('5.1', video.level, '5.1')}
          ${selectOption('5.2', video.level, '5.2')}
        </select><small>Auto is recommended. A manual level below the video requirement is rejected.</small></label>
        <label class="field ${String(video.format).startsWith('1080i') ? '' : 'hidden'}" data-field-order-wrap><span>Field order</span><select data-sender-field="field-order" required>
          ${selectOption('tff', video.field_order || 'tff', 'TFF — Top field first')}
          ${selectOption('bff', video.field_order, 'BFF — Bottom field first')}
        </select></label>
      </div>
    </section>

    <section class="card">
      <div class="card-header">
        <div><p class="section-kicker">MPEG-TS</p><h2>Transport stream</h2><p>Choose whether the stream uses a constant transport rate with null-packet stuffing.</p></div>
        <span class="card-index">03</span>
      </div>
      <div class="form-grid sender-grid">
        <label class="field"><span>Service provider</span><input data-sender-field="service-provider" maxlength="64" value="${escapeHtml(ts.service_provider)}"></label>
        <label class="field"><span>Service name</span><input data-sender-field="service-name" maxlength="96" value="${escapeHtml(ts.service_name)}"></label>
        <label class="switch-field"><input data-sender-field="constant-ts" type="checkbox" ${ts.constant_rate ? 'checked' : ''}><span><strong>Constant-rate TS</strong><small>Enable null-packet stuffing and reserve sufficient transport capacity.</small></span></label>
        <div class="field mpegts-rate-field ${ts.constant_rate ? '' : 'hidden'}" data-mpegts-rate-wrap>
          <span>MPEG-TS rate</span>
          <div class="mpegts-rate-control">
            <div class="unit-input"><input data-sender-field="muxrate-mbps" type="number" min="0.1" max="400" step="0.1" value="${formatMbps(ts.muxrate)}"><span>Mbps</span></div>
            <label class="auto-rate-toggle"><input data-sender-field="auto-muxrate" type="checkbox" ${ts.auto_muxrate !== false ? 'checked' : ''}><span>Auto</span></label>
          </div>
        </div>
      </div>
    </section>

    <section class="card">
      <div class="card-header">
        <div><p class="section-kicker">AUDIO</p><h2>SDI audio pairs</h2><p>Select the input channel count and codec for each stereo pair. NxFrame SDI audio currently operates at 48 kHz.</p></div>
        <span class="card-index">04</span>
      </div>
      <div class="form-grid sender-grid audio-master-grid">
        <label class="switch-field"><input data-sender-field="split-pairs" type="checkbox" ${audio.split_pairs ? 'checked' : ''}><span><strong>Separate audio streams per pair</strong><small>Allows each pair to use its own codec, including Dolby E passthrough.</small></span></label>
        <label class="field"><span>SDI input channels</span><select data-sender-field="input-channels">
          ${[2, 4, 8, 16].map(value => selectOption(value, audio.input_channels, `${value} channels`)).join('')}
        </select></label>
        <label class="field"><span>Sample rate</span><select data-sender-field="sample-rate">${selectOption(48000, audio.sample_rate, '48 kHz')}</select></label>
        <label class="field compact-audio-codec" data-stereo-codec-wrap><span>Single stereo stream codec</span><select data-sender-field="stereo-codec">
          ${selectOption('aac_lc_mpeg4', audio.stereo_codec || 'aac_lc_mpeg4', 'AAC-LC MPEG-4')}
          ${selectOption('aac_lc_mpeg2', audio.stereo_codec, 'AAC-LC MPEG-2')}
          ${selectOption('s302m', audio.stereo_codec, 'SMPTE 302M / PCM')}
          ${selectOption('dolby_e', audio.stereo_codec, 'Dolby E passthrough')}
        </select></label>
        <div class="stereo-aac-settings ${!audio.split_pairs && isAacCodec(audio.stereo_codec) ? '' : 'hidden'}" data-stereo-aac-settings>
          <label class="field"><span>AAC bitrate</span><select data-sender-field="stereo-bitrate-kbps">${aacBitrateOptions(audio.stereo_bitrate)}</select></label>
          <label class="field"><span>AAC profile</span><select data-sender-field="stereo-profile">${selectOption('aac_low', audio.stereo_profile, 'AAC-LC')}</select></label>
          <label class="field"><span>AAC transport</span><select data-sender-field="stereo-transport">${selectOption('adts', audio.stereo_transport, 'ADTS')}</select></label>
        </div>
      </div>
      <div class="audio-pairs" data-audio-pairs>${pairRowsHtml(settings)}</div>
    </section>

    <section class="card">
      <div class="card-header">
        <div><p class="section-kicker">STREAMING</p><h2>Protocol and destination</h2><p>Configure the transport protocol and destination for this SDI sender.</p></div>
        <span class="card-index">05</span>
      </div>
      <div class="form-grid sender-grid">
        <label class="field"><span>Protocol</span><select data-sender-field="protocol">
          ${selectOption('srt', streaming.protocol, 'SRT')}${selectOption('udp', streaming.protocol, 'UDP')}${selectOption('rtp', streaming.protocol, 'RTP')}
        </select></label>
        <label class="field"><span data-address-label>${streaming.protocol === 'srt' && streaming.mode === 'listener' ? 'Bind address' : 'Destination address'}</span><input data-sender-field="address" value="${escapeHtml(streaming.address || '')}" placeholder="0.0.0.0 or 192.168.10.20"></label>
        <label class="field"><span>Port</span><input data-sender-field="port" type="number" min="1" max="65535" value="${Number(streaming.port || 5000)}"></label>
      </div>
      <div class="form-grid sender-grid protocol-details" data-srt-fields>
        <label class="field"><span>SRT mode</span><select data-sender-field="srt-mode">
          ${selectOption('listener', streaming.mode || 'listener', 'Listener')}${selectOption('caller', streaming.mode, 'Caller')}${selectOption('rendezvous', streaming.mode, 'Rendezvous')}
        </select></label>
        <label class="field"><span>Latency</span><div class="unit-input"><input data-sender-field="srt-latency" type="number" min="20" max="30000" value="${Number(streaming.latency || 120)}"><span>ms</span></div></label>
        <label class="field"><span>Stream ID</span><input data-sender-field="streamid" maxlength="512" value="${escapeHtml(streaming.streamid || '')}" placeholder="Optional"></label>
        <label class="field"><span>Passphrase</span><input data-sender-field="passphrase" type="password" maxlength="79" value="${escapeHtml(streaming.passphrase || '')}" placeholder="Empty or 10–79 characters"></label>
        <label class="field"><span>AES key length</span><select data-sender-field="pbkeylen">
          ${selectOption(16, streaming.pbkeylen || 16, '16 bytes / AES-128')}${selectOption(24, streaming.pbkeylen, '24 bytes / AES-192')}${selectOption(32, streaming.pbkeylen, '32 bytes / AES-256')}
        </select></label>
      </div>
      <div class="form-grid sender-grid protocol-details" data-udp-fields>
        <label class="field"><span>Output interface</span><select data-sender-field="stream-interface">${interfaceOptions(streaming.interface || '')}</select></label>
        <label class="field"><span>TTL</span><input data-sender-field="ttl" type="number" min="0" max="255" value="${Number(streaming.ttl ?? 16)}"></label>
      </div>
    </section>

    <div class="save-bar sender-save-bar">
      <div><strong data-sender-save-summary>${channelState.autosaveState === 'saving' ? 'Saving changes…' : (channelState.autosaveState === 'error' ? 'Autosave paused' : 'Changes saved automatically')}</strong><span data-sender-save-detail>${channelState.autosaveState === 'error' ? escapeHtml(channelState.autosaveError || 'Correct the highlighted settings to continue.') : 'Start Streaming performs a final save and starts this SDI channel.'}</span></div>
      <div class="button-cluster">
        <button class="secondary-button" type="button" data-sender-action="validate" ${channelState.running || channelState.stopping ? 'disabled' : ''}>Validate</button>
        <button class="primary-button ${channelState.running ? 'stop-button' : ''}" type="button" data-sender-action="stream" ${channelState.stopping ? 'disabled' : ''}><span>${channelState.running ? 'Stop Streaming' : 'Start Streaming'}</span><span class="button-arrow">${channelState.running ? '■' : '→'}</span></button>
      </div>
    </div>`;

  bindSenderPanel(index);
}


function defaultReceiverSettings(index) {
  return {
    configuration_name: `SDI ${index + 1} Receiver`,
    input: {
      protocol: 'srt',
      mode: 'listener',
      address: '0.0.0.0',
      port: 5000,
      latency: 250,
      streamid: '',
      passphrase: '',
      pbkeylen: 0,
      interface: ''
    },
    output: { packed_audio_channels: 16 },
    audio_pair_route: [1, 2, 3, 4, 5, 6, 7, 8]
  };
}

function defaultReceiverState(index) {
  return {
    exists: false,
    settings: defaultReceiverSettings(index),
    running: false,
    stopping: false,
    busy: false,
    autosaveState: 'saved',
    autosaveError: '',
    lastSavedSignature: null,
    pendingSave: null,
    savePromise: null,
    receiverState: null,
    serviceAvailable: false
  };
}

function ensureReceiverState(index) {
  if (!state.receiverChannels[index]) state.receiverChannels[index] = defaultReceiverState(index);
  return state.receiverChannels[index];
}

async function loadReceiverChannel(index) {
  const channel = `sdi${index + 1}`;
  try {
    const [configResponse, statusResponse] = await Promise.all([
      fetch(`/api/receiver/channels/${channel}`, { cache: 'no-store' }),
      fetch(`/api/receiver/status/${channel}`, { cache: 'no-store' })
    ]);
    const configResult = await configResponse.json();
    const runtime = statusResponse.ok ? await statusResponse.json() : {};
    if (!configResponse.ok || !configResult.ok) throw new Error(configResult.error || `Unable to load ${channel} receiver.`);
    const settings = clone(configResult.settings || defaultReceiverSettings(index));
    if (!Array.isArray(settings.audio_pair_route)) settings.audio_pair_route = [1, 2, 3, 4, 5, 6, 7, 8];
    state.receiverChannels[index] = {
      exists: Boolean(configResult.exists),
      settings,
      running: Boolean(runtime.running && runtime.role === 'receiver'),
      stopping: Boolean(runtime.stopping && runtime.role === 'receiver'),
      busy: false,
      autosaveState: 'saved',
      autosaveError: '',
      lastSavedSignature: null,
      pendingSave: null,
      savePromise: null,
      receiverState: runtime.receiver_state || null,
      serviceAvailable: Boolean(runtime.available)
    };
  } catch (error) {
    state.receiverChannels[index] = defaultReceiverState(index);
    state.receiverChannels[index].loadError = error.message;
  }
}

function receiverSourceLabel(pair) {
  const logical = Number(pair.logical_pair || 0);
  const pid = formatReceiverPid(pair);
  return pid === '—' ? `Pair ${logical}` : `Pair ${logical} · PID ${pid}`;
}

function receiverRouteOptions(index, selected) {
  const channelState = ensureReceiverState(index);
  const livePairs = Array.isArray(channelState.receiverState?.source_pairs) ? channelState.receiverState.source_pairs : [];
  const values = new Map();
  livePairs.forEach(pair => values.set(Number(pair.logical_pair), receiverSourceLabel(pair)));
  const maxSaved = Math.max(0, ...((channelState.settings.audio_pair_route || []).map(Number)));
  for (let value = 1; value <= maxSaved; value += 1) {
    if (!values.has(value)) values.set(value, `Input Pair ${value}`);
  }
  return selectOption(0, selected, 'Mute') + [...values.entries()]
    .sort((a, b) => a[0] - b[0])
    .map(([value, label]) => selectOption(value, selected, label)).join('');
}

function receiverPidSummary(channelState) {
  const live = channelState.receiverState;
  if (!channelState.running) return '<div class="empty-state receiver-waiting">Start the receiver to inspect the incoming MPEG-TS.</div>';
  if (!live || !live.video) return '<div class="empty-state receiver-waiting">Waiting for MPEG-TS lock and stream information…</div>';

  const video = live.video || {};
  const stats = live.stats || {};
  const audio = Array.isArray(live.audio_streams) ? live.audio_streams : [];
  const videoTags = [
    Number(video.bit_depth) > 0 ? `${Number(video.bit_depth)}-bit` : '',
    video.chroma ? (String(video.chroma).toLowerCase() === 'rgb' ? 'RGB' : `4:${String(video.chroma)[1] || '2'}:${String(video.chroma)[2] || '0'}`) : '',
    video.field_order === 'tff' ? 'TFF' : (video.field_order === 'bff' ? 'BFF' : '')
  ].filter(Boolean);
  const measuredVideoBitrate = Number(stats.video_bitrate_bps);
  const signalledVideoBitrate = Number(video.bit_rate);
  const videoBitrate = measuredVideoBitrate > 0 ? measuredVideoBitrate : signalledVideoBitrate;
  const decodeStats = [
    ['Video bitrate', videoBitrate > 0 ? formatStreamBitrate(videoBitrate) : 'Acquiring'],
    ['Decoder queue', `${Number(stats.decoder_video_queue || 0)} / ${Number(stats.decoder_video_queue_high_water || 0)} peak`],
    ['Demux queue', `${Number(stats.demux_video_queue || 0)} video · ${Number(stats.demux_audio_queue || 0)} audio`],
    ['Decode drops', String(Number(stats.decoder_video_drops || 0))],
    ['TS continuity', String(Number(stats.continuity_errors || 0))]
  ];

  return `
    <div class="receiver-stream-grid">
      <article class="receiver-stream-card video-stream-card">
        <div class="receiver-stream-title"><span>VIDEO</span><strong>PID ${escapeHtml(formatReceiverPid(video))}</strong></div>
        <h3>${escapeHtml(friendlyCodecName(video.codec))}${receiverVideoFormat(video) ? ` · ${escapeHtml(receiverVideoFormat(video))}` : ''}</h3>
        ${videoTags.length ? `<div class="receiver-stream-tags">${videoTags.map(tag => `<span>${escapeHtml(tag)}</span>`).join('')}</div>` : ''}
        <div class="receiver-live-stats">${decodeStats.map(([label, value]) => `<div><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`).join('')}</div>
      </article>
      ${audio.map((item, audioIndex) => {
        const tags = [
          Number(item.channels) > 0 ? `${Number(item.channels)} ch` : '',
          Number(item.sample_rate) > 0 ? `${Number(item.sample_rate) / 1000} kHz` : '',
          formatStreamBitrate(item.bit_rate)
        ].filter(Boolean);
        return `
        <article class="receiver-stream-card audio-stream-card">
          <div class="receiver-stream-title"><span>AUDIO ${audioIndex + 1}</span><strong>PID ${escapeHtml(formatReceiverPid(item))}</strong></div>
          <h3>${escapeHtml(friendlyCodecName(item.codec))}</h3>
          <div class="receiver-stream-tags">${tags.map(tag => `<span>${escapeHtml(tag)}</span>`).join('')}</div>
        </article>`;
      }).join('')}
    </div>`;
}

function renderReceiverPanel(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureReceiverState(index);
  const settings = channelState.settings || defaultReceiverSettings(index);
  const input = settings.input || {};
  const outputChannels = Number(settings.output?.packed_audio_channels || 16);
  const outputPairs = Math.max(1, outputChannels / 2);
  const route = Array.isArray(settings.audio_pair_route) ? settings.audio_pair_route : [];
  const protocol = input.protocol || 'srt';
  const isSrt = protocol === 'srt';

  panel.innerHTML = `
    <div class="page-heading channel-heading">
      <div><p class="section-kicker">SDI ${index + 1} · RECEIVER</p><h1>Receiver input and SDI routing</h1>
      <p class="page-description">Receive MPEG-TS over SRT, UDP, or RTP and route detected audio pairs to the SDI output.</p></div>
      <div class="heading-badge"><span>STATUS</span><strong data-receiver-runtime-status>${channelState.stopping ? 'Stopping' : (channelState.running ? 'On air' : 'Stopped')}</strong></div>
    </div>

    <section class="card">
      <div class="card-heading"><div><p class="section-kicker">INPUT</p><h2>Transport input</h2></div></div>
      <div class="form-grid receiver-grid">
        <label class="field receiver-name-field"><span>Configuration name</span><input data-receiver-field="configuration-name" maxlength="96" value="${escapeHtml(settings.configuration_name || '')}"></label>
        <label class="field"><span>Protocol</span><select data-receiver-field="protocol">
          ${selectOption('srt', protocol, 'SRT')}${selectOption('udp', protocol, 'UDP')}${selectOption('rtp', protocol, 'RTP')}
        </select></label>
        <label class="field"><span data-receiver-address-label>${isSrt && input.mode === 'caller' ? 'Remote address' : (protocol === 'srt' ? 'Bind / remote address' : 'Bind or multicast address')}</span>
          <input data-receiver-field="address" value="${escapeHtml(input.address || '')}" placeholder="0.0.0.0 or 239.1.1.1"></label>
        <label class="field"><span>Port</span><input data-receiver-field="port" type="number" min="1" max="65535" value="${Number(input.port || 5000)}"></label>
      </div>
      <div class="form-grid receiver-grid protocol-details ${isSrt ? '' : 'hidden'}" data-receiver-srt-fields>
        <label class="field"><span>SRT mode</span><select data-receiver-field="mode">
          ${selectOption('caller', input.mode, 'Caller')}${selectOption('listener', input.mode, 'Listener')}${selectOption('rendezvous', input.mode, 'Rendezvous')}
        </select></label>
        <label class="field"><span>Latency</span><div class="unit-input"><input data-receiver-field="latency" type="number" min="20" max="30000" value="${Number(input.latency || 250)}"><span>ms</span></div></label>
        <label class="field"><span>Stream ID</span><input data-receiver-field="streamid" maxlength="512" value="${escapeHtml(input.streamid || '')}" placeholder="Optional"></label>
        <label class="field"><span>Passphrase</span><input data-receiver-field="passphrase" type="password" maxlength="79" value="${escapeHtml(input.passphrase || '')}" placeholder="Empty or 10–79 characters"></label>
        <label class="field"><span>AES key length</span><select data-receiver-field="pbkeylen">
          ${selectOption(0, input.pbkeylen, 'No encryption')}${selectOption(16, input.pbkeylen, 'AES-128')}${selectOption(24, input.pbkeylen, 'AES-192')}${selectOption(32, input.pbkeylen, 'AES-256')}
        </select></label>
      </div>
      <div class="form-grid receiver-grid protocol-details ${isSrt ? 'hidden' : ''}" data-receiver-udp-fields>
        <label class="field"><span>Input interface</span><select data-receiver-field="interface">${interfaceOptions(input.interface || '')}</select></label>
      </div>
    </section>

    <section class="card receiver-program-card">
      <div class="card-heading"><div><p class="section-kicker">MPEG-TS</p><h2>Detected streams</h2><p>Elementary stream PIDs appear after the receiver locks to the incoming transport stream.</p></div></div>
      <div data-receiver-pid-summary>${receiverPidSummary(channelState)}</div>
    </section>

    <section class="card">
      <div class="card-heading"><div><p class="section-kicker">AUDIO OUTPUT</p><h2>SDI pair routing</h2><p>Any detected input pair can be routed to one or more SDI output pairs.</p></div></div>
      <div class="form-grid receiver-grid receiver-output-grid">
        <label class="field"><span>SDI audio output</span><select data-receiver-field="packed-audio-channels">
          ${selectOption(2, outputChannels, '2 channels / 1 pair')}${selectOption(4, outputChannels, '4 channels / 2 pairs')}${selectOption(8, outputChannels, '8 channels / 4 pairs')}${selectOption(16, outputChannels, '16 channels / 8 pairs')}
        </select></label>
      </div>
      <div class="receiver-route-list" data-receiver-route-list>
        ${Array.from({ length: outputPairs }, (_, routeIndex) => `
          <div class="receiver-route-row">
            <div><strong>Out Pair ${routeIndex + 1}</strong><small>Ch ${routeIndex * 2 + 1}/${routeIndex * 2 + 2}</small></div>
            <select data-receiver-route="${routeIndex}">${receiverRouteOptions(index, Number(route[routeIndex] ?? routeIndex + 1))}</select>
          </div>`).join('')}
      </div>
    </section>

    <div class="save-bar sender-save-bar">
      <div><strong data-receiver-save-summary>${channelState.autosaveState === 'saving' ? 'Saving changes…' : (channelState.autosaveState === 'error' ? 'Autosave paused' : 'Changes saved automatically')}</strong>
      <span data-receiver-save-detail>${channelState.autosaveState === 'error' ? escapeHtml(channelState.autosaveError || 'Correct the receiver settings to continue.') : 'Start Receiver performs a final save and starts SDI playout.'}</span></div>
      <div class="button-cluster">
        <button class="secondary-button" type="button" data-receiver-action="validate" ${channelState.running || channelState.stopping ? 'disabled' : ''}>Validate</button>
        <button class="primary-button ${channelState.running ? 'stop-button' : ''}" type="button" data-receiver-action="run" ${channelState.stopping ? 'disabled' : ''}><span>${channelState.running ? 'Stop Receiver' : 'Start Receiver'}</span><span class="button-arrow">${channelState.running ? '■' : '→'}</span></button>
      </div>
    </div>`;

  panel.querySelectorAll('[data-receiver-field], [data-receiver-route]').forEach(control => {
    control.addEventListener('change', async () => {
      updateReceiverFieldVisibility(panel);
      const routeChanged = control.hasAttribute('data-receiver-route');
      const outputLayoutChanged = control.dataset.receiverField === 'packed-audio-channels';
      const saved = await saveReceiverNow(index, true);
      if (saved && routeChanged && channelState.running) await updateLiveReceiverRoute(index);
      if (outputLayoutChanged) renderReceiverPanel(index);
    });
  });
  if (channelState.running || channelState.stopping) {
    panel.querySelectorAll('[data-receiver-field]').forEach(control => { control.disabled = true; });
  }
  panel.querySelector('[data-receiver-action="validate"]')?.addEventListener('click', () => validateReceiver(index));
  panel.querySelector('[data-receiver-action="run"]')?.addEventListener('click', () => toggleReceiver(index));
  updateReceiverFieldVisibility(panel);
}

function updateReceiverFieldVisibility(panel) {
  const protocol = panel.querySelector('[data-receiver-field="protocol"]')?.value || 'srt';
  panel.querySelector('[data-receiver-srt-fields]')?.classList.toggle('hidden', protocol !== 'srt');
  panel.querySelector('[data-receiver-udp-fields]')?.classList.toggle('hidden', protocol === 'srt');
  const mode = panel.querySelector('[data-receiver-field="mode"]')?.value || 'listener';
  const label = panel.querySelector('[data-receiver-address-label]');
  if (label) label.textContent = protocol === 'srt'
    ? (mode === 'caller' ? 'Remote address' : (mode === 'listener' ? 'Bind address' : 'Peer address'))
    : 'Bind or multicast address';
}

function collectReceiverRequest(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const field = name => panel.querySelector(`[data-receiver-field="${name}"]`)?.value ?? '';
  const packed = Number(field('packed-audio-channels') || 16);
  const outputPairs = packed / 2;
  const route = Array.from({ length: outputPairs }, (_, routeIndex) => Number(panel.querySelector(`[data-receiver-route="${routeIndex}"]`)?.value ?? routeIndex + 1));
  const protocol = field('protocol') || 'srt';
  return {
    configuration_name: field('configuration-name').trim(),
    input: {
      protocol,
      mode: protocol === 'srt' ? field('mode') : 'listener',
      address: field('address').trim(),
      port: Number(field('port')),
      latency: protocol === 'srt' ? Number(field('latency')) : 120,
      streamid: protocol === 'srt' ? field('streamid') : '',
      passphrase: protocol === 'srt' ? field('passphrase') : '',
      pbkeylen: protocol === 'srt' ? Number(field('pbkeylen')) : 0,
      interface: protocol === 'srt' ? '' : field('interface')
    },
    output: { packed_audio_channels: packed },
    audio_pair_route: route
  };
}

function updateReceiverSaveUi(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureReceiverState(index);
  const summary = panel.querySelector('[data-receiver-save-summary]');
  const detail = panel.querySelector('[data-receiver-save-detail]');
  if (!summary || !detail) return;
  if (channelState.autosaveState === 'saving') {
    summary.textContent = 'Saving changes…'; detail.textContent = 'Updating this SDI receiver configuration.';
  } else if (channelState.autosaveState === 'error') {
    summary.textContent = 'Autosave paused'; detail.textContent = channelState.autosaveError || 'Correct the receiver settings to continue.';
  } else {
    summary.textContent = 'Changes saved automatically'; detail.textContent = 'Start Receiver performs a final save and starts SDI playout.';
  }
}

async function saveReceiverNow(index, silent = true, force = false) {
  const channelState = ensureReceiverState(index);
  let request;
  try {
    request = collectReceiverRequest(index);
  } catch (error) {
    channelState.autosaveState = 'error';
    channelState.autosaveError = error.message;
    updateReceiverSaveUi(index);
    if (!silent) showToast(error.message, 'error');
    return false;
  }

  const signature = JSON.stringify(request);
  if (!force && channelState.exists && signature === channelState.lastSavedSignature) {
    channelState.autosaveState = 'saved';
    channelState.autosaveError = '';
    updateReceiverSaveUi(index);
    return true;
  }

  channelState.pendingSave = { request, signature, silent, force };
  channelState.autosaveState = 'saving';
  channelState.autosaveError = '';
  updateReceiverSaveUi(index);

  if (channelState.savePromise) return channelState.savePromise;

  channelState.savePromise = (async () => {
    let success = true;
    while (channelState.pendingSave) {
      const job = channelState.pendingSave;
      channelState.pendingSave = null;
      if (!job.force && channelState.exists && job.signature === channelState.lastSavedSignature) continue;

      try {
        const response = await fetch(`/api/receiver/channels/sdi${index + 1}`, {
          method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(job.request)
        });
        const result = await response.json();
        if (!response.ok || !result.ok) throw new Error(result.error || 'Receiver save failed.');
        channelState.settings = clone(result.settings || job.request);
        channelState.exists = true;
        channelState.lastSavedSignature = job.signature;
        channelState.autosaveState = 'saved';
        channelState.autosaveError = '';
        updateReceiverSaveUi(index);
        updateChannelTabsFromForm();
      } catch (error) {
        success = false;
        channelState.pendingSave = null;
        channelState.autosaveState = 'error';
        channelState.autosaveError = error.message;
        updateReceiverSaveUi(index);
        if (!job.silent) showToast(error.message, 'error');
        break;
      }
    }
    return success;
  })().finally(() => {
    channelState.savePromise = null;
  });

  return channelState.savePromise;
}

async function validateReceiver(index) {
  try {
    const response = await fetch(`/api/receiver/validate/sdi${index + 1}`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(collectReceiverRequest(index))
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Receiver validation failed.');
    showToast(`SDI ${index + 1} receiver configuration is valid.`);
  } catch (error) { showToast(error.message, 'error'); }
}

async function updateLiveReceiverRoute(index) {
  const channelState = ensureReceiverState(index);
  if (!channelState.running) return;
  try {
    const request = collectReceiverRequest(index);
    const response = await fetch(`/api/receiver/route/sdi${index + 1}`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ audio_pair_route: request.audio_pair_route })
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Live audio routing update failed.');
  } catch (error) { showToast(error.message, 'error'); }
}

async function toggleReceiver(index) {
  const channelState = ensureReceiverState(index);
  if (channelState.busy || channelState.stopping) return;
  channelState.busy = true;
  const channel = `sdi${index + 1}`;
  try {
    if (channelState.running) {
      const response = await fetch(`/api/receiver/stop/${channel}`, { method: 'POST' });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || 'Unable to stop receiver.');
      channelState.stopping = true;
      renderReceiverPanel(index);
      updateCpuProfileAvailability();
      scheduleRuntimeStatusRefresh(500);
      return;
    }
    if (!(await saveReceiverNow(index, false, true))) return;
    const response = await fetch(`/api/receiver/start/${channel}`, { method: 'POST' });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Unable to start receiver.');
    channelState.running = Boolean(result.running);
    channelState.stopping = Boolean(result.stopping);
    renderReceiverPanel(index);
    updateCpuProfileAvailability();
    scheduleRuntimeStatusRefresh(1000);
    showToast(`SDI ${index + 1} receiver started.`);
  } catch (error) { showToast(error.message, 'error'); }
  finally { channelState.busy = false; }
}

function updateReceiverBitrateSample(index, receiverState) {
  const stats = receiverState?.stats;
  if (!stats) return;
  const currentTime = Number(stats.sample_time_ms);
  const currentBytes = Number(stats.video_packet_bytes_total);
  const generation = Number(stats.source_generation || 0);
  const previous = state.receiverBitrateSamples[index];
  if (previous && previous.generation === generation && currentTime > previous.time && currentBytes >= previous.bytes) {
    stats.video_bitrate_bps = ((currentBytes - previous.bytes) * 8000) / (currentTime - previous.time);
  }
  if (Number.isFinite(currentTime) && Number.isFinite(currentBytes)) {
    state.receiverBitrateSamples[index] = { time: currentTime, bytes: currentBytes, generation };
  }
}

function updateReceiverRouteOptions(index, panel) {
  const routeList = panel.querySelector('[data-receiver-route-list]');
  if (!routeList) return;
  const focused = document.activeElement;
  if (focused && routeList.contains(focused)) return;

  const channelState = ensureReceiverState(index);
  const pairs = Array.isArray(channelState.receiverState?.source_pairs) ? channelState.receiverState.source_pairs : [];
  const signature = JSON.stringify(pairs.map(pair => [pair.logical_pair, pair.pid, pair.pid_hex]));
  if (routeList.dataset.sourceSignature === signature) return;

  routeList.querySelectorAll('[data-receiver-route]').forEach(select => {
    const selected = Number(select.value || 0);
    select.innerHTML = receiverRouteOptions(index, selected);
    select.value = String(selected);
  });
  routeList.dataset.sourceSignature = signature;
}

function updateReceiverRuntimeUi(index) {
  if (selectedSdiRole(index) !== 'receiver') return;
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  if (!panel) return;
  const channelState = ensureReceiverState(index);
  const statusText = channelState.stopping ? 'Stopping' : (channelState.running ? 'On air' : 'Stopped');
  const status = panel.querySelector('[data-receiver-runtime-status]');
  if (status) status.textContent = statusText;

  const runButton = panel.querySelector('[data-receiver-action="run"]');
  if (runButton) {
    runButton.disabled = Boolean(channelState.stopping);
    runButton.classList.toggle('stop-button', Boolean(channelState.running));
    const text = runButton.querySelector('span:first-child');
    const arrow = runButton.querySelector('.button-arrow');
    if (text) text.textContent = channelState.running ? 'Stop Receiver' : 'Start Receiver';
    if (arrow) arrow.textContent = channelState.running ? '■' : '→';
  }
  const validate = panel.querySelector('[data-receiver-action="validate"]');
  if (validate) validate.disabled = Boolean(channelState.running || channelState.stopping);
  const fieldsLocked = Boolean(channelState.running || channelState.stopping);
  panel.querySelectorAll('[data-receiver-field]').forEach(control => {
    if (control.disabled !== fieldsLocked) control.disabled = fieldsLocked;
  });

  const summary = panel.querySelector('[data-receiver-pid-summary]');
  if (summary) {
    const html = receiverPidSummary(channelState);
    if (summary.innerHTML !== html) summary.innerHTML = html;
  }
  updateReceiverRouteOptions(index, panel);
}

async function refreshReceiverRuntimeStatus(index) {
  const channelState = ensureReceiverState(index);
  try {
    const response = await fetch(`/api/receiver/status/sdi${index + 1}`, { cache: 'no-store' });
    const result = await response.json();
    if (!response.ok || !result.ok) return;
    channelState.running = Boolean(result.running && result.role === 'receiver');
    channelState.stopping = Boolean(result.stopping && result.role === 'receiver');
    channelState.receiverState = result.receiver_state || null;
    channelState.serviceAvailable = Boolean(result.available);
    if (channelState.running) updateReceiverBitrateSample(index, channelState.receiverState);
    else state.receiverBitrateSamples[index] = null;
    updateReceiverRuntimeUi(index);
    updateCpuProfileAvailability();
  } catch { /* receiver polling must not interrupt operator input */ }
}

function renderChannelPlaceholder(index, role) {
  const port = state.config?.sdi_ports?.[index] || {};
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const receiver = role === 'receiver';
  panel.innerHTML = `
    <div class="placeholder-panel">
      <div>
        <span class="placeholder-role">${escapeHtml(role)}</span>
        <strong>${escapeHtml(port.name || `SDI ${index + 1}`)} ${receiver ? 'receiver' : 'workspace'}</strong>
        <p>${receiver ? 'Configure this receiver input and route detected MPEG-TS audio pairs to the SDI output.' : 'Enable this SDI connector as Sender or Receiver from the Admin tab.'}</p>
      </div>
    </div>`;
}

function renderChannelPanel(index) {
  const role = selectedSdiRole(index);
  if (role === 'sender') renderSenderPanel(index);
  else if (role === 'receiver') renderReceiverPanel(index);
  else renderChannelPlaceholder(index, role);
}

function senderValue(panel, name) {
  return panel.querySelector(`[data-sender-field="${name}"]`)?.value ?? '';
}

function senderChecked(panel, name) {
  return Boolean(panel.querySelector(`[data-sender-field="${name}"]`)?.checked);
}

function finiteNumber(value, label) {
  const number = Number(value);
  if (!Number.isFinite(number)) throw new Error(`${label} must be numeric.`);
  return number;
}

function isInterlacedVideoFormat(format) {
  return format === '1080i50' || format === '1080i60';
}

function collectSenderRequest(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureSenderState(index);
  const splitPairs = senderChecked(panel, 'split-pairs');
  const videoFormat = senderValue(panel, 'video-format');
  const constantTs = senderChecked(panel, 'constant-ts');
  const pairs = [...panel.querySelectorAll('.audio-pair-row')].map((row, pairIndex) => {
    const codec = row.querySelector('[data-pair-field="codec"]').value;
    const pair = {
      name: `Pair ${pairIndex + 1}`,
      channels: [pairIndex * 2 + 1, pairIndex * 2 + 2],
      codec
    };
    if (isAacCodec(codec)) {
      pair.bitrate = Math.round(finiteNumber(row.querySelector('[data-pair-field="bitrate-kbps"]').value, `Pair ${pairIndex + 1} AAC bitrate`) * 1000);
      pair.profile = row.querySelector('[data-pair-field="profile"]').value;
      pair.transport = row.querySelector('[data-pair-field="transport"]').value;
    }
    return pair;
  });

  return {
    template_id: senderValue(panel, 'template'),
    configuration_name: senderValue(panel, 'configuration-name').trim(),
    settings: {
      video: {
        format: videoFormat,
        field_order: isInterlacedVideoFormat(videoFormat) ? senderValue(panel, 'field-order') : undefined,
        bitrate: Math.round(finiteNumber(senderValue(panel, 'bitrate-mbps'), 'Video bitrate') * 1000000),
        rate_control: senderValue(panel, 'rate-control'),
        bit_depth: Math.trunc(finiteNumber(senderValue(panel, 'bit-depth'), 'Bit depth')),
        chroma: senderValue(panel, 'chroma'),
        level: senderValue(panel, 'h264-level')
      },
      mpegts: {
        service_provider: senderValue(panel, 'service-provider').trim(),
        service_name: senderValue(panel, 'service-name').trim(),
        constant_rate: constantTs,
        auto_muxrate: senderChecked(panel, 'auto-muxrate'),
        muxrate: constantTs
          ? Math.round(finiteNumber(senderValue(panel, 'muxrate-mbps'), 'MPEG-TS rate') * 1000000)
          : 0
      },
      audio: {
        split_pairs: splitPairs,
        input_channels: Math.trunc(finiteNumber(senderValue(panel, 'input-channels'), 'Audio input channels')),
        sample_rate: Math.trunc(finiteNumber(senderValue(panel, 'sample-rate'), 'Audio sample rate')),
        stereo_codec: senderValue(panel, 'stereo-codec'),
        stereo_bitrate: senderValue(panel, 'stereo-bitrate-kbps')
          ? Math.round(finiteNumber(senderValue(panel, 'stereo-bitrate-kbps'), 'Stereo AAC bitrate') * 1000)
          : 192000,
        stereo_profile: senderValue(panel, 'stereo-profile') || 'aac_low',
        stereo_transport: senderValue(panel, 'stereo-transport') || 'adts',
        pairs
      },
      streaming: {
        protocol: senderValue(panel, 'protocol'),
        address: senderValue(panel, 'address').trim(),
        port: Math.trunc(finiteNumber(senderValue(panel, 'port'), 'Streaming port')),
        mode: senderValue(panel, 'srt-mode'),
        latency: Math.trunc(finiteNumber(senderValue(panel, 'srt-latency'), 'SRT latency')),
        streamid: senderValue(panel, 'streamid').trim(),
        passphrase: senderValue(panel, 'passphrase'),
        pbkeylen: Math.trunc(finiteNumber(senderValue(panel, 'pbkeylen'), 'SRT key length')),
        interface: senderValue(panel, 'stream-interface'),
        ttl: Math.trunc(finiteNumber(senderValue(panel, 'ttl'), 'TTL'))
      }
    }
  };
}

function updateSenderVisibility(panel) {
  const splitPairs = senderChecked(panel, 'split-pairs');
  panel.querySelector('[data-audio-pairs]')?.classList.toggle('hidden', !splitPairs);
  panel.querySelector('[data-stereo-codec-wrap]')?.classList.toggle('hidden', splitPairs);
  const stereoCodec = senderValue(panel, 'stereo-codec');
  panel.querySelector('[data-stereo-aac-settings]')?.classList.toggle('hidden', splitPairs || !isAacCodec(stereoCodec));
  panel.querySelectorAll('.audio-pair-row').forEach(row => {
    const codec = row.querySelector('[data-pair-field="codec"]')?.value || 'disabled';
    row.querySelector('[data-pair-aac-settings]')?.classList.toggle('hidden', !isAacCodec(codec));
  });
  const inputChannels = panel.querySelector('[data-sender-field="input-channels"]');
  if (inputChannels) {
    if (!splitPairs) inputChannels.value = '2';
    inputChannels.disabled = !splitPairs;
  }

  const interlaced = isInterlacedVideoFormat(senderValue(panel, 'video-format'));
  const fieldOrderWrap = panel.querySelector('[data-field-order-wrap]');
  const fieldOrder = panel.querySelector('[data-sender-field="field-order"]');
  fieldOrderWrap?.classList.toggle('hidden', !interlaced);
  if (fieldOrder) {
    fieldOrder.required = interlaced;
    fieldOrder.disabled = !interlaced;
    if (interlaced && !fieldOrder.value) fieldOrder.value = 'tff';
  }

  const protocol = senderValue(panel, 'protocol');
  panel.querySelector('[data-srt-fields]')?.classList.toggle('hidden', protocol !== 'srt');
  panel.querySelector('[data-udp-fields]')?.classList.toggle('hidden', protocol === 'srt');
  const srtMode = senderValue(panel, 'srt-mode');
  const label = panel.querySelector('[data-address-label]');
  if (label) label.textContent = protocol === 'srt' && srtMode === 'listener' ? 'Bind address' : 'Destination address';

  const encrypted = Boolean(senderValue(panel, 'passphrase'));
  const key = panel.querySelector('[data-sender-field="pbkeylen"]');
  if (key) key.disabled = !encrypted;

  const constantTs = senderChecked(panel, 'constant-ts');
  const autoMuxrate = senderChecked(panel, 'auto-muxrate');
  panel.querySelector('[data-mpegts-rate-wrap]')?.classList.toggle('hidden', !constantTs);
  const autoControl = panel.querySelector('[data-sender-field="auto-muxrate"]');
  const muxrateInput = panel.querySelector('[data-sender-field="muxrate-mbps"]');
  if (autoControl) autoControl.disabled = !constantTs;
  if (muxrateInput) {
    muxrateInput.readOnly = constantTs && autoMuxrate;
    muxrateInput.classList.toggle('readonly-input', constantTs && autoMuxrate);
  }
}

function setMpegTsRateValue(index, value) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const input = panel?.querySelector('[data-sender-field="muxrate-mbps"]');
  if (!input || value <= 0) return;
  input.value = formatMbps(value);
}


function updateSenderSaveUi(index) {
  const channelState = ensureSenderState(index);
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  if (!panel) return;
  const status = panel.querySelector('[data-sender-status]');
  const summary = panel.querySelector('[data-sender-save-summary]');
  const detail = panel.querySelector('[data-sender-save-detail]');
  if (status) status.textContent = channelState.stopping ? 'Stopping' : (channelState.running ? 'On air' : 'Stopped');

  if (summary) {
    if (channelState.autosaveState === 'saving') summary.textContent = 'Saving changes…';
    else if (channelState.autosaveState === 'error') summary.textContent = 'Autosave paused';
    else if (channelState.dirty) summary.textContent = 'Waiting to save changes…';
    else summary.textContent = 'Changes saved automatically';
  }
  if (detail) {
    detail.textContent = channelState.autosaveState === 'error'
      ? (channelState.autosaveError || 'Correct the settings to continue.')
      : 'Start Streaming performs a final save and starts this SDI channel.';
  }
}

function markSenderDirty(index) {
  const channelState = ensureSenderState(index);
  channelState.dirty = true;
  channelState.revision = Number(channelState.revision || 0) + 1;
  channelState.autosaveState = 'pending';
  channelState.autosaveError = '';
  updateSenderSaveUi(index);
}


async function applyTemplate(index, templateId) {
  const template = templateById(templateId);
  if (!template) return;
  const channelState = ensureSenderState(index);
  channelState.templateId = template.id;
  channelState.configurationName = `SDI ${index + 1} - ${template.name}`;
  channelState.settings = normalizePairs(clone(template.editable));
  markSenderDirty(index);
  renderSenderPanel(index);
  await saveSenderNow(index, { silent: true });
}

async function syncInputChannelPairs(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureSenderState(index);
  try {
    const request = collectSenderRequest(index);
    channelState.configurationName = request.configuration_name;
    channelState.settings = normalizePairs(request.settings);
    channelState.templateId = request.template_id;
    markSenderDirty(index);
    renderSenderPanel(index);
    await saveSenderNow(index, { silent: true });
  } catch (error) {
    showToast(error.message, 'error');
  }
}

async function commitSenderChange(index) {
  const channelState = ensureSenderState(index);
  if (channelState.running || channelState.stopping) return;
  markSenderDirty(index);
  await saveSenderNow(index, { silent: true });
}

function setSenderEditingDisabled(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureSenderState(index);
  const locked = channelState.running || channelState.stopping;
  if (!panel) return;
  panel.querySelectorAll('[data-sender-field], [data-pair-field]').forEach(control => {
    control.disabled = locked || control.disabled;
  });
}

function bindSenderPanel(index) {
  const panel = document.getElementById(`panel-sdi${index + 1}`);
  const channelState = ensureSenderState(index);
  updateSenderVisibility(panel);

  panel.querySelector('[data-sender-field="template"]')?.addEventListener('change', event => {
    applyTemplate(index, event.target.value).catch(() => {});
  });
  panel.querySelector('[data-sender-field="input-channels"]')?.addEventListener('change', () => {
    syncInputChannelPairs(index).catch(() => {});
  });

  const bitrateSlider = panel.querySelector('[data-sender-field="bitrate-slider"]');
  const bitrateInput = panel.querySelector('[data-sender-field="bitrate-mbps"]');
  bitrateSlider?.addEventListener('input', () => {
    if (bitrateInput) bitrateInput.value = bitrateSlider.value;
  });
  bitrateSlider?.addEventListener('change', () => {
    if (bitrateInput) bitrateInput.value = bitrateSlider.value;
    commitSenderChange(index).catch(() => {});
  });
  bitrateInput?.addEventListener('input', () => {
    const value = Number(bitrateInput.value);
    if (bitrateSlider && Number.isFinite(value) && value >= Number(bitrateSlider.min) && value <= Number(bitrateSlider.max)) {
      bitrateSlider.value = String(value);
    }
  });
  bitrateInput?.addEventListener('change', () => {
    const value = Number(bitrateInput.value);
    if (!Number.isFinite(value)) return;
    const minimum = Number(bitrateInput.min);
    const maximum = Number(bitrateInput.max);
    const clamped = Math.min(maximum, Math.max(minimum, value));
    bitrateInput.value = String(Number(clamped.toFixed(1)));
    if (bitrateSlider) bitrateSlider.value = bitrateInput.value;
    commitSenderChange(index).catch(() => {});
  });

  panel.querySelectorAll('input, select').forEach(control => {
    if (control.dataset.senderField === 'template' ||
        control.dataset.senderField === 'input-channels' ||
        control.dataset.senderField === 'bitrate-slider' ||
        control.dataset.senderField === 'bitrate-mbps') return;
    control.addEventListener('change', () => {
      updateSenderVisibility(panel);
      commitSenderChange(index).catch(() => {});
    });
  });

  panel.querySelector('[data-sender-action="validate"]')?.addEventListener('click', () => validateSender(index));
  panel.querySelector('[data-sender-action="stream"]')?.addEventListener('click', () => toggleSenderStreaming(index));
  setSenderEditingDisabled(index);
  updateSenderSaveUi(index);
}

async function saveSenderNow(index, options = {}) {
  const { silent = false, force = false } = options;
  const channelState = ensureSenderState(index);

  let request;
  try {
    request = collectSenderRequest(index);
  } catch (error) {
    channelState.autosaveState = 'error';
    channelState.autosaveError = error.message;
    updateSenderSaveUi(index);
    if (!silent) showToast(error.message, 'error');
    return false;
  }

  const signature = JSON.stringify(request);
  if (!force && channelState.exists && signature === channelState.lastSavedSignature) {
    channelState.dirty = false;
    channelState.autosaveState = 'saved';
    channelState.autosaveError = '';
    updateSenderSaveUi(index);
    return true;
  }

  channelState.pendingSave = { request, signature, silent, force };
  channelState.dirty = true;
  channelState.autosaveState = 'saving';
  channelState.autosaveError = '';
  updateSenderSaveUi(index);

  if (channelState.savePromise) return channelState.savePromise;

  channelState.savePromise = (async () => {
    let success = true;
    while (channelState.pendingSave) {
      const job = channelState.pendingSave;
      channelState.pendingSave = null;
      if (!job.force && channelState.exists && job.signature === channelState.lastSavedSignature) continue;

      try {
        const response = await fetch(`/api/sender/channels/sdi${index + 1}`, {
          method: 'PUT',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(job.request)
        });
        const result = await response.json();
        if (!response.ok || !result.ok) throw new Error(result.error || 'Sender configuration save failed.');

        channelState.templateId = result.template_id;
        channelState.configurationName = result.configuration_name;
        channelState.settings = normalizePairs(clone(result.settings));
        channelState.exists = true;
        channelState.lastSavedSignature = job.signature;
        channelState.dirty = Boolean(channelState.pendingSave);
        channelState.autosaveState = channelState.pendingSave ? 'saving' : 'saved';
        channelState.autosaveError = '';
        const muxrate = Number(result.settings?.mpegts?.muxrate || 0);
        if (muxrate > 0) setMpegTsRateValue(index, muxrate);
        updateSenderSaveUi(index);
        if (!job.silent && Array.isArray(result.warnings) && result.warnings.length) {
          showToast(`Saved with warning: ${result.warnings.join(' • ')}`, 'error');
        }
      } catch (error) {
        success = false;
        channelState.pendingSave = null;
        channelState.autosaveState = 'error';
        channelState.autosaveError = error.message;
        channelState.dirty = true;
        updateSenderSaveUi(index);
        if (!job.silent) showToast(error.message, 'error');
        break;
      }
    }
    return success;
  })().finally(() => {
    channelState.savePromise = null;
  });

  return channelState.savePromise;
}

async function validateSender(index) {
  let request;
  try {
    request = collectSenderRequest(index);
  } catch (error) {
    showToast(error.message, 'error');
    return;
  }
  const channel = `sdi${index + 1}`;
  try {
    const response = await fetch(`/api/sender/validate/${channel}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request)
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Sender validation failed.');
    showToast(`SDI ${index + 1} sender configuration is valid.`);
  } catch (error) {
    showToast(error.message, 'error');
  }
}

async function toggleSenderStreaming(index) {
  const channelState = ensureSenderState(index);
  if (channelState.busy || channelState.stopping) return;
  const channel = `sdi${index + 1}`;
  channelState.busy = true;

  try {
    if (channelState.running) {
      const response = await fetch(`/api/sender/stop/${channel}`, { method: 'POST' });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || 'Unable to stop streaming.');
      channelState.stopping = true;
      updateCpuProfileAvailability();
      renderSenderPanel(index);
      scheduleRuntimeStatusRefresh(500);
      showToast(`Stopping SDI ${index + 1} streaming.`);
      return;
    }

    const saved = await saveSenderNow(index, { silent: false, force: true });
    if (!saved) return;

    const response = await fetch(`/api/sender/start/${channel}`, { method: 'POST' });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Unable to start streaming.');
    channelState.running = Boolean(result.running);
    channelState.stopping = Boolean(result.stopping);
    updateCpuProfileAvailability();
    renderSenderPanel(index);
    scheduleRuntimeStatusRefresh(1000);
    showToast(`SDI ${index + 1} streaming started.`);
  } catch (error) {
    showToast(error.message, 'error');
  } finally {
    channelState.busy = false;
  }
}

async function refreshSenderRuntimeStatus(index) {
  const channelState = ensureSenderState(index);
  const channel = `sdi${index + 1}`;
  try {
    const response = await fetch(`/api/sender/status/${channel}`, { cache: 'no-store' });
    const result = await response.json();
    if (!response.ok || !result.ok) return;
    const running = Boolean(result.running && result.role === 'sender');
    const stopping = Boolean(result.stopping && result.role === 'sender');
    const changed = running !== channelState.running || stopping !== channelState.stopping;
    channelState.running = running;
    channelState.stopping = stopping;
    channelState.serviceAvailable = Boolean(result.available);
    if (changed && selectedSdiRole(index) === 'sender') renderSenderPanel(index);
    updateCpuProfileAvailability();
  } catch {
    // Health polling must not interrupt active operator input.
  }
}

function activeSdiIndex() {
  const match = /^sdi([1-4])$/.exec(state.activeTab || '');
  return match ? Number(match[1]) - 1 : -1;
}

function runtimeRefreshDelay(index) {
  if (document.hidden || index < 0) return 0;
  const role = selectedSdiRole(index);
  const channelState = role === 'receiver' ? ensureReceiverState(index) : ensureSenderState(index);
  if (channelState.stopping) return 500;
  if (role === 'receiver' && channelState.running) return 5000;
  if (role === 'sender' && channelState.running) return 10000;
  return 0;
}

async function refreshVisibleRuntimeStatus() {
  window.clearTimeout(state.runtimeStatusTimer);
  state.runtimeStatusTimer = null;

  const index = activeSdiIndex();
  if (document.hidden || index < 0) return;

  const role = selectedSdiRole(index);
  if (role === 'receiver') await refreshReceiverRuntimeStatus(index);
  else if (role === 'sender') await refreshSenderRuntimeStatus(index);

  const delay = runtimeRefreshDelay(index);
  if (delay > 0) {
    state.runtimeStatusTimer = window.setTimeout(refreshVisibleRuntimeStatus, delay);
  }
}

function scheduleRuntimeStatusRefresh(delay = 0) {
  window.clearTimeout(state.runtimeStatusTimer);
  state.runtimeStatusTimer = null;
  if (document.hidden || activeSdiIndex() < 0) return;
  state.runtimeStatusTimer = window.setTimeout(refreshVisibleRuntimeStatus, Math.max(0, delay));
}

function collectInterfaceSettings(card) {
  const value = field => card.querySelector(`[data-field="${field}"]`)?.value?.trim() || '';
  const mode = value('mode') || 'dhcp';
  const dns = value('dns').split(',').map(item => item.trim()).filter(Boolean);
  return {
    interface: card.dataset.interface,
    mode,
    address: mode === 'static' ? value('address') : '',
    netmask: mode === 'static' ? prefixToNetmask(value('prefix')) : '255.255.255.0',
    gateway: mode === 'static' ? value('gateway') : '',
    dns: mode === 'static' ? dns : []
  };
}

function collectConfig() {
  const config = clone(state.config);
  config.device_name = elements.deviceName.value.trim();
  if (!config.device_name) throw new Error('Device name cannot be empty.');
  config.cpu = { ...(config.cpu || {}), profile: selectedCpuProfile() };

  let control = null;
  let streaming = null;
  for (const card of elements.networkList.querySelectorAll('.network-interface')) {
    const role = card.querySelector('[data-field="role"]').value;
    const settings = collectInterfaceSettings(card);
    if (role === 'control' || role === 'both') {
      if (control) throw new Error('Only one interface can be assigned to management/control.');
      control = settings;
    }
    if (role === 'streaming' || role === 'both') {
      if (streaming) throw new Error('Only one interface can be assigned to streaming.');
      streaming = settings;
    }
  }
  if (!control) throw new Error('Assign one interface to management/control.');

  config.network = config.network || {};
  config.network.control = { ...(config.network.control || {}), ...control };
  config.network.streaming = {
    ...(config.network.streaming || {}),
    ...(streaming || { interface: '', mode: 'dhcp', address: '', netmask: '255.255.255.0', gateway: '', dns: [] })
  };
  if (!Number.isInteger(config.network.streaming.multicast_ttl)) config.network.streaming.multicast_ttl = 16;

  config.sdi_ports = [...elements.sdiList.querySelectorAll('.sdi-row')].map((row, index) => {
    const previous = state.config.sdi_ports[index] || {};
    const role = row.querySelector('.segmented button.active')?.dataset.value || 'disabled';
    const device = Number(row.querySelector('[data-field="decklink-device"]').value);
    if (!Number.isInteger(device) || device < 0 || device > 128) throw new Error(`SDI ${index + 1}: DeckLink device index must be between 0 and 128.`);
    return {
      ...previous,
      id: previous.id || `sdi${index + 1}`,
      name: previous.name || `SDI ${index + 1}`,
      decklink_device: device,
      role
    };
  });
  return config;
}

async function saveConfiguration() {
  if (state.saving) return;
  let config;
  try {
    config = collectConfig();
  } catch (error) {
    showToast(error.message, 'error');
    return;
  }

  state.saving = true;
  elements.saveButton.disabled = true;
  elements.saveButton.querySelector('span').textContent = 'Saving…';
  try {
    const response = await fetch('/api/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(config)
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Configuration save failed.');
    state.config = config;
    markClean('Saved to the appliance configuration file.');
    updateChannelTabsFromForm();
    for (let index = 0; index < 4; index += 1) renderChannelPanel(index);
    showToast('NxFrame appliance configuration saved.');
  } catch (error) {
    state.dirty = true;
    elements.saveButton.disabled = false;
    elements.configurationState.textContent = 'Save failed';
    showToast(error.message, 'error');
  } finally {
    state.saving = false;
    elements.saveButton.querySelector('span').textContent = 'Save configuration';
  }
}

function setupTabs() {
  document.querySelectorAll('.channel-tab').forEach(tab => {
    tab.addEventListener('click', () => {
      const target = tab.dataset.tab;
      state.activeTab = target;
      document.querySelectorAll('.channel-tab').forEach(item => item.classList.toggle('active', item === tab));
      document.querySelectorAll('.tab-panel').forEach(panel => panel.classList.toggle('active', panel.dataset.panel === target));
      scheduleRuntimeStatusRefresh();
    });
  });
}

async function bootstrap() {
  try {
    const [bootstrapResponse] = await Promise.all([
      fetch('/api/bootstrap', { cache: 'no-store' }),
      loadTemplates()
    ]);
    const result = await bootstrapResponse.json();
    if (!bootstrapResponse.ok || !result.ok) throw new Error(result.error || 'Unable to load dashboard data.');

    state.config = result.config;
    state.interfaces = Array.isArray(result.interfaces) ? result.interfaces : [];
    state.cpuProfiles = Array.isArray(result.cpu_profiles) ? result.cpu_profiles : [];
    state.cpuProfileWarning = result.cpu_profile_warning || '';
    elements.deviceName.value = state.config.device_name || 'NxFrame';
    if (result.warning) {
      elements.notice.textContent = result.warning;
      elements.notice.classList.remove('hidden');
    }

    renderCpuProfiles();
    renderNetworks();
    renderSdiAssignments();
    await Promise.all([0, 1, 2, 3].map(index => Promise.all([loadSenderChannel(index), loadReceiverChannel(index)])));
    for (let index = 0; index < 4; index += 1) renderChannelPanel(index);
    updateCpuProfileAvailability();
    updateChannelTabsFromForm();
    markClean(result.warning ? 'Defaults are active until the first save.' : 'Configuration loaded from disk.');
    setOnline(true);
    scheduleRuntimeStatusRefresh();
  } catch (error) {
    setOnline(false);
    elements.configurationState.textContent = 'Unavailable';
    elements.networkList.innerHTML = `<div class="empty-state">${escapeHtml(error.message)}</div>`;
    elements.sdiList.innerHTML = '<div class="empty-state">The SDI configuration could not be loaded.</div>';
    showToast(error.message, 'error');
  }
}

setupTabs();
elements.deviceName.addEventListener('input', markDirty);
elements.cpuProfile.addEventListener('change', () => {
  markDirty();
  updateCpuProfileDescription();
});
elements.saveButton.addEventListener('click', saveConfiguration);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    window.clearTimeout(state.runtimeStatusTimer);
    state.runtimeStatusTimer = null;
  } else {
    scheduleRuntimeStatusRefresh();
  }
});
bootstrap();

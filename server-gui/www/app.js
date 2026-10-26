'use strict';
// Galdrun ServerGui: a kitchen sink over /api/v1 (see ../docs/Roadmap.md).
// Plain DOM, no build step. Everything the server says is rendered as text.
// The page's own text is by key, put through tt() (../../shared/web/i18n.js;
// English in app.en.js, German in app.de.js); what the server says is shown as it says it.

const $ = (s) => document.querySelector(s);
const enc = encodeURIComponent;

// The page's own markup in the chosen language, before anything reads it.
i18n.translate();
i18n.bind($('#language'));

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  let value;
  for (const [k, v] of Object.entries(props ?? {})) {
    if (v == null || v === false) continue;
    if (k === 'value') value = v;
    else if (k.startsWith('on')) { el[k] = v; (el._on ??= []).push(k); } // morph() carries them over
    else if (k === 'class') el.className = v;
    else if (typeof v !== 'string' && k in el) el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  appendKids(el, kids);
  if (value !== undefined) el.value = value;
  return el;
}

// Skips null, undefined and false, so `cond && h(...)` can sit in a list; the
// DOM's own append would print them as text.
function appendKids(el, kids) {
  for (const kid of kids.flat(Infinity)) {
    if (kid != null && kid !== false) el.append(kid instanceof Node ? kid : String(kid));
  }
}

// Brings `old` in place to what `now` shows, so a re-render keeps whatever did
// not change: loaded images, scroll, hover, focus, selection, open details.
// Handlers are taken from `now`, so they must not capture nodes built beside
// them (those are thrown away): reach them through the event instead.
function morph(old, now) {
  if (old.nodeName !== now.nodeName || now.nodeName === 'CANVAS') return old.replaceWith(now);
  if (old.nodeType !== Node.ELEMENT_NODE) {
    if (old.nodeValue !== now.nodeValue) old.nodeValue = now.nodeValue;
    return;
  }
  for (const a of [...old.attributes]) {
    // An open details and a placed popover keep what the page did to them.
    if (!now.hasAttribute(a.name) && !(a.name === 'open' && old.nodeName === 'DETAILS') && !(a.name === 'style' && old.hasAttribute('popover'))) old.removeAttribute(a.name);
  }
  for (const a of now.attributes) if (old.getAttribute(a.name) !== a.value) old.setAttribute(a.name, a.value);
  for (const k of new Set([...(old._on ?? []), ...(now._on ?? [])])) old[k] = now[k] ?? null;
  old._on = now._on;
  morphChildren(old, [...now.childNodes]);
  if (old.nodeName === 'INPUT' && old.checked !== now.checked) old.checked = now.checked;
  if (old.matches('input, select, textarea') && old.value !== now.value) old.value = now.value;
}

function morphChildren(parent, kids) {
  const old = [...parent.childNodes];
  kids.forEach((kid, i) => (old[i] ? morph(old[i], kid) : parent.append(kid)));
  for (const n of old.slice(kids.length)) n.remove();
}

// replaceChildren, but morphed; kids as h() takes them.
function patch(container, ...kids) {
  morphChildren(container, [...h('div', {}, kids).childNodes]);
}

const store = {
  get(key, fallback) { try { return localStorage.getItem(key) ?? fallback; } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(key, value); } catch { /* per-viewer convenience only */ } },
};

const S = {
  zones: [], rooms: [], status: null, providers: [], playlists: [], playlistFolders: [], favorites: [], stations: [], stationFolders: [], stationFolder: null, folders: [],
  openPlaylist: null, entries: [],
  target: store.get('vx.target', ''),
  history: {},             // client -> [{t, driftPpm, offsetUs, appliedPpm, bufferedMs}]
  system: null,            // GET system / the last system.metrics
  dragging: false,
  dirty: new Set(),
  lastInput: {},           // client -> the last room.input it sent, with when
  autoFilled: {},          // zone -> the last queue.autoFilled, with when (server phase 26)
  autoMarks: {},           // zone -> {ids, key}: the items a fill added, while the items are as it left them
  playback: null,          // GET settings/playback (server phase 31): {groupBooks}
  user: store.get('vx.user', ''), // the header's listener (server phase 36): '' the household or a name
  users: [],               // GET users
};
let page = '';             // the page shown, from the hash

// --- formatting --------------------------------------------------------------

const time = () => new Date().toLocaleTimeString(undefined, { hour12: false }) + '.' + String(Date.now() % 1000).padStart(3, '0');

function fmtDur(ts) { // a .NET TimeSpan, "[d.]hh:mm:ss[.fffffff]"
  const m = /^(?:(\d+)\.)?(\d+):(\d+):(\d+)/.exec(ts ?? '');
  if (!m) return ts ?? '';
  const hours = Number(m[1] ?? 0) * 24 + Number(m[2]);
  return (hours ? `${hours}:${m[3]}` : String(Number(m[3]))) + ':' + m[4];
}

function fmtNs(ns) {
  if (ns == null) return '—';
  const ms = Number(ns) / 1e6;
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}.${String(Math.floor(ms % 1000)).padStart(3, '0')}`;
}

const fmtNum = (v, digits = 2) => (v == null ? '—' : Number(v).toFixed(digits));
const fmtBytes = (b) => (b == null ? '—' : b >= 2 ** 30 ? `${(b / 2 ** 30).toFixed(2)} GiB`
  : b >= 2 ** 20 ? `${(b / 2 ** 20).toFixed(1)} MiB` : `${(b / 2 ** 10).toFixed(1)} KiB`);
function fmtUptime(ms) {
  if (ms == null) return '—';
  const s = Math.floor(ms / 1000), d = Math.floor(s / 86400);
  const hms = [Math.floor(s / 3600) % 24, Math.floor(s / 60) % 60, s % 60].map((n) => String(n).padStart(2, '0')).join(':');
  return d ? `${d}d ${hms}` : hms;
}
const fmtFmt = (f) => (f ? `${f.sampleRate} Hz ${f.channels}ch ${f.sampleType}` : '—');
// A track's AudioFormat: "FLAC 96 kHz 24 bit 2ch 3000 kbps", the parts known.
const fmtAudio = (f) => (f ? [f.codec, f.sampleRate && `${f.sampleRate / 1000} kHz`, f.bitDepth && `${f.bitDepth} bit`,
  f.channels && `${f.channels}ch`, f.bitrate && `${f.bitrate} kbps`].filter(Boolean).join(' ') : '');
const json = (v) => JSON.stringify(v, null, 2);

// Every secret field, at any depth, as dots: what the page shows of a request
// or an answer never holds one (the Roadmap's phases 6, 11 and 12). A login's
// `state` is masked only beside its code or its URL: a folder's `state` is not
// a secret.
const SECRETS = new Set(['password', 'key', 'apiKey', 'token', 'code', 'refreshToken', 'clientSecret', 'deviceCode', 'userCode', 'authorizeUrl', 'callbackUrl']);
function mask(v) {
  if (Array.isArray(v)) return v.map(mask);
  if (v === null || typeof v !== 'object') return v;
  const login = 'code' in v || 'authorizeUrl' in v || 'callbackUrl' in v;
  return Object.fromEntries(Object.entries(v).map(([k, x]) =>
    [k, x != null && (SECRETS.has(k) || (login && k === 'state')) ? '•••' : mask(x)]));
}

function linkify(text) {
  return String(text ?? '').split(/(https?:\/\/\S+)/).map((part, i) =>
    i % 2 ? h('a', { href: part, target: '_blank', rel: 'noreferrer' }, part) : part);
}

// --- icons -------------------------------------------------------------------
// Line icons in the shared theme's style; `<i data-icon>` in the page is
// replaced by its icon at the start.

const ICONS = {
  menu: '<path d="M4 6h16M4 12h16M4 18h16"/>',
  zones: '<rect x="3" y="3" width="10" height="18" rx="2"/><circle cx="8" cy="14" r="3"/><circle cx="8" cy="7.5" r="1"/><path d="M17 7v10M21 9v6"/>',
  rooms: '<path d="M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6h-6v6H4a1 1 0 0 1-1-1z"/>',
  people: '<circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0M16 4.6a3.5 3.5 0 0 1 0 6.8M18.5 14.5a6.5 6.5 0 0 1 3 5.5"/>',
  library: '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>',
  playlists: '<path d="M3 6h13M3 12h13M3 18h8M18 15v6M15 18h6"/>',
  home: '<path d="M3 11.5 12 4l9 7.5"/><path d="M5.5 10v10h5v-6h3v6h5V10"/>',
  heart: '<path d="M12 20s-7-4.4-9-8.5C1.6 8.4 3.6 5 7 5c2 0 3.3 1 5 3 1.7-2 3-3 5-3 3.4 0 5.4 3.4 4 6.5-2 4.1-9 8.5-9 8.5z"/>',
  radio: '<rect x="3" y="8" width="18" height="13" rx="2"/><path d="M7 8l10-5M14 13h4M14 16.5h4"/><circle cx="8.5" cy="14.5" r="2.5"/>',
  chat: '<path d="M4 5h16v11H9l-5 4z"/><path d="M8.5 10.5h7"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  device: '<rect x="6" y="3" width="12" height="18" rx="2"/><circle cx="12" cy="14.5" r="2.5"/><circle cx="12" cy="7.5" r="1"/>',
  chip: '<rect x="7" y="7" width="10" height="10" rx="1"/><path d="M9 3v4M15 3v4M9 17v4M15 17v4M3 9h4M3 15h4M17 9h4M17 15h4"/>',
  cloud: '<path d="M7 18a4 4 0 0 1-.6-8A6 6 0 0 1 18 9.5a4.3 4.3 0 0 1-.5 8.5z"/>',
  sparkle: '<path d="M12 3l1.8 4.7 4.7 1.8-4.7 1.8L12 16l-1.8-4.7L5.5 9.5l4.7-1.8zM19 15l.8 2.2 2.2.8-2.2.8L19 21l-.8-2.2-2.2-.8 2.2-.8z"/>',
  pulse: '<path d="M2 13h5l3-9 4 17 3-10 2 2h3"/>',
  gauge: '<path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/>',
  log: '<path d="M4 5h16M4 10h16M4 15h10M4 20h7"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  play: '<path d="M7 4.5v15l12-7.5z"/>',
  pause: '<path d="M8 5v14M16 5v14"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="1"/>',
  mic: '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5.5 11a6.5 6.5 0 0 0 13 0M12 17.5V21M9 21h6"/>',
  prev: '<path d="M6 5v14M19 5l-10 7 10 7z"/>',
  next: '<path d="M18 5v14M5 5l10 7-10 7z"/>',
  back: '<path d="M15 5l-7 7 7 7"/>',
  forward: '<path d="M9 5l7 7-7 7"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  wave: '<path d="M2 12h3l2-6 4 12 4-14 3 8h4"/>',
  key: '<circle cx="8" cy="14" r="4"/><path d="M11 11l9-9M16 6l3 3M14 8l2 2"/>',
  code: '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>',
  volume: '<path d="M4 9h4l5-4v14l-5-4H4z"/><path d="M17 9a4 4 0 0 1 0 6M19.5 6.5a8 8 0 0 1 0 11"/>',
  mute: '<path d="M4 9h4l5-4v14l-5-4H4z"/><path d="M17 9l5 6M22 9l-5 6"/>',
  shuffle: '<path d="M3 7h4l10 10h4M3 17h4l3-3M14 10l3-3h4M18 4l3 3-3 3M18 14l3 3-3 3"/>',
  star: '<path d="M12 2l3 6.5 7 1-5 5 1 7.5-6-3.5-6 3.5 1-7.5-5-5 7-1z"/>',
  starOn: '<path d="M12 2l3 6.5 7 1-5 5 1 7.5-6-3.5-6 3.5 1-7.5-5-5 7-1z" fill="currentColor"/>',
  heartOn: '<path d="M12 20s-7-4.4-9-8.5C1.6 8.4 3.6 5 7 5c2 0 3.3 1 5 3 1.7-2 3-3 5-3 3.4 0 5.4 3.4 4 6.5-2 4.1-9 8.5-9 8.5z" fill="currentColor"/>',
  shuffleOff: '<path d="M3 7h4l10 10h4M3 17h4l3-3M14 10l3-3h4M18 4l3 3-3 3M18 14l3 3-3 3" opacity=".45"/><path d="M4 3l16 18"/>',
  repeat: '<path d="M17 2l3 3-3 3M3 11V9a4 4 0 0 1 4-4h13M7 22l-3-3 3-3M21 13v2a4 4 0 0 1-4 4H4"/>',
  repeatOne: '<path d="M17 2l3 3-3 3M3 11V9a4 4 0 0 1 4-4h13M7 22l-3-3 3-3M21 13v2a4 4 0 0 1-4 4H4"/><text x="12" y="15.6" text-anchor="middle" font-size="9" font-weight="800" fill="currentColor" stroke="none">1</text>',
  // A cog: settings, wherever they open.
  setup: '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>',
  close: '<path d="M6 6l12 12M18 6L6 18"/>',
  // A client's network link (clientLink): Wi-Fi waves, as many lit as its
  // signal earns (wifiBars), an RJ45 plug, and a question mark where it is not known.
  linkUnknown: '<path d="M7.5 8.5a4.5 4.5 0 1 1 6.3 4.1c-1.1.5-1.8 1.4-1.8 2.6V16"/><path d="M12 20.5v.01"/>',
  wifi0: wifiArcs(0), wifi1: wifiArcs(1), wifi2: wifiArcs(2), wifi3: wifiArcs(3),
  ethernet: '<rect x="4" y="5" width="16" height="12" rx="1.5"/><path d="M8 17v3h8v-3M8 9v2M11 9v2M13 9v2M16 9v2"/>',
  link: '<path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1"/><path d="M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/>',
  unlink: '<path d="M14.5 6.5l1-1a3.5 3.5 0 0 1 5 5l-3 3"/><path d="M9.5 17.5l-1 1a3.5 3.5 0 0 1-5-5l3-3"/><path d="M8 2v3M2 8h3M16 22v-3M22 16h-3"/>',
  alert: '<path d="M12 3l10 18H2z"/><path d="M12 10v5M12 18v.01"/>',
  grip: '<circle cx="9" cy="6" r="1.2"/><circle cx="15" cy="6" r="1.2"/><circle cx="9" cy="12" r="1.2"/><circle cx="15" cy="12" r="1.2"/><circle cx="9" cy="18" r="1.2"/><circle cx="15" cy="18" r="1.2"/>',
  // Where an item comes from, by provider (SOURCE_ICONS).
  nas: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9.3h18M3 14.7h18M14 6.7h4M14 12h4M14 17.3h4"/><circle cx="6.5" cy="6.7" r=".6" fill="currentColor"/><circle cx="6.5" cy="12" r=".6" fill="currentColor"/><circle cx="6.5" cy="17.3" r=".6" fill="currentColor"/>',
  tidal: '<path fill="currentColor" d="M6 6l3 3-3 3-3-3zM12 6l3 3-3 3-3-3zM18 6l3 3-3 3-3-3zM12 12l3 3-3 3-3-3z"/>',
  tower: '<circle cx="12" cy="9" r="1.6"/><path d="M12 10.6 8.5 21M12 10.6 15.5 21M9.7 17.3h4.6M8.3 5.6a5 5 0 0 0 0 6.8M15.7 5.6a5 5 0 0 1 0 6.8M5.4 3a9 9 0 0 0 0 12M18.6 3a9 9 0 0 1 0 12"/>',
  globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
  bars: '<path d="M6 20v-7M12 20V5M18 20v-10"/>',
  more: '<circle cx="12" cy="5" r="1.3" fill="currentColor"/><circle cx="12" cy="12" r="1.3" fill="currentColor"/><circle cx="12" cy="19" r="1.3" fill="currentColor"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6"/>',
  power: '<path d="M12 2v10M6 5a9 9 0 1 0 12 0"/>',
  refresh: '<path d="M20 11a8 8 0 0 0-14.3-4.9L4 8"/><path d="M4 3v5h5"/><path d="M4 13a8 8 0 0 0 14.3 4.9L20 16"/><path d="M20 21v-5h-5"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="8.5" cy="9.5" r="1.5"/><path d="M21 16l-5-5-9 9"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13.5 6.5l4 4"/>',
  bookmark: '<path d="M6 3h12v18l-6-4-6 4z"/>',
  queue: '<path d="M4 6h12M4 11h12M4 16h7"/><path d="M15 14v6l5-3z"/>',
  repeatOff: '<path d="M17 2l3 3-3 3M3 11V9a4 4 0 0 1 4-4h13M7 22l-3-3 3-3M21 13v2a4 4 0 0 1-4 4H4" opacity=".45"/><path d="M4 3l16 18"/>',
};

function wifiArcs(lit) {
  return ['M8 16a6 6 0 0 1 8 0', 'M5 12a11 11 0 0 1 14 0', 'M2 8a16 16 0 0 1 20 0']
    .map((d, i) => `<path d="${d}"${i < lit ? '' : ' opacity=".25"'}/>`).join('') + '<circle cx="12" cy="20" r="1"/>';
}

function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  for (const [k, v] of Object.entries({ viewBox: '0 0 24 24', fill: 'none', stroke: 'currentColor', 'stroke-width': '1.7',
    'stroke-linecap': 'round', 'stroke-linejoin': 'round', 'aria-hidden': 'true' })) svg.setAttribute(k, v);
  svg.innerHTML = ICONS[name] ?? '';
  return svg;
}
for (const el of document.querySelectorAll('i[data-icon]')) el.replaceWith(icon(el.dataset.icon));

// --- flash -------------------------------------------------------------------

const flashEl = $('#toast');
let flashTimer;
// A message key is shown translated (act's done texts come here); a server's message as it is.
function flash(message, bad = false) {
  flashEl.textContent = tt(message);
  flashEl.className = 'toast ' + (bad ? 'bad' : 'good');
  flashEl.hidden = false;
  clearTimeout(flashTimer);
  flashTimer = setTimeout(() => { flashEl.hidden = true; }, bad ? 8000 : 3000);
}
flashEl.addEventListener('click', () => { flashEl.hidden = true; });

// --- the bottom logs ---------------------------------------------------------

const MAX_LINES = 2000;
let filter = '';

function appendLine(container, text, detail, cls = '') {
  const line = h('div', { class: 'line ' + cls }, text);
  line.dataset.text = text.toLowerCase();
  if (detail !== undefined) {
    line.addEventListener('click', (e) => {
      if (e.target.closest('pre')) return;
      const open = line.querySelector('pre');
      if (open) open.remove(); else line.append(h('pre', {}, typeof detail === 'string' ? detail : json(mask(detail))));
    });
  }
  line.hidden = Boolean(filter) && !line.dataset.text.includes(filter);
  insertLine(container, line);
}

function insertLine(container, line) {
  const stick = container.scrollTop + container.clientHeight >= container.scrollHeight - 4;
  container.append(line);
  while (container.childElementCount > MAX_LINES) container.firstElementChild.remove();
  if (stick) container.scrollTop = container.scrollHeight;
}

$('#bottomFilter').addEventListener('input', (e) => {
  filter = e.target.value.toLowerCase();
  for (const line of document.querySelectorAll('.log .line')) line.hidden = Boolean(filter) && !line.dataset.text.includes(filter);
});
$('#bottomClear').addEventListener('click', () => {
  const tab = document.querySelector('#bottom > .tab.on');
  tab.replaceChildren();
  if (tab.id === 'events') { evCounts.clear(); $('#evCount').textContent = ''; }
  if (tab.id === 'requests') { reqCount = 0; $('#reqCount').textContent = ''; }
  if (tab.id === 'aiLog') { aiCount = 0; $('#aiCount').textContent = ''; }
  if (tab.id === 'logs') { logCount = 0; $('#logCount').textContent = ''; store.set('vx.logsCleared', JSON.stringify(logOffsets)); }
});

// --- the API -----------------------------------------------------------------

let reqCount = 0;
// The AI tab: requests to ai/… and the ai.*, proposals.* and AutoFill events, also in their own tabs.
let aiCount = 0;

// ai.request in one line: who was asked, how it went, and the last user text.
function aiRequestLine(d) {
  const user = d.request.messages.findLast((m) => m.role === 'user')?.text ?? '';
  const u = d.answer?.usage;
  return `${time()} ${d.provider}/${d.model} ${d.ok ? (d.answer.stopReason ?? 'ok') : `${d.error}: ${d.message}`} (${d.ms} ms`
    + `${u ? `, ${u.inputTokens} in / ${u.outputTokens} out` : ''}) ${user.replace(/\s+/g, ' ').slice(0, 160)}`;
}

// ai.request opened: the prompt as sent and the answer, as text.
function aiPrompt(d) {
  const r = d.request;
  const lines = (...xs) => xs.flat().filter(Boolean).join('\n');
  const calls = (cs) => cs.map((c) => `→ ${c.name}(${c.arguments})`);
  return [
    lines(`${d.provider} / ${d.model} · max ${r.maxTokens} tokens · reasoning ${r.reasoning}${r.webSearch ? ' · web search' : ''}`
      + `${r.requiredTool ? ` · must call ${r.requiredTool}` : ''}`, r.tools.length && `tools: ${r.tools.join(', ')}`),
    r.system && lines('── system ──', r.system),
    ...r.messages.map((m) => lines(`── ${m.role}${m.toolCallId ? ` (${m.toolCallId})` : ''}${m.isError ? ' error' : ''} ──`,
      m.text, calls(m.toolCalls))),
    d.answer
      ? lines(`══ answer (${d.answer.stopReason}) ══`, d.answer.text, calls(d.answer.toolCalls),
        (d.answer.sources ?? []).map((x) => `source: ${x.title ?? ''} ${x.url}`))
      : lines('══ failed ══', `${d.error}: ${d.message}`),
  ].filter(Boolean).join('\n\n');
}
function appendAi(text, detail, cls) {
  $('#aiCount').textContent = `(${++aiCount})`;
  appendLine($('#aiLog'), text, detail, cls);
}

// timeoutMs gives up on an answer that does not come; the call then fails as
// one that did not reach the server.
async function api(method, path, body, { quiet = false, timeoutMs, signal } = {}) {
  const init = { method, headers: {} };
  if (timeoutMs) init.signal = AbortSignal.timeout(timeoutMs);
  if (signal) init.signal = signal;
  if (body instanceof Blob) {
    init.headers['Content-Type'] = body.type || 'application/octet-stream';
    init.body = body;
  } else if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = typeof body === 'string' ? body : JSON.stringify(body);
  }
  const t0 = performance.now();
  let status = 0, data = null, text = '';
  try {
    const res = await fetch('/api/v1/' + path, init);
    status = res.status;
    text = await res.text();
    try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  } catch (e) {
    data = { error: 'fetchFailed', message: String(e) };
  }
  const ms = Math.round(performance.now() - t0);
  const ok = status >= 200 && status < 300;
  if (!quiet) {
    $('#reqCount').textContent = `(${++reqCount})`;
    const line = `${time()} ${method} ${path} → ${status || 'ERR'} (${ms} ms)${ok ? '' : ` ${data?.error ?? ''}: ${data?.message ?? ''}`}`;
    const detail = { request: init.body instanceof Blob ? `${init.body.type} ${init.body.size} bytes` : init.body ? tryParse(init.body) : null, status, response: data };
    appendLine($('#requests'), line, detail, ok ? '' : 'bad');
    if (path.startsWith('ai/')) appendAi(line, detail, ok ? '' : 'bad');
  }
  if (!ok) {
    const err = new Error(`${method} ${path} → ${status || 'unreachable'} ${data?.error ?? ''}: ${data?.message ?? text}`);
    err.status = status;
    err.data = data;
    throw err;
  }
  return data;
}

function tryParse(text) { try { return JSON.parse(text); } catch { return text; } }

// Runs a user action: failures are flashed (and are in the request log), and
// the state is re-read afterwards.
async function act(fn, done) {
  try {
    const answer = await fn();
    if (done) flash(typeof done === 'function' ? done(answer) : done);
    refreshSoon();
    return answer;
  } catch (e) {
    flash(e.message, true);
    return undefined;
  }
}

// Where library actions go: the zone the player bar shows (the picked one,
// else the first).
function needTarget() {
  const zone = playerZone()?.zone;
  if (!zone) throw new Error(tt('api.noZoneYet'));
  return enc(zone);
}

// --- rendering helpers ---------------------------------------------------------

// Re-rendering a container under an input the user is in, or under a drag,
// would take it away; that render waits until focus leaves.
function render(key, container, build) {
  const active = document.activeElement;
  if (S.dragging || pointerDown || (container.contains(active) && active.matches('input, select, textarea'))) {
    S.dirty.add(key);
    return;
  }
  S.dirty.delete(key);
  const open = new Set([...container.querySelectorAll('details[open][data-key]')].map((d) => d.dataset.key));
  patch(container, build());
  for (const d of container.querySelectorAll('details[data-key]')) if (open.has(d.dataset.key)) d.open = true;
}

const renderers = { home: () => renderHome(), zones: () => renderZones(), playlistTree: () => renderPlaylistTree(), stationTree: () => stationTree.render(), player: () => renderPlayer(), rooms: () => renderRooms(), clients: () => renderClients(), folders: () => renderFolders(), ai: () => renderAi() };
function flushDirty() {
  setTimeout(() => { for (const key of [...S.dirty]) renderers[key]?.(); }, 0);
}
// A click moves focus on pointerdown but fires after pointerup: a render in
// between would replace the button being clicked (a form's "set" after typing
// into its field), and the click would land on nothing. So a focus change made
// by the pointer waits for the pointer to come up.
let pointerDown = false;
document.addEventListener('pointerdown', () => { pointerDown = true; }, true);
for (const type of ['pointerup', 'pointercancel']) {
  document.addEventListener(type, () => { pointerDown = false; flushDirty(); }, true);
}
document.addEventListener('focusout', () => { if (!pointerDown) flushDirty(); });

// Which zone and room cards stand collapsed, kept across renders and reloads.
let collapsedCards = [];
try { collapsedCards = JSON.parse(store.get('vx.collapsed', '[]')); } catch { /* per-viewer convenience only */ }
const collapsed = new Set(collapsedCards);

// The details' toggle event records the card's state; `this` is the card.
function cardToggled(key, open) {
  collapsed[open ? 'delete' : 'add'](key);
  store.set('vx.collapsed', JSON.stringify([...collapsed]));
}

// A card's header row: the summary of the details the card is. A click on a
// control in it acts on that control alone; only a click on the header itself
// collapses the card.
function cardHead(...kids) {
  return h('summary', {
    onclick(e) { if (e.target.closest('button, input, select, label, a, .handle')) e.preventDefault(); },
  }, ...kids);
}

// Every item names its picture by artworkId (server phase 40), one store for
// every list: GET artwork/{id}. One that failed is not asked for again: a
// re-render would otherwise put the image back, load it, fail and remove it,
// and the list would flicker. `artwork.changed` says one was found since.
const noArtwork = new Set();
// Artwork that always takes its place in a list row: the picture over a
// placeholder, which shows where there is none or it fails to load.
function artSlot(id, placeholder = 'library') {
  return h('span', { class: 'art-slot', 'data-art': id ?? '' }, icon(placeholder), art(id));
}

function art(id) {
  if (!id) return null;
  const src = `/api/v1/artwork/${enc(id)}`;
  if (noArtwork.has(src)) return null;
  return h('img', {
    class: 'art', loading: 'lazy', decoding: 'async', alt: '', src,
    onerror() { noArtwork.add(src); this.remove(); },
  });
}

// A picture found for an id: every slot that waits for it asks again.
function artworkFound(id) {
  noArtwork.delete(`/api/v1/artwork/${enc(id)}`);
  for (const slot of document.querySelectorAll('.art-slot')) {
    if (slot.dataset.art !== id) continue;
    slot.querySelector('img.art')?.remove();
    slot.append(art(id));
  }
}

// A list entry's item, when it is of that kind: a queue item, a favourite, a
// like or a playlist entry holds one item and names its kind.
const media = (entry, kind) => (entry?.item?.kind === kind ? entry.item : undefined);

// One item as a list row says it, by its kind.
function itemLabel(x) {
  if (x?.kind === 'group') {
    return ['📖 ', x.title, x.artist && ` — ${x.artist}`,
      h('span', { class: 'muted' }, ` · ${x.parts.length ? tt('common.parts', { n: x.parts.length }) : tt('common.book')} · ${x.type}`, x.source && ` [${x.source}]`)];
  }
  if (x?.kind === 'track') {
    return [typeMark(x), x.title, x.artist && ` — ${x.artist}`, x.album && ` · ${x.album}`, h('span', { class: 'muted' }, x.format && ` · ${fmtAudio(x.format)}`, ` [${x.providerId}]`)];
  }
  return ['📻 ', x?.name, x?.genre && ` · ${x.genre}`, h('span', { class: 'muted' }, ` [${x?.providerId}]`)];
}

// A track's media type (server phase 30): a book's chapter is marked, with
// the rule that said so; music and untyped tracks are not.
const typeMark = (t) => (t?.mediaType && t.mediaType !== 'music'
  ? h('span', { class: 'pill small', title: `mediaType ${t.mediaType}${t.mediaTypeSource ? ` (by ${t.mediaTypeSource})` : ''}` },
    t.mediaType === 'audiobook' ? '📖' : t.mediaType) : null);

// A book is a favourite by the node it came from ("local:book:3").
function addBookFavorite(source) {
  const at = source?.indexOf(':') ?? -1;
  if (at < 0) throw new Error(tt('common.thisBookNames'));
  return api('POST', 'favorites', { node: { provider: source.slice(0, at), id: source.slice(at + 1) }, ...ownerOf() });
}

// --- playback settings (server phase 31) ---------------------------------------

async function loadPlayback() {
  try {
    S.playback = await api('GET', 'settings/playback', undefined, { quiet: true });
  } catch {
    S.playback = null; // a server before the setting answers 404
  }
  renderPlayback();
}

// A setting a server from before it does not answer stays disabled.
const PLAYBACK_TOGGLES = {
  groupBooks: (on) => (on ? 'books go in as one item' : 'books go in as single tracks'),
  findRadioLikes: (on) => (on ? 'liked radio songs are looked for' : 'liked radio songs are not looked for'),
};

function renderPlayback() {
  for (const key of Object.keys(PLAYBACK_TOGGLES)) {
    const box = $(`#${key}`);
    box.disabled = typeof S.playback?.[key] !== 'boolean';
    box.checked = S.playback?.[key] ?? true;
    box.parentElement.classList.toggle('muted', box.disabled);
  }
}

for (const [key, done] of Object.entries(PLAYBACK_TOGGLES)) {
  $(`#${key}`).addEventListener('change', (e) => act(async () => {
    S.playback = await api('PATCH', 'settings/playback', { [key]: e.target.checked });
    renderPlayback();
    return S.playback;
  }, (p) => done(p[key])).then(() => e.target.blur()));
}

// --- zones ---------------------------------------------------------------------

// A zone's name (its rooms', from the server); the id while it is not known.
const zoneName = (zone) => S.zones.find((z) => z.zone === zone)?.name ?? zone;

// The zone picked: the one in view on the Zones page and the player bar's.
// Picking it in one brings it into the other.
function setTarget(zone) {
  S.target = zone;
  store.set('vx.target', zone);
  renderZones(); // which brings it into view
}

function renderZones() {
  render('zones', $('#zones'), () => (S.zones.length ? S.zones.map(zoneCard) : [h('p', { class: 'muted' }, tt('zones.noZonesYet'))]));
  renderZoneTabs();
  followCurrentItem();
  renderPlayer();
  renderZoneListeners();
  if (page === 'home') renderHome();
  if (page === 'listeners') renderUsersTable();
}

// The zones sit side by side in one track, one zone wide, swiped (or scrolled)
// between and snapped. A tab per zone shows which is in view, brings one into
// view, and takes a dropped room like the zone's card does. The page opens on
// the target zone.
function zoneInView() {
  const track = $('#zones');
  const step = track.clientWidth + (parseFloat(getComputedStyle(track).columnGap) || 0);
  return track.clientWidth ? Math.round(track.scrollLeft / step) : 0;
}

// A tab or an arrow jumps to the zone at once; a swipe still scrolls.
function showZone(index) {
  const track = $('#zones');
  const card = track.children[Math.max(0, Math.min(index, S.zones.length - 1))];
  if (card) track.scrollTo({ left: card.offsetLeft, behavior: 'instant' });
}

// Changing to a zone brings its queue's current item into the middle of the
// queue, as far as the list scrolls.
function centreCurrentItem(index) {
  const list = $('#zones').children[index]?.querySelector('.queue-list');
  const row = list?.querySelector('tr.current');
  if (!row) return;
  const l = list.getBoundingClientRect(), r = row.getBoundingClientRect();
  list.scrollTop += r.top - l.top - (l.height - r.height) / 2;
}

// When the zone in view moves on to another item and that row is out of
// sight in its queue (under the column heads too), the queue scrolls just
// far enough to show it -- but not while the viewer is at it: the pointer on
// the zones, or a scroll, click, touch or key there in the last few seconds.
// It tries again once they have let go.
const followedItem = new Map(); // zone -> the current item last shown
const FOLLOW_QUIET_MS = 4000;
let zonesTouchedAt = 0, followRetry = null;
for (const type of ['wheel', 'pointerdown', 'touchstart', 'keydown']) {
  $('#zones').addEventListener(type, () => { zonesTouchedAt = performance.now(); }, { passive: true, capture: true });
}
function followCurrentItem() {
  if (page !== 'zones') return;
  const index = zoneInView(), z = S.zones[index];
  const current = z?.queue?.current;
  if (current == null || followedItem.get(z.zone) === current) return;
  const list = $('#zones').children[index]?.querySelector('.queue-list');
  const row = list?.querySelector('tr.current');
  if (!row) return;

  const quietFor = performance.now() - zonesTouchedAt;
  if (quietFor < FOLLOW_QUIET_MS || $('#zones').matches(':hover')) {
    clearTimeout(followRetry);
    followRetry = setTimeout(followCurrentItem, Math.max(FOLLOW_QUIET_MS - quietFor, 1000));
    return;
  }
  followedItem.set(z.zone, current);
  const l = list.getBoundingClientRect(), r = row.getBoundingClientRect();
  const top = l.top + (list.querySelector('th')?.offsetHeight ?? 0);
  if (r.top < top) list.scrollTop -= top - r.top;
  else if (r.bottom > l.bottom) list.scrollTop += r.bottom - l.bottom;
}

// The tab of the zone in view stands in the middle of the tab bar: the bar
// slides under it as the zone changes (the bar's spacers let the first and
// last tab reach the middle too).
let centredTab = -1;
// The tab nearest the middle is the one in the marker, which takes its width.
function markZoneTab() {
  const tabs = $('#zoneTabs'), mid = tabs.scrollLeft + tabs.clientWidth / 2;
  let near = null;
  for (const tab of tabs.children) {
    if (!near || Math.abs(tab.offsetLeft + tab.offsetWidth / 2 - mid) < Math.abs(near.offsetLeft + near.offsetWidth / 2 - mid)) near = tab;
  }
  for (const tab of tabs.children) tab.classList.toggle('in-marker', tab === near);
  $('#zoneMarker').style.setProperty('--marker-w', near ? `${near.offsetWidth}px` : '0px');
}
$('#zoneTabs').addEventListener('scroll', markZoneTab, { passive: true });

function centreZoneTab(index) {
  const tabs = $('#zoneTabs'), tab = tabs.children[index];
  if (!tab || !tabs.clientWidth) return;
  const smooth = centredTab !== -1 && !matchMedia('(prefers-reduced-motion: reduce)').matches;
  centredTab = index;
  tabs.scrollTo({ left: tab.offsetLeft + tab.offsetWidth / 2 - tabs.clientWidth / 2, behavior: smooth ? 'smooth' : 'instant' });
}

let zonesPlaced = false;
function renderZoneTabs() {
  const current = zoneInView();
  patch($('#zoneTabs'), S.zones.map((z, i) => h('button', {
    type: 'button', class: 'zone-tab', title: zoneName(z.zone),
    'aria-current': i === current ? 'true' : null,
    onclick: () => showZone(i),
    ...dropTarget((client) => moveRoom(client, z.zone), (items) => dropQueued(z.zone, items)),
  }, zoneName(z.zone))));
  $('#zonePrev').disabled = current <= 0;
  $('#zoneNext').disabled = current >= S.zones.length - 1;
  centreZoneTab(current);
  markZoneTab();
  // The track stays on the picked zone when zones come and go (a room taken
  // out into a zone of its own), before a scroll could pick the one now there.
  if (S.zones.length && page === 'zones') {
    const want = S.zones.findIndex((z) => z.zone === S.target);
    if (!zonesPlaced || (want >= 0 && want !== current)) {
      zonesPlaced = true;
      showZone(want);
      centreCurrentItem(want);
    }
  }
}

// The library slides out and back in by the strip beside the zones; the zones
// widen into its space. Kept per viewer.
const LIBRARY_SLIDE_MS = 240; // the transition on .page-app in app.css
let librarySliding = null;
function showLibrary(open, slide = true) {
  const toggle = $('#libraryToggle');
  toggle.setAttribute('aria-expanded', String(open));
  toggle.title = tt(open ? 'zones.hideLibrary' : 'zones.showLibrary');
  store.set('vx.library', open ? 'open' : 'closed');
  for (const p of document.querySelectorAll('.page-app')) p.classList.toggle('library-closed', !open);
  // The track keeps the zone in view while it changes width: its scroll is
  // not a swipe, and the zone is put back in place once it stops.
  const index = S.zones.findIndex((z) => z.zone === S.target);
  clearTimeout(librarySliding);
  librarySliding = setTimeout(() => { librarySliding = null; showZone(index); }, slide ? LIBRARY_SLIDE_MS + 20 : 0);
}
$('#libraryToggle').addEventListener('click', (e) => showLibrary(e.currentTarget.getAttribute('aria-expanded') !== 'true'));
showLibrary(store.get('vx.library', 'open') === 'open', false);

$('#zones').addEventListener('scroll', () => {
  // Only a track in view picks: hidden (Home is the start page) it reads as at
  // the first zone, which would replace the zone kept from before (vx.target).
  if (librarySliding || page !== 'zones' || !$('#zones').clientWidth) return;
  const current = zoneInView();
  for (const [i, tab] of [...$('#zoneTabs').children].entries()) {
    if (i === current) tab.setAttribute('aria-current', 'true'); else tab.removeAttribute('aria-current');
  }
  $('#zonePrev').disabled = current <= 0;
  $('#zoneNext').disabled = current >= S.zones.length - 1;
  if (current !== centredTab) centreZoneTab(current);
  // The zone swiped to is the picked one, so the player bar follows it.
  const inView = S.zones[current]?.zone;
  if (inView && inView !== S.target) {
    S.target = inView;
    store.set('vx.target', inView);
    renderPlayer();
    centreCurrentItem(current);
  }
}, { passive: true });
addEventListener('resize', () => centreZoneTab(zoneInView()));
$('#zonePrev').addEventListener('click', () => showZone(zoneInView() - 1));
$('#zoneNext').addEventListener('click', () => showZone(zoneInView() + 1));

// What people call a room (server phase 39): its name, else its id.
const roomName = (client) => S.rooms.find((r) => r.client === client)?.name ?? client;

// A room's image (server phase 39) by client: the version that names it
// afresh after room.imageChanged, and the rooms known to have none.
const roomImageVersion = new Map();
const noRoomImage = new Set();
// One without, or whose image fails, shows its fixed art (roomArt).
function roomImage(client) {
  return h('img', {
    class: 'room-image', alt: '',
    src: noRoomImage.has(client) ? roomArt(client) : roomImageUrl(client),
    onerror() { noRoomImage.add(client); this.onerror = null; this.src = roomArt(client); },
  });
}

// A room as a tile, its image behind its name: dragged onto another zone, it
// moves there; clicked, its settings page opens.
function roomTile(client, extra) {
  const room = S.rooms.find((r) => r.client === client);
  const online = Boolean(room?.actual?.connected);
  return h('div', {
    class: 'room-tile' + (online ? '' : ' offline'), draggable: 'true', title: tt(online ? 'zones.openItsSettings' : 'zones.notConnectedOpen', { name: roomName(client), client }),
    role: 'link', tabindex: '0',
    // A click opens its settings page; its own buttons (take out) do not.
    onclick(e) { if (!e.target.closest('button')) openRoom(client); },
    onkeydown(e) { if (e.key === 'Enter' && e.target === e.currentTarget) openRoom(client); },
    ondragstart(e) { e.dataTransfer.setData(ROOM_TYPE, client); S.dragging = true; },
    ondragend() { S.dragging = false; flushDirty(); },
  }, icon('device'), roomImage(client),
  h('span', { class: 'room-label' }, h('span', { class: 'dot' + (online ? ' on' : '') }), h('span', { class: 'room-name' }, roomName(client))),
  extra);
}

// The tile with a +, first in the strip: it lists the rooms in other zones in a popover, and the
// one picked moves into this zone.
function roomAddTile(zone, others) {
  return h('span', { class: 'room-add-pop' },
    h('button', {
      type: 'button', class: 'room-tile room-add', title: tt('zones.addRoom'), 'aria-label': tt('zones.addRoom'), 'aria-haspopup': 'dialog',
      onclick: (e) => e.currentTarget.nextElementSibling.togglePopover(),
    }, icon('plus')),
    h('div', {
      class: 'room-add-popover', popover: 'auto', role: 'dialog', 'aria-label': tt('zones.addRoom'),
      ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); },
    }, others.length
      ? others.map((c) => {
        const room = S.rooms.find((r) => r.client === c);
        const from = room?.desired?.zone && zoneName(room.desired.zone);
        return h('button', {
          type: 'button', title: c, onclick: (e) => { e.currentTarget.closest('[popover]').hidePopover(); moveRoom(c, zone); },
        }, h('span', { class: 'dot' + (room?.actual?.connected ? ' on' : '') }), h('span', { class: 'room-name' }, roomName(c)),
        // The zone it leaves, where that is more than the room on its own.
        from && from !== roomName(c) && h('span', { class: 'muted small' }, from));
      })
      : h('p', { class: 'muted small' }, tt('zones.everyRoomIs'))));
}

// --- multi-select ----------------------------------------------------------------
// As in Windows: a click selects that one, Ctrl+click adds or removes one,
// Shift+click takes the range from the one clicked last (Ctrl+Shift adds the
// range); a click elsewhere or Esc clears. A plain click on a link still
// follows it. One selection at a time: rows of one queue or playlist (by id),
// or library items (by the body they drag).
const SEL = { list: null, ids: new Set(), items: new Map(), anchor: null };
const rowSelected = (list, id) => (SEL.list === list && SEL.ids.has(String(id)) ? '' : null);
function clearSelection() {
  if (!SEL.ids.size && !SEL.items.size) return;
  SEL.list = null;
  SEL.ids.clear();
  SEL.items.clear();
  SEL.anchor = null;
  paintSelection();
}
function paintSelection() {
  for (const r of document.querySelectorAll('tr[data-list]')) r.toggleAttribute('data-selected', SEL.list === r.dataset.list && SEL.ids.has(r.dataset.id));
  for (const el of document.querySelectorAll('[data-drag]')) el.toggleAttribute('data-selected', SEL.items.has(el.dataset.drag));
}
// Selects within `el`'s own list: its siblings that are selectable too.
function select(e, el, keyOf, add) {
  const peers = [...el.parentNode.children].filter((x) => x.matches(el.matches('tr[data-list]') ? 'tr[data-list]' : '[data-drag]'));
  const at = peers.indexOf(el), from = peers.findIndex((x) => keyOf(x) === SEL.anchor);
  const ctrl = e.ctrlKey || e.metaKey;
  if (e.shiftKey && from >= 0) {
    if (!ctrl) { SEL.ids.clear(); SEL.items.clear(); }
    for (const x of peers.slice(Math.min(at, from), Math.max(at, from) + 1)) add(x, true);
  } else if (ctrl) {
    add(el, !(SEL.ids.has(keyOf(el)) || SEL.items.has(keyOf(el))));
    SEL.anchor = keyOf(el);
  } else {
    SEL.ids.clear();
    SEL.items.clear();
    add(el, true);
    SEL.anchor = keyOf(el);
  }
  paintSelection();
}
document.addEventListener('click', (e) => {
  if (e.target.closest('button, input, select, textarea, label')) return;
  const row = e.target.closest('tr[data-list]'), item = row ? null : e.target.closest('[data-drag]');
  const modified = e.ctrlKey || e.metaKey || e.shiftKey;
  if (!(row || item) || (!modified && e.target.closest('a'))) {
    // Browsing the library keeps a list's rows selected: "next" goes after them (afterSelected).
    if (!(SEL.list && e.target.closest('#browserPanel'))) clearSelection();
    return;
  }
  if (modified) {
    e.preventDefault();
    e.stopPropagation();
  }
  if (row) {
    if (SEL.list !== row.dataset.list) { clearSelection(); SEL.list = row.dataset.list; }
    select(e, row, (x) => x.dataset.id, (x, on) => (on ? SEL.ids.add(x.dataset.id) : SEL.ids.delete(x.dataset.id)));
  } else {
    if (SEL.list) clearSelection();
    select(e, item, (x) => x.dataset.drag, (x, on) => (on ? SEL.items.set(x.dataset.drag, JSON.parse(x.dataset.drag)) : SEL.items.delete(x.dataset.drag)));
  }
}, true);
// Shift+click would otherwise select the page's text from the last click.
document.addEventListener('mousedown', (e) => { if (e.shiftKey && e.target.closest('tr[data-list], [data-drag]')) e.preventDefault(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') clearSelection(); });

// What is seen dragging when more than one goes: "3 items".
function dragBadge(e, text) {
  const badge = h('div', { class: 'drag-badge' }, text);
  document.body.append(badge);
  e.dataTransfer.setDragImage(badge, 12, 12);
  setTimeout(() => badge.remove());
}

// A library item drags the bodies POST zones/{zone}/queue/items takes: tracks,
// a station, a node, a playlist or a favourite. A selected one drags the whole
// selection, in the order it is shown.
const ITEM_TYPE = 'application/x-galdrun-queue-item', ROOM_TYPE = 'application/x-galdrun-room';
// What a list shows where library items would land (listDrop): what is
// dragged, by name or as "3 items". Known only here: a dragover cannot read the data.
let itemDragging = null; // {label}
function dragItem(body) {
  const key = JSON.stringify(body);
  return {
    draggable: 'true',
    'data-drag': key,
    'data-selected': SEL.items.has(key) ? '' : null,
    ondragstart(e) {
      let bodies = [body];
      if (SEL.items.has(key) && SEL.items.size > 1) {
        const shown = [...new Set([...document.querySelectorAll('[data-drag][data-selected]')].map((el) => el.dataset.drag))];
        bodies = [...shown, ...[...SEL.items.keys()].filter((k) => !shown.includes(k))].map((k) => SEL.items.get(k));
        dragBadge(e, tt('drag.items', { n: bodies.length }));
      }
      e.dataTransfer.setData(ITEM_TYPE, JSON.stringify(bodies));
      e.dataTransfer.effectAllowed = 'copy';
      itemDragging = { label: bodies.length > 1 ? tt('drag.items', { n: bodies.length }) : this.innerText.trim().split('\n')[0].slice(0, 120) };
      S.dragging = true;
    },
    ondragend() {
      // Dropped elsewhere or cancelled while over a list: its gap closes.
      for (const g of document.querySelectorAll('tr.drop-ghost')) g.remove();
      itemDragging = null;
      S.dragging = false;
      flushDirty();
    },
  };
}

// Tracks next to each other go as one body.
const mergeTracks = (bodies) => bodies.reduce((out, b) => {
  if (b.tracks && out.at(-1)?.tracks) out.at(-1).tracks.push(...b.tracks);
  else out.push(b.tracks ? { tracks: [...b.tracks] } : b);
  return out;
}, []);

// Each body is its own POST. Before a row they all go in at its index, the
// last first, so they end up in the order dragged.
async function postBodies(path, bodies, index) {
  const answers = [];
  if (index == null) for (const b of bodies) answers.push(await api('POST', path, b));
  else for (const b of [...bodies].reverse()) answers.unshift(await api('POST', path, { ...b, index }));
  return answers;
}

// Takes a dragged room (its client) when onRoom is given and a library item
// when onItem is; any other drag (a link, an image) passes on to what is below.
// mark(el, event) shows where an item would land, mark(el, null) clears it.
function dropTarget(onRoom, onItem, mark) {
  const takes = (e) => (onRoom && e.dataTransfer.types.includes(ROOM_TYPE)) || (onItem && e.dataTransfer.types.includes(ITEM_TYPE));
  return {
    ondragover(e) {
      if (!takes(e)) return;
      e.preventDefault();
      e.stopPropagation();
      this.classList.add('drop');
      mark?.(this, e);
    },
    ondragleave() { this.classList.remove('drop'); mark?.(this, null); },
    ondrop(e) {
      if (!takes(e)) return;
      e.preventDefault();
      e.stopPropagation();
      this.classList.remove('drop');
      mark?.(this, null);
      const items = e.dataTransfer.getData(ITEM_TYPE);
      if (items && onItem) onItem(JSON.parse(items), e); else onRoom(e.dataTransfer.getData(ROOM_TYPE));
    },
  };
}

// A row of a queue or a playlist dragged to another place in its own list.
// What is dragged is known only here (a dragover cannot read the data), so a
// row dropped on another list is not taken.
const ROW_TYPE = 'application/x-galdrun-row';
let rowDragging = null; // {list, row, group, rows, ids, carried, dropped}
// The grip a row is dragged by, in its first cell: only it starts a move, so
// the row's text and buttons stay as they are. The whole row is what is seen
// dragging, or "3 items" when it is selected with others: they all go. While
// they are dragged, the rows themselves move to where they would land
// (listDrop); a drag that is not dropped puts them back. With items(id), the
// bodies a row is added by, they drag as library items too, so they can be
// dropped on another list (a playlist in the tree); `carried` names the rows
// that have bodies.
function rowHandle(list, label, items) {
  return h('td', {
    class: 'drag-handle', draggable: 'true', title: tt('drag.dragToMoveTitle'), 'aria-label': tt('drag.dragToMove'),
    ondragstart(e) {
      const row = this.closest('tr');
      const rows = [...row.parentNode.querySelectorAll(':scope > tr[data-index]')];
      const many = SEL.list === list && SEL.ids.has(row.dataset.id) && SEL.ids.size > 1;
      const group = many ? rows.filter((r) => SEL.ids.has(r.dataset.id)) : [row];
      e.dataTransfer.setData(ROW_TYPE, list);
      const carried = items ? group.map((r) => [r.dataset.id, items(r.dataset.id)]).filter(([, b]) => b.length) : [];
      if (items) e.dataTransfer.setData(ITEM_TYPE, JSON.stringify(carried.flatMap(([, b]) => b)));
      e.dataTransfer.effectAllowed = 'move';
      if (group.length > 1) dragBadge(e, tt('drag.items', { n: group.length })); else e.dataTransfer.setDragImage(row, 12, 12);
      rowDragging = { list, row, group, rows, ids: rows.map((r) => r.dataset.id), carried: carried.map(([id]) => id) };
      S.dragging = true;
      // After the drag image is taken, so it is not faded.
      setTimeout(() => group.forEach((r) => r.classList.add('row-dragging')));
    },
    ondragend() {
      const { group, rows, dropped } = rowDragging;
      for (const r of rows) Object.assign(r.style, { transition: '', transform: '' });
      group.forEach((r) => r.classList.remove('row-dragging'));
      if (!dropped) rows[0].parentNode.append(...rows);
      rowDragging = null;
      S.dragging = false;
      flushDirty();
    },
  }, label != null && h('span', { class: 'handle-label' }, label), icon('grip'));
}

// The moves PATCH {index} makes one at a time (each taken out, then put in at
// its index) to turn the order `before` into `after`, where only the ids in
// `moved` changed places: those going down first, the lowest last, then those
// going up, the highest last; one already in place is not sent.
function rowMoves(before, after, moved) {
  const cur = [...before], to = new Map(after.map((id, i) => [id, i]));
  const at = (id) => before.indexOf(id);
  const ids = before.filter((id) => moved.has(id));
  const down = ids.filter((id) => to.get(id) > at(id)).sort((a, b) => to.get(b) - to.get(a));
  const up = ids.filter((id) => to.get(id) < at(id)).sort((a, b) => to.get(a) - to.get(b));
  const moves = [];
  for (const id of [...down, ...up]) {
    if (cur[to.get(id)] === id) continue;
    cur.splice(cur.indexOf(id), 1);
    cur.splice(to.get(id), 0, id);
    moves.push({ id, to: to.get(id) });
  }
  return moves;
}

// While something is dragged near the top or bottom edge of what scrolls a
// list (the list itself, as a queue does, or else the page), it scrolls,
// faster the nearer the edge. A dragover comes every few dozen ms even when
// the pointer rests, so this runs from there.
const DRAG_EDGE_PX = 48;
function dragScroll(el, e) {
  let s = el;
  while (s && !(/auto|scroll/.test(getComputedStyle(s).overflowY) && s.scrollHeight > s.clientHeight)) s = s.parentElement;
  s ??= document.scrollingElement;
  const { top, bottom } = s === document.scrollingElement ? { top: 0, bottom: innerHeight } : s.getBoundingClientRect();
  const up = top + DRAG_EDGE_PX - e.clientY, down = e.clientY - (bottom - DRAG_EDGE_PX);
  if (up > 0) s.scrollTop -= Math.ceil(up / 3);
  else if (down > 0) s.scrollTop += Math.ceil(down / 3);
}

// The drop side of a list: rows of `list` go where they are shown when
// dropped (onRows gets the moves); with onItem, library items too, where
// their placeholder row is shown when dropped (onItem gets the index before
// which they go, null for the end).
function listDrop(list, onRows, onItem) {
  const takesRow = (e) => e.dataTransfer.types.includes(ROW_TYPE) && rowDragging?.list === list;
  const takesItem = (e) => onItem && e.dataTransfer.types.includes(ITEM_TYPE);
  const ROWS = 'tr[data-index], tr.drop-ghost';
  // The rows in `group` (the dragged rows, or the placeholder) move, together,
  // before the row under the pointer when the pointer is in that row's upper
  // half, after it in its lower half (so they do not flip back and forth), and
  // to the end below the last row.
  const preview = (box, e, group) => {
    const together = group[0].isConnected && group.every((r, i) => i === 0 || group[i - 1].nextElementSibling === r);
    let over = e.target.closest?.(ROWS);
    if (over && group.includes(over)) return;
    let below = false;
    if (!over) {
      const rows = box.querySelectorAll(ROWS);
      over = rows[rows.length - 1];
      if (!over || group.includes(over) || e.clientY <= over.getBoundingClientRect().bottom) return;
      below = true;
    }
    const { top, height } = over.getBoundingClientRect();
    if (!below && e.clientY < top + height / 2) {
      if (!(together && over.previousElementSibling === group.at(-1))) slide(box, () => over.before(...group));
    } else if (!(together && over.nextElementSibling === group[0])) {
      slide(box, () => over.after(...group));
    }
  };
  // The rows a move passes slide to their new places instead of jumping:
  // each starts where it was and is animated to where it now is.
  const slide = (box, move) => {
    const rows = [...box.querySelectorAll(ROWS)];
    const was = new Map(rows.map((r) => [r, r.getBoundingClientRect().top]));
    move();
    const kept = rows.filter((r) => r.isConnected);
    // Measured without a slide still running, which would offset it.
    for (const r of kept) Object.assign(r.style, { transition: 'none', transform: '' });
    for (const r of kept) {
      const dy = was.get(r) - r.getBoundingClientRect().top;
      if (!dy) continue;
      r.style.transform = `translateY(${dy}px)`;
      r.getBoundingClientRect(); // starts the animation from there
      r.style.transition = 'transform 150ms ease';
      r.style.transform = '';
    }
  };
  // Library items are shown as one placeholder row where they would land.
  const ghostOf = (box) => box.querySelector('tr.drop-ghost')
    ?? h('tr', { class: 'drop-ghost' }, h('td', { colspan: '99' }, itemDragging?.label ?? tt('drag.dropHere')));
  return {
    ondragover(e) {
      if (takesRow(e)) {
        e.preventDefault();
        e.stopPropagation();
        e.dataTransfer.dropEffect = 'move';
        dragScroll(this, e);
        preview(this, e, rowDragging.group);
        return;
      }
      if (!takesItem(e)) return;
      e.preventDefault();
      e.stopPropagation();
      e.dataTransfer.dropEffect = 'copy';
      dragScroll(this, e);
      this.classList.add('drop');
      preview(this, e, [ghostOf(this)]);
    },
    ondragleave(e) {
      // Between two of its own rows the pointer has not left.
      if (this.contains(e.relatedTarget)) return;
      this.classList.remove('drop');
      const ghost = this.querySelector('tr.drop-ghost');
      if (ghost) slide(this, () => ghost.remove());
    },
    ondrop(e) {
      if (!takesRow(e) && !takesItem(e)) return;
      e.preventDefault();
      e.stopPropagation();
      this.classList.remove('drop');
      if (takesRow(e)) {
        // Where the rows are now shown is where they go.
        const after = [...this.querySelectorAll('tr[data-index]')].map((r) => r.dataset.id);
        const moves = rowMoves(rowDragging.ids, after, new Set(rowDragging.group.map((r) => r.dataset.id)));
        rowDragging.dropped = moves.length > 0;
        if (moves.length) onRows(moves);
        return;
      }
      const ghost = this.querySelector('tr.drop-ghost');
      const rows = [...this.querySelectorAll(ROWS)];
      const at = ghost && ghost !== rows.at(-1) ? rows.indexOf(ghost) : null;
      ghost?.remove();
      onItem(JSON.parse(e.dataTransfer.getData(ITEM_TYPE)), at);
    },
  };
}

// Library items dropped on a zone: before the queue row at `index`, or at
// the end.
async function dropOnQueue(zone, items, index = null) {
  const answers = await postBodies(`zones/${enc(zone)}/queue/items`, mergeTracks(items), index);
  const queued = answers.map((r) => r?.queued).filter((n) => n != null);
  return queuedFlash({
    queued: queued.length ? queued.reduce((a, b) => a + b, 0) : null,
    refused: answers.flatMap((r) => r?.refused ?? []),
    grouped: answers.some((r) => r?.grouped),
    truncated: answers.some((r) => r?.truncated),
  });
}
// Drops under way per zone: the queue is covered until they are answered and
// the zones read again, so the items are in place when it clears.
const dropping = new Map(); // zone -> count
async function dropQueued(zone, items, index = null) {
  dropping.set(zone, (dropping.get(zone) ?? 0) + 1);
  renderZones();
  try {
    await act(() => dropOnQueue(zone, items, index));
    await refreshCore(true);
  } finally {
    const left = dropping.get(zone) - 1;
    if (left) dropping.set(zone, left); else dropping.delete(zone);
    renderZones();
  }
}
// Re-read either way: a refused move puts the rows back where they were shown from.
const queueDrop = (zone) => listDrop(`queue:${zone}`, (moves) => act(async () => {
  try {
    for (const m of moves) await api('PATCH', `zones/${enc(zone)}/queue/items/${m.id}`, { index: m.to });
  } finally {
    refreshSoon();
  }
}, 'common.moved'), (items, index) => dropQueued(zone, items, index));

// Library items dropped on a playlist, as on a queue. A playlist keeps tracks
// and nodes (POST playlists/{id}/entries); stations, playlists and
// favourites are left out, and said so.
async function dropOnPlaylist(id, items, index = null) {
  const kept = items.filter((b) => b.tracks || b.node);
  if (!kept.length) throw new Error(tt('drag.playlistTakesTracks'));
  await postBodies(`playlists/${id}/entries`, mergeTracks(kept), index);
  const left = items.length - kept.length;
  flash(tt('drag.addedToPlaylist') + (left ? ` · ${tt('drag.leftOutStations', { n: left })}` : ''), left > 0);
}

// A transport command sent and not answered yet, by zone: the button asked
// (play stands for pause too, they share a place). It shows a spinner and the
// zone's other transport buttons wait, until the answer or its failure.
const transporting = new Map();
const TRANSPORT_TIMEOUT_MS = 30_000;

function transportButton(name, title, onclick, cls = 'icon-only', zone = null, slot = name) {
  const busy = zone != null && transporting.get(zone) === slot;
  return h('button', {
    class: cls, title, 'aria-label': title, 'aria-busy': busy ? 'true' : null,
    disabled: zone != null && transporting.has(zone), onclick,
  }, busy ? h('span', { class: 'spinner' }) : icon(name));
}

// Sends a transport command for `zone` with its button held busy; the answer's
// queue state is shown at once, the events follow.
async function transport(zone, slot, action, body) {
  if (transporting.has(zone)) return;
  transporting.set(zone, slot);
  renderZones();
  const answer = await act(() => api('POST', `zones/${enc(zone)}/${action}`, body, { timeoutMs: TRANSPORT_TIMEOUT_MS }));
  transporting.delete(zone);
  const z = S.zones.find((x) => x.zone === zone);
  if (answer?.items && z) { z.queue = answer; received.set(answer, performance.now()); }
  renderZones();
}

// A zone's shuffle, previous, play or pause, next and repeat: in the player
// bar and in the zone's queue head.
function transportControls(z) {
  const q = z.queue, path = `zones/${enc(z.zone)}`;
  return [
    shuffleButton(Boolean(q?.shuffle), !q, (shuffle) => act(() => api('PATCH', `${path}/queue`, { shuffle }))),
    transportButton('prev', tt('player.previous'), () => transport(z.zone, 'prev', 'previous'), 'icon-only', z.zone),
    q?.transport === 'playing'
      ? transportButton('pause', tt('common.pause'), () => transport(z.zone, 'play', 'pause'), 'primary play', z.zone, 'play')
      : transportButton('play', tt('player.play'), () => transport(z.zone, 'play', 'play'), 'primary play', z.zone),
    transportButton('next', tt('player.next'), () => transport(z.zone, 'next', 'next'), 'icon-only', z.zone),
    repeatButton(q?.repeat ?? 'off', !q, (repeat) => act(() => api('PATCH', `${path}/queue`, { repeat }))),
  ];
}

// Repeat cycles off → all → one; its icon says which is set.
const REPEAT = { off: ['repeatOff', 'all'], all: ['repeat', 'one'], one: ['repeatOne', 'off'] };
function repeatButton(mode, disabled, set) {
  const [name, next] = REPEAT[mode] ?? REPEAT.off;
  const title = tt('player.repeatClick', { mode: tv('player.repeat', mode), next: tv('player.repeat', next) });
  return h('button', {
    class: 'icon-only mode' + (mode !== 'off' ? ' on' : ''), title, 'aria-label': title, disabled, onclick: () => set(next),
  }, icon(name));
}

function shuffleButton(on, disabled, set) {
  const title = tt(on ? 'player.shuffleOnClick' : 'player.shuffleOffClick');
  return h('button', {
    class: 'icon-only mode' + (on ? ' on' : ''), title, 'aria-label': tt('player.shuffle'), 'aria-pressed': on ? 'true' : 'false',
    disabled, onclick: () => set(!on),
  }, icon(on ? 'shuffle' : 'shuffleOff'));
}

// What the zone plays: the current queue item, else the core's source.
// What the zone plays: the current queue item, else the core's source. The
// zone card and the player bar show the same.
function playing(z) {
  const q = z.queue, a = z.actual;
  const item = q?.items.find((i) => i.id === q.current);
  const t = media(item, 'track'), s = media(item, 'station'), g = media(item, 'group');
  const title = t?.title ?? g?.title ?? s?.name ?? a?.source?.title ?? tt('player.nothingPlaying');
  const sub = t ? [t.artist, t.album].filter(Boolean).join(' · ')
    : g ? [g.artist, q.part && tt('player.chapterOf', { n: q.part.index + 1, count: q.part.count, title: q.part.title })].filter(Boolean).join(' · ')
      : s ? z.streamTitle ?? s.genre ?? '' : '';
  const cover = () => art(item?.item.artworkId) ?? icon(s ? 'radio' : 'library');
  // On a station the heart is the song it announces, liked by its title,
  // artist and album; a station itself is never liked (none while it
  // announces nothing).
  const like = () => (t ? likeButton('track', t) : s && z.radioTrack && likeButton('track', z.radioTrack, true));
  const favorite = () => (item && (t || s || g) && favoriteButton(g ? `book|${g.source}` : likeKey(t ? 'track' : 'station', t ?? s),
    () => (g ? addBookFavorite(g.source) : addFavorite(t, s)), Boolean(g)));
  return { item, t, s, g, title, sub, cover, like, favorite };
}

// A small cover on the left; beside it the title and the artist and album.
// The transport and the seek bar are the player bar's.
// Anything in lead stands above the title.
function nowPlaying(z, ...lead) {
  const p = playing(z);
  const artwork = p.item?.item.artworkId;
  return h('div', { class: 'now' },
    artwork
      ? h('button', { type: 'button', class: 'cover-art', title: tt('player.showArtwork'), 'aria-label': tt('player.showArtwork'), onclick: () => openArtwork(artwork) }, p.cover())
      : h('div', { class: 'cover-art' }, p.cover()),
    h('div', { class: 'now-text' },
      lead,
      h('div', { class: 'now-title', title: p.title }, p.title),
      // Its line is kept with nothing in it, so the header keeps its height.
      h('div', { class: 'now-sub', title: p.sub || null }, p.sub || ' ')));
}

// Artwork over the whole page, as large as fits; a click anywhere or Esc
// closes it (the browser's popover).
function openArtwork(id) {
  const view = h('div', {
    class: 'art-view', popover: 'auto', role: 'dialog', 'aria-label': tt('player.artwork'),
    onclick() { this.hidePopover(); },
    ontoggle(e) { if (e.newState === 'closed') this.remove(); },
  }, h('img', { src: `/api/v1/artwork/${enc(id)}`, alt: '' }));
  document.body.append(view);
  view.showPopover();
}

// --- the player bar ------------------------------------------------------------
// Fixed at the foot of every page: what plays in one zone, its transport, its
// queue modes and its own volume. The zone is the picked one (setTarget), the
// same as the one in view on the Zones page; with none, the first.
function playerZone() {
  return S.zones.find((z) => z.zone === S.target) ?? S.zones[0];
}

// The bar's progress line: the current item's length, null where there is
// nothing to seek in (a station, an unknown length).
function playerLength(q) {
  const item = q?.items.find((i) => i.id === q.current);
  const ms = q?.durationMs ?? durationMs(media(item, 'track')?.duration);
  return q?.current != null && ms ? ms : null;
}

function showPlayerProgress() {
  const line = $('#player .player-progress');
  const q = S.zones.find((z) => z.zone === line?.dataset.zone)?.queue;
  const ms = playerLength(q);
  if (!line) return;
  if (ms && !received.has(q)) received.set(q, performance.now());
  const at = ms ? seeking.get(line.dataset.zone) ?? positionNow(q, ms) : 0;
  line.firstChild.style.width = ms ? `${(at / ms) * 100}%` : '0';
  line.title = ms ? tt('player.clickToSeek', { at: fmtMs(at), length: fmtMs(ms) }) : tt('player.nothingToSeek');
}

// The bar's zone, and a popup of every zone to pick another: the pick is the
// target zone, where library actions go too.
function zonePicker(z) {
  return h('span', { class: 'zone-pick' },
    h('button', {
      class: 'player-zone', title: tt('player.changeZone', { zone: zoneName(z.zone) }), 'aria-label': tt('player.zoneChange', { zone: zoneName(z.zone) }), 'aria-haspopup': 'dialog',
      onclick: (e) => e.currentTarget.nextElementSibling.togglePopover(),
    }, icon('zones'), h('span', {}, zoneName(z.zone))),
    h('div', {
      class: 'volume-popover zone-popover', popover: 'auto', role: 'dialog', 'aria-label': tt('common.zones'),
      ontoggle(e) {
        if (e.newState !== 'open') return;
        // Above the button: the bar is at the foot of the window.
        const b = this.previousElementSibling.getBoundingClientRect();
        this.style.left = `${Math.max(8, Math.min(innerWidth - this.offsetWidth - 8, b.right - this.offsetWidth))}px`;
        this.style.top = `${Math.max(8, b.top - this.offsetHeight - 8)}px`;
        this.querySelector('button.on, button')?.focus({ preventScroll: true });
      },
    }, S.zones.map((x) => h('button', {
      class: 'zone-choice' + (x.zone === z.zone ? ' on' : ''), 'aria-current': x.zone === z.zone ? 'true' : null,
      onclick() { this.closest('[popover]').hidePopover(); setTarget(x.zone); },
    }, h('span', { class: 'grow' }, zoneName(x.zone)),
    h('span', { class: 'small muted' }, (x.queue?.transport === 'playing' ? tt('player.playing') : tv('state', x.actual?.transport)))))));
}

function renderPlayer() {
  const z = playerZone();
  const bar = $('#player');
  bar.hidden = !z;
  if (!z) return;
  const q = z.queue, path = `zones/${enc(z.zone)}`, p = playing(z);
  render('player', bar, () => [
    h('div', {
      class: 'player-progress' + (playerLength(q) ? '' : ' off'), 'data-zone': z.zone,
      onclick(e) {
        const line = e.currentTarget, zone = line.dataset.zone;
        const queue = S.zones.find((x) => x.zone === zone)?.queue, ms = playerLength(queue);
        if (!ms) return;
        const r = line.getBoundingClientRect();
        const positionMs = Math.round(Math.min(Math.max((e.clientX - r.left) / r.width, 0), 1) * ms);
        seek(zone, queue.current, positionMs);
      },
    }, h('div', { class: 'fill' })),
    h('div', { class: 'player-now' },
      h('div', { class: 'cover-art' }, p.cover()),
      h('div', { class: 'player-text' },
        h('div', { class: 'player-title', title: p.title }, p.title),
        h('div', { class: 'player-sub' }, p.sub || '\u00a0'))),
    // The like before shuffle and the star after repeat, a little apart; an
    // empty slot keeps the place when there is nothing to mark.
    h('div', { class: 'player-transport' },
      h('span', { class: 'player-mark' }, p.like()),
      transportControls(z),
      h('span', { class: 'player-mark' }, p.favorite())),
    h('div', { class: 'player-side' },
      zonePicker(z),
      z.autoFill && autoFillButton(z),
      // The Zones page opens on the picked zone (onShow).
      transportButton('queue', tt('player.showQueue'), () => { location.hash = '#/zones'; }),
      // The zone's own volume and mute, over its rooms' own (server phase 15).
      z.desired && volumeRow(z.desired.volume, z.desired.muted,
        (volume) => act(() => api('PATCH', path, { volume })),
        (muted) => act(() => api('PATCH', path, { muted })))),
  ]);
  showPlayerProgress();
}

// --- likes and favourites ----------------------------------------------------
// A like (heart) is the household's taste, for recommendations; a favourite
// (star) is something kept to play again. The likes are read once and then
// followed by like.changed, so every heart on the page shows the server's state.

const likes = new Map(); // 'track|provider|item' (or station, album, artist, audiobook) -> like id
const likeKey = (kind, x) => `${kind}|${x.providerId}|${x.providerItemId}`;

const likedItems = new Map(); // like id -> the LikedItem, for the Liked page
const likedKind = (l) => l.item?.kind;
// A song liked off the radio, and the track the server found it as (settings/playback findRadioLikes), if any.
const radioSong = (l) => likedKind(l) === 'track' && l.item.providerId === 'radio-metadata';
const foundTrack = (l) => (radioSong(l) ? l.found?.track ?? null : null);
// Where a found radio song came from, before its title.
const radioMark = (found) => h('span', {
  class: 'pill small radio-mark', title: tt(found.close ? 'likes.likedOffRadio' : 'likes.likedOffRadioFound', { provider: found.track.providerId }),
}, found.close ? `${tt('likes.radio')} ≈` : tt('likes.radio'));

// A liked book (server phase 37) is its node, "provider:node" in `reference`;
// the node itself may hold colons.
function bookRef(reference) {
  const at = reference?.indexOf(':') ?? -1;
  return at < 0 ? null : { providerId: reference.slice(0, at), providerItemId: reference.slice(at + 1) };
}
const likedThing = (kind, l) => (kind === 'audiobook' ? bookRef(l.item.reference) : l.item);

function noteLike(l) {
  const kind = likedKind(l);
  const x = kind && likedThing(kind, l);
  if (!x) return;
  likes.set(likeKey(kind, x), l.id);
  likedItems.set(l.id, l);
}

function forgetLike(id) {
  for (const [key, x] of likes) if (x === id) likes.delete(key);
  likedItems.delete(id);
}

// --- the listener (server phase 36) -------------------------------------------
// Whose marks are read: the household's ('') or a user's own, without the
// household's (only=true), so a heart shown is one a click on it removes.
// A like or favourite made here is the listener's. Everyone's are in the
// library's All roots. Proposals are per listener already (proposalsQuery).

const scopeQuery = (sep = '?') => (S.user ? `${sep}user=${enc(S.user)}&only=true` : '');
const proposalsQuery = () => (S.user ? `?user=${enc(S.user)}` : '');
const ownerOf = () => (S.user ? { user: S.user } : {});
const userPill = (user) => (user ? h('span', { class: 'pill small', title: tt('listeners.whose') }, `👤 ${user}`) : null);

async function loadUsers() {
  S.users = await api('GET', 'users', undefined, { quiet: true });
  if (S.user && !S.users.some((u) => u.name === S.user)) setListener('');
  renderUsers();
}

function renderUsers() {
  renderSources(); // the listeners' roots
  renderHeaderListener();
  renderZones(); // the zone cards' listener selects, and the Listeners page
}

// The header's listener: a people button, the one shown beneath it, and a
// click lists the household and the users to pick from.
function renderHeaderListener() {
  const name = (u) => u || tt('common.household');
  $('#listenerName').textContent = name(S.user);
  $('#listener').setAttribute('aria-label', `${tt('header.listener')}: ${name(S.user)}`);
  patch($('#listenerPick'), ['', ...S.users.map((u) => u.name)].map((u) => h('button', {
    type: 'button', 'aria-pressed': u === S.user ? 'true' : 'false',
    onclick: () => { $('#listenerPick').hidePopover(); if (u !== S.user) setListener(u); },
  }, h('span', { class: 'room-name' }, name(u)))));
}
$('#listener').addEventListener('click', () => $('#listenerPick').togglePopover());
$('#listenerPick').addEventListener('toggle', (e) => { if (e.newState === 'open') placeUnderButton(e.target); });

// The Listeners page: each one, the zones they listen in, and the header's choice.
function renderUsersTable() {
  patch($('#users'), S.users.length
    ? h('table', { class: 'small' },
      h('tr', {}, h('th', {}, tt('listeners.name')), h('th', {}, tt('listeners.listeningIn')), h('th', {}, tt('listeners.added')), h('th', {})),
      S.users.map((u) => h('tr', {},
        h('td', {}, u.name, S.user === u.name && h('span', { class: 'pill good small' }, ` ${tt('listeners.shown')}`)),
        h('td', {}, S.rooms.filter((r) => r.user === u.name).map((r) => r.name ?? r.client).join(', ') || h('span', { class: 'muted' }, '—')),
        h('td', { class: 'muted' }, new Date(u.added).toLocaleString()),
        h('td', {},
          h('button', { title: tt('listeners.showTheirLikes'), disabled: S.user === u.name, onclick: () => setListener(u.name) }, tt('listeners.showAsListener')),
          h('button', { class: 'danger', title: `DELETE users/${u.name}`, onclick: () => removeListener(u.name) }, '✕')))))
    : h('p', { class: 'muted' }, tt('listeners.noListenersYet')));
}

// Removing a listener asks what becomes of their likes, favourites, plays
// and book positions: moved to the household, or deleted with them.
function removeListener(name) {
  const remove = (data) => {
    dialog.close();
    act(() => api('DELETE', `users/${enc(name)}?data=${data}`), 'listeners.listenerRemoved').then(() => {
      act(loadUsers);
      loadLikes().catch(() => {});
      act(loadFavorites);
      if (MB.at >= 0) mbLoad();
    });
  };
  const dialog = h('dialog', { class: 'confirm-dialog', 'aria-label': tt('listeners.removeListener', { name }), onclose: () => dialog.remove() },
    h('div', { class: 'confirm-head' }, h('span', { class: 'confirm-icon' }, icon('people')), h('h2', {}, tt('listeners.removeListener', { name }))),
    h('p', {}, tt('listeners.removeWhatOfTheirs', { name })),
    h('div', { class: 'confirm-buttons' },
      h('button', { type: 'button', onclick: () => dialog.close() }, tt('common.cancel')),
      h('button', { type: 'button', onclick: () => remove('household') }, tt('listeners.moveToHousehold')),
      h('button', { type: 'button', class: 'danger', onclick: () => remove('delete') }, tt('listeners.deleteTheirs'))));
  document.body.append(dialog);
  dialog.showModal();
}

// A room's listener (PATCH rooms/{client} {user}): '' the household.
const setRoomListener = (client, user) => api('PATCH', `rooms/${enc(client)}`, { user });
const listenerNames = (also) => [...new Set([...S.users.map((u) => u.name), also].filter(Boolean))];

// Each room and who listens there; a zone's listener follows from its rooms'.
function renderZoneListeners() {
  if (page !== 'listeners') return;
  patch($('#zoneListeners'), S.rooms.length
    ? h('table', { class: 'small' },
      h('tr', {}, h('th', {}, tt('listeners.room')), h('th', {}, tt('listeners.listener')), h('th', {}, tt('listeners.zoneListener'))),
      S.rooms.map((r) => {
        const z = S.zones.find((x) => (x.desired?.clients ?? x.actual?.clients ?? []).includes(r.client));
        return h('tr', {},
          h('td', {}, r.name ?? r.client),
          h('td', {}, h('select', {
            value: r.user ?? '', 'aria-label': tt('listeners.listenerIn', { zone: r.name ?? r.client }),
            onchange: (e) => act(() => setRoomListener(r.client, e.target.value), 'listeners.listenerSet'),
          }, h('option', { value: '' }, tt('common.household')), listenerNames(r.user).map((n) => h('option', { value: n }, n)))),
          h('td', { class: 'muted' }, z ? `${z.name ?? z.zone}: ${z.user || tt('common.household')}` : '—'));
      }))
    : h('p', { class: 'muted' }, tt('listeners.noRoomsYet')));
}

function setListener(user) {
  S.user = user;
  store.set('vx.user', user);
  renderHeaderListener();
  loadLikes().catch(() => {});
  act(loadFavorites);
  if (MB.places[MB.at]?.provider === PROPOSALS) mbLoad();
  // Home's plays and proposals are the listener's too.
  if (page === 'home') act(loadHome);
  else if (page === 'aiStatus') act(loadProposals);
  renderZones();
}
$('#userForm').addEventListener('submit', (e) => {
  e.preventDefault();
  // Sent as typed: the server trims it and refuses what it cannot take.
  act(() => api('POST', 'users', { name: e.target.name.value }), (u) => tt('listeners.userAdded', { name: u.name }))
    .then((u) => { if (u) e.target.reset(); act(loadUsers); });
});

async function loadLikes() {
  const all = await api('GET', `likes${scopeQuery()}`, undefined, { quiet: true });
  likes.clear();
  likedItems.clear();
  all.forEach(noteLike);
  likesChanged();
}

// Whose a like is decides whether this scope shows it, and an item marked by
// the household and the listener is one row: read the scope again.
function onLikeChanged() {
  loadLikes().catch(() => {});
}

// The zones re-render; the lists built once (search, browse) are set in place.
function likesChanged() {
  renderZones();
  if (page === 'home') renderHome();
  for (const b of document.querySelectorAll('button.like[data-like]')) showLike(b);
}

function showLike(b) {
  const on = likes.has(b.dataset.like);
  b.classList.toggle('on', on);
  b.title = tt(on ? 'likes.unlike' : 'likes.iLikeIt');
  b.setAttribute('aria-label', b.title);
  b.setAttribute('aria-pressed', String(on));
  b.replaceChildren(icon(on ? 'heartOn' : 'heart'));
}

// A song a station announced is liked by its title, artist and album, as
// the server asks for it (it makes the same track id the zone names).
// `body` is what is liked when it is not the item itself: a book's node (server phase 37).
function likeButton(kind, x, announced = false, body = null) {
  const key = likeKey(kind, x);
  const b = h('button', {
    class: 'like', 'data-like': key,
    onclick: () => act(async () => {
      const id = likes.get(key);
      if (id != null) { await api('DELETE', `likes/${id}`); forgetLike(id); likesChanged(); return 'like removed'; }
      const what = body ?? (announced ? { metadata: { title: x.title, artist: x.artist ?? undefined, album: x.album ?? undefined } } : { [kind]: x });
      const l = await api('POST', 'likes', { ...what, ...ownerOf() });
      noteLike(l);
      likesChanged();
      return l.item?.kind === 'audiobook' && kind !== 'audiobook' ? 'liked as a book' : 'liked';
    }, (done) => done),
  });
  showLike(b);
  return b;
}

// The favourites by what they hold: a track or a station by its ids, a book
// by the node it came from. Read with the Favourites list, in the listener's
// scope, and again on favorite.changed (server phase 36).
const favorites = new Map(); // 'track|provider|item', 'station|…' or 'book|<source>' -> favorite id
const favoriteKey = (f) => (media(f, 'track') ? likeKey('track', f.item) : media(f, 'station') ? likeKey('station', f.item)
  : media(f, 'group')?.source ? `book|${f.item.source}` : null);

function favoritesChanged() {
  favorites.clear();
  for (const f of S.favorites) { const key = favoriteKey(f); if (key) favorites.set(key, f.id); }
  renderZones();
  if (page === 'home') renderHome();
  for (const b of document.querySelectorAll('button.fav[data-fav]')) showFavorite(b);
}

function favoriteButton(key, add, book = false) {
  const b = h('button', {
    class: 'fav', 'data-fav': key, 'data-book': book ? 'true' : null,
    onclick: () => act(async () => {
      const id = favorites.get(key);
      if (id != null) { await api('DELETE', `favorites/${id}`); return 'favourite removed'; }
      await add();
      return 'saved as a favourite';
    }, (done) => done).then(() => act(loadFavorites)),
  });
  showFavorite(b);
  return b;
}

function showFavorite(b) {
  const on = favorites.has(b.dataset.fav);
  b.classList.toggle('on', on);
  b.title = tt(on ? 'likes.removeFromFavourites' : b.dataset.book ? 'likes.saveThisBook' : 'likes.saveAsFavourite');
  b.setAttribute('aria-label', b.title);
  b.setAttribute('aria-pressed', String(on));
  b.replaceChildren(icon(on ? 'starOn' : 'star'));
}

// --- likes as the library's Liked shows books and songs heard on the radio.

// Books and songs liked off the radio (not found as a track yet), as the
// library's tracks are listed (mbTracks): a book plays by its node, as a
// favourite book does (its saved place applies when resumeBooks is on); a
// radio song is its title alone, so it only has its heart.
function likedRow(l) {
  const x = l.item, book = likedKind(l) === 'audiobook';
  const node = book ? bookRef(x.reference) : null;
  const ref = node && { node: { provider: node.providerId, id: node.providerItemId } };
  const sub = book ? [x.author, x.reference] : [x.artist, x.album, tt('likes.heardOnRadio')];
  // A radio song cannot be dragged: its grip only keeps the rows in line.
  const grip = dragGrip('td');
  if (!ref) grip.style.visibility = 'hidden';
  return h('tr', ref ? dragItem(ref) : {},
    grip,
    h('td', {}, artSlot(x.artworkId, book ? 'library' : 'radio')),
    h('td', { class: 'row-text' }, h('div', { class: 'track-lines', title: book ? `${x.title} [${x.reference}]` : x.title },
      h('div', { class: 'line' }, book && '📖 ', x.title),
      h('div', { class: 'muted line' }, [...sub, tt('likes.liked', { when: new Date(l.added).toLocaleString() })].filter(Boolean).join(' · '), ' ', userPill(l.user)))),
    h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
      h('span', { class: 'num' }, book && x.durationMs != null ? fmtMs(x.durationMs) : ''),
      h('span', { class: 'row-tools' }, ref && refButtons(ref, 'row-play'), likeButton(book ? 'audiobook' : 'track', node ?? x)))));
}

function volumeRow(volume, muted, setVolume, setMuted, { disabled = false, title = '' } = {}) {
  const pct = (v) => `${Math.round(Number(v) * 100)} %`;
  return h('div', { class: 'volume-row' },
    h('button', {
      class: 'icon-only' + (muted ? ' danger' : ''), title: tt(muted ? 'zones.unmuteTitle' : 'zones.muteTitle'), 'aria-label': tt(muted ? 'zones.unmute' : 'zones.mute'),
      'aria-pressed': muted ? 'true' : 'false', onclick: () => setMuted(!muted),
    }, icon(muted ? 'mute' : 'volume')),
    h('input', {
      type: 'range', min: 0, max: 1, step: 0.01, value: String(volume ?? 1), disabled, title, 'aria-label': tt('zones.volume'),
      oninput: (e) => { e.target.nextElementSibling.textContent = pct(e.target.value); },
      onchange: (e) => setVolume(Number(e.target.value)),
    }),
    h('span', { class: 'vol' }, volume == null ? '—' : pct(volume)));
}

// A queue item's failure as a warning sign; a click shows the message in a
// popover, so a long reason never stretches the row.
function failureBadge(failure) {
  return h('span', { class: 'failure-pop' },
    h('button', {
      class: 'failure-button', title: failure, 'aria-label': tt('zones.playbackFailedShow'), 'aria-haspopup': 'dialog',
      onclick: (e) => e.currentTarget.nextElementSibling.togglePopover(),
    }, icon('alert')),
    h('div', {
      class: 'failure-popover', popover: 'auto', role: 'dialog', 'aria-label': tt('zones.playbackError'),
      ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); },
    }, failure));
}

// A popover centred under the button before it, kept inside the viewport.
function placeUnderButton(popover) {
  const b = popover.previousElementSibling.getBoundingClientRect();
  const w = popover.offsetWidth;
  popover.style.left = `${Math.max(8, Math.min(innerWidth - w - 8, b.left + b.width / 2 - w / 2))}px`;
  popover.style.top = `${b.bottom + 8}px`;
}

// They are placed once, so a scroll that would leave one behind closes it.
document.addEventListener('scroll', () => {
  hideItemInfo();
  for (const p of document.querySelectorAll('.volume-popover:popover-open, .failure-popover:popover-open, .room-add-popover:popover-open, .pick-popover:popover-open')) p.hidePopover();
}, { passive: true, capture: true });

function zoneCard(z) {
  const a = z.actual, q = z.queue, desired = z.desired?.clients ?? [];
  // Rows whose removal is under way are already gone from view.
  const shownCount = q?.items.filter((i) => !removing.has(i.id)).length ?? 0;
  // One zone in view at a time, so a zone is never collapsed.
  const card = h('div', {
    class: 'card zone',
    ...dropTarget((client) => moveRoom(client, z.zone), (items) => dropQueued(z.zone, items)),
  });

  const others = S.rooms.map((r) => r.client).filter((c) => !desired.includes(c));
  // Its members' health, live from rooms.sync (server phase 34): the core's
  // unstable and the server's degraded.
  const unwell = (a?.members ?? []).map((c) => [c, S.rooms.find((r) => r.client === c)?.actual?.health ?? a?.health?.[c]])
    .filter(([, hl]) => hl && hl.state !== 'ok');
  const unstable = unwell.filter(([, hl]) => hl.state === 'unstable');

  appendKids(card, [
    h('div', { class: 'zone-main' },
    // Behind it, as on a room's settings page (zoneBackdrop).
    zoneBackdrop(z, desired[0]),
    // A header as on a room's settings page: the cover, the zone's state, its
    // name and what plays.
    h('div', { class: 'zone-top' },
      nowPlaying(z,
        h('div', { class: 'zone-pills' },
          a ? h('span', { class: 'pill ' + (a.state === 'failed' ? 'bad' : a.transport === 'playing' ? 'good' : ''), title: `core ${a.state}/${a.transport}` }, (a.state === 'failed' ? tt('zones.failed') : tv('state', a.transport)))
            : h('span', { class: 'pill warn' }, tt('zones.notInCore')),
          unwell.length > 0 && h('span', { class: 'pill ' + (unstable.length ? 'bad' : 'warn'), title: unwell.map(([c, hl]) => `${roomName(c)}: ${healthText(hl)}`).join('\n') },
            `⚠ ${unwell.map(([c, hl]) => `${roomName(c)} ${tv('state', hl.state)}`).join(', ')}`)),
        h('h2', { class: 'zone-name' }, z.name ?? z.zone))),

    h('div', { class: 'zone-rooms', role: 'group', 'aria-label': tt('zones.rooms') },
      // One line of fixed tiles, scrolled sideways when they do not fit.
      h('div', { class: 'room-strip' },
        roomAddTile(z.zone, others),
        desired.map((c) => roomTile(c,
          // A room alone in its zone has nowhere to leave to.
          desired.length > 1 && h('button', { class: 'room-out', title: tt('zones.takeOutInto'), 'aria-label': tt('zones.takeOut', { name: roomName(c) }), onclick: () => moveRoom(c, null) }, icon('close'))))),
      // Who listens here (server phase 36), right of the rooms.
      'user' in z && h('div', { class: 'zone-status' }, listenerButton(z)))),
    // The queue: always shown, filling the zone's height, scrolled within, and
    // where library items are dropped.
    h('div', { class: 'queue', 'aria-busy': dropping.has(z.zone) ? 'true' : null },
      h('div', { class: 'queue-head' }, h('span', { class: 'queue-count' }, icon('queue'), tt('zones.tracks', { n: shownCount })),
        h('span', { class: 'player-transport queue-transport' }, transportControls(z)),
        h('span', { class: 'tools' },
        z.autoFill && autoFillButton(z),
        h('button', { class: 'icon-only', disabled: !shownCount, title: tt('zones.saveAsPlaylistTitle'), 'aria-label': tt('zones.saveAsPlaylist'), onclick: () => saveQueue(z.zone) }, icon('playlists')),
        transportButton('setup', tt('zones.zoneSetup'), () => openZoneSetup(z.zone)))),
      h('div', { class: 'table-wrap plain queue-list', ...queueDrop(z.zone) },
        q ? queueTable(z.zone, q) : h('p', { class: 'muted small' }, tt('zones.noQueue')),
        h('p', { class: 'muted small drop-hint' }, tt('zones.dragTracksAlbums'))),
      dropping.has(z.zone) && h('div', { class: 'queue-busy', role: 'status' },
        h('span', { class: 'spinner', 'aria-hidden': 'true' }), tt('zones.addingToQueue')),
      // Its setup, over the queue while open.
      S.setupZone === z.zone && zoneSetup(z)),
  ]);
  return card;
}

// Who listens here (server phase 36): a click lists the household and the
// users in a popover, and the one picked is set on each of the zone's rooms
// (PATCH rooms/{client} {user}); the zone's listener follows from them.
function listenerButton(z) {
  const label = z.user || tt('common.household');
  const names = listenerNames(z.user);
  const rooms = z.desired?.clients ?? z.actual?.clients ?? [];
  return h('span', { class: 'listener-pop' },
    h('button', {
      type: 'button', class: 'zone-listener', title: tt('zones.listenerWhosePlays', { name: label }),
      'aria-label': tt('zones.listenerChange', { name: label }), 'aria-haspopup': 'dialog',
      onclick: (e) => e.currentTarget.nextElementSibling.togglePopover(),
    }, label, icon('rooms')),
    h('div', {
      class: 'pick-popover', popover: 'auto', role: 'dialog', 'aria-label': tt('zones.listenerIn', { zone: z.name ?? z.zone }),
      ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); },
    }, ['', ...names].map((n) => h('button', {
      type: 'button', 'aria-pressed': n === (z.user ?? '') ? 'true' : 'false',
      onclick: (e) => {
        e.currentTarget.closest('[popover]').hidePopover();
        if (n !== (z.user ?? '')) act(() => Promise.all(rooms.map((c) => setRoomListener(c, n))), 'zones.listenerSet');
      },
    }, h('span', { class: 'room-name' }, n || tt('common.household'))))));
}

// AutoFill on or off, beside shuffle; its settings are in the zone's setup.
function autoFillButton(z) {
  const f = z.autoFill, on = f.enabled;
  // On but held back by the server: dimmed, with a pause mark and why. Repeat
  // all and shuffle are read off the queue, which follows a toggle at once;
  // the server's waiting comes only with the zone's AutoFill view.
  const waiting = z.queue?.repeat === 'all' ? 'repeatAll' : z.queue?.shuffle ? 'shuffle'
    : f.waiting === 'repeatAll' || f.waiting === 'shuffle' ? null : f.waiting;
  const paused = on && !f.filling && { repeatAll: 'zones.pausedRepeatAll', shuffle: 'zones.pausedShuffle', nothingAdded: 'zones.pausedNothingAdded' }[waiting];
  const title = tt(on ? 'zones.autofillOn' : 'zones.autofillOff') + (f.filling ? `, ${tt('zones.filling')}` : '') + (paused ? `, ${tt(paused)}` : '')
    + ` (${tt(on ? 'zones.clickOff' : 'zones.clickOn')})`;
  return h('button', {
    class: 'icon-only mode' + (on ? ' on' : '') + (f.filling ? ' busy' : '') + (paused ? ' paused' : ''), title, 'aria-label': 'AutoFill', 'aria-pressed': on ? 'true' : 'false',
    onclick: () => act(() => api('PATCH', `zones/${enc(z.zone)}`, { autoFill: { enabled: !on } }), on ? 'zones.autofillOff' : 'zones.autofillOn'),
  }, icon('sparkle'));
}

// A zone's setup: laid over its queue, the queue back when it closes -- its
// AutoFill (the listener is picked on the zone itself). Escape or ✕ closes it.
function openZoneSetup(zone) {
  S.setupZone = zone;
  renderZones();
  $('#zones .zone-setup')?.querySelector('select, input, button')?.focus();
}
function closeZoneSetup() {
  const card = $('#zones .zone-setup')?.closest('.zone');
  S.setupZone = null;
  // A render waits while an input has focus (render): Escape from one closes at once.
  if (card?.contains(document.activeElement)) document.activeElement.blur();
  renderZones();
  card?.querySelector(`button[aria-label="${tt('zones.zoneSetup')}"]`)?.focus();
}

// --- general settings --------------------------------------------------------------
// This page's own settings, the same in every browser: the server's GUI
// listener keeps them whole (GET/PUT /gui/settings), not the API. Missing ones
// take the defaults; a server without them leaves the General page disabled.
const GUI_DEFAULTS = { zoneBackground: 'media' }; // zoneBackground: 'room' | 'media'
let guiSettings = { ...GUI_DEFAULTS }, guiSettingsLoaded = false;
async function loadGuiSettings() {
  try {
    const r = await fetch('/gui/settings');
    if (r.ok) { guiSettings = { ...GUI_DEFAULTS, ...(await r.json()) }; guiSettingsLoaded = true; }
  } catch { /* the defaults stand */ }
  renderGeneral();
  renderZones();
}
function renderGeneral() {
  const select = $('#zoneBackground');
  select.value = guiSettings.zoneBackground;
  select.disabled = !guiSettingsLoaded;
}
$('#zoneBackground').addEventListener('change', (e) => act(async () => {
  const r = await fetch('/gui/settings', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...guiSettings, zoneBackground: e.target.value }),
  });
  const answer = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(answer.message ?? `/gui/settings: HTTP ${r.status}`);
  guiSettings = { ...GUI_DEFAULTS, ...answer };
  renderZones();
}, 'settings.saved').finally(renderGeneral));

// Behind a zone's header: its first room's picture, or the artwork of what
// plays (the General page's zoneBackground). The room's lie under the artwork,
// shown where there is none or it does not load.
function zoneBackdrop(z, room) {
  const art = guiSettings.zoneBackground === 'media' ? playing(z).item?.item.artworkId : null;
  const layers = [...(art ? [`/api/v1/artwork/${enc(art)}`] : []),
    ...(room ? [...(noRoomImage.has(room) ? [] : [roomImageUrl(room)]), roomArt(room)] : [])];
  return layers.length > 0 && h('div', {
    class: 'room-backdrop', 'aria-hidden': 'true', style: `--room-art: ${layers.map((u) => `url("${u}")`).join(', ')}`,
  });
}

function zoneSetup(z) {
  return h('section', { class: 'zone-setup', 'aria-label': tt('zones.zoneSetup'), onkeydown: (e) => { if (e.key === 'Escape') closeZoneSetup(); } },
    h('div', { class: 'pane-bar' },
      h('span', { class: 'setup-title' }, `${tt('zones.setup')} · `, z.name ?? z.zone),
      transportButton('close', tt('zones.closeSetup'), closeZoneSetup)),
    h('div', { class: 'setup-body' },
      z.autoFill && h('section', {},
        h('h3', {}, 'AutoFill'),
        h('p', { class: 'help' }, tt('zones.addsSongsWhen')),
        autoFillRow(z.zone, z.autoFill))));
}

// AutoFill (server phase 26): the zone's settings as one PATCH, how it stands,
// and the last fill as the server said it. `ai` is offered disabled, with the
// reason, while no AI provider is configured, switched on and the default.
function autoFillRow(zone, f) {
  const ai = aiUsable();
  const fill = S.autoFilled[zone];
  return h('form', { class: 'row autofill', autocomplete: 'off', onsubmit(e) {
    e.preventDefault();
    const F = e.target.elements;
    act(() => api('PATCH', `zones/${enc(zone)}`, { autoFill: {
      enabled: F.enabled.checked, threshold: Number(F.threshold.value), count: Number(F.count.value), source: F.source.value,
      request: F.request.value, fallback: F.fallback.checked, avoidRecent: Number(F.avoidRecent.value),
    } }), 'zones.autofillSaved').then(() => document.activeElement?.blur());
  } },
    h('label', {}, h('input', { type: 'checkbox', name: 'enabled', checked: f.enabled }), ' AutoFill'),
    h('label', { title: tt('zones.itemsLeftAfter') }, `${tt('zones.at')} ≤ `,
      h('input', { name: 'threshold', type: 'number', min: 0, max: 20, value: String(f.threshold), style: 'width:3.5em' })),
    h('label', {}, `${tt('zones.add')} `, h('input', { name: 'count', type: 'number', min: 1, max: 50, value: String(f.count), style: 'width:3.5em' })),
    h('select', { name: 'source', value: f.source },
      h('option', { value: 'ai', disabled: !ai.ok && f.source !== 'ai', title: ai.why }, ai.ok ? tt('zones.ai') : `${tt('zones.ai')} (${ai.why})`),
      h('option', { value: 'recommendations' }, tt('zones.recommendations'))),
    h('input', { name: 'request', value: f.request, size: 22, title: tt('zones.whatAiIs') }),
    h('label', { title: tt('zones.fillFromRecommendations') }, h('input', { type: 'checkbox', name: 'fallback', checked: f.fallback }), ` ${tt('zones.fallback')}`),
    h('label', { title: tt('zones.songsAmongHouses') }, `${tt('zones.avoidLast')} `,
      h('input', { name: 'avoidRecent', type: 'number', min: 0, max: 10000, value: String(f.avoidRecent), style: 'width:5em' })),
    h('button', {}, tt('zones.save')),
    f.filling && h('span', { class: 'pill warn' }, tt('zones.filling')),
    f.waiting === 'repeatAll' && h('span', { class: 'pill muted' }, tt('zones.autofillPausedRepeat')),
    f.waiting === 'shuffle' && h('span', { class: 'pill muted' }, tt('zones.autofillPausedShuffle')),
    f.waiting === 'nothingAdded' && h('span', { class: 'pill muted', title: tt('zones.nextFillWaits') }, tt('zones.waitingLastFill')),
    fill ? h('span', { class: 'pill ' + (fill.added ? 'good' : 'bad'), title: fill.at }, autoFillText(fill))
      : f.last && h('span', { class: 'pill muted', title: f.last.at }, tt('zones.last', { fill: autoFillText(f.last) })));
}

// "+5 from ai", "+0 from ai (timeout: …)", "+5 from recommendations (ai failed: …)".
const autoFillText = (r) => tt('zones.from', { n: r.added, source: tv('zones.source', r.source) }) + (r.reason ? ` (${r.reason})` : '');

function aiUsable() {
  if (!AI.settings) return { ok: true, why: '' }; // not read yet: the server says it on the first fill
  const p = AI.providers.find((x) => x.id === AI.settings.defaultProvider);
  if (!p) return { ok: false, why: tt('zones.noDefaultAi') };
  if (!p.enabled) return { ok: false, why: tt('zones.isSwitchedOff', { id: p.id }) };
  if (!p.configured) return { ok: false, why: tt('zones.isNotConfigured', { id: p.id }) };
  return { ok: true, why: '' };
}

// queue.autoFilled: flashed on the zone card, and the items it added marked
// in the queue until the queue's items change.
function onAutoFilled(data) {
  S.autoFilled[data.zone] = { ...data, at: time() };
  const q = S.zones.find((z) => z.zone === data.zone)?.queue;
  if (q && data.added) {
    const ids = q.items.slice(-data.added).map((i) => i.id);
    S.autoMarks[data.zone] = { ids: new Set(ids), key: q.items.map((i) => i.id).join(',') };
  }
  renderZones();
}

// When each queue state arrived: its positionMs was true then.
const received = new WeakMap();

// A seek sent and not answered yet, by zone: the player bar's line holds the
// position asked for until the answer's queue state shows the zone there.
const seeking = new Map();

async function seek(zone, item, positionMs) {
  seeking.set(zone, positionMs);
  renderZones();
  const answer = await act(() => api('POST', `zones/${enc(zone)}/seek`, { item, positionMs }));
  seeking.delete(zone);
  const z = S.zones.find((x) => x.zone === zone);
  if (answer?.items && z) { z.queue = answer; received.set(answer, performance.now()); }
  renderZones();
}

// Where the zone is now: its positionMs as answered, plus the time since it
// was answered while the queue plays.
function positionNow(q, durMs) {
  const ms = (q.positionMs ?? 0) + (q.transport === 'playing' ? performance.now() - received.get(q) : 0);
  return Math.min(Math.max(ms, 0), durMs);
}

// The player bar's line follows its zone.
setInterval(showPlayerProgress, 500);

function durationMs(ts) { // a .NET TimeSpan, "[d.]hh:mm:ss[.fffffff]"
  const m = /^(?:(\d+)\.)?(\d+):(\d+):(\d+(?:\.\d+)?)$/.exec(ts ?? '');
  return m ? ((Number(m[1] ?? 0) * 24 + Number(m[2])) * 3600 + Number(m[3]) * 60 + Number(m[4])) * 1000 : null;
}

function fmtMs(ms) {
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

// The label and its title are keys, translated here.
function kv(k, v, title) {
  return [h('span', { class: 'k', title: title && tt(title) }, tt(k)), h('span', {}, v), h('span', {})];
}

// The queue: a row's number (bars on the one playing, the grip to drag it
// by under the pointer), the item, its length, and its tools -- play, like,
// favourite and remove slide in from the right on hover.
// Items whose DELETE is under way: their rows are hidden at once, so a quick
// second click lands on the next row instead of removing the same item twice
// (the second answered 404). A refused delete shows its row again.
// Several go one DELETE after another (there is no batch call); a refused one
// stops there and shows the rows not yet removed again.
const removing = new Set();
function removeQueueItems(path, ids) {
  ids = ids.filter((id) => !removing.has(id));
  if (!ids.length) return;
  for (const id of ids) removing.add(id);
  clearSelection();
  renderZones();
  const left = new Set(ids);
  act(async () => {
    for (const id of ids) {
      await api('DELETE', `${path}/${id}`);
      left.delete(id);
    }
    return true;
  }, ids.length > 1 && tt('queue.itemsRemoved', { n: ids.length })).then((answer) => {
    if (answer !== undefined) return;
    for (const id of left) removing.delete(id);
    refreshSoon();
  });
}
// A selected row's trash removes the whole selection of that queue.
function queueSelection(zone, q, id) {
  return SEL.list === `queue:${zone}` && SEL.ids.has(String(id)) ? q.items.filter((i) => SEL.ids.has(String(i.id))).map((i) => i.id) : [id];
}

function queueTable(zone, q) {
  const path = `zones/${enc(zone)}/queue/items`;
  if (!q.items.some((i) => !removing.has(i.id))) return h('p', { class: 'muted small' }, tt('queue.queueEmpty'));
  const marks = S.autoMarks[zone]?.key === q.items.map((i) => i.id).join(',') ? S.autoMarks[zone].ids : null;
  return h('table', { class: 'small' },
    h('tr', {}, h('th', {}, '#'), h('th', {}, tt('common.track')), h('th', { class: 'queue-actions' }, tt('queue.duration'))),
    q.items.map((item, i) => h('tr', { 'data-index': String(i), class: [item.id === q.current && 'current', marks?.has(item.id) && 'autofilled'].filter(Boolean).join(' '),
      'data-list': `queue:${zone}`, 'data-id': String(item.id), 'data-selected': rowSelected(`queue:${zone}`, item.id),
      hidden: removing.has(item.id),
      title: marks?.has(item.id) ? tt('queue.addedByAutofill') : null },
      rowHandle(`queue:${zone}`, item.id === q.current ? icon('bars') : i + 1),
      h('td', {}, h('div', { class: 'with-art' },
        infoArt(item.item, artSlot(item.item.artworkId, media(item, 'station') ? 'radio' : 'library')),
        h('div', { class: 'grow' }, queueLines(item.item),
          media(item, 'group') && partsList(zone, q, item)),
        item.failure && failureBadge(item.failure))),
      // The length and the tools share one cell of a fixed width, so the
      // tools sliding in move only their own row's length.
      h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
        h('span', { class: 'num' }, fmtDur(item.item.duration)),
        h('span', { class: 'row-tools' },
          h('button', {
            class: 'row-play', title: tt('queue.playThisItemTitle'), 'aria-label': tt('queue.playThisItem'),
            'aria-busy': transporting.get(zone) === `item:${item.id}` ? 'true' : null, disabled: transporting.has(zone),
            onclick: () => transport(zone, `item:${item.id}`, 'play', { item: item.id }),
          }, transporting.get(zone) === `item:${item.id}` ? h('span', { class: 'spinner' }) : icon('play')),
          media(item, 'track') && likeButton('track', item.item),
          favoriteButton(media(item, 'group') ? `book|${item.item.source}` : likeKey(item.item.kind, item.item),
            () => (media(item, 'group') ? addBookFavorite(item.item.source) : addFavorite(media(item, 'track'), media(item, 'station'))),
            Boolean(media(item, 'group'))),
          h('button', { class: 'danger', title: tt('queue.removeFromQueueTitle'), 'aria-label': tt('queue.removeFromQueue'), onclick: () => removeQueueItems(path, queueSelection(zone, q, item.id)) }, icon('trash'))))))));
}

// A queue item as two lines: what it is, and who and where from.
function queueLines(x) {
  if (x.kind === 'track') return trackLines(x);
  const title = x.kind === 'group' ? x.title : x.name;
  const sub = x.kind === 'group'
    ? [x.artist, x.parts.length ? tt('common.parts', { n: x.parts.length }) : tt('queue.book'), x.type].filter(Boolean).join(' · ')
    : [x.genre, tt('queue.radio')].filter(Boolean).join(' · ');
  return h('div', { class: 'track-lines', title: `${title} [${x.providerId ?? x.source ?? ''}]` },
    h('div', { class: 'line' }, x.kind === 'group' ? '📖 ' : '', title),
    h('div', { class: 'muted line' }, sub));
}

// A book's parts, the one playing marked; ▶ seeks to a part's start on the
// whole book's timeline, which the server needs every earlier part's length for.
function partsList(zone, q, item) {
  const parts = item.item.parts;
  const playing = item.id === q.current ? q.part?.index : null;
  let start = 0;
  const rows = parts.map((p, i) => {
    const at = start;
    const len = durationMs(p.track.duration);
    start = start == null || len == null ? null : start + len;
    return h('tr', { class: i === playing ? 'current' : '' },
      h('td', { class: 'num muted' }, i + 1),
      h('td', {}, p.track.title, p.failure && h('div', { class: 'bad' }, p.failure)),
      h('td', { class: 'num' }, fmtDur(p.track.duration)),
      h('td', {}, h('button', {
        title: tt(at == null ? 'queue.earlierPartsLength' : 'queue.playFromThis'),
        disabled: at == null,
        onclick: () => act(() => api('POST', `zones/${enc(zone)}/seek`, { item: item.id, positionMs: at })),
      }, '▶')));
  });
  return h('details', { 'data-key': `parts:${zone}:${item.id}`, class: 'small' },
    h('summary', {}, playing != null ? tt('queue.chapterOf', { n: playing + 1, count: parts.length }) : tt('common.parts', { n: parts.length })),
    h('table', { class: 'small' }, rows));
}

// The queue as a stored playlist: a name and the folder it goes in, picked in
// a tree of the folders. A name in use in that folder is an error in the
// dialog, which then offers to replace that playlist. The folder last saved
// to is picked again (kept per viewer).
async function saveQueue(zone) {
  await loadPlaylists().catch(() => {});
  const folders = S.playlistFolders;
  const byId = (id) => folders.find((f) => f.id === id);
  let folder = Number(store.get('vx.saveFolder', '')) || null;
  if (folder != null && !byId(folder)) folder = null;
  const open = new Set();
  for (let at = byId(byId(folder)?.parent); at; at = byId(at.parent)) open.add(at.id);
  const folderName = () => byId(folder)?.name ?? tt('queue.theTopLevel');
  const taken = (name) => S.playlists.some((p) => (p.folder ?? null) === folder && p.name.trim().toLowerCase() === name.toLowerCase());

  const input = h('input', { id: 'saveName', autocomplete: 'off', spellcheck: 'false', maxlength: 200, placeholder: tt('queue.playlistName'), required: true, oninput: check });
  const tree = h('ul', { class: 'save-tree', role: 'tree', 'aria-label': tt('queue.folder') });
  const error = h('p', { class: 'save-error', role: 'alert', hidden: true });
  const ok = h('button', { type: 'submit', class: 'primary', value: 'save' }, icon('playlists'), tt('common.save'));
  const row = (id, name, depth, kids) => {
    const isOpen = open.has(id);
    return h('li', { role: 'treeitem', 'aria-selected': String(id === folder), ...(kids.length ? { 'aria-expanded': String(isOpen) } : {}) },
      h('div', { class: 'save-row' + (id === folder ? ' current' : ''), style: `padding-left:${depth * 16}px` },
        kids.length
          ? h('button', { type: 'button', class: 'save-fold', 'aria-label': tt(isOpen ? 'queue.close' : 'queue.open', { name }), onclick() { if (isOpen) open.delete(id); else open.add(id); draw(); } },
            h('span', { class: 'tree-chevron' }, icon('forward')))
          : h('span', { class: 'save-fold' }),
        h('button', { type: 'button', class: 'save-pick', title: name, onclick() { folder = id; draw(); check(); input.focus(); } },
          icon(id == null ? 'playlists' : 'folder'), h('span', {}, name))),
      kids.length > 0 && isOpen && h('ul', { role: 'group' }, kids));
  };
  const branch = (parent, depth) => folders.filter((f) => (f.parent ?? null) === parent)
    .sort((a, b) => a.name.localeCompare(b.name)).map((f) => row(f.id, f.name, depth, branch(f.id, depth + 1)));
  const draw = () => tree.replaceChildren(row(null, tt('queue.topLevel'), 0, []), ...branch(null, 1));
  let replace = false;
  function check() {
    const name = input.value.trim();
    replace = false;
    error.hidden = !name || !taken(name);
    ok.disabled = !name || !error.hidden;
    if (!error.hidden) error.replaceChildren(`${tt('queue.playlistNamedAlready', { name, folder: folderName() })} `,
      h('button', { type: 'button', class: 'link', onclick() { replace = true; dialog.querySelector('form').requestSubmit(); } }, tt('queue.replaceIt')));
  }
  const dialog = h('dialog', { class: 'confirm-dialog save-dialog', 'aria-label': tt('queue.saveAsPlaylist'), onclose() { this.remove(); } },
    h('form', { method: 'dialog', async onsubmit(e) {
      e.preventDefault();
      const name = input.value.trim();
      if (!name || (taken(name) && !replace)) return check();
      ok.disabled = true;
      try {
        const r = await api('POST', `zones/${enc(zone)}/queue/save`, { name, folder, overwrite: replace });
        store.set('vx.saveFolder', folder == null ? '' : String(folder));
        dialog.close();
        flash(tt(r.skipped ? 'queue.savedAsIn' : 'queue.savedAsInEntries',
          { name: r.playlist.name, folder: folderName(), n: r.playlist.count, skipped: r.skipped }));
        act(loadPlaylists);
      } catch (err) {
        // Taken meanwhile, or refused (an empty queue): said here, the dialog stays.
        if (err.data?.error === 'alreadyExists') { await loadPlaylists().catch(() => {}); check(); return; }
        error.hidden = false; error.replaceChildren(err.message); ok.disabled = false;
      }
    } },
      h('div', { class: 'confirm-head' }, h('span', { class: 'confirm-icon move-icon' }, icon('playlists')), h('h2', {}, tt('queue.saveAsPlaylist'))),
      h('label', { class: 'save-label', for: 'saveName' }, tt('queue.name')), input,
      h('span', { class: 'save-label' }, tt('queue.folder')), tree,
      error,
      h('div', { class: 'confirm-buttons' },
        h('button', { type: 'button', onclick: () => dialog.close() }, tt('common.cancel')), ok)));
  draw(); check();
  document.body.append(dialog);
  dialog.showModal();
  input.focus();
}

// One PUT does it: joining a zone takes the room out of its old one, which the
// server deletes when it is left empty. With no zone, the room leaves its own
// and the server gives it a zone of its own.
async function moveRoom(client, zone) {
  if (!client) return;
  const from = S.zones.find((z) => (z.desired?.clients ?? []).includes(client));
  const target = zone ?? from?.zone;
  if (!target || (zone == null && from.desired.clients.length < 2) || from?.zone === zone) return;
  const current = S.zones.find((z) => z.zone === target)?.desired?.clients ?? [];
  const clients = zone == null ? current.filter((c) => c !== client) : [...current, client];
  await act(() => api('PUT', `zones/${enc(target)}/clients`, { clients }),
    zone == null ? tt('queue.hasZoneOf', { name: client }) : `${client} → ${zoneName(zone)}`);
}


// --- clients (rooms) -----------------------------------------------------------
// One page: a row per client, what the core hears (clients/discovered) joined
// by client id with the rooms the server knows. The gear opens the room's
// settings page, #/client/<id>.

let discovered = null, discoveredError = ''; // GET clients/discovered, or why not
let roomOpen = null; // the client the settings page shows (#/client/<id>)

function renderRooms() {
  renderClients();
  renderZoneListeners(); // the rooms' listeners
  if (page !== 'client') return;
  const r = S.rooms.find((x) => x.client === roomOpen);
  render('rooms', $('#rooms'), () => [r ? roomSettings(r) : h('p', { class: 'muted' }, tt('clients.noRoomKnown', { name: roomOpen ?? '' }))]);
}

const openRoom = (client) => { location.hash = `#/client/${encodeURIComponent(client)}`; };

function renderClients() {
  const heard = discovered?.clients ?? [];
  const ids = [...new Set([...heard.map((c) => c.client), ...S.rooms.map((r) => r.client)])];
  const rows = ids.map((id) => ({ c: heard.find((c) => c.client === id), r: S.rooms.find((x) => x.client === id) }));
  const label = ({ c, r }) => r?.name ?? c?.name ?? (c ?? r).client;
  rows.sort((x, y) => label(x).localeCompare(label(y)));
  $('#clientsNote').textContent = discovered
    ? [tt('clients.heard', { n: heard.length }), discovered.forgotten && tt('clients.forgotten', { n: discovered.forgotten }), tt('clients.rooms', { n: S.rooms.length })].filter(Boolean).join(' · ')
    : discoveredError && tt('clients.discoveryUnavailable', { error: discoveredError });
  // Through render, as the zones' sliders: a card's volume being dragged is
  // not redrawn under the pointer by the next discovery tick.
  render('clients', $('#clients'), () => [rows.length
    ? h('table', { class: 'client-table' },
      h('thead', {}, h('tr', {}, h('th', {}), h('th', {}, tt('clients.client')), h('th', { class: 'icon-col' }, tt('clients.state')), h('th', { class: 'icon-col' }, tt('clients.power')),
        h('th', { class: 'icon-col' }, tt('clients.bitperfect')), h('th', {}, tt('clients.eq')), h('th', { class: 'icon-col' }, tt('clients.network')), h('th', {}, tt('clients.ip')),
        h('th', {}, tt('clients.version')), h('th', {}, tt('clients.volume')), h('th', { class: 'client-spare' }), h('th', {}))),
      h('tbody', {}, rows.map(clientRow)))
    : h('p', { class: 'muted' }, tt('clients.noClientHeard'))]);
}

// Whether the room plays the source's own samples: the core sends them
// unchanged, and nothing on the client changes them after it -- software
// volume below full or muted, or a shared output the system mixer may change.
// Value is null without a plan; reason says why it is not.
function bitPerfectOf(a) {
  const plan = a?.plan, settings = a?.settings;
  const scaling = a?.volumeControl?.effective === 'software' && ((settings?.volume ?? 1) < 1 || settings?.muted);
  const shared = a?.capabilities?.accessMode === 'shared';
  return {
    value: plan ? plan.bitPerfect && !scaling && !shared : null,
    reason: [
      plan?.bitPerfect === false && (plan.reason || tt('clients.serverIsResampling')),
      scaling && tt('clients.softwareVolumeOr'),
      shared && tt('clients.sharedAudioOutput'),
    ].filter(Boolean).join('; '),
  };
}

// What a client's row shows, worked out from its beacon and its room.
function clientFacts({ c, r }) {
  const a = r?.actual, id = (c ?? r).client;
  const [state, cls, text] = c ? attachment(c) : a?.connected ? ['here', 'good', tt('clients.attachedHere')] : ['unknown', 'muted', tt('clients.notHeard')];
  const attached = state === 'here' || state === 'pending';
  // The link the client reaches the server by, as it reports it (MATP section 55).
  const m = S.system?.rooms.find((x) => x.client === id)?.metrics;
  let link = m?.networkLink === 'wifi' ? [`wifi${m.wifiRssiDbm == null ? 3 : wifiBars(m.wifiRssiDbm)}`, `WLAN${m.wifiRssiDbm != null ? ` · ${wifiSignal(m.wifiRssiDbm)}` : ''}`]
    : m?.networkLink === 'ethernet' ? ['ethernet', 'ETH'] : null;
  // Kept per viewer: a client not heard shows the link it last had, greyed.
  if (link) store.set(`vx.link:${id}`, m.networkLink);
  const stale = !link && store.get(`vx.link:${id}`, null);
  if (stale) link = stale === 'wifi' ? ['wifi3', 'WLAN'] : ['ethernet', 'ETH'];
  link ??= ['linkUnknown', tt('clients.networkLinkUnknown')];
  const eq = r && (r.desired?.equalizer ?? a?.settings?.equalizer);
  const { value: bitPerfect, reason: bitPerfectReason } = bitPerfectOf(a);
  // What the equalizer does, as eqBadge says it on the room's page.
  const core = a?.plan?.equalizer, clipped = core?.clippedSamples > 0 ? `, ${tt('clients.samplesClipped', { n: core.clippedSamples })}` : '';
  const eqInfo = !r ? ['—'] : !eq?.enabled ? [tt('common.off'), 'muted']
    : core?.active ? [tt('clients.active') + clipped, clipped ? 'bad' : 'good']
      : a?.plan?.bitPerfect ? [tt('clients.onNotApplied'), 'warn']
        : [tt('clients.onNotActive') + clipped, a?.plan ? 'warn' : 'muted', a?.plan ? '' : tt('clients.roomIsNot')];
  // A room that announced deep sleep keeps saying so while it is away.
  const power = [a?.powerState ? powerLabel(a.powerState) : '—', a?.powerState ? (a.powerState === 'on' ? 'good' : 'warn') : 'muted',
    a?.connected || a?.powerState ? '' : tt('clients.reportedWhileConnected')];
  // Its health as the zone cards name it: the core's unstable (rejoins), the
  // server's degraded (underruns).
  const hl = a?.health ?? S.zones.find((z) => z.actual?.members?.includes(id))?.actual?.health?.[id];
  return {
    c, r, a, id, state, cls, text, attached, m, link, stale, eqInfo, power, bitPerfect, bitPerfectReason,
    // The device: its own name from its beacon, else what its machine says it is
    // (GET system) or its platform.
    device: c?.host ?? m?.machineModel ?? c?.platform,
    unwell: hl && hl.state !== 'ok' ? hl : null,
    ip: c?.address ?? a?.address ?? '—',
    version: c?.clientVersion ?? '—',
  };
}

const clientPicture = (f) => h('span', { class: 'cover-art' }, f.r ? roomImage(f.id) : icon('device'));

const clientHealth = (f) => f.unwell && h('span', { class: 'client-health ' + (f.unwell.state === 'unstable' ? 'bad' : 'warn'), title: healthText(f.unwell),
  'aria-label': `${tv('state', f.unwell.state)}: ${healthText(f.unwell)}`, role: 'img' }, icon('alert'));

const clientLink = (f) => h('span', { class: 'client-link' + (f.stale ? ' stale' : '') + (f.link[0] === 'linkUnknown' ? ' unknown' : ''),
  title: f.stale ? tt('clients.lastLinkNot', { link: f.link[1] }) : f.link[1],
  'aria-label': f.stale ? tt('clients.lastLinkNotHeard', { link: f.link[1] }) : f.link[1], role: 'img' }, icon(f.link[0]));

// The room's own volume and mute, as on its page; none without a room.
const clientVolume = (f) => f.r && h('div', { class: 'client-volume' }, roomVolumeRow(f.r));

// Every client has every button, in the same place; one that does not apply is disabled.
// `iconOnly` shows attach/detach as a link icon, its word as the title.
function clientButtons({ c, r, a, id, state, attached }, iconOnly = false) {
  const word = tt(attached ? 'clients.detach' : 'clients.attach');
  return [
    // A spinner while the call and the re-read that follows it run.
    h('button', {
      class: 'attach' + (iconOnly ? ' icon-only' : ''), disabled: pointing.has(id) || !(c || attached) || Boolean(c && !c.present),
      'aria-busy': pointing.has(id) ? 'true' : null, title: iconOnly ? word : null, 'aria-label': iconOnly ? word : null,
      onclick: () => (attached ? detachClient(id) : attachClient(id, state)),
    }, pointing.has(id) ? h('span', { class: 'spinner', 'aria-hidden': 'true' }) : iconOnly ? icon(attached ? 'unlink' : 'link') : word),
    h('button', {
      class: 'icon-only danger', disabled: !r || Boolean(a?.connected),
      title: tt(!r ? 'clients.noRoomTo' : a?.connected ? 'clients.connectedRoomIs' : 'clients.forgetRoom'),
      'aria-label': tt('clients.forgetName', { name: r?.name ?? id }), onclick: () => forgetRoom(r),
    }, icon('trash')),
    h('button', {
      class: 'icon-only', disabled: !r, title: tt(r ? 'clients.roomSettings' : 'clients.noRoomYet'),
      'aria-label': tt('clients.settingsOf', { name: r?.name ?? id }), onclick: () => openRoom(id),
    }, icon('setup')),
  ];
}

// A client as a table row: its facts in columns, state, power and bit-perfect
// as coloured icons that say themselves on hover.
function clientRow(row) {
  const f = clientFacts(row);
  const { c, r, m } = f;
  const mark = (name, cls, label) => h('span', { class: 'client-mark ' + cls, title: label, 'aria-label': label, role: 'img' }, icon(name));
  const bp = f.bitPerfect == null ? tt('clients.unknown') : f.bitPerfect ? tt('clients.yes') : `${tt('clients.no')}: ${f.bitPerfectReason}`;
  return h('tr', { class: f.attached ? 'attached' : '' },
    // The picture opens the room's settings, as the gear does.
    h('td', { class: 'client-pic' }, f.r ? h('button', { class: 'client-pic-open', title: tt('clients.roomSettings'),
      'aria-label': tt('clients.settingsOf', { name: r.name ?? f.id }), onclick: () => openRoom(f.id) }, clientPicture(f)) : clientPicture(f)),
    h('td', { class: 'client-who' },
      h('div', { class: 'client-title' }, h('span', { class: 'client-name', title: r?.name ?? c?.name }, r?.name ?? c?.name ?? '—'), clientHealth(f)),
      h('div', { class: 'muted small', title: [f.device, f.id, c?.platform, m?.machineModel].filter(Boolean).join(' · ') }, f.device ?? '—', ' · ', h('span', { class: 'mono' }, f.id))),
    h('td', { class: 'icon-col' }, mark(f.state === 'unknown' ? 'linkUnknown' : 'link', f.cls, `${tt('clients.state')}: ${f.text}`)),
    h('td', { class: 'icon-col' }, mark('power', f.power[1], `${tt('clients.power')}: ${f.power[0]}${f.power[2] ? ` (${f.power[2]})` : ''}`)),
    h('td', { class: 'icon-col' }, mark('wave', f.bitPerfect == null ? 'muted' : f.bitPerfect ? 'good' : 'warn', `${tt('clients.bitperfect')}: ${bp}`)),
    h('td', { class: f.eqInfo[1] ?? '', title: f.eqInfo[2] ?? '' }, f.eqInfo[0]),
    h('td', { class: 'icon-col' }, clientLink(f)),
    h('td', { class: 'client-ip' }, f.ip),
    h('td', {}, f.version),
    h('td', { class: 'client-vol' }, clientVolume(f)),
    h('td', { class: 'client-spare' }),
    h('td', { class: 'client-tools' }, clientButtons(f, true)));
}

// A room's own volume and mute (PATCH rooms/{client}), on its page and its card.
function roomVolumeRow(r) {
  const a = r.actual, d = r.desired, s = a?.settings;
  const patch = (body) => act(() => api('PATCH', `rooms/${enc(r.client)}`, body));
  const noVolume = a?.volumeControl?.effective === 'disabled';
  return volumeRow(d.volume ?? s?.volume ?? 1, Boolean(d.muted ?? s?.muted), (volume) => patch({ volume }), (muted) => patch({ muted }),
    { disabled: noVolume, title: noVolume ? tt('clients.volumeControlIs') + (a.volumeControl.reason ? `: ${a.volumeControl.reason}` : '') : '' });
}

// DELETE rooms/{client} (server phase 39), for a room that is not connected.
async function forgetRoom(r) {
  // By its id too: two rooms can carry one name (a client that got a new id).
  if (await confirmDelete({ title: tt('clients.forgetRoom'), what: r.name && r.name !== r.client ? `${r.name} (${r.client})` : r.client, action: tt('clients.forget'),
    text: tt('clients.itsZoneVolume') })) {
    act(() => api('DELETE', `rooms/${enc(r.client)}`), 'clients.roomForgotten');
  }
}

// A room corrected through its own clock (MATP section 61): sync.appliedPpm is
// the trim its client reports having programmed, sync.hardwareTargetPpm the
// core's command. Positive slows the clock, so the sample clock it gives is
// the stream's nominal rate times (1 - ppm / 1e6) -- what was programmed,
// not a measured frequency.
function hardwareClockOf(a) {
  if (a?.plan?.driftActuator !== 'hardwareClock' || a.sync?.appliedPpm == null) return null;
  const rate = a.plan.outputFormat?.sampleRate;
  return {
    ppm: a.sync.appliedPpm,
    targetPpm: a.sync.hardwareTargetPpm,
    hz: rate ? rate * (1 - a.sync.appliedPpm / 1e6) : null,
  };
}

function recordHistory(rooms) {
  const t = Date.now();
  for (const r of rooms) {
    const a = r.actual;
    if (!a) continue;
    const list = (S.history[r.client] ??= []);
    const clock = hardwareClockOf(a);
    list.push({ t, driftPpm: a.sync?.driftPpm, offsetUs: a.sync?.offsetUs, appliedPpm: a.sync?.appliedPpm, bufferedMs: a.playback?.bufferedMs ?? a.bufferedMs,
      clockPpm: clock?.ppm, clockTargetPpm: clock?.targetPpm, clockHz: clock?.hz });
    if (list.length > 300) list.shift();
  }
}

// rooms.sync, every second: each connected room's sync and buffer, named as in
// its actual half, merged in and charted. A figure left out is unmeasured.
function onRoomsSync(data) {
  const changed = [];
  let healthMoved = false; // a zone card names its unhealthy rooms
  for (const s of data.rooms) {
    const r = S.rooms.find((x) => x.client === s.client);
    if (!r?.actual) continue;
    const was = r.actual.health?.state;
    Object.assign(r.actual, { sync: s.sync, bufferedMs: s.bufferedMs, playback: s.playback, rejoins: s.rejoins });
    if ('health' in s) r.actual.health = s.health;
    if (r.actual.health?.state !== was) healthMoved = true;
    changed.push(r);
  }
  recordHistory(changed);
  renderRooms();
  if (healthMoved) renderZones();
}

// A room's power state by its key (power.standbyManual), else as the server names it.
const powerLabel = (state) => tv('power', state);

// A room's health as the server answers it (server phase 34), verbatim.
function healthText(hl) {
  return [tv('state', hl.state), hl.state === 'unstable' && tt('clients.rejoinsThisEpisode', { n: hl.rejoins }),
    hl.recentUnderruns > 0 && tt('clients.underrunsInLast', { n: hl.recentUnderruns }),
    hl.since && tt('clients.since', { time: new Date(hl.since).toLocaleTimeString(undefined, { hour12: false }) }),
    hl.backoffMs != null && tt('clients.backOffMs', { n: hl.backoffMs }), hl.trigger && tt('clients.trigger', { name: hl.trigger }), hl.detail,
    hl.wifiRssiDbm != null && `Wi-Fi ${wifiSignal(hl.wifiRssiDbm)}`].filter(Boolean).join(' · ');
}

// A room without an image of its own shows one of these, always the same for
// the same client (picked from its id); a fixed set, for testing.
const ROOM_ART = ['living', 'office', 'kitchen', 'bedroom', 'bathroom', 'terrace'];
function roomArt(client) {
  let hash = 0;
  for (const ch of client) hash = (hash * 31 + ch.charCodeAt(0)) >>> 0;
  return `img/rooms/${ROOM_ART[hash % ROOM_ART.length]}.svg`;
}
const roomImageUrl = (client) => `/api/v1/rooms/${enc(client)}/image?v=${roomImageVersion.get(client) ?? 0}`;

// A setting in a row of the client's settings page: its label (translated
// here), its controls.
const roomField = (label, ...control) => h('div', { class: 'room-field' }, h('span', { class: 'room-field-label' }, tt(label)), h('div', { class: 'room-field-control' }, ...control));

// One client's settings page (#/client/<id>): a header with the room's
// picture, state, name, zone and volume and its actions; under it one card,
// a row per part -- the audio device, the equalizer, sync and buffer -- and
// everything else folded into Advanced.
function roomSettings(r) {
  const a = r.actual, d = r.desired, s = a?.settings, sync = a?.sync, plan = a?.plan;
  const bitPerfect = bitPerfectOf(a);
  const path = `rooms/${enc(r.client)}`;
  const patch = (body) => act(() => api('PATCH', path, body));
  const name = r.name ?? r.client;
  const zone = S.zones.find((z) => z.zone === d.zone);
  const alone = !d.zone || (zone?.desired?.clients?.length ?? 0) < 2;
  const own = noRoomImage.has(r.client) ? null : roomImageUrl(r.client);
  const art = roomArt(r.client);
  const pill = (cls, text, title) => h('span', { class: `pill ${cls}`, title }, text);
  // A control the device answers is shown while it is offline too, disabled,
  // with why; one it has not reported yet stands as an empty select.
  const offline = !a?.connected;
  const why = tt(a ? 'room.isNotConnected' : 'room.hasNeverReported', { name });
  const live = (...controls) => (offline ? h('fieldset', { class: 'room-offline', disabled: true, title: why }, ...controls) : controls);
  const unknown = () => h('select', { disabled: true, title: why, 'aria-label': why }, h('option', {}, '—'));

  // The picture: the room's own, the fixed one where it has none (or it fails).
  const picture = h('img', {
    class: 'room-hero-image', alt: '', src: own ?? art,
    onerror() { noRoomImage.add(r.client); this.onerror = null; this.src = art; },
  });

  // The name, renamed in place: a connected room only (server phase 39).
  const title = h('h1', { class: 'room-hero-name' }, h('span', {}, name),
    h('button', {
      type: 'button', class: 'icon-only', disabled: offline, title: offline ? `${tt('common.rename')}: ${why}` : tt('common.rename'), 'aria-label': tt('room.rename', { name }),
      onclick(e) {
        const head = e.currentTarget.closest('h1');
        head.replaceChildren(h('form', {
          class: 'room-rename', autocomplete: 'off',
          onsubmit(ev) {
            ev.preventDefault();
            const value = ev.target.elements.name.value.trim();
            if (value && value !== name) act(() => api('PATCH', path, { name: value }), 'room.renamed');
            document.activeElement?.blur();
            renderRooms();
          },
        }, h('input', { name: 'name', value: name, maxlength: 64, 'aria-label': tt('room.roomName'), onkeydown(ev) { if (ev.key === 'Escape') { ev.target.blur(); renderRooms(); } } }),
        h('button', { class: 'primary' }, tt('common.save'))));
        head.querySelector('input').select();
      },
    }, icon('edit')));

  // The zone it is in, picked here too: another zone, or one of its own.
  const zoneSelect = h('select', {
    class: 'pill-select', value: d.zone ?? '', 'aria-label': tt('room.zoneOf', { name }), title: tt('room.moveToAnother'),
    onchange: (e) => (e.target.value === '+own' ? moveRoom(r.client, null) : moveRoom(r.client, e.target.value)),
  },
  d.zone ? null : h('option', { value: '' }, tt('room.zone', { name: '—' })),
  S.zones.map((z) => h('option', { value: z.zone }, tt('room.zone', { name: z.name ?? z.zone }))),
  !alone && h('option', { value: '+own' }, tt('room.zoneOfIts')));

  const health = a?.health && a.health.state !== 'ok';
  const backdrop = h('div', { class: 'room-backdrop', style: `--room-art: url("${own ?? art}"), url("${art}")`, 'aria-hidden': 'true' });
  const hero = h('section', { class: 'room-hero' },
    picture,
    h('div', { class: 'room-hero-main' },
      h('div', { class: 'room-pills' },
        pill(a?.connected ? 'good' : 'bad', tt(a ? (a.connected ? 'room.connected' : 'room.disconnected') : 'room.neverReported')),
        zone?.queue?.transport === 'playing' && pill('good', tt('room.playing')),
        health && pill(a.health.state === 'unstable' ? 'bad' : 'warn', `⚠ ${tv('state', a.health.state)}`, healthText(a.health)),
        a?.powerState && a.powerState !== 'on' && pill('warn', tt('room.powerState', { state: powerLabel(a.powerState) }))),
      title,
      zone && h('div', { class: 'room-hero-zone muted' }, zone.name ?? zone.zone),
      h('div', { class: 'room-pills' },
        zoneSelect,
        a?.address && pill('', `IP ${a.address}`),
        h('span', { class: 'pill mono', title: tt('room.clientId') }, r.client)),
      roomVolumeRow(r)),
    h('div', { class: 'room-hero-actions' },
      h('button', { type: 'button', disabled: offline, title: offline ? why : '', onclick: () => act(() => api('POST', `${path}/rejoin`), 'room.rejoinSent') }, icon('refresh'), tt('room.rejoin')),
      // Asleep on Ethernet: a Wake-on-LAN magic packet ends it; playing its zone does too.
      a?.powerState === 'sleep' && h('button', { type: 'button', title: tt('room.wakeOnLan', { address: a.wakeOnLan ?? tt('room.itsWiredAddress') }), onclick: () => act(() => api('POST', `${path}/wake`), 'room.wakeSent') }, icon('power'), tt('room.wake')),
      h('button', { type: 'button', onclick: (e) => e.currentTarget.nextElementSibling.click() }, icon('image'), tt('room.setImage')),
      h('input', {
        type: 'file', hidden: true, accept: 'image/jpeg,image/png,image/gif,image/webp', 'aria-label': tt('room.imageOf', { name }),
        onchange(e) {
          const file = e.target.files[0];
          e.target.value = '';
          if (file) act(() => api('PUT', `${path}/image`, file), 'room.imageSet');
        },
      }),
      h('span', { class: 'room-more' },
        h('button', { type: 'button', class: 'icon-only', title: tt('common.more'), 'aria-label': tt('common.more'), 'aria-haspopup': 'menu', onclick: (e) => e.currentTarget.nextElementSibling.togglePopover() }, icon('more')),
        h('div', { class: 'pick-popover', popover: 'auto', role: 'menu', ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); } },
          h('button', { type: 'button', disabled: !own, onclick: () => act(() => api('DELETE', `${path}/image`), 'room.imageRemoved') }, icon('trash'), tt('room.removeImage')),
          d.zone && !alone && h('button', { type: 'button', onclick: () => moveRoom(r.client, null) }, icon('zones'), tt('room.takeOutOf')),
          h('button', { type: 'button', class: 'danger', disabled: Boolean(a?.connected), title: a?.connected ? tt('room.connectedRoomIs') : '', onclick: () => forgetRoom(r) }, icon('trash'), tt('room.forgetRoom'))))));

  // A row: what it is on the left, its controls on the right.
  const row = (iconName, heading, about, ...content) => h('section', { class: 'room-row' },
    h('div', { class: 'room-row-intro' }, h('span', { class: 'card-icon' }, icon(iconName)),
      h('div', {}, h('h2', {}, tt(heading)), h('p', {}, tt(about)))),
    h('div', { class: 'room-row-content' }, ...content));
  const field = roomField;

  // Who uses the room: its listener (server phase 36), so far.
  const listening = row('people', 'room.listening', 'room.listeningAbout',
    field('room.listener', h('select', {
      value: r.user ?? '', 'aria-label': tt('room.listenerOf', { name }), title: tt('room.listenerOfRoom'),
      onchange: (e) => act(() => setRoomListener(r.client, e.target.value), 'listeners.listenerSet'),
    }, h('option', { value: '' }, tt('room.household')), listenerNames(r.user).map((n) => h('option', { value: n }, n)))));

  const audio = row('volume', 'room.audioDevice', 'room.outputDeviceIts',
    (a?.audioDevices || offline) && field('room.output', live(a?.audioDevices ? audioDevicePicker(a.audioDevices, patch, path) : unknown())),
    (a?.bufferMs != null || offline) && field('room.bufferSizeMs', live(a?.bufferMs != null ? bufferEditor(a.bufferMs, path) : unknown())),
    (a?.volumeControl || offline) && field('room.volumeControl', live(a?.volumeControl
      ? volumeControlEditor(a.volumeControl, d.audioControl ?? s?.audioControl, a.capabilities?.hardwareVolume, path) : unknown())),
    (a?.driftCorrection?.mode || offline) && field('room.driftCorrection', live(a?.driftCorrection?.mode ? driftCorrectionEditor(a.driftCorrection, path) : unknown())),
    field('room.audioControl',
      h('select', { value: d.audioControl ?? s?.audioControl ?? 'auto', 'aria-label': tt('room.audioControl'), onchange: (e) => patch({ audioControl: e.target.value }) },
        AUDIO_CONTROLS.map(([m, label, about]) => h('option', { value: m, title: tt(about) }, tt(label)))),
      plan && h('span', { class: 'pill badge ' + (bitPerfect.value ? 'good' : s?.audioControl === 'bitPerfect' ? 'bad' : plan.bitPerfect ? 'warn' : 'muted'),
        title: bitPerfect.value ? plan.reason : bitPerfect.reason },
        tt(bitPerfect.value ? (plan.driftActuator === 'hardwareClock' ? 'room.bitperfectHardwareClock' : 'room.bitperfect')
          : s?.audioControl === 'bitPerfect' ? 'room.forceBitPerfect'
            : plan.bitPerfect ? 'room.notBitperfectAt' : 'room.resampled')),
      plan?.replanPending && h('span', { class: 'pill badge muted', title: tt('room.leftAloneIn') },
        tt('room.aloneFromNext'))),
    offline && h('p', { class: 'muted small' }, `${why}: ${tt('room.itsOutputBuffer')}`));

  const eq = row('wave', 'room.equalizer', 'room.adjustSoundFor', eqEditor(r.client, d.equalizer ?? s?.equalizer, plan, path));

  // Sync and buffer: the measures as tiles, each with its graph, then the room's own timing.
  const tile = (label, value, extra, attrs = {}) => h('div', { class: 'room-stat', ...attrs }, h('span', { class: 'muted small' }, tt(label)), h('strong', {}, value), extra);
  const graph = (key, digits, off) => spark('', r.client, key, digits, off);
  const bufferPct = a?.bufferMs && a?.bufferedMs != null ? Math.min(100, (a.bufferedMs / a.bufferMs) * 100) : null;
  const clock = hardwareClockOf(a);
  const clockError = a?.driftCorrection?.hardwareClock?.error;
  const syncRow = row('clock', 'room.syncBuffer', 'room.synchronisationAndBuffering',
    a?.connected ? h('div', { class: 'room-stats' },
      tile(plan && !plan.driftCorrected ? 'room.driftNotCorrected' : 'room.drift', `${fmtNum(sync?.driftPpm, 1)} ppm`, graph('driftPpm', 3)),
      tile('room.offsetTarget', `${fmtNum(sync?.offsetUs, 0)} µs`, graph('offsetUs', 1)),
      // Through the hardware clock the applied correction is what the clock is set to.
      clock ? tile('room.hardwareClock', `${clock.ppm > 0 ? '+' : ''}${clock.ppm} ppm`, graph('clockPpm', 0), {
        title: tt('room.positiveSlowsClock') + (a.driftCorrection?.hardwareClock?.saturated ? `; ${tt('room.atDevicesLimit')}` : '') + (clockError ? `; ${tt('room.error', { error: clockError })}` : ''),
      }) : tile(plan && !plan.driftCorrected ? 'room.appliedNotCorrected' : 'room.appliedCorrection', `${fmtNum(sync?.appliedPpm, 1)} ppm`,
        graph('appliedPpm', 3, plan && !plan.driftCorrected)),
      clock && tile('room.clockTargetCore', `${fmtNum(clock.targetPpm, 1)} ppm`, graph('clockTargetPpm', 1)),
      clock?.hz != null && tile('room.sampleClock', `${fmtNum(clock.hz, 3)} Hz`, graph('clockHz', 3), { title: tt('room.nominalHz', { rate: a.plan.outputFormat.sampleRate }) }),
      tile('room.buffer', a.bufferedMs != null ? `${fmtNum(a.bufferedMs / 1000, 1)} s` : '—', graph('bufferedMs', 0),
        { title: bufferPct != null ? tt('room.ofSBuffer', { pct: Math.round(bufferPct), size: fmtNum(a.bufferMs / 1000, 1) }) : '' }),
      tile('room.underrunsRejoins', `${a.playback?.underruns ?? '—'} / ${a.rejoins ?? '—'}`)) : h('p', { class: 'muted small' }, tt('room.measuredWhileRoom')),
    h('form', {
      class: 'room-inline-form',
      onsubmit(e) {
        e.preventDefault();
        const F = e.target.elements, body = {};
        if (F.db.value !== '') body.maxVolumeDb = Number(F.db.value);
        if (F.ms.value !== '') body.outputLatencyMs = Number(F.ms.value);
        if (Object.keys(body).length) patch(body);
      },
    },
    h('label', {}, tt('room.maxVolumeDb'), h('input', { name: 'db', type: 'number', min: -60, max: 0, step: 1, class: 'w-num', value: String(d.maxVolumeDb ?? 0) })),
    h('label', {}, tt('room.outputLatencyMs'), h('input', { name: 'ms', type: 'number', min: 0, class: 'w-num', value: String(d.outputLatencyMs ?? s?.outputLatencyMs ?? '') })),
    h('button', { class: 'primary' }, tt('room.apply'))));

  // The device's own settings: its network link, LEDs and power.
  const device = row('device', 'room.device', 'room.networkLinkLeds',
    offline ? h('p', { class: 'muted small' }, `${why}: ${tt('room.itsEthernetSpeed')}`)
      : deviceSettingsEditor(r.client, path));

  const buttons = a?.inputs?.length > 0 && row('grip', 'room.buttonsEncoders', 'room.whatButtonsAnd',
    S.lastInput[r.client] && lastInputLine(S.lastInput[r.client]),
    inputsEditor(r.client, a.inputs, path, a.deepSleep));

  const caps = a?.capabilities;
  const capabilities = caps && row('chip', 'room.capabilities', 'room.formatsAndControls',
    caps.accessMode && field('room.accessMode', caps.accessMode),
    caps.hardwareVolume && field('room.hardwareVolume', fmtHardwareVolume(caps.hardwareVolume)),
    caps.hardwareClock && field('room.hardwareClock', caps.hardwareClock.available
      ? `${caps.hardwareClock.control ?? tt('room.yes')} (${caps.hardwareClock.minPpm} … ${caps.hardwareClock.maxPpm} ppm)` : tt('common.none')),
    h('details', { class: 'sub', 'data-key': `caps:${r.client}` },
      h('summary', {}, tt('room.formats', { n: caps.formats.length })),
      h('div', { class: 'small mono', style: 'white-space: pre-wrap' }, caps.formats.map(fmtFmt).join('\n'))));

  // The rest: the desired/effective/core table, the plan and the raw record.
  const text = (v) => (v == null ? '—' : String(v));
  const cell = (label, want, eff, have) => h('tr', {}, h('td', {}, tt(label)), h('td', {}, text(want)),
    h('td', { class: eff != null && want != null && String(eff) !== String(want) ? 'warn' : '' }, text(eff)),
    h('td', { class: eff != null && have != null && String(eff) !== String(have) ? 'diff' : '' }, text(have)));
  const diagnostics = row('pulse', 'room.diagnostics', 'room.playbackPlanSettings',
    a?.notReadyReason && h('p', { class: 'warn small' }, tt('room.notReady', { reason: a.notReadyReason })),
    health && h('p', { class: (a.health.state === 'unstable' ? 'bad' : 'warn') + ' small' }, healthText(a.health)),
    h('div', { class: 'room-pills' }, a?.volumeControl && volumeBadge(a.volumeControl, s),
      plan && h('span', { class: 'pill badge ' + (plan.driftCorrected ? 'good' : 'muted'), title: a?.driftCorrection?.disabledBy ?? plan.description ?? plan.reason },
        tt(plan.driftCorrected ? (plan.driftActuator === 'hardwareClock' ? 'room.driftCorrectedHardware' : 'room.driftCorrected') : 'room.noDriftCorrection'))),
    plan && h('div', { class: 'small' }, h('span', { class: 'muted' }, `${tt('room.plan')}: `), plan.reason,
      h('div', {}, `out ${fmtFmt(plan.outputFormat)} · src ${fmtFmt(plan.sourceFormat)}`),
      plan.description && h('div', { class: 'muted' }, plan.description)),
    a?.startDetail && h('div', { class: 'small muted' }, `${tt('room.start')}: ${a.startDetail}`),
    d.grouped && h('div', { class: 'small warn' }, driftNotCorrected(a?.driftCorrection)
      ? tt('room.inZoneWith', { reason: driftNotCorrected(a.driftCorrection) })
      : tt('room.inZoneWithOther')),
    h('div', { class: 'table-wrap plain' }, h('table', { class: 'small' },
      h('tr', {}, h('th', {}, tt('room.setting')), h('th', {}, tt('room.desired')), h('th', {}, tt('room.effective')), h('th', {}, tt('room.core'))),
      cell('room.setting.audioControl', audioControlLabel(d.audioControl), audioControlLabel(d.audioControl), s && audioControlLabel(s.audioControl)),
      cell('room.volume', d.volume, d.effectiveVolume, s?.volume),
      cell('room.muted', d.muted, d.effectiveMuted, s?.muted),
      cell('room.setting.maxVolumeDb', d.maxVolumeDb, d.maxVolumeDb, undefined),
      cell('room.setting.outputLatencyMs', d.outputLatencyMs, d.outputLatencyMs, s?.outputLatencyMs))));

  return h('div', { class: 'room-settings' }, backdrop, hero,
    h('div', { class: 'room-rows' }, audio, listening, eq, syncRow, device, buttons, capabilities, diagnostics));
}

// The size of the room's own buffer (MATP section 58), as its client reported
// it. The client keeps it in its own config and refuses -- never clamps -- a
// value it cannot use (out of 1000..11000 ms, or too large for its memory), so the
// value shown is only the new one once its BUFFER_CONFIG has come back, which
// the room.bufferConfig event announces. A refusal leaves the old value and an
// ERROR in the core's log.
const deviceSettings = new Map();
function deviceSettingsEditor(client, path) {
  const state = deviceSettings.get(client) ?? {};
  const reload = async () => {
    state.loading = true; deviceSettings.set(client, state); renderRooms();
    const results = await Promise.allSettled([api('GET', `${path}/network`), api('GET', `${path}/leds`), api('GET', `${path}/power`)]);
    for (const [i, key] of ['network', 'leds', 'power'].entries()) {
      state[key] = results[i].status === 'fulfilled' ? results[i].value : null;
      state[key + 'Error'] = results[i].status === 'rejected' ? results[i].reason.message : '';
    }
    state.loading = false; state.loaded = true; renderRooms();
  };
  const change = (key, body, message) => act(async () => {
    state.loading = true; renderRooms();
    try { state[key] = await api('PATCH', `${path}/${key}`, body); }
    finally { state.loading = false; renderRooms(); }
  }, message);
  // standbyOn, standbyOff, toggleStandby or deepSleep, sent through the core.
  const power = (action, message) => act(async () => {
    state.loading = true; renderRooms();
    try { state.power = { ...state.power, ...await api('POST', `${path}/power`, { action }) }; }
    finally { state.loading = false; renderRooms(); }
  }, message);
  const minutes = (seconds) => String(Math.round((seconds ?? 0) / 60));
  // Read once each time the settings page opens (showPage forgets the last read).
  if (!state.loaded && !state.loading) setTimeout(reload, 0);
  const field = roomField;
  return [
    !state.loaded && h('p', { class: 'muted small' }, tt('room.readingDeviceSettings')),
    state.networkError && h('p', { class: 'bad small' }, state.networkError),
    state.ledsError && h('p', { class: 'bad small' }, state.ledsError),
    state.powerError && h('p', { class: 'bad small' }, state.powerError),
    state.network?.supported && field('room.ethernetSpeed', h('select', {
      value: state.network.network.ethernet.speedMbps, disabled: state.loading, 'aria-label': tt('room.ethernetLinkSpeed', { name: client }),
      onchange: e => change('network', { speedMbps: e.target.value }, 'room.ethernetSpeedSaved'),
    }, [['auto', 'room.speedAuto'], ['10', 'room.speed10'], ['100', 'room.speed100']].map(([value, label]) => h('option', { value }, tt(label))))),
    state.leds?.supported && field('room.leds', h('select', {
      value: String(state.leds.off), disabled: state.loading, 'aria-label': tt('room.ledModeOf', { name: client }),
      onchange: e => change('leds', { off: e.target.value === 'true' }, 'room.ledModeSaved'),
    }, h('option', { value: 'false' }, tt('room.on')), h('option', { value: 'true' }, tt('room.off'))),
    h('span', { class: 'small muted' }, tt('room.bootAndFirmware'))),
    state.power?.supported && field('room.power',
      h('span', { class: 'pill' }, powerLabel(state.power.state ?? 'unknown')),
      h('button', { type: 'button', disabled: state.loading, onclick: () => power('standbyOn', 'room.standbySent') }, tt('room.standby')),
      h('button', { type: 'button', disabled: state.loading, onclick: () => power('standbyOff', 'room.wakeFromStandby') }, tt('room.wake')),
      state.power.deepSleep && h('button', { type: 'button', disabled: state.loading,
        onclick: () => confirm(tt('room.putIntoDeep', { name: client })) && power('deepSleep', 'room.deepSleepSent') }, tt('room.deepSleep'))),
    // Idle delays in minutes here, seconds on the client; 0 switches one off.
    state.power?.supported && field('room.idleTimers', h('form', {
      class: 'room-inline-form',
      onsubmit(e) {
        e.preventDefault();
        const body = { standbyAfterSeconds: Number(e.target.standby.value) * 60 };
        if (state.power.deepSleep) body.deepSleepAfterSeconds = Number(e.target.deepSleep.value) * 60;
        change('power', body, 'room.powerSettingsSaved');
      },
    },
    h('label', {}, tt('room.standbyAfterMin'), h('input', { name: 'standby', type: 'number', min: 0, max: 1440, step: 1, class: 'w-num',
      value: minutes(state.power.standbyAfterSeconds), disabled: state.loading, 'aria-label': tt('room.automaticStandbyOf', { name: client }) })),
    state.power.deepSleep && h('label', {}, tt('room.deepSleepAfter'), h('input', { name: 'deepSleep', type: 'number', min: 0, max: 1440, step: 1, class: 'w-num',
      value: minutes(state.power.deepSleepAfterSeconds), disabled: state.loading, 'aria-label': tt('room.automaticDeepSleep', { name: client }) })),
    h('button', { class: 'primary', disabled: state.loading }, tt('room.apply')))),
    state.power?.supported && h('p', { class: 'small muted' }, tt('room.0SwitchesTimer')),
    (state.network?.supported === false && state.leds?.supported === false) && h('p', { class: 'small muted' }, tt('room.thisFirmwareHas')),
  ];
}

function bufferEditor(bufferMs, path) {
  return h('form', {
    class: 'room-controls',
    onsubmit(e) {
      e.preventDefault();
      const ms = Number(e.target.ms.value);
      act(() => api('PATCH', path, { bufferMs: ms }), tt('room.bufferMsSent', { n: ms }));
    },
  },
  h('input', {
    name: 'ms', type: 'number', min: 1000, max: 11000, step: 1, class: 'w-num', value: String(bufferMs), 'aria-label': tt('room.bufferInMilliseconds'),
    title: tt('room.sizeOfRooms'),
  }),
  h('button', {}, tt('room.set')));
}

// A room's Audio Control (MATP section 59): what the core may do to its audio,
// and so what its client may do to the samples for volume. There is no drift
// setting: the core corrects every resampled room and every room sharing its
// zone, unless it is Bit-Perfect (_docs/GroupedDriftCorrection.md).
const AUDIO_CONTROLS = [
  ['bitPerfect', 'room.audioMode.bitPerfect', 'room.audioMode.bitPerfectHint'],
  ['auto', 'room.audioMode.auto', 'room.audioMode.autoHint'],
];
const audioControlLabel = (m) => (m == null ? null : tt(AUDIO_CONTROLS.find(([v]) => v === m)?.[1] ?? m));

// How the room's client plays its volume (MATP section 59). The client keeps
// it and executes it; auto resolves from the Audio Control and the device.
const VOLUME_CONTROLS = [
  ['auto', 'room.volumeMode.auto', 'room.volumeMode.autoHint'],
  ['hardware', 'room.volumeMode.hardware', 'room.volumeMode.hardwareHint'],
  ['software', 'room.volumeMode.software', 'room.volumeMode.softwareHint'],
  ['disabled', 'room.volumeMode.disabled', 'room.volumeMode.disabledHint'],
];
const volumeControlLabel = (m) => tt(VOLUME_CONTROLS.find(([v]) => v === m)?.[1] ?? m);

function fmtHardwareVolume(hv) {
  if (!hv.available) return tt('common.none');
  const range = hv.minDb != null ? `${fmtNum(hv.minDb, 1)} … ${fmtNum(hv.maxDb, 1)} dB, ` : '';
  return `${hv.control ?? tt('room.yes')} (${range}${tt('room.steps', { n: hv.steps })}${hv.hasMute ? `, ${tt('room.muteControl')}` : ''})`;
}

// What the room's volume actually does: bad where the client plays none,
// warn where it scales the samples (the room is not bitperfect while it does).
function volumeBadge(vc, s) {
  const scaling = vc.effective === 'software' && ((s?.volume ?? 1) < 1 || s?.muted);
  return h('span', {
    class: 'pill badge ' + (vc.effective === 'hardware' ? 'good' : vc.effective === 'software' ? 'warn' : 'muted'),
    title: vc.reason ?? tt(VOLUME_CONTROLS.find(([v]) => v === vc.effective)?.[2] ?? ''),
  }, `${tt('room.volume')}: ${volumeControlLabel(vc.effective)}${scaling ? ` ${tt('room.scalingNotBitperfect')}` : ''}`);
}

// --- a room's equalizer (../_docs/EqualizerRoadmap.md, step 3) ---------------
// The server keeps it whole (server phase 41) and the core runs it on a
// resampled stream only: on a bitperfect one it is on and does nothing.

// Basic mode: ten peaks an octave apart, the layout of the built-in profiles.
const EQ_BASIC_HZ = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
const EQ_BASIC_Q = 1.41;
const eqFlatBands = () => EQ_BASIC_HZ.map((frequencyHz) => ({ type: 'peak', frequencyHz, q: EQ_BASIC_Q, gainDb: 0 }));
const eqIsBasic = (bands) => bands.length === EQ_BASIC_HZ.length && bands.every((b, i) =>
  b.type === 'peak' && b.frequencyHz === EQ_BASIC_HZ[i] && Math.abs(b.q - EQ_BASIC_Q) < 1e-9);
const eqHz = (f) => (f >= 1000 ? `${f / 1000}k` : String(f));
const eqDb = (v) => `${v > 0 ? '+' : ''}${fmtNum(v, 1)}`;

// RBJ-cookbook coefficients, normalised, exactly as the core designs them
// (ServerCore processing/Equalizer.cpp), at `rate`. Null is no filter: a
// peak or shelf of 0 dB.
function eqCoefficients(band, rate) {
  const gain = band.gainDb ?? 0;
  if (band.type !== 'highPass' && band.type !== 'lowPass' && gain === 0) return null;
  const w0 = 2 * Math.PI * Math.min(band.frequencyHz, 0.49 * rate) / rate;
  const cos = Math.cos(w0), alpha = Math.sin(w0) / (2 * band.q), A = 10 ** (gain / 40);
  const shelf = 2 * Math.sqrt(A) * alpha;
  const [b0, b1, b2, a0, a1, a2] = {
    peak: [1 + alpha * A, -2 * cos, 1 - alpha * A, 1 + alpha / A, -2 * cos, 1 - alpha / A],
    lowShelf: [A * ((A + 1) - (A - 1) * cos + shelf), 2 * A * ((A - 1) - (A + 1) * cos), A * ((A + 1) - (A - 1) * cos - shelf),
      (A + 1) + (A - 1) * cos + shelf, -2 * ((A - 1) + (A + 1) * cos), (A + 1) + (A - 1) * cos - shelf],
    highShelf: [A * ((A + 1) + (A - 1) * cos + shelf), -2 * A * ((A - 1) + (A + 1) * cos), A * ((A + 1) + (A - 1) * cos - shelf),
      (A + 1) - (A - 1) * cos + shelf, 2 * ((A - 1) - (A + 1) * cos), (A + 1) - (A - 1) * cos - shelf],
    highPass: [(1 + cos) / 2, -(1 + cos), (1 + cos) / 2, 1 + alpha, -2 * cos, 1 - alpha],
    lowPass: [(1 - cos) / 2, 1 - cos, (1 - cos) / 2, 1 + alpha, -2 * cos, 1 - alpha],
  }[band.type];
  return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

// The bands' combined gain in dB at `hz`, preamp left out.
function eqBandsDb(bands, hz, rate = 48000) {
  const w = 2 * Math.PI * hz / rate;
  const c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  let db = 0;
  for (const band of bands) {
    const k = eqCoefficients(band, rate);
    if (!k) continue;
    const [b0, b1, b2, a1, a2] = k;
    const nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    db += 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  return db;
}

// The highest point of the bands' response, 20 Hz to 20 kHz on a log scale.
function eqPeakDb(bands) {
  let peak = -Infinity;
  for (let i = 0; i <= 240; i++) peak = Math.max(peak, eqBandsDb(bands, 20 * 1000 ** (i / 240)));
  return peak;
}

// Automatic headroom: a preamp that takes the highest boost back to 0 dB,
// within the core's -30 dB floor.
const eqHeadroom = (bands) => Math.max(-30, -Math.ceil(Math.max(0, eqPeakDb(bands)) * 10) / 10) || 0; // never -0

// What the equalizer does to this room, as the core says. `inSummary` keeps
// the summary short and shows "off" there too.
function eqBadge(eq, plan, inSummary = false) {
  if (!eq?.enabled) return inSummary ? h('span', { class: 'pill badge muted' }, tt('common.off')) : null;
  const core = plan?.equalizer;
  const clipping = core?.clippedSamples > 0 ? ` · ${tt('eq.samplesClipped', { n: core.clippedSamples })}` : '';
  if (core?.active) return h('span', { class: 'pill badge ' + (clipping ? 'bad' : 'good') }, tt('eq.eqActive') + clipping);
  if (plan?.bitPerfect) {
    return h('span', { class: 'pill badge warn', title: tt('eq.bitperfectStreamIs') },
      tt('eq.notAppliedStream'));
  }
  return h('span', { class: 'pill badge ' + (plan ? 'warn' : 'muted'), title: plan ? '' : tt('eq.roomIsNot') },
    plan ? tt('eq.eqOnNot') + clipping : tt('eq.eqOn'));
}

// A room's equalizer, in two modes (a per-viewer choice per room). Basic:
// ten sliders at fixed frequencies. Advanced: up to 20 parametric bands, each
// its type, frequency, gain and Q. Both share on/off, a profile copied in,
// the preamp, the response curve, saving as a profile and REW text. Every
// change sends the whole equalizer. While "auto preamp" is ticked (a
// per-viewer choice) a band change recomputes the preamp. Basic shows an
// equalizer of another layout without editing it, so a slider cannot throw
// away bands it does not show.
const EQ_TYPES = [
  ['peak', 'eq.type.peak'], ['lowShelf', 'eq.type.lowShelf'], ['highShelf', 'eq.type.highShelf'], ['highPass', 'eq.type.highPass'], ['lowPass', 'eq.type.lowPass'],
]; // labels translated where shown
const EQ_MAX_BANDS = 20;
const eqUsesGain = (type) => type !== 'highPass' && type !== 'lowPass';

function eqEditor(client, eq, plan, path) {
  const current = eq ?? { enabled: false, preampDb: 0, bands: eqFlatBands() };
  const bands = current.bands.length === 0 ? eqFlatBands() : current.bands;
  const basicLayout = eqIsBasic(bands);
  const advanced = store.get(`vx.eqMode:${client}`, basicLayout ? 'basic' : 'advanced') === 'advanced';
  const auto = store.get('vx.eqAutoPreamp', '1') === '1';
  // Shown at once: the page re-renders as soon as a slider or a handle lets
  // go, and drawing the room as last read would put it back where it was
  // until the server's answer is in. Re-read after a refusal too, which puts
  // right what was refused; with no core connected the server keeps the
  // equalizer all the same and answers notConnected.
  const send = (next, done) => {
    eqShowNow(client, next);
    return act(() => api('PATCH', path, { equalizer: next }), done).then(refreshSoon);
  };
  const withBands = (next) => ({ ...current, bands: next, preampDb: auto ? eqHeadroom(next) : current.preampDb });
  const setBand = (i, change) => send(withBands(bands.map((x, j) => (j === i ? { ...x, ...change } : x))));
  const peak = eqPeakDb(bands);
  const loudest = peak + current.preampDb;
  const mode = (value) => () => { store.set(`vx.eqMode:${client}`, value); renderRooms(); };
  const own = (S.eqProfiles ?? []).filter((p) => !p.builtIn);
  const rewOpen = Boolean(S.eqRewOpen?.[client]);
  const closeMenu = (e) => e.currentTarget.closest('[popover]')?.hidePopover();

  const head = h('div', { class: 'eq-head' },
    h('label', { class: 'switch' },
      h('input', {
        type: 'checkbox', role: 'switch', checked: Boolean(current.enabled), 'aria-label': tt('eq.enableEqualizer'),
        onchange: (e) => send({ ...current, bands, enabled: e.target.checked }, e.target.checked ? 'eq.equalizerOn' : 'eq.equalizerOff'),
      }), h('span', { class: 'switch-track', 'aria-hidden': 'true' }), tt('eq.enableEqualizer')),
    eqBadge(current, plan),
    h('select', {
      value: '', 'aria-label': tt('eq.applyEqualizerProfile'),
      title: tt('eq.copiesProfileInto'),
      onchange(e) {
        const profile = (S.eqProfiles ?? []).find((p) => p.name === e.target.value);
        if (!profile) return;
        eqShowNow(client, { enabled: true, preampDb: profile.preampDb, bands: profile.bands });
        act(() => api('POST', `${path}/equalizer/profile`, { profile: profile.name }), tt('eq.profileApplied', { name: profile.name })).then(refreshSoon);
      },
    }, h('option', { value: '' }, tt('eq.applyProfile')),
    (S.eqProfiles ?? []).map((p) => h('option', { value: p.name, title: tt('eq.bandsPreampDb', { n: p.bands.length, db: eqDb(p.preampDb) }) },
      p.builtIn ? p.name : tt('eq.own', { name: p.name })))),
    // The rest: automatic preamp, REW text, deleting an own profile.
    h('span', { class: 'eq-more' },
      h('button', { type: 'button', class: 'icon-only', title: tt('common.more'), 'aria-label': tt('eq.moreEqualizerOptions'), 'aria-haspopup': 'menu', onclick: (e) => e.currentTarget.nextElementSibling.togglePopover() }, icon('more')),
      h('div', { class: 'pick-popover eq-menu', popover: 'auto', role: 'menu', ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); } },
        h('label', { class: 'eq-menu-item', title: tt('eq.setPreampSo') },
          h('input', {
            type: 'checkbox', checked: auto,
            onchange(e) {
              store.set('vx.eqAutoPreamp', e.target.checked ? '1' : '0');
              if (e.target.checked) send(withBands(bands), 'eq.preampSetFor');
              else renderRooms();
            },
          }), tt('eq.automaticPreamp')),
        h('button', { type: 'button', onclick(e) { (S.eqRewOpen ??= {})[client] = !rewOpen; closeMenu(e); renderRooms(); } },
          icon('code'), tt(rewOpen ? 'eq.hideRewImport' : 'eq.rewImportExport')),
        own.length > 0 && h('select', {
          value: '', 'aria-label': tt('eq.deleteProfile'),
          onchange(e) {
            const gone = e.target.value;
            e.target.value = '';
            if (gone && confirm(tt('eq.deleteProfileRooms', { name: gone }))) {
              act(() => api('DELETE', `equalizer/profiles/${enc(gone)}`), tt('eq.profileDeleted', { name: gone })).then(loadEqProfiles);
            }
          },
        }, h('option', { value: '' }, tt('eq.deleteProfileMenu')), own.map((p) => h('option', { value: p.name }, p.name))))),
    h('span', { class: 'seg eq-mode' },
      h('button', { type: 'button', class: advanced ? '' : 'on', 'aria-pressed': String(!advanced), onclick: mode('basic') }, tt('eq.basic')),
      h('button', { type: 'button', class: advanced ? 'on' : '', 'aria-pressed': String(advanced), onclick: mode('advanced') }, tt('eq.advanced'))));

  // Basic: ten slim sliders over the response, the dB scale at the left and
  // the frequencies under them. Advanced: the draggable curve and the bands.
  let board;
  if (advanced) {
    board = h('div', { class: 'eq-board eq-board-advanced' },
      eqCurve(bands, current.preampDb, false, (next) => send(withBands(next))),
      eqBandTable(bands, setBand, (next) => send(withBands(next))));
  } else if (basicLayout) {
    board = h('div', { class: 'eq-board' },
      h('div', { class: 'eq-scale', 'aria-hidden': 'true' }, h('span', {}, '+12 dB'), h('span', {}, '0 dB'), h('span', {}, '−12 dB')),
      h('div', { class: 'eq-sliders' },
        eqFill(bands),
        bands.map((b, i) => h('label', { class: 'eq-slider', title: `${eqHz(b.frequencyHz)} Hz: ${eqDb(b.gainDb)} dB` },
          h('input', {
            type: 'range', min: -12, max: 12, step: 0.5, value: String(b.gainDb), 'aria-label': tt('eq.hzGain', { hz: eqHz(b.frequencyHz) }),
            style: `--p:${((b.gainDb + 12) / 24) * 100}%`,
            oninput: (e) => { e.target.style.setProperty('--p', `${((Number(e.target.value) + 12) / 24) * 100}%`); },
            onchange: (e) => setBand(i, { gainDb: Number(e.target.value) }),
          }),
          h('span', {}, eqHz(b.frequencyHz))))));
  } else {
    board = h('div', { class: 'eq-board eq-board-other small' },
      h('p', { class: 'muted' }, tt('eq.bandsInAnother', { n: bands.length })),
      h('button', {
        type: 'button',
        onclick: () => confirm(tt('eq.replaceTheseBands')) && send({ ...current, bands: eqFlatBands(), preampDb: 0 }, 'eq.equalizerResetTo'),
      }, tt('eq.replaceWithBasic')));
  }

  const profileName = S.eqProfileName?.[client] ?? '';
  const side = h('div', { class: 'eq-side' },
    h('span', { class: 'eq-side-label' }, tt('eq.preamp')),
    h('div', { class: 'eq-preamp', title: auto ? tt('eq.setAutomaticallyFor') : '' },
      h('input', {
        type: 'range', min: -30, max: 12, step: 0.1, value: String(current.preampDb), disabled: auto, 'aria-label': tt('eq.preampDb'),
        style: `--p:${((current.preampDb + 30) / 42) * 100}%`,
        onchange: (e) => send({ ...current, bands, preampDb: Number(e.target.value) }),
      }),
      h('span', { class: 'eq-value' }, `${fmtNum(current.preampDb, 1)} dB`)),
    h('span', { class: 'small ' + (loudest > 0.05 ? 'warn' : 'muted'), title: tt('eq.highestPointOf') },
      tt('eq.peakDb', { db: eqDb(loudest) }) + (loudest > 0.05 ? ` ${tt('eq.mayClip')}` : '')),
    h('button', { type: 'button', class: 'eq-action', onclick: () => send({ ...current, bands: eqFlatBands(), preampDb: 0 }, 'eq.equalizerReset') }, icon('refresh'), tt('eq.reset')),
    h('span', { class: 'eq-save' },
      h('button', { type: 'button', class: 'eq-action', 'aria-haspopup': 'dialog', onclick: (e) => e.currentTarget.nextElementSibling.togglePopover() }, icon('bookmark'), tt('eq.saveAsProfile')),
      h('form', {
        class: 'pick-popover eq-save-form', popover: 'auto', autocomplete: 'off',
        ontoggle(e) { if (e.newState === 'open') { placeUnderButton(this); this.elements.profile.focus(); } },
        onsubmit(e) {
          e.preventDefault();
          const wanted = e.target.elements.profile.value.trim();
          if (!wanted) return;
          e.target.hidePopover();
          act(() => api('PUT', `equalizer/profiles/${enc(wanted)}`, { preampDb: current.preampDb, bands }), tt('eq.savedAs', { name: wanted })).then(loadEqProfiles);
        },
      },
      h('input', {
        name: 'profile', value: profileName, maxlength: 64, placeholder: tt('eq.profileName'), 'aria-label': tt('eq.profileName'),
        oninput: (e) => { (S.eqProfileName ??= {})[client] = e.target.value; },
      }),
      h('button', { class: 'primary', title: tt('eq.keepTheseBands') }, tt('common.save')))));

  return h('div', { class: 'eq' }, head, h('div', { class: 'eq-body' }, board, side),
    rewOpen && eqRewTools(client, current, bands, send));
}

// The Basic sliders' response as a soft filled curve behind them: the same
// log scale, a band in the middle of its slider, ±12 dB over the track.
function eqFill(bands) {
  const at = (t) => 31.25 * 2 ** (t * 10 - 0.5);
  const y = (db) => 50 - (Math.max(-12, Math.min(12, db)) / 12) * 50;
  let d = '';
  for (let i = 0; i <= 100; i++) d += `${i ? 'L' : 'M'}${i} ${y(eqBandsDb(bands, at(i / 100))).toFixed(2)}`;
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 100 100');
  svg.setAttribute('preserveAspectRatio', 'none');
  svg.setAttribute('class', 'eq-fill');
  svg.setAttribute('aria-hidden', 'true');
  const ns = 'http://www.w3.org/2000/svg';
  const gradient = document.createElementNS(ns, 'linearGradient');
  gradient.setAttribute('id', 'eq-fill-fade');
  gradient.setAttribute('x1', '0'); gradient.setAttribute('y1', '0'); gradient.setAttribute('x2', '0'); gradient.setAttribute('y2', '1');
  for (const [offset, opacity] of [['0', '.28'], ['1', '0']]) {
    const stop = document.createElementNS(ns, 'stop');
    stop.setAttribute('offset', offset);
    stop.setAttribute('stop-color', 'currentColor');
    stop.setAttribute('stop-opacity', opacity);
    gradient.append(stop);
  }
  const defs = document.createElementNS(ns, 'defs');
  defs.append(gradient);
  svg.append(defs);
  const area = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  area.setAttribute('d', `${d}L100 100L0 100Z`);
  area.setAttribute('class', 'eq-fill-area');
  const line = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  line.setAttribute('d', d);
  line.setAttribute('class', 'eq-fill-line');
  svg.append(area, line);
  return svg;
}

// The room's equalizer as just sent, in the page's copy of the room, drawn
// before the server answers; the next read replaces it with the server's.
function eqShowNow(client, eq) {
  const room = S.rooms.find((r) => r.client === client);
  if (!room) return;
  room.desired = { ...room.desired, equalizer: eq };
  renderRooms();
}

// Advanced: one row per band. A row's fields are sent when they change; the
// server checks the ranges and says which one is out.
function eqBandTable(bands, setBand, setBands) {
  const num = (label, value, change, props = {}) => h('input', {
    type: 'number', step: 'any', class: 'w-num', value: String(value), 'aria-label': label,
    onchange: (e) => e.target.value !== '' && change(Number(e.target.value)), ...props,
  });
  return h('div', { class: 'table-wrap plain' }, h('table', { class: 'small eq-table' },
    h('tr', {}, h('th', {}, '#'), h('th', {}, tt('eq.type')), h('th', {}, 'Hz'), h('th', {}, tt('eq.gainDb')), h('th', {}, 'Q'), h('th', {}, '')),
    bands.map((b, i) => h('tr', {},
      h('td', { class: 'muted' }, String(i + 1)),
      h('td', {}, h('select', { value: b.type, 'aria-label': tt('eq.bandType', { n: i + 1 }), onchange: (e) => setBand(i, { type: e.target.value }) },
        EQ_TYPES.map(([v, label]) => h('option', { value: v }, tt(label))))),
      h('td', {}, num(tt('eq.bandFrequencyHz', { n: i + 1 }), b.frequencyHz, (v) => setBand(i, { frequencyHz: v }), { min: 10, max: 24000 })),
      h('td', {}, eqUsesGain(b.type)
        ? num(tt('eq.bandGainDb', { n: i + 1 }), b.gainDb ?? 0, (v) => setBand(i, { gainDb: v }), { min: -24, max: 24 })
        : h('span', { class: 'muted' }, '—')),
      h('td', {}, num(tt('eq.bandQ', { n: i + 1 }), b.q, (v) => setBand(i, { q: v }), { min: 0.1, max: 10 })),
      h('td', {}, h('button', { title: tt('eq.removeThisBand'), 'aria-label': tt('eq.removeBand', { n: i + 1 }), onclick: () => setBands(bands.filter((_, j) => j !== i)) }, '✕')))),
    h('tr', {}, h('td', { colspan: 6 }, h('button', {
      disabled: bands.length >= EQ_MAX_BANDS,
      title: bands.length >= EQ_MAX_BANDS ? tt('eq.atMostBands', { n: EQ_MAX_BANDS }) : tt('eq.1KhzPeak'),
      onclick: () => setBands([...bands, { type: 'peak', frequencyHz: 1000, q: 1, gainDb: 0 }]),
    }, tt('eq.band'))))));
}

// The combined response, 20 Hz to 20 kHz, the preamp included, with a handle
// per band. A handle is dragged for frequency and gain (gain only in Basic,
// frequency only for a pass filter); the wheel over one changes its Q
// (Advanced). The new bands are sent when the drag ends, or a moment after
// the wheel stops.
const EQ_W = 600, EQ_H = 200, EQ_DB = 18;
const eqX = (hz) => (Math.log10(hz / 20) / 3) * EQ_W;
const eqHzAt = (x) => 20 * 1000 ** (x / EQ_W);
const eqY = (db) => EQ_H / 2 - (db / EQ_DB) * (EQ_H / 2);
const eqDbAt = (y) => (EQ_H / 2 - y) / (EQ_H / 2) * EQ_DB;
const eqHandleDb = (b, bands) => (eqUsesGain(b.type) ? b.gainDb ?? 0 : eqBandsDb([b], b.frequencyHz));

function eqCurve(bands, preampDb, basic, setBands) {
  const ratio = window.devicePixelRatio || 1;
  const canvas = h('canvas', {
    width: EQ_W * ratio, height: EQ_H * ratio, class: 'eq-curve', tabindex: 0,
    'aria-label': tt('eq.equalizerResponseDrag'),
  });
  canvas._bands = bands;
  eqDraw(canvas, bands, preampDb);

  // Canvas coordinates of a pointer event, whatever size the canvas is shown at.
  const at = (e) => {
    const box = e.currentTarget.getBoundingClientRect();
    return [(e.clientX - box.left) / box.width * EQ_W, (e.clientY - box.top) / box.height * EQ_H];
  };
  const nearest = (c, [x, y]) => {
    let best = -1, distance = 14;
    c._bands.forEach((b, i) => {
      const d = Math.hypot(eqX(b.frequencyHz) - x, eqY(eqHandleDb(b, c._bands)) - y);
      if (d < distance) { best = i; distance = d; }
    });
    return best;
  };
  canvas.onpointerdown = (e) => {
    const c = e.currentTarget, i = nearest(c, at(e));
    if (i < 0) return;
    c.focus(); // the page does not re-render under a focused control
    c.setPointerCapture(e.pointerId);
    c._drag = i;
  };
  canvas.onpointermove = (e) => {
    const c = e.currentTarget;
    const [x, y] = at(e);
    if (c._drag == null) { c.style.cursor = nearest(c, [x, y]) >= 0 ? 'grab' : ''; return; }
    const b = c._bands[c._drag];
    const moved = { ...b };
    if (!basic) moved.frequencyHz = Math.round(Math.min(20000, Math.max(20, eqHzAt(x))));
    if (eqUsesGain(b.type)) moved.gainDb = Math.round(Math.min(basic ? 12 : 24, Math.max(basic ? -12 : -24, eqDbAt(y))) * 10) / 10;
    c._bands = c._bands.map((x2, j) => (j === c._drag ? moved : x2));
    c._moved = true;
    eqDraw(c, c._bands, preampDb);
  };
  canvas.onpointerup = (e) => {
    const c = e.currentTarget;
    if (c._drag != null && c._moved) setBands(c._bands);
    c._drag = null;
    c._moved = false;
  };
  canvas.onwheel = (e) => {
    const c = e.currentTarget;
    if (basic) return;
    const i = nearest(c, at(e));
    if (i < 0) return;
    e.preventDefault();
    const b = c._bands[i];
    const q = Math.round(Math.min(10, Math.max(0.1, b.q * (e.deltaY < 0 ? 1.1 : 1 / 1.1))) * 100) / 100;
    c._bands = c._bands.map((x2, j) => (j === i ? { ...b, q } : x2));
    c.title = `Q ${q}`;
    eqDraw(c, c._bands, preampDb);
    clearTimeout(c._wheel);
    c._wheel = setTimeout(() => setBands(c._bands), 500);
  };
  return canvas;
}

function eqDraw(canvas, bands, preampDb) {
  const ratio = canvas.width / EQ_W;
  const ctx = canvas.getContext('2d');
  const style = getComputedStyle(document.documentElement);
  const color = (name) => style.getPropertyValue(name).trim();
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, EQ_W, EQ_H);
  ctx.font = '11px system-ui, sans-serif';
  ctx.lineWidth = 1;

  // Grid: decades and octaves' worth of frequencies, every 6 dB.
  ctx.strokeStyle = color('--line');
  ctx.fillStyle = color('--muted');
  for (const hz of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
    ctx.beginPath(); ctx.moveTo(eqX(hz), 0); ctx.lineTo(eqX(hz), EQ_H); ctx.stroke();
    ctx.fillText(eqHz(hz), eqX(hz) + 3, EQ_H - 4);
  }
  for (let db = -EQ_DB + 6; db < EQ_DB; db += 6) {
    ctx.setLineDash(db === 0 ? [] : [3, 4]);
    ctx.beginPath(); ctx.moveTo(0, eqY(db)); ctx.lineTo(EQ_W, eqY(db)); ctx.stroke();
    if (db !== 0) ctx.fillText(eqDb(db), 3, eqY(db) - 3);
  }
  ctx.setLineDash([]);

  // The bands alone (thin), and with the preamp (the one that is heard).
  const trace = (offset, width, dash) => {
    ctx.lineWidth = width;
    ctx.setLineDash(dash);
    ctx.beginPath();
    for (let x = 0; x <= EQ_W; x += 2) {
      const y = eqY(eqBandsDb(bands, eqHzAt(x)) + offset);
      if (x) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    }
    ctx.stroke();
  };
  ctx.strokeStyle = color('--muted');
  trace(0, 1, [4, 3]);
  ctx.strokeStyle = color('--accent');
  trace(preampDb, 2.5, []);
  ctx.setLineDash([]);

  // Handles.
  bands.forEach((b, i) => {
    const x = eqX(b.frequencyHz), y = eqY(eqHandleDb(b, bands));
    ctx.fillStyle = color('--card');
    ctx.strokeStyle = color('--accent');
    ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(x, y, 7, 0, 2 * Math.PI); ctx.fill(); ctx.stroke();
    ctx.fillStyle = color('--fg');
    ctx.textAlign = 'center';
    ctx.fillText(String(i + 1), x, y + 4);
    ctx.textAlign = 'start';
  });
}

// Room EQ Wizard's filter text, as REW exports it and Equalizer APO reads it:
// "Filter 1: ON PK Fc 100 Hz Gain -3.0 dB Q 2.00", and "Preamp: -6 dB".
const REW_TYPES = { PK: 'peak', PEQ: 'peak', LS: 'lowShelf', LSC: 'lowShelf', HS: 'highShelf', HSC: 'highShelf', HP: 'highPass', HPQ: 'highPass', LP: 'lowPass', LPQ: 'lowPass' };
const REW_NAMES = { peak: 'PK', lowShelf: 'LSC', highShelf: 'HSC', highPass: 'HPQ', lowPass: 'LPQ' };

// Answers {preampDb, bands, skipped}: what could not be taken, by line.
function parseRew(text) {
  const number = (s) => Number(String(s).replace(',', '.'));
  const bands = [], skipped = [];
  let preampDb = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const preamp = /^preamp:\s*([-+]?[\d.,]+)\s*db/i.exec(line);
    if (preamp) { preampDb = number(preamp[1]); continue; }
    const filter = /^filter\s*\d*\s*:\s*(on|off)\s+(\S+)(.*)$/i.exec(line);
    if (!filter) continue;
    if (filter[1].toUpperCase() === 'OFF' || /^none$/i.test(filter[2])) continue;
    // "LS 12dB" and "HS 6dB" name a slope rather than a Q.
    const code = filter[2].toUpperCase();
    const type = REW_TYPES[code];
    const fc = /fc\s+([\d.,]+)\s*hz/i.exec(filter[3]);
    if (!type || !fc) { skipped.push(`${line} (${type ? tt('eq.noFc') : tt('eq.typeNotSupported', { name: filter[2] })})`); continue; }
    const gain = /gain\s+([-+]?[\d.,]+)\s*db/i.exec(filter[3]);
    const q = /\bq\s+([\d.,]+)/i.exec(filter[3]);
    const band = { type, frequencyHz: number(fc[1]), q: q ? number(q[1]) : 0.7071, gainDb: gain ? number(gain[1]) : 0 };
    if (!eqUsesGain(type)) band.gainDb = 0;
    if (bands.length >= EQ_MAX_BANDS) { skipped.push(`${line} (${tt('eq.moreThanFilters', { n: EQ_MAX_BANDS })})`); continue; }
    bands.push(band);
  }
  return { preampDb, bands, skipped };
}

function rewText(preampDb, bands) {
  // At least `d` decimals, more where the value has them (Q 0.7071), up to 4,
  // so that what is exported reads back as the same bands.
  const exact = (v, d) => (Number(v.toFixed(d)) === v ? v.toFixed(d) : String(Number(v.toFixed(4))));
  return [`Preamp: ${exact(preampDb, 1)} dB`, ...bands.map((b, i) =>
    `Filter ${i + 1}: ON ${REW_NAMES[b.type]} Fc ${exact(b.frequencyHz, 0)} Hz` +
      (eqUsesGain(b.type) ? ` Gain ${exact(b.gainDb ?? 0, 1)} dB` : '') + ` Q ${exact(b.q, 2)}`)].join('\n') + '\n';
}

// Import: pasted or from a file, applied to this room as Advanced bands (the
// file's preamp, else automatic headroom). Export: the same text, to copy or
// download. The pasted text is kept here so a re-render does not lose it.
function eqRewTools(client, current, bands, send) {
  const text = S.eqImport?.[client] ?? '';
  const apply = (source) => {
    const parsed = parseRew(source);
    if (parsed.bands.length === 0) { flash('eq.noFilterFound', true); return; }
    const preampDb = parsed.preampDb ?? (store.get('vx.eqAutoPreamp', '1') === '1' ? eqHeadroom(parsed.bands) : current.preampDb);
    store.set(`vx.eqMode:${client}`, 'advanced');
    send({ ...current, preampDb, bands: parsed.bands },
      tt('eq.filtersImported', { n: parsed.bands.length }) + (parsed.skipped.length ? `, ${tt('eq.skipped', { n: parsed.skipped.length })}: ${parsed.skipped.join('; ')}` : ''));
    (S.eqImport ??= {})[client] = '';
  };
  const exported = rewText(current.preampDb, bands);
  return h('details', { class: 'small', 'data-key': `eqRew:${client}` }, h('summary', {}, tt('eq.rewImportExport')),
    h('textarea', {
      rows: 5, class: 'mono eq-rew', value: text, placeholder: 'Filter 1: ON PK Fc 100 Hz Gain -3.0 dB Q 2.00',
      'aria-label': tt('eq.rewFilterText'),
      oninput: (e) => { (S.eqImport ??= {})[client] = e.target.value; },
    }),
    h('div', { class: 'row' },
      h('button', { onclick: () => apply(S.eqImport?.[client] ?? '') }, tt('eq.importText')),
      h('button', { onclick: (e) => e.currentTarget.nextElementSibling.click() }, tt('eq.importFile')),
      h('input', {
        type: 'file', hidden: true, accept: '.txt,text/plain', 'aria-label': tt('eq.rewFilterFile'),
        async onchange(e) {
          const file = e.target.files[0];
          e.target.value = '';
          if (file) apply(await file.text());
        },
      }),
      h('button', { onclick: () => navigator.clipboard.writeText(exported).then(() => flash('eq.rewTextCopied'), () => flash('eq.copyRefused', true)) }, tt('eq.copyAsRew')),
      h('button', {
        onclick() {
          const link = h('a', { href: URL.createObjectURL(new Blob([exported], { type: 'text/plain' })), download: `${client}-eq.txt` });
          link.click();
          URL.revokeObjectURL(link.href);
        },
      }, tt('eq.download'))),
    h('pre', { class: 'json' }, exported));
}

// The house's equalizer profiles (server phase 41): built-ins first.
async function loadEqProfiles() {
  try {
    S.eqProfiles = await api('GET', 'equalizer/profiles', undefined, { quiet: true });
  } catch {
    S.eqProfiles = []; // a server before phase 41 answers 404
  }
  renderRooms();
}

// The client keeps its Volume Control in its own config, like the buffer; the
// value shown is the new one once its VOLUME_CONFIG has come back, which the
// room.volumeControl event announces. Software is not offered under
// Bit-Perfect: the core would refuse it.
function volumeControlEditor(vc, audioControl, hardwareVolume, path) {
  const strict = audioControl === 'bitPerfect';
  const noHardware = hardwareVolume && !hardwareVolume.available;
  return h('div', { class: 'room-controls', title: tt('room.howClientPlays') },
    h('select', {
      value: vc.configured,
      'aria-label': tt('room.volumeControlField'),
      onchange: (e) => act(() => api('PATCH', path, { volumeControl: e.target.value }), tt('room.volumeControlSent', { mode: e.target.value })),
    }, VOLUME_CONTROLS.map(([m, label, title]) => h('option', {
      value: m,
      disabled: (m === 'software' && strict) || (m === 'hardware' && noHardware && vc.configured !== 'hardware'),
      title: tt(m === 'software' && strict ? 'room.notAvailableUnder' : m === 'hardware' && noHardware ? 'room.thisDeviceHas' : title),
    }, tt(label)))),
    h('span', { class: 'small ' + (vc.effective === 'disabled' && vc.configured !== 'disabled' ? 'warn' : 'muted') },
      `→ ${volumeControlLabel(vc.effective)}`, vc.reason && ` (${vc.reason})`));
}

// The actuator a room's drift is corrected through
// (_docs/HardwareDriftCorrectionRoadmap.md). The client keeps it; a change
// reopens the room's stream. The hardware clock is offered only where the
// device has a verified trim; whether the room is corrected at all still
// follows from its Audio Control and its zone.
const DRIFT_CORRECTIONS = [
  ['auto', 'room.driftMode.auto', 'room.driftMode.autoHint'],
  ['resampling', 'room.driftMode.resampling', 'room.driftMode.resamplingHint'],
  ['hardwareClock', 'room.driftMode.hardwareClock', 'room.driftMode.hardwareClockHint'],
];

// Why a room that asks for drift correction does not get it, with what to
// change, or null. The texts are the core's (driftDisabledText).
const DRIFT_FIXES = {
  "the configured resampler cannot vary a ratio": 'room.driftFix.resampler',
  "the room's Audio Control is bitPerfect": 'room.driftFix.audioControl',
};
function driftNotCorrected(dc) {
  if (!dc?.requested || dc.active || !dc.disabledBy) return null;
  const fix = DRIFT_FIXES[dc.disabledBy];
  return fix ? `${dc.disabledBy}: ${tt(fix)}` : dc.disabledBy;
}

function driftCorrectionEditor(dc, path) {
  const noClock = !dc.hardwareClockAvailable;
  const clock = dc.hardwareClock;
  const why = driftNotCorrected(dc);
  // What corrects it, not by how much: the Sync & buffer tiles show the ppm.
  const now = tt(dc.actuator === 'hardwareClock' ? 'room.nowHardwareClock' : dc.actuator === 'resampling' ? 'room.resampling' : 'room.notCorrected');
  return h('div', { class: 'room-controls', title: tt('room.howThisRoom') },
    h('select', {
      value: dc.mode,
      'aria-label': tt('room.driftCorrectionField'),
      onchange: (e) => act(() => api('PATCH', path, { driftCorrection: e.target.value }), tt('room.driftCorrectionSent', { mode: e.target.value })),
    }, DRIFT_CORRECTIONS.map(([m, label, title]) => h('option', {
      value: m,
      disabled: m === 'hardwareClock' && noClock && dc.mode !== 'hardwareClock',
      title: tt(m === 'hardwareClock' && noClock ? 'room.thisDeviceHasNo' : title),
    }, tt(label)))),
    h('span', { class: 'small ' + (why || clock?.error || (dc.mode === 'hardwareClock' && dc.actuator === 'resampling') ? 'warn' : 'muted') },
      `→ ${now}`, why && `: ${why}`, clock?.error && ` (${clock.error})`));
}

// One measure per chart, over the samples this page has read (at the start and after events).
// `off` dims a measure the room is not given (applied ppm on an uncorrected room).
// The room's sound cards as its client reported them (MATP section 56). The
// client keeps the choice in its own config; '' is its system default. A
// configured card the client could not find is shown against the one it fell
// back to. A rescan is answered before the client has scanned, which takes
// seconds; the room.audioDevices event brings the new list when it has.
function audioDevicePicker(ad, patch, path) {
  const name = (id) => ad.devices.find((x) => x.id === id)?.name ?? id;
  const fellBack = ad.configured !== '' && ad.selected !== ad.configured;
  return h('div', { class: 'room-controls' },
    h('select', { class: 'grow', value: ad.configured, 'aria-label': tt('room.outputDevice'), onchange: (e) => patch({ audioDeviceId: e.target.value }) },
      h('option', { value: '' }, tt('room.systemDefault') + (ad.configured === '' && ad.selected ? ` (${name(ad.selected)})` : '')),
      ad.devices.map((x) => h('option', { value: x.id, title: x.id }, `${x.name || x.id}${x.isDefault ? ` (${tt('room.systemDefault')})` : ''}`))),
    h('button', {
      title: tt('room.scanClientsSound'),
      onclick: () => act(() => api('POST', `${path}/rescan`), 'room.rescanSent'),
    }, tt('room.rescan')),
    fellBack && h('span', { class: 'warn small' }, tt('room.configuredCardMissing', { name: ad.selected ? name(ad.selected) : tt('room.nothing') })),
    !ad.selected && !fellBack && h('span', { class: 'bad small' }, tt('room.noUsableDevice')));
}

// A room's buttons and encoders as its client reported them (MATP section 57,
// server phase 20), one row per gesture. The client keeps the bindings; a row's
// "set" sends only that gesture, and the room reports its inputs again. Which
// parameters an action takes is the protocol's: step for volumeUp/Down, value
// for setVolume, scope for those and toggleMute -- the others are disabled, not
// hidden, so the row does not jump while choosing.
const INPUT_ACTIONS = ['none', 'play', 'pause', 'togglePause', 'stop', 'next', 'previous',
  'volumeUp', 'volumeDown', 'setVolume', 'toggleMute', 'standbyOn', 'standbyOff', 'toggleStandby', 'deepSleep',
  // User actions: the server runs nothing, room.input on the event stream is all.
  ...Array.from({ length: 10 }, (_, i) => `action${i + 1}`)];
const takesStep = (a) => a === 'volumeUp' || a === 'volumeDown';
const takesValue = (a) => a === 'setVolume';
const takesScope = (a) => takesStep(a) || takesValue(a) || a === 'toggleMute';

function inputsEditor(client, inputs, path, deepSleep) {
  const rows = [];
  for (const input of inputs) {
    for (const [gesture, b] of Object.entries(input.bindings)) {
      const step = h('input', { name: 'step', type: 'number', min: 0.001, max: 1, step: 0.001, style: 'width:5em', value: String(b.step ?? 0.05) });
      const value = h('input', { name: 'value', type: 'number', min: 0, max: 1, step: 0.01, style: 'width:5em', value: String(b.value ?? 0.5) });
      const scope = h('select', { name: 'scope', value: b.scope ?? 'room' }, h('option', { value: 'room' }, tt('room.room')), h('option', { value: 'zone' }, tt('room.scopeZone')));
      // Through the form's elements: a re-render keeps the nodes on the page.
      const enable = (F, action) => {
        F.step.disabled = !takesStep(action);
        F.value.disabled = !takesValue(action);
        F.scope.disabled = !takesScope(action);
      };
      const action = h('select', { name: 'action', value: b.action, onchange: (e) => enable(e.target.form.elements, e.target.value) },
        // deepSleep only where the client can run it, or to show a binding it has.
        INPUT_ACTIONS.filter((a) => a !== 'deepSleep' || deepSleep || b.action === 'deepSleep')
          .map((a) => h('option', { value: a }, a)));
      const form = h('form', {
        class: 'row',
        onsubmit(e) {
          e.preventDefault();
          const F = e.target.elements;
          const binding = { action: F.action.value };
          if (takesStep(binding.action)) binding.step = Number(F.step.value);
          if (takesValue(binding.action)) binding.value = Number(F.value.value);
          if (takesScope(binding.action)) binding.scope = F.scope.value;
          act(() => api('PATCH', `${path}/inputs/${enc(input.id)}`, { [gesture]: binding }),
            (r) => `${client} ${input.id} ${gesture}: ${binding.action}${r.changed ? '' : ` ${tt('room.unchanged')}`}`);
        },
      }, action, h('span', { class: 'muted' }, tt('room.step')), step, h('span', { class: 'muted' }, tt('room.value')), value, scope, h('button', {}, tt('common.set')));
      enable(form.elements, b.action);
      const last = S.lastInput[client];
      rows.push(h('tr', { class: last?.inputId === input.id && last?.gesture === gesture ? 'current' : '' },
        h('td', {}, input.id, h('div', { class: 'muted' }, input.kind)),
        h('td', {}, gesture),
        h('td', {}, form)));
    }
  }
  return h('div', { class: 'table-wrap plain' }, h('table', { class: 'small' },
    h('tr', {}, h('th', {}, tt('room.input')), h('th', {}, tt('room.gesture')), h('th', {}, tt('room.boundTo'))),
    rows));
}

function lastInputLine(e) {
  const param = e.step != null ? ` ${e.step} × ${e.count}` : e.value != null ? ` ${e.value}` : e.count > 1 ? ` × ${e.count}` : '';
  return h('div', { class: 'small' }, h('span', { class: 'muted' }, `${tt('room.lastInput', { at: e.at })}: `),
    `${e.inputId} ${e.gesture} → ${e.action}${param}${e.scope ? ` (${e.scope})` : ''}`);
}

function spark(label, client, key, digits, off = false, history = S.history) {
  const points = (history[client] ?? []).filter((p) => p[key] != null);
  const canvas = h('canvas', { width: 300, height: 68 });
  const last = points.at(-1)?.[key];
  const caption = h('span', {}, `${label}: ${fmtNum(last, digits)}`);
  if (points.length >= 2) {
    const values = points.map((p) => p[key]);
    let min = Math.min(...values), max = Math.max(...values);
    if (min === max) { min -= 1; max += 1; }
    const t0 = points[0].t, t1 = points.at(-1).t;
    const x = (t) => 4 + ((t - t0) / (t1 - t0 || 1)) * 292;
    const y = (v) => 64 - ((v - min) / (max - min)) * 60;
    const ctx = canvas.getContext('2d');
    const style = getComputedStyle(document.documentElement);
    if (min < 0 && max > 0) {
      ctx.strokeStyle = style.getPropertyValue('--line');
      ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(0, y(0)); ctx.lineTo(300, y(0)); ctx.stroke();
      ctx.setLineDash([]);
    }
    ctx.strokeStyle = style.getPropertyValue('--accent');
    ctx.lineWidth = 3;
    ctx.lineJoin = 'round';
    ctx.beginPath();
    points.forEach((p, i) => (i ? ctx.lineTo(x(p.t), y(p[key])) : ctx.moveTo(x(p.t), y(p[key]))));
    ctx.stroke();
    caption.title = tt('room.minMaxOver', { min: fmtNum(min, digits), max: fmtNum(max, digits), n: Math.round((t1 - t0) / 1000) });
    canvas.addEventListener('mousemove', (e) => {
      const t = t0 + ((e.offsetX / canvas.clientWidth) * 300 - 4) / 292 * (t1 - t0);
      const near = points.reduce((b, p) => (Math.abs(p.t - t) < Math.abs(b.t - t) ? p : b));
      canvas.title = `${new Date(near.t).toLocaleTimeString()}  ${fmtNum(near[key], digits)}`;
    });
  }
  if (off) caption.append(` ${tt('room.notApplied')}`);
  return h('div', { class: 'spark' + (off ? ' off' : '') }, caption, canvas);
}

// --- library helpers ----------------------------------------------------------

async function addTracks(tracks, next) {
  const zone = needTarget();
  return api('POST', `zones/${zone}/queue/items`, next ? { tracks, next: true } : { tracks });
}
async function addStation(station, next) {
  const zone = needTarget();
  return api('POST', `zones/${zone}/queue/items`, next ? { station, next: true } : { station });
}
function addFavorite(track, station) {
  return api('POST', 'favorites', { ...(track ? { track } : { station }), ...ownerOf() });
}
// Stations as the library's tracks are listed (mbTracks): live instead of a
// length, and under the pointer play, next, last and favourite.
function stationTable(stations) {
  return h('table', { class: 'small rows lib-tracks' }, stations.map((s) => h('tr', dragItem({ station: s }),
    dragGrip('td'),
    h('td', {}, infoArt({ ...s, kind: 'station' }, artSlot(s.artworkId, 'radio'))),
    h('td', { class: 'row-text' }, h('div', { class: 'track-lines', title: `${s.name} [${s.providerId} ${s.providerItemId}]` },
      h('div', { class: 'line' }, s.name),
      h('div', { class: 'muted line' }, [s.genre, s.homepage].filter(Boolean).join(' · ')))),
    h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
      h('span', { class: 'muted' }, tt('library.live')),
      h('span', { class: 'row-tools' }, refButtons({ station: s }, 'row-play'),
        favoriteButton(likeKey('station', s), () => addFavorite(null, s)),
        // Removed, its row goes at once: the library's page is not read again.
        s.providerId === 'webradio' && h('button', { class: 'danger', title: tt('library.removeStation'), 'aria-label': tt('library.removeStation'), onclick(e) {
          const row = e.currentTarget.closest('tr');
          if (confirm(tt('library.removeStationConfirm', { name: s.name }))) act(async () => { await api('DELETE', `stations/${enc(s.providerItemId)}`); row.remove(); }, 'library.stationRemoved').then(() => act(loadStations));
        } }, icon('trash'))))))));
}

// --- Home ---------------------------------------------------------------------
// The start page, as the multiroom apps have theirs (Sonos, Roon, BluOS): a
// greeting, every zone at a glance with what it plays (a card goes to the
// zone, its button plays and pauses), then shelves of covers -- what the AI
// proposes, the favourites, the likes and last what was played lately -- whose
// buttons play into the player bar's zone, as the library's do. A shelf's
// "Show all" is its own view, #/home/<view>: all of it as a grid of the same
// covers.
const HOME_SHELF = 14;
document.addEventListener('scroll', () => document.body.classList.toggle('scrolled', scrollY > 400), { passive: true });
// [title, about] keys, translated when the page starts (i18n.js before this script).
const HOME_VIEWS = Object.fromEntries(Object.entries({ foryou: ['home.view.forYou', 'home.view.forYouAbout'], favorites: ['home.view.favorites', 'home.view.favoritesAbout'], liked: ['home.view.liked', 'home.view.likedAbout'], recent: ['home.view.recent', 'home.view.recentAbout'] })
  .map(([k, texts]) => [k, texts.map((t) => tt(t))]));
let homeView = null; // one of HOME_VIEWS, or null for Home itself
// An album or artist opened from a cover, #/home/open/<provider>/<node>:
// { provider, node, key, back (opened from within the app), r (its browse page), error }.
let homeDetail = null;
const openHomeDetail = (provider, node) => { location.hash = `#/home/open/${enc(provider)}/${enc(node)}`; };
async function loadHomeDetail() {
  const d = homeDetail;
  try {
    d.r = await api('GET', `providers/${enc(d.provider)}/browse?node=${enc(d.node)}&limit=${MB_LIMIT}`);
  } catch (e) {
    d.error = e.message;
  }
  if (d === homeDetail) renderHome();
}
let homePlays = null;
async function loadHome() {
  const [plays] = await Promise.all([
    api('GET', `history?limit=100${scopeQuery('&')}`, undefined, { quiet: true }).catch(() => null),
    loadProposals().catch(() => {})]);
  homePlays = plays;
  renderHome();
}

function renderHome() {
  if (page !== 'home') return;
  // A shelf's "Show all" takes the shelves' place; the greeting and the zones
  // stay above it, held under the header while its list scrolls.
  // The same top on both, so nothing moves between them; in a view it is held.
  // (A view's "to top" shows once the page is scrolled down: body.scrolled.)
  render('home', $('#home'), () => [h('div', { class: 'home-top' + (homeView || homeDetail ? ' held' : '') }, homeHello(), homeZones()),
    ...(homeDetail ? homeDetailView(homeDetail) : homeView ? homeAll(homeView) : [homeProposals(),
      homeShelf('favorites', homeTiles('favorites')),
      homeShelf('liked', homeTiles('liked')),
      homeShelf('recent', homeTiles('recent'))])]);
}

// Every cover of a view (For you: of the kind picked).
function homeTiles(view) {
  if (view === 'favorites') return S.favorites.map(favoriteTile);
  if (view === 'liked') return [...likedItems.values()].sort((a, b) => b.id - a.id).map(likedTile).filter(Boolean);
  if (view === 'recent') return homeRecent();
  return forYou().tiles;
}

// A shelf's "Show all": its covers as a grid, wrapping, under its title (and
// For you's tabs), under the greeting and the zones (to drop on), Home a link away.
function homeAll(view) {
  const [title, about] = HOME_VIEWS[view];
  const f = view === 'foryou' ? forYou() : null;
  const tiles = f ? f.tiles : homeTiles(view);
  // Its head as the shelf's on Home: one line, Home where "Show all" was.
  return [h('section', { class: 'home-section home-shelf', 'aria-label': title },
    h('div', { class: 'home-shelf-head' },
      // The title picks among the views: a menu under it.
      h('h2', {},
        h('span', { class: 'home-view-pick' },
          h('button', { type: 'button', class: 'home-view-button', 'aria-haspopup': 'menu', title: tt('home.showAnother'), onclick: (e) => e.currentTarget.nextElementSibling.togglePopover() },
            title, icon('forward')),
          h('div', { class: 'pick-popover', popover: 'auto', role: 'menu', 'aria-label': tt('common.show'), ontoggle(e) { if (e.newState === 'open') this.style.cssText = `left: ${this.previousElementSibling.getBoundingClientRect().left}px; top: ${this.previousElementSibling.getBoundingClientRect().bottom + 8}px`; } },
            Object.entries(HOME_VIEWS).map(([v, [name]]) => h('button', {
              type: 'button', role: 'menuitemradio', 'aria-pressed': String(v === view), 'aria-checked': String(v === view),
              onclick(e) { e.currentTarget.closest('[popover]').hidePopover(); location.hash = `#/home/${v}`; },
            }, icon({ foryou: 'sparkle', favorites: 'star', liked: 'heart', recent: 'clock' }[v]), name)))),
        h('span', { class: 'muted' }, about)), f?.tabs,
      h('a', { class: 'home-back', href: '#/home' }, icon('back'), tt('home.home'))),
    f ? [h('div', { class: 'home-grid' }, freshPicksTile(), tiles), !tiles.length && h('p', { class: 'muted home-empty' }, f.empty)]
      : tiles.length ? h('div', { class: 'home-grid' }, tiles) : h('p', { class: 'muted home-empty' }, tt('home.nothingHereYet'))),
    // Back to the top, shown once the list is scrolled down (body.scrolled).
    h('button', { type: 'button', class: 'to-top', title: tt('home.backToTop'), 'aria-label': tt('home.backToTop'),
      onclick: () => scrollTo({ top: 0, behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' }) }, icon('back'))];
}

// An opened album or artist: its cover, name and buttons, then what it holds
// (albums as covers that open here too, tracks as the library's rows). Back
// goes to where it was opened from; the library has the rest of a long list.
function homeDetailView(d) {
  const r = d.r, n = r?.node ?? { providerId: d.provider, nodeId: d.node, title: '', kind: '' };
  const back = h('a', { class: 'home-back', href: '#/home', onclick(e) { if (d.back) { e.preventDefault(); history.back(); } } }, icon('back'), tt('home.back'));
  if (!r) {
    return [h('section', { class: 'home-section home-detail' }, h('div', { class: 'home-detail-bar' }, back),
      d.error ? h('p', { class: 'bad' }, d.error) : h('span', { class: 'spinner', role: 'status', 'aria-label': tt('home.loading') }))];
  }
  const node = { provider: n.providerId, id: n.nodeId };
  const entries = (nodes, items) => [
    nodes.length > 0 && h('div', { class: 'home-grid' }, nodes.map((x) => homeTile({
      artworkId: x.artworkId, title: x.title, sub: x.subtitle, ref: x.playable && { node: { provider: x.providerId, id: x.nodeId } },
      like: nodeLike(x), open: () => openHomeDetail(x.providerId, x.nodeId) }))),
    items.tracks.length > 0 && mbTracks(items.tracks),
    items.stations.length > 0 && stationTable(items.stations)];
  const count = r.nodes.length + r.items.tracks.length + r.items.stations.length;
  const library = () => openInLibrary(n.nodeId, n.title, n.providerId);
  return [h('section', { class: 'home-section home-detail', 'aria-label': n.title },
    h('div', { class: 'home-detail-bar' }, back),
    h('div', { class: 'home-detail-head', ...dragNode(n) },
      h('span', { class: 'cover-art' }, icon('library'), art(n.artworkId)),
      h('div', { class: 'home-detail-text' },
        n.kind && h('div', { class: 'home-detail-kind' }, tv('kind', n.kind)),
        h('h2', {}, n.title),
        n.subtitle && h('div', { class: 'muted' }, n.subtitle),
        h('div', { class: 'row' },
          n.playable && refActions({ node }),
          nodeLike(n),
          h('button', { type: 'button', onclick: library }, icon('library'), tt('home.openInLibrary'))))),
    r.sections ? r.sections.map((s) => [
      h('h3', { class: 'home-detail-section' }, s.title, s.total != null && h('span', { class: 'muted' }, ` ${s.total}`),
        s.more && h('a', { href: '#', onclick(e) { e.preventDefault(); openHomeDetail(s.more.providerId, s.more.nodeId); } }, tt('home.showAll'))),
      ...entries(s.nodes, s.items)]) : entries(r.nodes, r.items),
    !count && !r.sections?.length && h('p', { class: 'muted home-empty' }, tt('home.nothingHere')),
    !r.sections && (r.total != null ? r.total > count : count >= MB_LIMIT) && h('p', { class: 'muted small' },
      `${r.total != null ? tt('home.firstOf', { n: count, total: r.total }) : tt('home.first', { n: count })}; `, h('a', { href: '#', onclick(e) { e.preventDefault(); library(); } }, tt('home.restInLibrary')), '.'))];
}

function homeHello() {
  const hour = new Date().getHours();
  const hello = tt(hour < 5 ? 'home.goodNight' : hour < 12 ? 'home.goodMorning' : hour < 18 ? 'home.goodAfternoon' : 'home.goodEvening');
  const who = S.user ? `, ${S.user}` : '';
  const on = S.zones.filter((z) => z.queue?.transport === 'playing').length;
  return h('header', { class: 'home-hello' },
    h('h1', {}, hello, who),
    h('p', {}, S.zones.length ? tt(on ? 'home.ofZonesPlaying' : S.zones.length === 1 ? 'home.zoneAllQuiet' : 'home.zonesAllQuiet', { on, n: S.zones.length }) : tt('home.noZonesYet')));
}

function homeZones() {
  if (!S.zones.length) return null;
  return h('section', { class: 'home-section', 'aria-label': tt('common.zones') },
    h('div', { class: 'home-zones' }, S.zones.map((z) => {
      const p = playing(z), on = z.queue?.transport === 'playing';
      const open = () => { setTarget(z.zone); location.hash = '#/zones'; };
      // A cover dropped on it goes to the end of its queue.
      return h('div', { class: 'home-zone' + (on ? ' playing' : ''), onclick: open, ...dropTarget(null, (items) => dropQueued(z.zone, items)) },
        // What plays, over its first room's picture: the room's where there is no artwork or it does not load.
        h('span', { class: 'cover-art' }, dropping.has(z.zone) ? h('span', { class: 'spinner', role: 'status', 'aria-label': tt('home.addingToQueue') })
          : z.desired?.clients?.[0] ? [roomImage(z.desired.clients[0]), art(p.item?.item.artworkId)] : p.cover()),
        h('div', { class: 'home-zone-text' },
          h('a', { class: 'home-zone-name', href: '#/zones', onclick(e) { e.preventDefault(); } }, on && icon('bars'), z.name ?? z.zone),
          h('div', { class: 'home-zone-title' }, p.title),
          p.sub && h('div', { class: 'home-zone-sub' }, p.sub)),
        z.queue?.items.length ? h('span', { class: 'home-zone-play', onclick: (e) => e.stopPropagation() },
          on ? transportButton('pause', tt('common.pause'), () => transport(z.zone, 'play', 'pause'), 'primary play', z.zone, 'play')
            : transportButton('play', tt('home.play'), () => transport(z.zone, 'play', 'play'), 'primary play', z.zone)) : null,
        // The zone's own volume and mute, as in the player bar.
        z.desired && h('div', { class: 'home-zone-volume', onclick: (e) => e.stopPropagation() },
          volumeRow(z.desired.volume, z.desired.muted,
            (volume) => act(() => api('PATCH', `zones/${enc(z.zone)}`, { volume })),
            (muted) => act(() => api('PATCH', `zones/${enc(z.zone)}`, { muted })))));
    })));
}

// A shelf: its title, "Show all" into its own view, a row of its first covers.
// Empty, it stays, with one placeholder cover saying how it fills.
const HOME_EMPTY = { favorites: 'star', liked: 'heart', recent: 'clock' };
function homeShelf(view, tiles) {
  const [title, about] = HOME_VIEWS[view];
  return h('section', { class: 'home-section home-shelf', 'aria-label': title },
    h('div', { class: 'home-shelf-head' },
      h('h2', {}, title, h('span', { class: 'muted' }, about)),
      tiles.length > 0 && h('a', { href: `#/home/${view}` }, tt('home.showAll'))),
    h('div', { class: 'shelf-row' }, tiles.length ? tiles.slice(0, HOME_SHELF)
      : homeTile({ placeholder: HOME_EMPTY[view], title: tt(`home.empty.${view}`), sub: tt(`home.empty.${view}Hint`) })));
}

// A cover as the library's (its buttons slid up under the pointer), its
// title and second line under it. `open` is what a click on it does.
function homeTile({ artworkId, placeholder = 'library', title, sub, ref, like, fav, open, why }) {
  // Dragged onto a zone (here or on the Zones page) it goes to the end of its queue.
  return h('div', { class: 'cover home-tile', title: why ? `${title}\n${why}` : title, ...(ref ? dragItem(ref) : {}) },
    // Its name on the picture's bottom, as a library cover's (mbCover).
    h('div', { class: 'pic', onclick: open ?? (() => {}) , style: open || ref ? '' : 'cursor: default' }, icon(placeholder), art(artworkId),
      h('span', { class: 'cover-name' }, title),
      (ref || like || fav) && h('div', { class: 'cover-actions', onclick: (e) => e.stopPropagation() }, ref && refButtons(ref, 'cover-play'), like, fav)),
    sub && h('div', { class: 'small muted title' }, sub),
    why && h('div', { class: 'home-why' }, why));
}

// Played lately, newest first, each track once.
function homeRecent() {
  const seen = new Set();
  return (homePlays?.items ?? []).filter((p) => !seen.has(likeKey('track', p.track)) && seen.add(likeKey('track', p.track)))
    .map((p) => homeTile({
      artworkId: p.track.artworkId, title: p.track.title, sub: [p.track.artist, zoneName(p.zone)].filter(Boolean).join(' · '),
      ref: { tracks: [p.track] }, like: likeButton('track', p.track),
      fav: favoriteButton(likeKey('track', p.track), () => addFavorite(p.track)) }));
}

function favoriteTile(f) {
  const x = f.item, station = media(f, 'station'), book = media(f, 'group');
  return homeTile({
    artworkId: x.artworkId, placeholder: station ? 'radio' : 'library', title: x.title ?? x.name,
    sub: station ? tt('home.radio') : book ? [x.artist, tt('home.book')].filter(Boolean).join(' · ') : x.artist,
    ref: { favorite: f.id }, like: media(f, 'track') && likeButton('track', x),
    fav: favoriteButton(favoriteKey(f), () => (book ? addBookFavorite(x.source) : addFavorite(media(f, 'track'), station)), Boolean(book)) });
}

function likedTile(l) {
  const kind = likedKind(l), x = l.item;
  if (foundTrack(l)) {
    const t = foundTrack(l);
    return homeTile({ artworkId: t.artworkId, title: t.title, sub: t.artist, ref: { tracks: [t] }, like: likeButton('track', x), fav: favoriteButton(likeKey('track', t), () => addFavorite(t)) });
  }
  if (radioSong(l)) return null;
  if (kind === 'artist') return homeTile({ artworkId: x.artworkId, title: x.name, sub: tt('home.artist'), ref: { node: { provider: x.providerId, id: x.providerItemId } }, like: likeButton('artist', x), open: () => openHomeDetail(x.providerId, x.providerItemId) });
  if (kind === 'album') return homeTile({ artworkId: x.artworkId, title: x.title, sub: x.artist ?? tt('home.album'), ref: { node: { provider: x.providerId, id: x.providerItemId } }, like: likeButton('album', x), open: () => openHomeDetail(x.providerId, x.providerItemId) });
  if (kind === 'audiobook') {
    const b = bookRef(x.reference);
    return b && homeTile({ artworkId: x.artworkId, title: x.title, sub: x.author ?? tt('home.bookLabel'), ref: { node: { provider: b.providerId, id: b.providerItemId } }, like: likeButton('audiobook', b) });
  }
  if (kind === 'station') return homeTile({ artworkId: x.artworkId, placeholder: 'radio', title: x.name, sub: tt('home.radio'), ref: { station: x }, like: likeButton('station', x) });
  return homeTile({ artworkId: x.artworkId, title: x.title, sub: x.artist, ref: { tracks: [x] }, like: likeButton('track', x), fav: favoriteButton(likeKey('track', x), () => addFavorite(x)) });
}

// What the AI proposes (as the library's Proposals has it): one kind, picked by the
// tabs (Tracks first, then as last picked, kept per viewer), or all of it
// mixed -- tracks, albums, artists and books in turn; each with why. A cover's
// second line says what it is.
const FOR_YOU_KINDS = [['all', 'home.kind.all'], ['track', 'home.kind.tracks'], ['album', 'common.albums'], ['artist', 'common.artists'], ['audiobook', 'home.kind.books']]; // labels translated where shown
function forYou() {
  const v = proposals ?? {};
  const lists = [['track', v.tracks], ['album', v.albums], ['artist', v.artists], ['audiobook', v.audiobooks]].filter(([, l]) => l?.length);
  const kinds = FOR_YOU_KINDS.filter(([k]) => k === 'all' || lists.some(([l]) => l === k));
  let pick = store.get('vx.homeForYou', 'track');
  if (!kinds.some(([k]) => k === pick)) pick = kinds[1]?.[0] ?? 'all';
  const shown = [];
  if (pick === 'all') {
    for (let i = 0; lists.some(([, l]) => i < l.length); i++) for (const [kind, l] of lists) if (i < l.length) shown.push([kind, l[i]]);
  } else {
    shown.push(...lists.find(([k]) => k === pick)[1].map((x) => [pick, x]));
  }
  const tabs = kinds.length > 2 && h('div', { class: 'home-tabs', role: 'tablist', 'aria-label': tt('common.show') }, kinds.map(([k, label]) => h('button', {
    type: 'button', role: 'tab', 'aria-selected': String(k === pick),
    onclick: () => { store.set('vx.homeForYou', k); renderHome(); },
  }, tt(label), k !== 'all' && h('span', { class: 'home-tab-count' }, lists.find(([l]) => l === k)[1].length))));
  const empty = tt(v.run?.state === 'running' ? 'home.aiIsChoosingForYou'
    : v.aiAvailable ? 'home.noProposalsYet' : 'home.noProposalsConnect');
  return { tabs, empty, tiles: shown.map(([kind, p]) => proposalTile(kind, p)) };
}

// "Fresh picks": For you's first cover, which asks the AI for new proposals
// (POST ai/proposals/refresh, as the Proposals page's button). A ring turns
// round its sparkle while the AI works; proposals.running and .changed bring
// the outcome (onEvent).
function freshPicksTile() {
  const v = proposals ?? {}, running = v.run?.state === 'running', can = Boolean(v.aiAvailable);
  const sub = tt(running ? 'home.aiIsChoosing' : can ? 'home.askAiFor' : 'home.connectAiProvider');
  return h('div', { class: 'cover home-tile fresh-picks' + (running ? ' running' : ''), title: `${tt('home.freshPicks')}\n${sub}` },
    h('button', {
      type: 'button', class: 'pic fresh-art', disabled: running || !can, 'aria-label': tt(running ? 'home.aiIsChoosingFresh' : 'home.freshPicksAsk'),
      onclick: () => act(() => api('POST', 'ai/proposals/refresh', ownerOf()), 'home.aiIsChoosingFreshPicks').then(() => act(loadProposals)),
    },
    h('span', { class: 'fresh-glow', 'aria-hidden': 'true' }),
    running && h('span', { class: 'fresh-ring', role: 'status', 'aria-label': tt('home.aiIsChoosingNow') }),
    icon('sparkle'),
    h('span', { class: 'cover-name' }, tt('home.freshPicks'))),
    h('div', { class: 'small muted title' }, sub));
}

function proposalTile(kind, p) {
  const x = p.item;
  if (kind === 'track') return homeTile({ artworkId: x.artworkId, title: x.title, sub: [tt('common.track'), x.artist].filter(Boolean).join(' · '), why: p.reason, ref: { tracks: [x] }, like: likeButton('track', x), fav: favoriteButton(likeKey('track', x), () => addFavorite(x)) });
  if (kind === 'artist') return homeTile({ artworkId: x.artworkId, title: x.name, sub: tt('home.artist'), why: p.reason, ref: { node: { provider: x.providerId, id: x.providerItemId } }, like: likeButton('artist', x), open: () => openHomeDetail(x.providerId, x.providerItemId) });
  if (kind === 'album') return homeTile({ artworkId: x.artworkId, title: x.title, sub: [tt('home.album'), x.artist].filter(Boolean).join(' · '), why: p.reason, ref: { node: { provider: x.providerId, id: x.providerItemId } }, like: likeButton('album', x), open: () => openHomeDetail(x.providerId, x.providerItemId) });
  const b = bookRef(x.reference);
  return homeTile({ artworkId: x.artworkId, title: x.title, sub: [tt('home.bookLabel'), x.author].filter(Boolean).join(' · '), why: p.reason, ref: b && { node: { provider: b.providerId, id: b.providerItemId } }, like: b && likeButton('audiobook', b) });
}

function homeProposals() {
  if (!proposals) return null;
  const f = forYou();
  const [title, about] = HOME_VIEWS.foryou;
  return h('section', { class: 'home-section home-shelf', 'aria-label': title },
    h('div', { class: 'home-shelf-head' },
      h('h2', {}, title, h('span', { class: 'muted' }, about)), f.tabs,
      h('a', { href: '#/home/foryou' }, tt('home.showAll'))),
    h('div', { class: 'shelf-row' }, freshPicksTile(), f.tiles.slice(0, HOME_SHELF)),
    !f.tiles.length && h('p', { class: 'muted home-empty' }, f.empty));
}

// --- playlists and favourites ---------------------------------------------------

async function loadPlaylists() {
  [S.playlists, S.playlistFolders] = await Promise.all([api('GET', 'playlists'), api('GET', 'playlist-folders')]);
  // The sources beside the media browser list the playlists to drop on.
  renderSources();
  if (!S.playlists.some((p) => p.id === S.openPlaylist)) S.openPlaylist = null;
  renderPlaylistTree();
  await renderPlaylistEntries();
}

// --- folder trees: the Playlists and Radio stations pages --------------------
// A page's folders as a tree: folders first, by name, then what lies in each
// (the playlists, in their order; stations are listed beside the tree). A
// folder opens and closes (kept per viewer). Folders and items are dragged
// onto a folder to go in it, onto an item to go beside it, onto the tree's
// ground (or its root) to go to the top. Each has a ⋯ menu for the same and
// more. A new folder or item, or a new name, is typed in the tree itself.
//
// cfg: key (its render key), store (where the open folders are kept), el,
// label, path (the folders' API path), noun/nouns (what an item is called),
// folders(), items() (each {id, name, folder}), reload(), moveItem(id, folder)
// and empty (the text of an empty tree). Optional: item (how items show in the
// tree: icon, current(x), open(x), count(x), menu(x), drop(x, bodies)),
// create(name, folder) and rename(id, name) (an item made and named in the
// tree), created(item), root (a row for the top: name, icon, count()),
// current() and pick(folder) (a folder picked, null the root), drop(folder,
// bodies) (library items dropped on a folder), contents(items) (how much a
// folder's deletion takes, "12 entries") and deleting(folders, items).
function folderTree(cfg) {
  const type = `application/x-galdrun-tree-${cfg.key.toLowerCase()}`;
  const open = new Set((() => { try { return JSON.parse(store.get(cfg.store, '[]')); } catch { return []; } })());
  const keepOpen = () => store.set(cfg.store, JSON.stringify([...open]));
  const folderOf = (id) => cfg.folders().find((f) => f.id === id);
  // The names from the top down to the folder.
  const pathOf = (f) => { const names = []; for (let at = f; at; at = folderOf(at.parent)) names.unshift(at.name); return names; };
  // A folder and those it lies in opened, so what goes into it is seen.
  function openTo(folder) {
    for (let at = folderOf(folder); at; at = folderOf(at.parent)) open.add(at.id);
    keepOpen();
  }

  function draw() {
    const folders = cfg.folders(), items = cfg.items();
    const kids = (parent) => [
      edit?.isNew && edit.parent === parent && h('li', { role: 'treeitem' }, editRow(edit.kind === 'folder' ? 'folder' : cfg.item.icon, '')),
      ...folders.filter((f) => (f.parent ?? null) === parent).map(folderNode),
      ...(cfg.item ? items.filter((x) => (x.folder ?? null) === parent).map(itemNode) : [])];
    const folderNode = (f) => {
      const isOpen = open.has(f.id), current = cfg.current?.() === f.id;
      return h('li', { role: 'treeitem', 'aria-expanded': String(isOpen), ...(cfg.pick ? { 'aria-selected': String(current) } : {}) },
        h('div', { class: 'tree-row folder' + (current ? ' current' : ''), ...drag({ folder: f.id }), ...drop(f.id, cfg.drop && ((b) => cfg.drop(f.id, b))) },
          editing('folder', f.id) ? editRow('folder', f.name) : [
            h('button', { type: 'button', class: 'tree-open', title: f.name, onclick() {
              // Picked, a folder opens too; picked already, it opens and closes.
              if (cfg.pick && !current) { open.add(f.id); cfg.pick(f.id); } else if (isOpen) open.delete(f.id); else open.add(f.id);
              keepOpen();
              draw();
            } },
              h('span', { class: 'tree-chevron' }, icon('forward')), icon('folder'), h('span', { class: 'tree-name' }, f.name),
              !cfg.item && h('span', { class: 'tree-count' }, items.filter((x) => x.folder === f.id).length || '')),
            treeMenu(tt('tree.folder', { name: f.name }), folderMenu(f))]),
        isOpen && h('ul', { role: 'group' }, kids(f.id)));
    };
    const itemNode = (x) => h('li', { role: 'treeitem', 'aria-selected': String(cfg.item.current(x)) },
      h('div', { class: 'tree-row' + (cfg.item.current(x) ? ' current' : ''), ...drag({ item: x.id }),
        ...drop(x.folder ?? null, cfg.item.drop && ((b) => cfg.item.drop(x, b))) },
        editing('item', x.id) ? editRow(cfg.item.icon, x.name) : [
          h('button', { type: 'button', class: 'tree-open', title: x.name, onclick: () => cfg.item.open(x) },
            icon(cfg.item.icon), h('span', { class: 'tree-name' }, x.name), h('span', { class: 'tree-count' }, cfg.item.count(x))),
          treeMenu(tt(`${cfg.texts}.itemMenu`, { name: x.name }), cfg.item.menu(x))]));
    const empty = !folders.length && !(cfg.item && items.length) && !edit && h('li', { class: 'muted small tree-empty' }, tt(cfg.empty));
    const top = (b) => cfg.drop?.(null, b);
    render(cfg.key, cfg.el, () => [h('ul', { class: 'tree', role: 'tree', 'aria-label': tt(cfg.label), ...drop(null, cfg.drop && top) },
      cfg.root ? h('li', { role: 'treeitem', 'aria-expanded': 'true', 'aria-selected': String(cfg.current() == null) },
        h('div', { class: 'tree-row' + (cfg.current() == null ? ' current' : ''), ...drop(null, cfg.drop && top) },
          h('button', { type: 'button', class: 'tree-open', title: tt(cfg.root.name), onclick: () => cfg.pick(null) },
            icon(cfg.root.icon), h('span', { class: 'tree-name' }, tt(cfg.root.name)), h('span', { class: 'tree-count' }, cfg.root.count() || ''))),
        h('ul', { role: 'group' }, kids(null), empty)) : [kids(null), empty])]);
    const input = cfg.el.querySelector('.tree-input');
    if (input && document.activeElement !== input) { input.focus(); input.select(); }
  }

  // A name typed in the tree itself: a new folder or item, or a new name.
  // Enter or leaving the field keeps it, Escape (or an empty name) drops it.
  let edit = null; // {kind: 'folder'|'item', id} or {kind, isNew: true, parent}
  const editing = (kind, id) => edit && !edit.isNew && edit.kind === kind && edit.id === id;
  // A folder's text or an item's, by key: tree.folderNew, playlists.itemNew.
  const textOf = (kind, what) => (kind === 'folder' ? `tree.folder${what}` : `${cfg.texts}.item${what}`);
  function editRow(iconName, value) {
    return h('div', { class: 'tree-row editing' },
      h('span', { class: 'tree-edit' }, icon(iconName),
        h('input', {
          class: 'tree-input', value, maxlength: 200,
          'aria-label': edit.isNew ? tt(textOf(edit.kind, 'NewName')) : tt('tree.newNameFor', { name: value }),
          placeholder: edit.isNew ? tt(textOf(edit.kind, 'New')) : '',
          onkeydown(e) {
            if (e.key === 'Enter') { e.preventDefault(); endEdit(this.value); }
            else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); endEdit(null); }
          },
          onblur() { endEdit(this.value); },
        })));
  }
  function startEdit(next) {
    if (next.isNew && next.parent != null) openTo(next.parent);
    edit = next;
    draw();
  }
  function endEdit(value) {
    const e = edit;
    if (!e || e.done) return;
    e.done = true;
    edit = null;
    // Leaving the field lets the tree render again (render waits on a focused input).
    document.activeElement?.blur();
    const name = value?.trim();
    const old = e.isNew ? null : (e.kind === 'folder' ? cfg.folders() : cfg.items()).find((x) => x.id === e.id)?.name;
    if (!name || name === old) { draw(); return; }
    const call = e.kind === 'folder'
      ? (e.isNew ? api('POST', cfg.path, { name, parent: e.parent ?? 0 }) : api('PATCH', `${cfg.path}/${e.id}`, { name }))
      : (e.isNew ? cfg.create(name, e.parent) : cfg.rename(e.id, name));
    act(() => call, e.isNew ? textOf(e.kind, 'Created') : 'tree.renamed').then((r) => {
      if (r && e.isNew && e.kind === 'item') cfg.created?.(r);
      return act(cfg.reload);
    }).finally(draw);
  }

  const drag = (what) => ({
    draggable: 'true',
    ondragstart(e) {
      e.stopPropagation();
      e.dataTransfer.setData(type, JSON.stringify(what));
      e.dataTransfer.effectAllowed = 'move';
    },
  });

  // Into `folder` (null: the top); with onItems, library items too.
  function drop(folder, onItems) {
    const takes = (e) => e.dataTransfer.types.includes(type) || (onItems && e.dataTransfer.types.includes(ITEM_TYPE));
    return {
      ondragover(e) {
        if (!takes(e)) return;
        e.preventDefault();
        e.stopPropagation();
        this.classList.add('drop');
      },
      ondragleave() { this.classList.remove('drop'); },
      ondrop(e) {
        if (!takes(e)) return;
        e.preventDefault();
        e.stopPropagation();
        this.classList.remove('drop');
        const tree = e.dataTransfer.getData(type);
        if (tree) move(JSON.parse(tree), folder);
        else onItems(JSON.parse(e.dataTransfer.getData(ITEM_TYPE)));
      },
    };
  }

  function move(what, folder) {
    if (what.folder === folder) return;
    if (folder != null) openTo(folder);
    act(() => (what.folder != null
      ? api('PATCH', `${cfg.path}/${what.folder}`, { parent: folder ?? 0 })
      : cfg.moveItem(what.item, folder)), 'common.moved').then(() => act(cfg.reload));
  }

  // "Move to…": a dialog with every folder as its path ("Music › Rock"),
  // filtered as the name is typed; Enter takes the first shown. A folder is not
  // offered itself or what lies within it; where it already is, is marked.
  function moveDialog(name, current, onMove, self = null) {
    const within = (f) => { for (let at = f; at; at = folderOf(at.parent)) if (at.id === self) return true; return false; };
    const path = (f) => pathOf(f).join(' › ');
    const targets = [{ id: null, path: tt('tree.topLevel') },
      ...cfg.folders().filter((f) => !within(f)).map((f) => ({ id: f.id, path: path(f) })).sort((a, b) => a.path.localeCompare(b.path))];
    const list = h('div', { class: 'move-list', role: 'listbox', 'aria-label': tt('common.folders') });
    const show = (q) => {
      const words = q.toLowerCase().split(/\s+/).filter(Boolean);
      const shown = targets.filter((t) => words.every((w) => t.path.toLowerCase().includes(w)));
      list.replaceChildren(...shown.map((t) => h('button', {
        type: 'button', role: 'option', class: t.id === current ? 'here' : '', disabled: t.id === current,
        title: t.id === current ? tt('tree.itIsHere') : t.path,
        onclick() { dialog.close(); onMove(t.id); },
      }, icon(t.id == null ? 'back' : 'folder'), h('span', {}, t.path))),
      ...(shown.length ? [] : [h('p', { class: 'muted small' }, tt('tree.noFolderOf'))]));
    };
    const dialog = h('dialog', { class: 'confirm-dialog move-dialog', 'aria-label': tt('tree.moveName', { name }), onclose() { this.remove(); } },
      h('div', { class: 'confirm-head' }, h('span', { class: 'confirm-icon move-icon' }, icon('folder')), h('h2', {}, `${tt('tree.move')} `, h('span', { class: 'move-name' }, name))),
      h('input', { type: 'search', class: 'move-filter', placeholder: tt('tree.findFolderPlaceholder'), 'aria-label': tt('tree.findFolder'), autocomplete: 'off',
        oninput() { show(this.value); },
        onkeydown(e) { if (e.key === 'Enter') { e.preventDefault(); list.querySelector('button:not(:disabled)')?.click(); } } }),
      list,
      h('div', { class: 'confirm-buttons' }, h('button', { type: 'button', onclick: () => dialog.close() }, tt('common.cancel'))));
    show('');
    document.body.append(dialog);
    dialog.showModal();
  }

  const newFolder = (parent = null) => startEdit({ kind: 'folder', isNew: true, parent });
  const newItem = (folder = null) => startEdit({ kind: 'item', isNew: true, parent: folder });
  const folderMenu = (f) => [
    cfg.create && h('button', { type: 'button', onclick: () => newItem(f.id) }, icon('plus'), tt(`${cfg.texts}.itemNewHere`)),
    h('button', { type: 'button', onclick: () => newFolder(f.id) }, icon('folder'), tt('tree.newFolderHere')),
    h('button', { type: 'button', onclick: () => startEdit({ kind: 'folder', id: f.id }) }, icon('edit'), tt('common.rename')),
    h('button', { type: 'button', class: 'danger', async onclick() {
      // Everything within it goes too: with items among it, its name is typed again.
      const tree = [f.id];
      for (let i = 0; i < tree.length; i++) tree.push(...cfg.folders().filter((x) => x.parent === tree[i]).map((x) => x.id));
      const folders = tree.length - 1, inside = cfg.items().filter((x) => tree.includes(x.folder ?? null));
      const n = (count, texts) => tt(`${texts}${count === 1 ? 'One' : 'Many'}`, { n: count });
      const text = inside.length
        ? tt(folders + inside.length === 1 ? 'tree.inItIs' : 'tree.inItAre',
          { what: [folders && n(folders, 'tree.folders'), n(inside.length, `${cfg.texts}.items`)].filter(Boolean).join(` ${tt('tree.and')} `) })
          + (cfg.contents ? `, ${tt('tree.inAll', { n: cfg.contents(inside) })}` : '') + `. ${tt('tree.thisCannotBe')}`
        : folders ? tt(`${cfg.texts}.${folders === 1 ? 'folderGoes' : 'foldersGo'}`, { folders: n(folders, 'tree.folders') }) : tt('tree.itIsEmpty');
      if (await confirmDelete({ title: tt('tree.deleteFolder'), what: f.name, text, typeName: inside.length > 0 })) {
        cfg.deleting?.(tree, inside);
        act(() => api('DELETE', `${cfg.path}/${f.id}`), 'tree.folderDeleted').then(() => act(cfg.reload));
      }
    } }, icon('trash'), tt('tree.deleteFolder')),
    h('button', { type: 'button', onclick: () => moveDialog(f.name, f.parent ?? null, (to) => move({ folder: f.id }, to), f.id) }, icon('folder'), tt('tree.moveTo'))];

  return { render: draw, startEdit, newFolder, newItem, moveDialog, moveItem: (id, folder) => move({ item: id }, folder), path: (id) => pathOf(folderOf(id)) };
}

function treeMenu(label, items) {
  return h('span', { class: 'tree-more' },
    h('button', { type: 'button', class: 'icon-only', title: tt('common.more'), 'aria-label': tt('tree.more', { name: label }), 'aria-haspopup': 'menu', onclick: (e) => e.currentTarget.nextElementSibling.togglePopover() }, icon('more')),
    h('div', { class: 'pick-popover', popover: 'auto', role: 'menu', ontoggle(e) { if (e.newState === 'open') placeUnderButton(this); },
      onclick(e) { if (e.target.closest('button')) this.hidePopover(); } }, items));
}

// Asks before deleting, in a dialog of the page's own; with typeName the
// name must be typed again before the button can be pressed. `action` names
// the button. Resolves true to delete.
function confirmDelete({ title, what, text, typeName = false, action = tt('tree.delete') }) {
  return new Promise((resolve) => {
    const ok = h('button', { type: 'submit', class: 'danger-solid', value: 'delete', disabled: typeName }, icon('trash'), action);
    const dialog = h('dialog', { class: 'confirm-dialog', 'aria-label': title,
      onclose() { resolve(this.returnValue === 'delete'); this.remove(); } },
      h('form', { method: 'dialog' },
        h('div', { class: 'confirm-head' }, h('span', { class: 'confirm-icon' }, icon('trash')), h('h2', {}, title)),
        h('p', {}, h('strong', {}, what), ': ', text),
        typeName && h('label', { class: 'confirm-type' }, tt('tree.typeNameTo'),
          h('input', { autocomplete: 'off', spellcheck: 'false', placeholder: what,
            oninput() { ok.disabled = this.value.trim() !== what.trim(); } })),
        h('div', { class: 'confirm-buttons' },
          h('button', { type: 'submit', value: 'cancel', formnovalidate: true }, tt('common.cancel')), ok)));
    document.body.append(dialog);
    dialog.showModal();
  });
}

// A tree page's tree slides away by its strip, as the library does. Kept per viewer.
function treeStrip(toggle, key, texts) {
  const show = (open) => {
    toggle.setAttribute('aria-expanded', String(open));
    toggle.title = tt(open ? `${texts}.hideTree` : `${texts}.showTree`);
    store.set(key, open ? 'open' : 'closed');
    toggle.closest('.page-app').classList.toggle('tree-closed', !open);
  };
  toggle.addEventListener('click', () => show(toggle.getAttribute('aria-expanded') !== 'true'));
  show(store.get(key, 'open') === 'open');
}

// On a small screen a tree page shows one part at a time, picked from the ☰
// menu of its bar (app.css: data-part). Answers how to show a part.
const smallScreen = matchMedia('(max-width: 960px)');
function pageParts(page, labelOf) {
  const menu = page.querySelector('.pl-small-bar > button'), popover = menu.nextElementSibling;
  const show = (part) => {
    page.dataset.part = part;
    page.querySelector('.pl-part-label').textContent = tt(labelOf(part));
    for (const b of popover.querySelectorAll('button')) b.setAttribute('aria-pressed', String(b.dataset.part === part));
  };
  menu.addEventListener('click', () => popover.togglePopover());
  popover.addEventListener('toggle', (e) => { if (e.newState === 'open') placeUnderButton(popover); });
  popover.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-part]');
    if (!b) return;
    popover.hidePopover();
    show(b.dataset.part);
  });
  show('tree');
  return show;
}

// --- the Playlists page: the tree of folders and playlists -------------------
// Library items dropped on a playlist are added to it.

const playlistTree = folderTree({
  key: 'playlistTree', store: 'vx.playlistFolders', el: $('#plTree'), label: 'playlists.playlistsAndFolders', path: 'playlist-folders',
  texts: 'playlists', empty: 'playlists.treeEmpty',
  folders: () => S.playlistFolders, items: () => S.playlists, reload: loadPlaylists,
  moveItem: (id, folder) => api('PATCH', `playlists/${id}`, { folder: folder ?? 0 }),
  create: (name, folder) => api('POST', 'playlists', { name, folder: folder ?? 0 }),
  rename: (id, name) => api('PATCH', `playlists/${id}`, { name }),
  created: (p) => { S.openPlaylist = p.id; },
  contents: (lists) => { const sum = lists.reduce((all, p) => all + p.count, 0); return tt(sum === 1 ? 'playlists.entry' : 'playlists.entries', { n: sum }); },
  deleting: (_, lists) => { if (lists.some((p) => p.id === S.openPlaylist)) S.openPlaylist = null; },
  item: {
    icon: 'playlists', current: (p) => p.id === S.openPlaylist, open: (p) => openPlaylist(p.id), count: (p) => p.count, menu: playlistMenu,
    drop: (p, items) => {
      // Entries of another playlist move here: added, then taken out of it.
      const from = rowDragging?.list.startsWith('playlist:') ? { id: Number(rowDragging.list.slice(9)), ids: rowDragging.carried } : null;
      if (from?.id === p.id) return;
      if (from) clearSelection();
      act(async () => {
        await dropOnPlaylist(p.id, items);
        if (from) for (const e of from.ids) await api('DELETE', `playlists/${from.id}/entries/${e}`);
      }, from && (from.ids.length > 1 ? tt('playlists.entriesMoved', { n: from.ids.length }) : 'common.moved')).then(() => act(loadPlaylists));
    },
  },
});
const renderPlaylistTree = () => playlistTree.render();

function openPlaylist(id) {
  S.openPlaylist = id;
  $('#plSearch').value = '';
  if (smallScreen.matches) showPlaylistPart('list');
  renderPlaylistTree();
  act(renderPlaylistEntries);
}

function playlistMenu(p) {
  return [
    h('button', { type: 'button', onclick: () => act(() => enqueuePlaylist(p.id, false)) }, icon('queue'), tt('playlists.addToZones')),
    h('button', { type: 'button', onclick: () => act(() => enqueuePlaylist(p.id, true)) }, icon('next'), tt('playlists.playNext')),
    h('button', { type: 'button', onclick: () => playlistTree.startEdit({ kind: 'item', id: p.id }) }, icon('edit'), tt('common.rename')),
    h('button', { type: 'button', class: 'danger', async onclick() {
      if (await confirmDelete({ title: tt('playlists.deletePlaylist'), what: p.name, text: tt(p.count === 1 ? 'playlists.itsEntryGoes' : 'playlists.itsEntriesGo', { n: p.count }), typeName: true })) act(() => api('DELETE', `playlists/${p.id}`), 'playlists.playlistDeleted').then(() => act(loadPlaylists));
    } }, icon('trash'), tt('playlists.deletePlaylist')),
    h('button', { type: 'button', onclick: () => playlistTree.moveDialog(p.name, p.folder ?? null, (to) => playlistTree.moveItem(p.id, to)) }, icon('folder'), tt('playlists.moveTo'))];
}

$('#plNew').addEventListener('click', () => playlistTree.newItem());
$('#plSearch').addEventListener('input', filterPlaylistEntries);
$('#plNewFolder').addEventListener('click', () => playlistTree.newFolder());
treeStrip($('#plTreeToggle'), 'vx.playlistTree', 'playlists');
// The tree, the open playlist or the library.
const PL_PARTS = { tree: 'common.playlists', list: 'playlists.playlist', library: 'common.library' };
const showPlaylistPart = pageParts($('.page[data-page="playlists"]'),
  (part) => (part === 'list' ? S.playlists.find((p) => p.id === S.openPlaylist)?.name ?? PL_PARTS.list : PL_PARTS[part]));

// A selected row's trash removes the whole selection of that playlist, as a
// queue's does: the rows go at once, the playlist is read again after.
function removeEntries(id, list, entries, entry) {
  const ids = SEL.list === list && SEL.ids.has(String(entry)) ? entries.filter((e) => SEL.ids.has(String(e.id))).map((e) => e.id) : [entry];
  for (const row of $('#entries').querySelectorAll('tr[data-id]')) if (ids.includes(Number(row.dataset.id))) row.hidden = true;
  clearSelection();
  act(async () => {
    for (const e of ids) await api('DELETE', `playlists/${id}/entries/${e}`);
  }, ids.length > 1 ? tt('playlists.entriesRemoved', { n: ids.length }) : 'playlists.removed').then(() => act(loadPlaylists));
}

// --- the open playlist: its entries as a zone's queue shows its items --------
// Dragged by their grip, entries move (PATCH {index}); library items dropped
// here are added where they are dropped.
async function renderPlaylistEntries() {
  const p = S.playlists.find((x) => x.id === S.openPlaylist);
  if (!p) {
    $('#plHead').replaceChildren(h('span', { class: 'queue-count' }, icon('playlists'), tt('playlists.noPlaylistOpen')));
    $('#entries').replaceChildren(h('p', { class: 'muted small pl-pick' }, tt(S.playlists.length ? 'playlists.pickPlaylistOn' : 'playlists.makePlaylistWith')));
    return;
  }
  const id = p.id;
  const entries = await api('GET', `playlists/${id}/entries`);
  if (S.openPlaylist !== id) return;
  const list = `playlist:${id}`;
  const reload = () => act(loadPlaylists);
  const move = (moves) => act(async () => {
    for (const m of moves) await api('PATCH', `playlists/${id}/entries/${m.id}`, { index: m.to });
  }, 'common.moved').then(reload);
  $('#plHead').replaceChildren(
    h('span', { class: 'queue-count pl-title' }, icon('playlists'), h('span', { class: 'pl-name' }, p.name), h('span', { class: 'muted pl-count' }, tt('playlists.tracks', { n: entries.length }))),
    h('span', { class: 'tools' },
      h('button', { class: 'icon-only', title: tt('playlists.playNextIn'), 'aria-label': tt('playlists.playNextIn'), disabled: !entries.length, onclick: () => act(() => enqueuePlaylist(id, true)) }, icon('next')),
      h('button', { class: 'icon-only', title: tt('playlists.addToZones'), 'aria-label': tt('playlists.addToZones'), disabled: !entries.length, onclick: () => act(() => enqueuePlaylist(id, false)) }, icon('queue'))));
  $('#entries').replaceChildren(h('div', { class: 'table-wrap plain queue-list', ...listDrop(list, move, (items, index) => act(() => dropOnPlaylist(id, items, index)).then(reload)) },
    h('table', { class: 'small' },
      h('tr', {}, h('th', {}, '#'), h('th', {}, tt('common.track')), h('th', { class: 'queue-actions' }, tt('playlists.duration'))),
      entries.map((en, i) => h('tr', { 'data-index': String(i), 'data-list': list, 'data-id': String(en.id), 'data-selected': rowSelected(list, en.id) },
        rowHandle(list, i + 1, (rowId) => entryBody(entries.find((e) => String(e.id) === rowId))),
        h('td', {}, h('div', { class: 'with-art' },
          infoArt(en.item, artSlot(en.item.artworkId)),
          h('div', { class: 'grow' }, queueLines(en.item)))),
        h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
          h('span', { class: 'num' }, fmtDur(en.item.duration)),
          h('span', { class: 'row-tools' },
            media(en, 'track') && likeButton('track', en.item),
            favoriteButton(media(en, 'group') ? `book|${en.item.source}` : likeKey('track', en.item),
              () => (media(en, 'group') ? addBookFavorite(en.item.source) : addFavorite(en.item)), Boolean(media(en, 'group'))),
            h('button', { class: 'danger', title: tt('playlists.removeFromPlaylistTitle'), 'aria-label': tt('playlists.removeFromPlaylist'), onclick: () => removeEntries(id, list, entries, en.id) }, icon('trash')))))))),
    !entries.length && h('p', { class: 'muted small' }, tt('playlists.emptyDragTracks')),
    h('p', { class: 'muted small pl-no-match', hidden: true }, tt('playlists.noTrackMatches')),
    h('p', { class: 'muted small drop-hint' }, tt('playlists.dropToAdd'))));
  filterPlaylistEntries();
}

// An entry as the bodies it is added by elsewhere: a book by the node it came
// from (none when it names none).
function entryBody(en) {
  const book = media(en, 'group');
  if (!book) return [{ tracks: [en.item] }];
  const at = book.source?.indexOf(':') ?? -1;
  return at < 0 ? [] : [{ node: { provider: book.source.slice(0, at), id: book.source.slice(at + 1) }, group: true }];
}

// Searched, only the entries whose title, artist or album hold every word show.
function filterPlaylistEntries() {
  const words = $('#plSearch').value.toLowerCase().split(/\s+/).filter(Boolean);
  const rows = [...$('#entries').querySelectorAll('tr[data-id]')];
  let shown = 0;
  for (const row of rows) {
    const hit = words.every((w) => row.querySelector('.with-art').textContent.toLowerCase().includes(w));
    row.classList.toggle('no-match', !hit);
    if (hit) shown++;
  }
  const count = $('#plHead .pl-count'), none = $('#entries .pl-no-match');
  if (count) count.textContent = words.length ? tt('playlists.tracksOf', { n: shown, total: rows.length }) : tt('playlists.tracks', { n: rows.length });
  if (none) none.hidden = !rows.length || shown > 0;
}

async function loadFavorites() {
  S.favorites = await api('GET', `favorites${scopeQuery()}`);
  favoritesChanged();
}

// --- the Radio stations page: the tree of folders, a folder's stations -------
// The tree's root is the top; the folder picked lists its stations beside it.
// Stations are dragged by their grip onto a folder to go in it (a selection
// all of it); added or imported, they go in the folder picked.

async function loadStations() {
  const [stations, folders] = await Promise.all([api('GET', 'stations'), api('GET', 'station-folders')]);
  S.stations = stations.map((e) => ({ id: e.station.providerItemId, name: e.station.name, folder: e.folder, station: e.station }));
  S.stationFolders = folders;
  if (!folders.some((f) => f.id === S.stationFolder)) S.stationFolder = null;
  stationTree.render();
  renderStations();
}

const stationTree = folderTree({
  key: 'stationTree', store: 'vx.stationFolders', el: $('#stTree'), label: 'stations.stationFolders', path: 'station-folders',
  texts: 'stations', empty: 'stations.treeEmpty',
  folders: () => S.stationFolders, items: () => S.stations, reload: loadStations,
  moveItem: (id, folder) => api('PATCH', `stations/${enc(id)}`, { folder: folder ?? 0 }),
  root: { name: 'stations.radioStations', icon: 'radio', count: () => S.stations.filter((s) => s.folder == null).length },
  current: () => S.stationFolder,
  pick(folder) {
    S.stationFolder = folder;
    $('#stSearch').value = '';
    if (smallScreen.matches) showStationPart('list');
    stationTree.render();
    renderStations();
  },
  drop: (folder, bodies) => moveStations(bodies.filter((b) => b.station?.providerId === 'webradio').map((b) => b.station.providerItemId), folder),
  deleting: (folders) => { if (folders.includes(S.stationFolder)) S.stationFolder = null; },
});

function moveStations(ids, folder) {
  if (!ids.length) return;
  clearSelection();
  act(async () => {
    for (const id of ids) await api('PATCH', `stations/${enc(id)}`, { folder: folder ?? 0 });
  }, ids.length > 1 ? tt('stations.stationsMoved', { n: ids.length }) : 'common.moved').then(() => act(loadStations));
}

const stationFolderName = () => (S.stationFolder == null ? tt('stations.radioStations') : stationTree.path(S.stationFolder).join(' › '));

// Searched, the stations of every folder whose name, genre or homepage holds every word.
function renderStations() {
  const folder = S.stationFolder;
  const query = $('#stSearch').value.trim();
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  const hit = (s) => { const text = [s.name, s.genre, s.homepage].filter(Boolean).join(' ').toLowerCase(); return words.every((w) => text.includes(w)); };
  const shown = S.stations.filter((s) => (query ? hit(s.station) : (s.folder ?? null) === folder)).map((s) => s.station);
  const name = query ? tt('stations.search', { query }) : stationFolderName();
  $('#stHead').replaceChildren(
    h('span', { class: 'queue-count pl-title' }, icon(query ? 'search' : folder == null ? 'radio' : 'folder'), h('span', { class: 'pl-name', title: name }, name),
      h('span', { class: 'muted' }, tt('stations.stationsCount', { n: shown.length }))),
    h('span', { class: 'tools' },
      h('button', { class: 'icon-only', title: tt('stations.importM3uOr'), 'aria-label': tt('stations.importM3uOr'), onclick: () => $('#importDialog').showModal() }, icon('playlists')),
      h('button', { class: 'icon-only', title: tt('stations.addStation'), 'aria-label': tt('stations.addStation'), onclick: () => $('#stationDialog').showModal() }, icon('plus'))));
  $('#stations').replaceChildren(shown.length
    ? h('div', { class: 'table-wrap plain station-list' }, stationTable(shown))
    : h('p', { class: 'muted small pl-pick' }, tt(query && S.stations.length ? 'stations.noStationMatches' : folder == null && !S.stations.length
      ? 'stations.noStationsYet'
      : 'stations.emptyDragStations')));
  if (smallScreen.matches && $('.page[data-page="stations"]').dataset.part === 'list') showStationPart('list');
}

$('#stNewFolder').addEventListener('click', () => stationTree.newFolder());
$('#stSearch').addEventListener('input', renderStations);
treeStrip($('#stTreeToggle'), 'vx.stationTree', 'stations');
const showStationPart = pageParts($('.page[data-page="stations"]'), (part) => (part === 'list' ? stationFolderName() : 'common.folders'));

for (const dialog of [$('#stationDialog'), $('#importDialog')]) {
  dialog.querySelector('[data-close]').addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => { dialog.querySelector('form').reset(); dialog.querySelector('.save-error').hidden = true; });
}
// A refusal is said inside the dialog (a toast is hidden behind the backdrop);
// the dialog stays open so the entry can be corrected.
async function submitStations(dialog, call, done) {
  const error = dialog.querySelector('.save-error');
  error.hidden = true;
  try {
    const r = await call();
    dialog.close();
    flash(typeof done === 'function' ? done(r) : done);
    act(loadStations);
  } catch (err) {
    error.textContent = err.data?.message ?? err.message;
    error.hidden = false;
  }
}
$('#stationForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const body = { folder: S.stationFolder ?? 0 };
  for (const [k, v] of new FormData(e.target)) if (String(v).trim()) body[k] = String(v).trim();
  submitStations($('#stationDialog'), () => api('POST', 'stations', body), 'stations.stationAdded');
});
$('#importForm').addEventListener('submit', (e) => {
  e.preventDefault();
  submitStations($('#importDialog'), () => api('POST', 'stations/import', { playlist: e.target.playlist.value, folder: S.stationFolder ?? 0 }),
    (r) => tt('stations.stationSImported', { n: r.length }));
});

// --- index ----------------------------------------------------------------------
// What the indexer did: its counters (from status and `indexer.progress`), what
// the local provider browses now, and the indexer's lines in the tailed server log
// (the API names no failed file).

const indexLog = { lines: [], failed: new Map() }; // failed: path -> reason
const INDEX_LOG_MAX = 500;

function renderIndexer() {
  const ix = S.status?.library?.indexer;
  if (!ix) { patch($('#indexer'), h('p', { class: 'muted' }, tt('index.noStatusYet'))); return; }
  const local = S.status.providers.find((p) => p.id === 'local');
  patch($('#indexer'),
    h('table', { class: 'small' },
      h('tr', {}, ['scanned', 'indexed', 'failed', 'queue', 'passes'].map((k) => h('th', {}, tt(`index.column.${k}`)))),
      h('tr', {},
        [ix.scanned, ix.indexed].map((v) => h('td', { class: 'num' }, v)),
        h('td', { class: 'num' + (ix.failed ? ' bad' : '') }, ix.failed),
        [ix.queueDepth, ix.passes].map((v) => h('td', { class: 'num' }, v)))),
    h('p', { class: 'small' },
      h('span', { class: ix.queueDepth ? 'warn' : 'good' }, tt(ix.queueDepth ? 'index.indexing' : ix.passes ? 'index.idle' : 'index.noPassYet')),
      ` · ${tt('index.countedSinceServer')} · ${tt('index.schema', { n: S.status.library.schemaVersion })}`),
    local && h('p', { class: 'small' }, `${tt('index.local')}: `, h('span', { class: local.state === 'ready' ? 'good' : 'warn' }, tv('state', local.state)), ' ', local.detail ?? ''));
}

// Counts a browse node's entries in pages of 100, up to 1000.
async function countLocal(node) {
  let count = 0;
  for (let offset = 0; offset < 1000; offset += 100) {
    const r = await api('GET', `providers/local/browse?node=${enc(node)}&offset=${offset}&limit=100`);
    const got = r.nodes.length + r.items.artists.length + r.items.albums.length;
    count += got;
    if (got < 100) return String(count);
  }
  return '1000+';
}

// Opens a node in the library beside the zones (the library slid out).
function openInLibrary(node, title, provider = 'local') {
  location.hash = '#/zones';
  showLibrary(true);
  mbGo({ provider, trail: [{ node, title }] });
}

async function loadFound() {
  const [artists, albums, folders] = await Promise.all([
    countLocal('artists'), countLocal('albums'), api('GET', 'providers/local/browse?node=folders&limit=100')]);
  const link = (node, title, text = title) => h('a', { href: '#', onclick(e) { e.preventDefault(); openInLibrary(node, title); } }, text);
  $('#indexFound').replaceChildren(
    h('p', {}, link('artists', tt('common.artists'), tt('index.artists', { n: artists })), ' · ', link('albums', tt('common.albums'), tt('index.albums', { n: albums })), ' · ',
      link('folders', tt('common.folders'), tt('index.folderSWith', { n: `${folders.nodes.length}${folders.nodes.length === 100 ? '+' : ''}` }))),
    h('div', { class: 'small' }, folders.nodes.map((n) => h('div', {}, link(n.nodeId, n.title)))));
}

let indexLogTimer;
function noteIndexLine(line) {
  const m = /LibraryIndexer: (.*)$/.exec(line);
  if (!m) return;
  const failed = /^not indexed (.+?): (.*)$/.exec(m[1]);
  if (failed) {
    indexLog.failed.delete(failed[1]);
    indexLog.failed.set(failed[1], failed[2]);
    if (indexLog.failed.size > INDEX_LOG_MAX) indexLog.failed.delete(indexLog.failed.keys().next().value);
  } else {
    indexLog.lines.push(line);
    if (indexLog.lines.length > INDEX_LOG_MAX) indexLog.lines.shift();
  }
  // A log burst (the tail starts 64 KiB back) renders once.
  indexLogTimer ??= setTimeout(() => { indexLogTimer = undefined; renderIndexLog(); }, 200);
}

function renderIndexLog() {
  $('#indexPasses').replaceChildren(...(indexLog.lines.length ? indexLog.lines.map((l) => h('div', {}, l))
    : [h('p', { class: 'muted' }, tt('index.noneServerTails'))]));
  $('#indexFailed').replaceChildren(...(indexLog.failed.size ? [h('table', {}, [...indexLog.failed].map(([path, reason]) =>
    h('tr', {}, h('td', {}, path), h('td', { class: 'bad' }, reason))))] : [h('p', { class: 'muted' }, tt('common.none'))]));
}

$('#reloadIndex').addEventListener('click', () => act(loadFound));

// --- library folders ------------------------------------------------------------
// Both lists: the config's (read-only here) and the API's. A password is only
// ever typed and sent; the server answers hasPassword, and the field is
// emptied as soon as the request is made.

async function loadFolders() {
  S.folders = await api('GET', 'library/folders');
  renderFolders();
}

function renderFolderProgress() {
  const ix = S.status?.library?.indexer;
  patch($('#folderProgress'), ix ? h('span', { class: ix.queueDepth ? 'warn' : 'muted' },
    tt('folders.indexerSinceServer',
      { state: tt(ix.queueDepth ? 'folders.indexing' : 'folders.idle'), scanned: ix.scanned, indexed: ix.indexed, failed: ix.failed, queue: ix.queueDepth, passes: ix.passes })) : '');
}

function renderFolders() {
  render('folders', $('#folders'), () => (S.folders.length ? S.folders.map(folderCard) : [h('p', { class: 'muted' }, tt('folders.noFolders'))]));
}

function folderCard(f) {
  const path = `library/folders/${enc(f.id)}`;
  const fixed = f.origin === 'config';
  const lp = f.lastPassResult;
  const tone = f.state === 'ready' ? 'good' : f.state === 'unreachable' || f.state === 'authFailed' ? 'bad' : 'muted';
  const change = (fn, done) => act(fn, done).then(() => act(loadFolders));
  return h('div', { class: 'card' },
    h('h3', {}, f.path,
      h('span', { class: 'pill' }, f.type),
      h('span', { class: 'pill ' + tone }, tv('state', f.state)),
      fixed ? h('span', { class: 'pill warn', title: tt('folders.fromConfigFile') }, 'config') : h('span', { class: 'pill' }, 'api'),
      f.mediaType && h('span', { class: 'pill', title: tt('folders.declaredEverythingIn') }, f.mediaType === 'audiobook' ? `📖 ${tt('folders.audiobooks')}` : tv('media', f.mediaType))),
    f.detail && h('div', { class: 'small ' + tone }, f.detail),
    h('div', { class: 'kv small' },
      kv('folders.id', f.id),
      kv('folders.files', lp ? [`${lp.files} · ${tt('folders.indexed', { n: lp.files - lp.unreadable })} · `,
        h('span', { class: lp.unreadable ? 'bad' : '' }, tt('folders.unreadable', { n: lp.unreadable }))] : tt('folders.noPassYet')),
      kv('folders.lastPass', [f.lastPass ? new Date(f.lastPass).toLocaleString() : '—',
        lp && h('span', { class: 'muted' }, ` · ${tt('folders.probedMovedFailed', lp)} · ${lp.seconds.toFixed(1)} s`)]),
      kv('folders.sinceServerStart', h('span', { class: 'muted' }, tt('folders.scannedIndexedFailed', f)),
        'folders.countedOverEvery'),
      f.type === 'smb' && kv('folders.share', `${f.host}${f.port && f.port !== 445 ? `:${f.port}` : ''} · ${f.share} · ${f.directory || '/'}`),
      f.type === 'smb' && kv('folders.login', `${f.user ? (f.domain ? `${f.domain}\\${f.user}` : f.user) : tt('folders.guest')} · ${tt(f.hasPassword ? 'folders.hasPassword' : 'folders.noPassword')}`)),
    h('div', { class: 'row' },
      h('label', { title: fixed ? tt('folders.configFoldersAre') : '' }, h('input', {
        type: 'checkbox', checked: f.enabled, disabled: fixed,
        onchange: (e) => change(() => api('PATCH', path, { enabled: e.target.checked })),
      }), ` ${tt('folders.enabled')}`),
      h('label', { title: tt(fixed ? 'folders.configFoldersAre' : 'folders.reclassifiesItsTracks') }, `${tt('folders.holds')} `, h('select', {
        value: f.mediaType ?? '', disabled: fixed,
        onchange: (e) => change(() => api('PATCH', path, { mediaType: e.target.value }), 'folders.declaredTypeChanged'),
      }, h('option', { value: '' }, tt('folders.tagsDecide')), h('option', { value: 'music' }, tt('folders.music')), h('option', { value: 'audiobook' }, tt('folders.audiobooks')))),
      h('button', { onclick: () => change(() => api('POST', `${path}/scan`), 'folders.passQueued') }, tt('folders.scanNow')),
      !fixed && h('button', {
        class: 'danger',
        onclick: () => confirm(tt('folders.removeFolderIts', { path: f.path })) && change(() => api('DELETE', path), 'folders.folderRemoved'),
      }, tt('folders.remove'))),
    !fixed && f.type === 'smb' && h('details', { 'data-key': `login:${f.id}` }, h('summary', {}, tt('folders.editLogin')),
      h('form', { class: 'row', autocomplete: 'off', onsubmit(e) {
        e.preventDefault();
        const F = e.target.elements, body = {};
        if (F.user.value !== (f.user ?? '')) body.user = F.user.value;
        if (F.domain.value !== (f.domain ?? '')) body.domain = F.domain.value;
        if (F.password.value) body.password = F.password.value;
        F.password.value = '';
        change(() => api('PATCH', path, body), 'folders.loginChanged');
      } },
        h('input', { name: 'user', placeholder: tt('folders.userEmptyGuest'), size: 12, value: f.user ?? '' }),
        h('input', { name: 'password', type: 'password', placeholder: tt(f.hasPassword ? 'folders.unchanged' : 'common.none'), size: 12, autocomplete: 'new-password' }),
        h('input', { name: 'domain', placeholder: tt('folders.domain'), size: 8, value: f.domain ?? '' }),
        h('button', {}, tt('common.set')))));
}

function onFolderChanged(data) {
  const i = S.folders.findIndex((f) => f.id === data.id);
  if (data.removed) { if (i >= 0) S.folders.splice(i, 1); } else if (i >= 0) S.folders[i] = data; else S.folders.push(data);
  renderFolders();
}

const folderForm = $('#folderForm');
const FF = folderForm.elements;
FF.type.addEventListener('change', () => { $('#smbFields').hidden = FF.type.value !== 'smb'; });
FF.share.addEventListener('change', () => { FF.path.value = ''; if (FF.share.value) act(() => listDirs('')); });

// The NAS and its login as typed; the password leaves the page only in a body.
function smbLogin() {
  const body = { host: FF.host.value.trim() };
  if (FF.port.value) body.port = Number(FF.port.value);
  for (const k of ['user', 'password', 'domain']) if (FF[k].value) body[k] = FF[k].value;
  return body;
}

$('#listShares').addEventListener('click', () => act(async () => {
  const shares = await api('POST', 'library/smb/shares', smbLogin());
  FF.share.replaceChildren(h('option', { value: '' }, `${tt('folders.sharePick')} (${shares.length})`), ...shares.map((s) => h('option', { value: s }, s)));
  FF.path.value = '';
  // The shares as links too: a filled select does not open by itself. Picking
  // one lists its directories.
  const pick = (s) => (e) => { e.preventDefault(); FF.share.value = s; FF.share.dispatchEvent(new Event('change')); };
  $('#smbDirs').replaceChildren(shares.length
    ? h('div', {}, h('div', { class: 'muted' }, `${tt('folders.shares')}:`), shares.map((s) => h('div', {}, '🗄 ', h('a', { href: '#', onclick: pick(s) }, s))))
    : h('p', { class: 'muted' }, tt('folders.thisLoginSees')));
  return shares;
}, (shares) => tt('folders.shareS', { n: shares.length })));

// The directories below `path` in the picked share; a click goes down, the
// trail goes up, and the path field follows, so what is shown is what is added.
async function listDirs(path) {
  const share = FF.share.value;
  if (!share) throw new Error(tt('folders.listSharesAnd'));
  const body = { ...smbLogin(), share };
  if (path) body.path = path;
  const dirs = await api('POST', 'library/smb/directories', body);
  FF.path.value = path;
  const parts = path ? path.split('/') : [];
  const go = (to) => (e) => { e.preventDefault(); act(() => listDirs(to)); };
  $('#smbDirs').replaceChildren(
    h('div', {}, h('a', { href: '#', onclick: go('') }, share), parts.map((p, i) => [' / ', h('a', { href: '#', onclick: go(parts.slice(0, i + 1).join('/')) }, p)])),
    dirs.length ? h('div', {}, dirs.map((d) => h('div', {}, '📁 ', h('a', { href: '#', onclick: go(path ? `${path}/${d}` : d) }, d))))
      : h('p', { class: 'muted' }, tt('folders.noDirectoriesBelow')));
}
$('#listDirs').addEventListener('click', () => act(() => listDirs(FF.path.value.trim().replace(/^\/+|\/+$/g, ''))));

folderForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const type = FF.type.value, path = FF.path.value.trim();
  const body = type === 'smb' ? { type, ...smbLogin(), share: FF.share.value } : { type };
  if (path) body.path = path;
  body.enabled = FF.enabled.checked;
  if (FF.mediaType.value) body.mediaType = FF.mediaType.value;
  FF.password.value = '';
  act(() => api('POST', 'library/folders', body), (f) => tt('folders.folderAdded', { id: f.id, state: tv('state', f.state) })).then((r) => {
    if (r) {
      folderForm.reset();
      $('#smbFields').hidden = true;
      FF.share.replaceChildren(h('option', { value: '' }, tt('folders.sharePick')));
      $('#smbDirs').replaceChildren();
    }
    return act(loadFolders);
  });
});
$('#reloadFolders').addEventListener('click', () => act(loadFolders));

// --- media browser --------------------------------------------------------------
// The sources (GET providers, each with its root) and one page at a time,
// drawn from the browse contract alone: what a node is, its header, sections,
// total, sorts, filter, letters and layout. A place is a provider, the trail
// of nodes into it and the page's options; back and forward walk the places.

const MB = { providers: [], places: [], at: -1, loads: 0 };
const MB_LIMIT = 50;
const PLAYLISTS = '#playlists', FAVORITES = '#favorites', LIKED = '#liked', HISTORY = '#history', PROPOSALS = '#proposals', SEARCH = '#search';

// A search is a place too, so back and forward walk through it. Changing what
// it searches for searches again.
function mbSearch() {
  const q = $('#mbSearchQ').value.trim();
  if (q) mbGo({ provider: SEARCH, trail: [], q, types: $('#mbSearchTypes').value, mediaType: $('#mbSearchMedia').value, source: $('#mbSearchSource').value });
}
$('#mbSearch').addEventListener('submit', (e) => { e.preventDefault(); mbSearch(); });
$('#mbSearchTypes').addEventListener('change', mbSearch);
$('#mbSearchMedia').addEventListener('change', mbSearch);
$('#mbSearchSource').addEventListener('change', mbSearch);

// A search over every source, its results drawn like a browsed page's. One
// source picked: only what it found (each source answers its own page).
async function mbSearchPage(place) {
  const q = new URLSearchParams({ q: place.q, types: place.types, limit: MB_LIMIT });
  if (place.mediaType) q.set('mediaType', place.mediaType);
  const r = await api('GET', `search?${q}`);
  const mine = (list = []) => (place.source ? list.filter((x) => x.providerId === place.source) : list);
  const [tracks, albums, artists, stations, playlists] = [r.items.tracks, r.items.albums, r.items.artists, r.items.stations, r.items.playlists].map(mine);
  const sourceName = place.source && (MB.providers.find((p) => p.id === place.source)?.displayName ?? place.source);
  const node = (kind, a, title, subtitle) => ({ kind, providerId: a.providerId, nodeId: a.providerItemId, title, subtitle, artworkId: a.artworkId });
  const section = (title, list, kids) => (list.length ? [h('h4', {}, `${tt(title)} (${list.length})`), kids] : []);
  const out = [
    h('div', { class: 'row small' }, [tt('library.searchFor', { query: r.query }), place.types !== 'all' && tv('types', place.types), place.mediaType && tv('media', place.mediaType), sourceName].filter(Boolean).join(' · '),
      r.failures.filter((f) => !place.source || f.provider === place.source).map((f) => h('span', { class: 'bad' }, ` · ${f.provider}: ${f.error} (${f.message})`))),
    ...section('common.artists', artists, h('div', {}, artists.map((a) => mbRow(node('artist', a, a.name), true)))),
    ...section('common.albums', albums, h('div', {}, albums.map((a) => mbRow(node('album', a, a.title, [a.artist, a.year].filter(Boolean).join(' · ')), true)))),
    ...section('library.tracks', tracks, mbTracks(tracks)),
    ...section('common.playlists', playlists, h('div', {}, playlists.map((p) => mbRow(p, true)))),
    ...section('library.stations', stations, stationTable(stations)),
  ];
  if (out.length === 1) out.push(h('p', { class: 'muted' }, tt('library.nothingFound')));
  return out;
}

async function loadSources() {
  MB.providers = await api('GET', 'providers');
  // The search's sources: those that can search, by their names.
  const pick = $('#mbSearchSource'), was = pick.value;
  pick.replaceChildren(h('option', { value: '' }, tt('library.allSources')),
    ...MB.providers.filter((p) => p.capabilities?.includes('search')).map((p) => h('option', { value: p.id }, sourceLabel(p.displayName ?? p.id))));
  pick.value = [...pick.options].some((o) => o.value === was) ? was : '';
  renderSources();
}

// The sources folded away, kept per viewer.
const closedSources = new Set((() => { try { return JSON.parse(store.get('vx.closedSources', '[]')); } catch { return []; } })());
function renderSources() {
  const here = MB.places[MB.at];
  const on = (provider, trail) => here?.provider === provider && json(here.trail.map((c) => c.node)) === json(trail.map((c) => c.node));
  const link = (text, provider, trail = []) => h('a', {
    href: '#', class: on(provider, trail) ? 'on' : '', onclick(e) { e.preventDefault(); mbGo({ provider, trail }); },
  }, text);
  // A source row spans the pane; one with roots folds them away by its
  // chevron or by a click on the source, which opens it too. Not ready: a warning sign.
  const keepClosed = () => store.set('vx.closedSources', JSON.stringify([...closedSources]));
  const source = (provider, name, iconName, roots = [], state = 'ready') => {
    const open = !closedSources.has(provider);
    return h('div', { class: 'source', 'aria-expanded': roots.length ? String(open) : null },
      h('div', { class: 'source-head' },
        roots.length ? h('button', {
          type: 'button', class: 'source-fold', 'aria-label': tt(open ? 'library.collapse' : 'library.expand', { name }),
          onclick() { if (open) closedSources.add(provider); else closedSources.delete(provider); keepClosed(); renderSources(); },
        }, h('span', { class: 'tree-chevron' }, icon('forward'))) : h('span', { class: 'source-fold' }),
        h('a', {
          href: '#', class: on(provider, []) ? 'on' : '',
          onclick(e) {
            e.preventDefault();
            if (roots.length) { if (open) closedSources.add(provider); else closedSources.delete(provider); keepClosed(); }
            mbGo({ provider, trail: [] });
          },
        }, icon(iconName), h('span', { class: 'source-name' }, name),
        state !== 'ready' && h('span', { class: 'source-state warn', title: tv('state', state), 'aria-label': tv('state', state) }, icon('alert')))),
      open && roots.length > 0 && h('div', { class: 'roots' }, roots));
  };
  $('#mbSources').replaceChildren(
    // A provider's name and its roots are its fixed labels, translated as the page's own.
    ...MB.providers.map((p) => source(p.id, sourceLabel(p.displayName ?? p.id), SOURCE_ICONS[p.id] ?? 'cloud',
      (p.root ?? []).map((n) => h('div', {}, link(sourceLabel(n.title), p.id, [{ node: n.nodeId, title: sourceLabel(n.title) }]))), p.state)),
    // Each playlist takes library items dropped on it, at its end.
    source(PLAYLISTS, tt('common.playlists'), 'playlists', S.playlists.map((p) => h('div', {
      class: 'playlist-drop', title: tt('library.dropTracksOr', { name: p.name }),
      ...dropTarget(null, (items) => act(() => dropOnPlaylist(p.id, items)).then(() => act(loadPlaylists))),
    }, link(p.name, PLAYLISTS, [{ node: String(p.id), title: p.name }])))),
    source(FAVORITES, tt('library.favourites'), 'star', listenerRoots(FAVORITES)),
    source(LIKED, tt('library.liked'), 'heart', listenerRoots(LIKED)),
    source(HISTORY, tt('library.playedLately'), 'clock', listenerRoots(HISTORY)),
    source(PROPOSALS, tt('common.proposals'), 'sparkle', listenerRoots(PROPOSALS)));

  // With listeners, a root each: all of them, the household and each one.
  function listenerRoots(provider) {
    if (!S.users.length) return [];
    return ['*', '', ...S.users.map((u) => u.name)].map((u) => {
      const title = listenerTitle(u);
      return h('div', {}, link(title, provider, [{ node: `@${u}`, title }]));
    });
  }
}

// A favourites, liked, played or proposals place under a listener's root
// ('@' and the name, '@*' all of them) shows strictly theirs; null at the
// source itself, which follows the header's listener.
const listenerTitle = (u) => (u === '*' ? tt('listeners.all') : u === '' ? tt('common.household') : u);
const mbListener = (place) => (place.trail[0]?.node?.startsWith('@') ? place.trail[0].node.slice(1) : null);
function mbScopeQuery(place, sep = '?') {
  const u = mbListener(place);
  if (u === null) return scopeQuery(sep);
  return `${sep}user=${enc(u)}${u && u !== '*' ? '&only=true' : ''}`;
}

function mbGo(place) {
  MB.places.length = MB.at + 1;
  MB.places.push(place);
  MB.at++;
  mbLoad();
}

function mbOpen(n) {
  const here = MB.places[MB.at];
  const trail = here?.provider === n.providerId ? here.trail : [];
  mbGo({ provider: n.providerId, trail: [...trail, { node: n.nodeId, title: n.title }] });
}

$('#mbBack').addEventListener('click', () => { if (MB.at > 0) { MB.at--; mbLoad(); } });
$('#mbForward').addEventListener('click', () => { if (MB.at < MB.places.length - 1) { MB.at++; mbLoad(); } });

async function mbLoad() {
  mbList?.dispose?.();
  mbList = null;
  mbLetterBubble.classList.remove('on');
  const place = MB.places[MB.at];
  $('#mbBack').disabled = MB.at <= 0;
  $('#mbForward').disabled = MB.at >= MB.places.length - 1;
  renderSources();
  if (place.provider === SEARCH) {
    $('#mbSearchQ').value = place.q;
    $('#mbSearchTypes').value = place.types;
    $('#mbSearchMedia').value = place.mediaType;
    $('#mbSearchSource').value = place.source ?? '';
  }
  // The items stay behind a spinner until the page is read; only the latest
  // place's answer is drawn, so a slow search cannot replace a newer page.
  const seq = ++MB.loads;
  $('#mbBusy').hidden = false;
  const show = (nodes) => { if (seq === MB.loads) $('#mbView').replaceChildren(...nodes); };
  try {
    show(await mbPlace(place));
  } catch (e) {
    show([mbCrumbs(place), h('p', { class: 'bad' }, e.message)]);
  } finally {
    if (seq === MB.loads) { $('#mbBusy').hidden = true; $('#mbView').scrollTop = 0; }
  }
}

async function mbPlace(place) {
  if (place.provider === SEARCH) return mbSearchPage(place);
  if (place.provider === PLAYLISTS) return mbPlaylists(place);
  if (place.provider === FAVORITES) return mbFavorites(place);
  if (place.provider === LIKED) return mbLiked(place);
  if (place.provider === HISTORY) return mbHistory(place);
  if (place.provider === PROPOSALS) return mbProposals(place);
  const provider = MB.providers.find((p) => p.id === place.provider);
  if (provider?.state === 'authRequired') {
    return [mbCrumbs(place), h('p', { class: 'warn' }, `${tt('library.authrequired')} — `, linkify(provider.detail))];
  }
  // The local library's lists are whole from their start (mbLocalList).
  if (place.provider === 'local') Object.assign(place, { offset: 0, letter: null });
  const seq = MB.loads;
  const r = await mbBrowse(place);
  if (seq !== MB.loads) return [];
  // A letter is where the page started: from here on it pages by offset.
  if (place.letter) { place.offset = r.letters?.find((l) => l.letter === place.letter)?.offset ?? 0; place.letter = null; }
  return mbPage(place, r);
}

function mbBrowse(place) {
  const q = new URLSearchParams();
  if (place.trail.length) q.set('node', place.trail.at(-1).node);
  for (const k of ['sort', 'filter', 'letter']) if (place[k]) q.set(k, place[k]);
  if (place.offset) q.set('offset', place.offset);
  q.set('limit', MB_LIMIT);
  return api('GET', `providers/${enc(place.provider)}/browse?${q}`);
}

// The local library has no pages. A list whose length is known stands whole
// from the start, what is not loaded yet a blank of its size, so the
// scrollbar does not move under the pointer; a stretch loads as it scrolls
// into view. Its letters scroll to where they start, and while it scrolls
// the letter at its top shows beside the scrollbar (mbList).
let mbList = null; // The current list, its letter positions and cleanup.
function mbLocalList(place, r) {
  const tracks = r.items.tracks.length > 0;
  const rows = !tracks && r.layout !== 'grid' && r.nodes[0]?.kind === 'folder';
  if (!tracks && !rows && r.total > 0) return mbVirtualCovers(place, r);
  const make = (page) => (tracks ? page.items.tracks.map((t) => mbTrackRow(t)) : page.nodes.map((n) => (rows ? mbRow(n) : mbCover(n))));
  const line = (cls) => h('div', { class: cls }, '\u00a0');
  const blank = (page) => {
    const el = tracks ? h('tr', { class: 'blank' }, h('td', { colspan: 4 }, h('div', { class: 'track-lines' }, line('line'), line('line'))))
      : rows ? h('div', { class: 'nodeRow blank' }, artSlot(null), '\u00a0')
        : h('div', { class: 'cover blank' }, h('div', { class: 'pic' }), line('small muted title'), r.nodes[0]?.subtitle && line('small muted title'));
    el.dataset.page = page;
    return el;
  };
  const slots = make(r);
  for (let at = slots.length; at < r.total; at++) slots.push(blank(at - (at % MB_LIMIT)));
  const list = tracks ? h('table', { class: 'small rows lib-tracks' }, slots) : h('div', { class: rows ? 'mb-rows' : 'covers' }, slots);
  const loading = new Set([0]), seq = MB.loads;
  const seen = new IntersectionObserver((es) => { for (const e of es) if (e.isIntersecting) load(Number(e.target.dataset.page)); }, { rootMargin: '800px 0px' });
  async function load(offset) {
    if (loading.has(offset)) return;
    loading.add(offset);
    let page;
    try {
      page = await mbBrowse({ ...place, offset });
    } catch (e) {
      loading.delete(offset);
      flash(e.message, true);
      return;
    }
    if (seq !== MB.loads) return;
    make(page).forEach((el, i) => {
      const at = offset + i;
      if (!slots[at]?.classList.contains('blank')) return;
      seen.unobserve(slots[at]);
      slots[at].replaceWith(el);
      slots[at] = el;
    });
  }
  for (const el of slots) if (el.classList.contains('blank')) seen.observe(el);
  mbList = { list, slots, letters: r.letters ?? [], dispose: () => seen.disconnect() };
  return mbList;
}

// Keep only nearby covers mounted. The full height and item positions are
// independent of loaded pages, including when jumping straight to a letter.
function mbVirtualCovers(place, r) {
  const view = $('#mbView'), list = h('div', { class: 'covers mb-virtual-covers' });
  const pages = new Map([[0, r.nodes]]), loading = new Set(), failed = new Set(), mounted = new Map();
  const seq = MB.loads;
  let frame = 0, columns = 0, stride = 0, width = 0, dragged = null, disposed = false;
  const schedule = () => { if (!disposed) frame ||= requestAnimationFrame(render); };
  const listTop = () => view.scrollTop + list.getBoundingClientRect().top - view.getBoundingClientRect().top - view.clientTop;
  const indexAtTop = () => Math.min(r.total - 1, Math.max(0, Math.floor((view.scrollTop - listTop()) / stride)) * columns);
  async function load(offset) {
    if (pages.has(offset) || loading.has(offset) || failed.has(offset)) return;
    loading.add(offset);
    try {
      const page = await mbBrowse({ ...place, offset });
      if (!disposed && seq === MB.loads) pages.set(offset, page.nodes);
    } catch (e) {
      if (!disposed && seq === MB.loads) { failed.add(offset); flash(e.message, true); }
    } finally {
      loading.delete(offset);
      schedule();
    }
  }
  function render() {
    frame = 0;
    if (disposed || !list.isConnected || !list.clientWidth) return;
    const style = getComputedStyle(list), gap = parseFloat(style.rowGap);
    const nextColumns = style.gridTemplateColumns.split(' ').length;
    const nextWidth = (list.clientWidth - gap * (nextColumns - 1)) / nextColumns;
    const top = listTop(), anchor = columns && indexAtTop(), inList = view.scrollTop > top;
    const resized = width !== nextWidth || columns !== nextColumns;
    width = nextWidth;
    columns = nextColumns;
    // One subtitle line is reserved even when a particular cover has none.
    stride = width + 24 + gap;
    list.style.height = `${Math.ceil(r.total / columns) * stride - gap}px`;
    if (resized && inList) view.scrollTop = top + Math.floor(anchor / columns) * stride;
    const first = Math.max(0, Math.floor((view.scrollTop - top) / stride) - 3) * columns;
    const end = Math.min(r.total, (Math.ceil((view.scrollTop - top + view.clientHeight) / stride) + 3) * columns);
    for (const [at, el] of mounted) {
      if ((at < first || at >= end) && !el.contains(document.activeElement) && el !== dragged) {
        el.remove();
        mounted.delete(at);
      }
    }
    for (let at = first; at < end; at++) {
      const offset = at - at % MB_LIMIT, node = pages.get(offset)?.[at - offset];
      let el = mounted.get(at);
      if (el && (el.classList.contains('blank') !== !node || el.classList.contains('load-failed') !== failed.has(offset))) {
        el.remove();
        mounted.delete(at);
        el = null;
      }
      if (!el) {
        el = node ? mbCover(node) : h('div', { class: `cover blank${failed.has(offset) ? ' load-failed' : ''}` },
          h('div', { class: 'pic' }, failed.has(offset) && h('button', {
            onclick() { failed.delete(offset); load(offset); schedule(); },
          }, tt('library.retryLoading'))));
        mounted.set(at, el);
        el.dataset.index = at;
        list.append(el);
      }
      if (!node) load(offset);
    }
    // Retain focused/dragged cards and DOM order for keyboard navigation.
    [...mounted].sort(([a], [b]) => a - b).forEach(([at, el], i) => {
      el.style.cssText = `top:${Math.floor(at / columns) * stride}px;left:${at % columns * (width + gap)}px;width:${width}px;height:${stride - gap}px`;
      if (list.children[i] !== el) list.insertBefore(el, list.children[i] ?? null);
    });
  }
  const resized = new ResizeObserver(schedule);
  resized.observe(view);
  view.addEventListener('scroll', schedule, { passive: true });
  list.addEventListener('focusout', schedule);
  list.addEventListener('dragstart', (e) => { dragged = e.target.closest('.cover'); });
  list.addEventListener('dragend', () => { dragged = null; schedule(); });
  schedule();
  mbList = {
    list, letters: r.letters ?? [], indexAtTop,
    scrollToIndex(at) {
      if (!stride) render();
      view.scrollTop = listTop() + Math.floor(at / columns) * stride;
      schedule();
    },
    dispose() {
      disposed = true;
      resized.disconnect();
      view.removeEventListener('scroll', schedule);
      cancelAnimationFrame(frame);
      mounted.clear();
    },
  };
  return mbList;
}

const mbLetterBubble = h('div', { class: 'mb-letter', 'aria-hidden': 'true' });
document.body.append(mbLetterBubble);
let mbLetterTimer, mbLetterFrame = 0;
$('#mbView').addEventListener('scroll', () => {
  mbLetterFrame ||= requestAnimationFrame(showScrollLetter);
}, { passive: true });
function showScrollLetter() {
  mbLetterFrame = 0;
  const l = mbList, view = $('#mbView');
  if (!l?.list.isConnected || !l.letters.length) return;
  const box = view.getBoundingClientRect();
  // The first entry not above the view's top: their bottoms only grow.
  let lo = l.indexAtTop?.() ?? 0;
  if (!l.indexAtTop) {
    let hi = l.slots.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (l.slots[mid].getBoundingClientRect().bottom <= box.top) lo = mid + 1; else hi = mid;
    }
  }
  const letter = l.letters.findLast((x) => x.offset <= lo)?.letter;
  if (!letter) return;
  // Beside the scrollbar's thumb.
  const share = view.scrollTop / Math.max(1, view.scrollHeight - view.clientHeight);
  mbLetterBubble.textContent = letter;
  mbLetterBubble.style.cssText = `top: ${box.top + 8 + share * Math.max(0, box.height - 72)}px; left: ${box.right - 76}px`;
  mbLetterBubble.classList.add('on');
  clearTimeout(mbLetterTimer);
  mbLetterTimer = setTimeout(() => mbLetterBubble.classList.remove('on'), 700);
}

// A list of unknown length grows instead, as its end is scrolled to.
const mbMoreAt = (place, r, offset, count) => {
  const next = offset + count;
  return (r.total != null ? next < r.total : count >= MB_LIMIT) ? mbMore(place, next) : null;
};
function mbMore(place, offset) {
  const more = h('div', { class: 'mb-more' }, h('span', { class: 'spinner', role: 'status', 'aria-label': tt('library.loadingMore') }));
  const seq = MB.loads;
  const seen = new IntersectionObserver(async ([e]) => {
    if (!e.isIntersecting) return;
    seen.disconnect();
    let r;
    try {
      r = await mbBrowse({ ...place, letter: null, offset });
    } catch (err) {
      more.replaceChildren(h('p', { class: 'bad' }, err.message));
      return;
    }
    if (seq !== MB.loads || !more.isConnected) return;
    const view = more.parentElement;
    for (const el of mbEntries(r.nodes, r.items, r.layout)) {
      const into = [...view.children].reverse().find((x) => x !== more && x.tagName === el.tagName && x.className === el.className);
      if (into) into.append(...el.childNodes); else more.before(el);
    }
    const next = mbMoreAt(place, r, offset, r.nodes.length + r.items.tracks.length + r.items.stations.length);
    if (next) more.replaceWith(next); else more.remove();
  }, { rootMargin: '400px' });
  seen.observe(more);
  return more;
}

function mbCrumbs(place) {
  const own = { [PLAYLISTS]: 'common.playlists', [FAVORITES]: 'library.favourites', [LIKED]: 'library.liked', [HISTORY]: 'library.playedLately', [PROPOSALS]: 'common.proposals', [SEARCH]: 'library.search' }[place.provider];
  const name = own ? tt(own) : sourceLabel(MB.providers.find((p) => p.id === place.provider)?.displayName ?? place.provider);
  const go = (i) => (e) => { e.preventDefault(); mbGo({ provider: place.provider, trail: place.trail.slice(0, i) }); };
  return h('div', { class: 'row small' },
    h('a', { href: '#', onclick: go(0) }, name),
    place.trail.map((c, i) => [' › ', h('a', { href: '#', onclick: go(i + 1) }, c.title)]),
    h('span', { class: 'muted' }, ` · ${tt('library.node')} ${place.trail.at(-1)?.node ?? tt('library.root')}`));
}

function mbPage(place, r) {
  const out = [mbCrumbs(place)];
  if (r.node) out.push(mbHeader(r.node));

  const count = r.nodes.length + r.items.tracks.length + r.items.stations.length;
  const offset = place.offset ?? 0;
  const tools = h('div', { class: 'row small mb-tools' });
  const local = place.provider === 'local';
  const pager = !r.sections && !local && mbPager(place, r, offset, count);
  if (pager) tools.append(pager);
  // The local library's lists have their letters; sort and filter are not offered there.
  if (r.sorts?.length && !local) {
    tools.append(h('label', {}, `${tt('library.sort')} `, h('select', {
      value: r.sort ?? '', onchange: (e) => mbGo({ ...place, sort: e.target.value, offset: 0 }),
    }, r.sorts.map((s) => h('option', { value: s }, s)))));
  }
  if (r.filterable && !local) {
    tools.append(h('form', { onsubmit(e) { e.preventDefault(); mbGo({ ...place, filter: e.target.elements.filter.value.trim() || null, offset: 0 }); } },
      h('input', { name: 'filter', placeholder: tt('library.filter'), size: 14, value: place.filter ?? '' }), h('button', {}, tt('library.filter'))));
  }
  out.push(tools);
  const whole = local && !r.sections && r.total != null ? mbLocalList(place, r) : null;
  if (!whole) mbList = null;
  if (r.letters?.length) {
    out.push(h('div', { class: 'letters small' }, r.letters.map((l) => h('button', {
      title: tt('library.from', { n: l.offset }),
      onclick() {
        if (!whole) mbGo({ ...place, letter: l.letter, offset: 0 });
        else if (whole.scrollToIndex) whole.scrollToIndex(l.offset);
        else whole.slots[l.offset]?.scrollIntoView({ block: 'start' });
      },
    }, l.letter))));
  }

  if (r.sections) {
    for (const s of r.sections) {
      out.push(h('h4', {}, s.title, s.total != null && h('span', { class: 'muted' }, ` (${s.total})`),
        s.more && [' ', h('a', { href: '#', onclick(e) { e.preventDefault(); mbOpen({ ...s.more, title: s.title }); } }, tt('library.more'))]));
      out.push(...mbEntries(s.nodes, s.items, s.layout));
    }
  } else if (whole) {
    out.push(whole.list);
  } else {
    out.push(...mbEntries(r.nodes, r.items, r.layout));
  }
  if (!count && !r.sections?.length) out.push(h('p', { class: 'muted' }, tt('library.empty')));
  // Once more under a long page, so the next one is not a scroll away.
  if (pager && count > 12) out.push(h('div', { class: 'row small mb-tools' }, mbPager(place, r, offset, count)));
  if (local && !r.sections && !whole) out.push(mbMoreAt(place, r, offset, count));
  return out;
}

// What a page shows of its list, and buttons to the pages before and after
// it: the first, the last and those next to this one, numbered; without a
// total only back and forward. None for a list on one page.
function mbPager(place, r, offset, count) {
  const last = r.total != null ? offset + count >= r.total : count < MB_LIMIT;
  if (offset === 0 && last) return count ? h('span', { class: 'muted' }, tt('library.entries', { n: r.total ?? count })) : null;
  const go = (to) => () => mbGo({ ...place, offset: to });
  const at = Math.floor(offset / MB_LIMIT);
  const pages = r.total != null ? Math.ceil(r.total / MB_LIMIT) : null;
  const shown = pages == null ? [] : [...new Set([0, at - 1, at, at + 1, pages - 1])].filter((i) => i >= 0 && i < pages).sort((a, b) => a - b);
  return h('nav', { class: 'mb-pager', 'aria-label': tt('library.pages') },
    h('span', { class: 'muted' }, `${offset + 1}–${offset + count}`, r.total != null ? ` ${tt('library.of', { n: r.total })}` : ''),
    h('button', { type: 'button', class: 'icon-only', disabled: offset === 0, title: tt('library.previousPage'), 'aria-label': tt('library.previousPage'),
      onclick: go(Math.max(0, offset - MB_LIMIT)) }, icon('back')),
    shown.map((i, k) => [k > 0 && i > shown[k - 1] + 1 && h('span', { class: 'muted' }, '…'),
      h('button', { type: 'button', class: 'mb-page', 'aria-current': i === at ? 'page' : null, disabled: i === at,
        'aria-label': tt('library.page', { n: i + 1 }), onclick: go(i * MB_LIMIT) }, i + 1)]),
    h('button', { type: 'button', class: 'icon-only', disabled: last, title: tt('library.nextPage'), 'aria-label': tt('library.nextPage'),
      onclick: go(offset + count) }, icon('forward')));
}

function mbHeader(n) {
  return h('div', { class: 'mbHead', ...dragNode(n) }, art(n.artworkId),
    h('div', {},
      h('h3', {}, n.title, ' ', h('span', { class: 'pill' }, tv('kind', n.kind))),
      n.subtitle && h('div', { class: 'muted' }, n.subtitle),
      h('div', { class: 'row' }, nodeActions(n))));
}

// Folders as rows; albums, artists and the other groups as square covers.
function mbEntries(nodes, items, layout) {
  const out = [];
  const rows = layout === 'grid' ? [] : nodes.filter((n) => n.kind === 'folder');
  const covers = nodes.filter((n) => !rows.includes(n));
  if (rows.length) out.push(h('div', { class: 'mb-rows' }, rows.map((n) => mbRow(n))));
  if (covers.length) out.push(h('div', { class: 'covers' }, covers.map(mbCover)));
  if (items.tracks.length) out.push(mbTracks(items.tracks));
  if (items.stations.length) out.push(stationTable(items.stations));
  return out;
}

// The grip on a library item that can be dragged, as a queue row has one.
const dragGrip = (tag = 'span') => h(tag, { class: 'drag-grip', title: tt('library.dragToQueue'), 'aria-hidden': 'true' }, icon('grip'));

// A playable node drags as a reference the server expands.
const dragNode = (n) => (n.playable ? dragItem({ node: { provider: n.providerId, id: n.nodeId } }) : {});

function mbCover(n) {
  return h('div', { class: 'cover', title: `${n.kind} ${n.nodeId}`, ...dragNode(n) }, n.playable && dragGrip(),
    // Its name on the picture's bottom; the link only for the keyboard, the picture opens it.
    h('div', { class: 'pic', onclick: () => mbOpen(n) }, icon('library'), art(n.artworkId),
      h('a', { class: 'cover-name', href: '#', onclick: (e) => e.preventDefault() }, n.title), coverActions(n)),
    n.subtitle && h('div', { class: 'small muted title' }, n.subtitle));
}

// What a cover offers under the pointer, slid up over its picture: play, next,
// last, like and favourite. Next and last go to the zone in view, or on the
// Playlists page to the open playlist (where play is not offered: app.css).
function coverActions(n) {
  const node = { provider: n.providerId, id: n.nodeId };
  return h('div', { class: 'cover-actions', onclick: (e) => e.stopPropagation() },
    n.playable && refButtons({ node }, 'cover-play'),
    nodeLike(n),
    n.playable && favoriteButton(`book|${n.providerId}:${n.nodeId}`, () => api('POST', 'favorites', { node, ...ownerOf() }), true));
}

// A node's like: an album, an artist, or (playable) a book.
function nodeLike(n) {
  const x = { providerId: n.providerId, providerItemId: n.nodeId };
  return n.kind === 'album' ? likeButton('album', { ...x, title: n.title, artist: n.subtitle ?? undefined })
    : n.kind === 'artist' ? likeButton('artist', { ...x, name: n.title })
      : n.playable && likeButton('audiobook', x, false, { node: { provider: n.providerId, id: n.nodeId } });
}

// Play, next and last for a library item (a node, or tracks). On the Zones
// page they go to the zone in view; on the Playlists page to the open
// playlist, where "next" is after its selected rows and play is not offered
// (app.css: .lib-play). Titles follow the page under the pointer.
function refButtons(ref, playClass) {
  const button = (name, texts, onclick, cls, titles = texts.map((t) => tt(t))) => h('button', {
    class: cls, title: titles[0], 'aria-label': titles[0], onclick,
    onmouseenter() { this.title = titles[page === 'playlists' ? 1 : 0]; this.setAttribute('aria-label', this.title); },
    onfocus() { this.onmouseenter(); },
  }, icon(name));
  return [
    button('play', ['library.playNow', 'library.playNow'], () => act(() => queueRef(ref, 'now')), `${playClass} lib-play`),
    button('next', ['library.playNext', 'library.addNextAfter'], () => act(() => addRef(ref, true))),
    button('queue', ['library.addToEnd', 'library.addToEndOf'], () => act(() => addRef(ref, false)))];
}

async function addRef(ref, next) {
  if (page !== 'playlists') return queueRef(ref, next ? 'next' : 'end');
  if (S.openPlaylist == null) throw new Error(tt('library.openPlaylistFirst'));
  await dropOnPlaylist(S.openPlaylist, [ref], next ? afterSelected(`playlist:${S.openPlaylist}`) : null);
  return act(loadPlaylists);
}

// Where "next" puts something in a list: after its last selected row, or at
// its start when none is selected.
function afterSelected(list) {
  if (SEL.list !== list || !SEL.ids.size) return 0;
  const rows = [...document.querySelectorAll(`tr[data-list="${list}"][data-id]`)];
  return Math.max(...rows.map((r, i) => (SEL.ids.has(r.dataset.id) ? i + 1 : 0)));
}

// `source`: where it comes from, as its source's icon (search results mix them).
function mbRow(n, source = false) {
  const name = source && (MB.providers.find((p) => p.id === n.providerId)?.displayName ?? n.providerId);
  return h('div', { class: 'nodeRow', ...dragNode(n) }, n.playable && dragGrip(), artSlot(n.artworkId),
    h('span', { class: 'grow' },
      h('a', { href: '#', onclick(e) { e.preventDefault(); mbOpen(n); } }, n.title),
      n.subtitle && h('span', { class: 'muted' }, ` · ${n.subtitle}`),
      source && h('span', { class: 'src-badge', title: name, 'aria-label': name, role: 'img' }, icon(SOURCE_ICONS[n.providerId] ?? 'cloud'))),
    h('span', { class: 'actions small' }, nodeActions(n)));
}

// A track as two lines: its title, and its artist, album and format; where
// it comes from is in the title.
function trackLines(t, mark = null) {
  return h('div', { class: 'track-lines', title: `${t.title} [${t.providerId} ${t.providerItemId}]` },
    h('div', { class: 'line' }, mark, typeMark(t), t.title),
    h('div', { class: 'muted line' }, [t.artist, t.album].filter(Boolean).join(' · ')));
}

// The providers' names and roots are fixed English labels, shown by their keys.
const SOURCE_LABELS = {
  'Local files': 'library.fixed.localFiles',
  'Web radio': 'library.fixed.webRadio',
  'Album artists': 'library.fixed.albumArtists',
  'Artists': 'library.fixed.artists',
  'Albums': 'library.fixed.albums',
  'Tracks': 'library.fixed.tracks',
  'Genres': 'library.fixed.genres',
  'Composers': 'library.fixed.composers',
  'Years': 'library.fixed.years',
  'Recently added': 'library.fixed.recentlyAdded',
  'All stations': 'library.fixed.allStations',
  'New releases': 'library.fixed.newReleases',
  'My collection': 'library.fixed.myCollection',
  'Mixes': 'library.fixed.mixes',
  'Moods': 'library.fixed.moods',
  'Playlists': 'library.fixed.playlists',
  'Favourites': 'library.fixed.favourites',
  'Stations': 'library.fixed.stations',
  'Folders': 'library.fixed.folders',
  'Audiobooks': 'library.fixed.audiobooks',
  'For you': 'library.fixed.forYou',
};
const sourceLabel = (text) => tt(SOURCE_LABELS[text] ?? text);

// An item's artwork that, held under the pointer for half a second, shows
// how it sounds (two lines) and where it comes from (its source's icon) in a
// popover beside it.
const SOURCE_ICONS = { local: 'nas', tidal: 'tidal', tunein: 'tower', webradio: 'globe' };
const INFO_DELAY_MS = 500;
let infoTimer = null;
function infoArt(x, slot) {
  return h('span', {
    class: 'info-art',
    onmouseenter(e) { const el = e.currentTarget; clearTimeout(infoTimer); infoTimer = setTimeout(() => showItemInfo(el, x), INFO_DELAY_MS); },
    onmouseleave: hideItemInfo,
    ondragstart: hideItemInfo,
  }, slot);
}
function hideItemInfo() {
  clearTimeout(infoTimer);
  document.querySelector('.info-popover')?.remove();
}
function showItemInfo(anchor, x) {
  hideItemInfo();
  if (!anchor.isConnected) return;
  const box = h('div', { class: 'info-popover', popover: 'manual', role: 'tooltip' }, itemInfo(x));
  document.body.append(box);
  box.showPopover();
  const a = anchor.getBoundingClientRect();
  const right = a.right + 10 + box.offsetWidth <= innerWidth - 8;
  box.style.left = `${right ? a.right + 10 : Math.max(8, a.left - 10 - box.offsetWidth)}px`;
  box.style.top = `${Math.max(8, Math.min(innerHeight - box.offsetHeight - 8, a.top + a.height / 2 - box.offsetHeight / 2))}px`;
}

// How an item sounds, on two lines, and its source's icon and name: in the
// popover and beside the zone's title.
function itemInfo(x) {
  const f = x.format;
  const [first, second] = x.kind === 'station' ? [tt('library.liveStream'), x.genre ?? '']
    : x.kind === 'group' ? [x.parts.length ? tt('common.parts', { n: x.parts.length }) : tt('library.book'), x.type ?? '']
      : f ? [[f.codec, f.sampleRate && `${f.sampleRate / 1000} kHz`, f.bitDepth && `${f.bitDepth} bit`].filter(Boolean).join(' · '),
        [f.channels && (f.channels === 1 ? tt('library.mono') : f.channels === 2 ? tt('library.stereo') : tt('library.channels', { n: f.channels })), f.bitrate && `${f.bitrate} kbps`].filter(Boolean).join(' · ')]
        : [tt('library.formatUnknown'), ''];
  const id = x.providerId ?? x.source?.split(':')[0];
  const name = MB.providers.find((p) => p.id === id)?.displayName ?? id ?? tt('library.unknownSource');
  return [
    h('div', { class: 'info-lines' }, h('div', {}, first), second && h('div', { class: 'muted' }, second)),
    h('div', { class: 'info-source', title: name }, icon(SOURCE_ICONS[id] ?? 'cloud'), h('span', {}, name))];
}

// Tracks as a zone's queue shows its rows: the length, and under the pointer
// play, next, last, like and favourite, going where addRef sends them.
// `radio`: a track found for a song liked off the radio -> that like, marked
// so and its heart the song's.
function mbTracks(tracks, radio = new Map()) {
  return h('table', { class: 'small rows lib-tracks' }, tracks.map((t) => mbTrackRow(t, radio)));
}
function mbTrackRow(t, radio = new Map()) {
  return h('tr', dragItem({ tracks: [t] }),
    dragGrip('td'),
    h('td', {}, infoArt(t, artSlot(t.artworkId))),
    h('td', { class: 'row-text' }, trackLines(t, radio.has(t) && radioMark(radio.get(t).found))),
    h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
      h('span', { class: 'num' }, fmtDur(t.duration)),
      h('span', { class: 'row-tools' }, refButtons({ tracks: [t] }, 'row-play'),
        likeButton('track', radio.get(t)?.item ?? t),
        favoriteButton(likeKey('track', t), () => addFavorite(t))))));
}

// A container is queued as one reference; one that cannot be (a list, opened) has no buttons.
// A node also goes in as single tracks (group: false), into the target
// playlist (a book as one entry), and as a favourite (a book only; the server
// refuses anything else by name).
function nodeActions(n) {
  if (!n.playable) return null;
  const node = { provider: n.providerId, id: n.nodeId };
  return [
    refActions({ node }),
    h('button', { title: tt('library.addAsSingle'), onclick: () => act(() => queueRef({ node, group: false }, 'end')) }, '+Q ⇉'),
    likeButton('audiobook', { providerId: n.providerId, providerItemId: n.nodeId }, false, { node }),
    favoriteButton(`book|${n.providerId}:${n.nodeId}`, () => api('POST', 'favorites', { node, ...ownerOf() }), true),
  ];
}

function refActions(ref, { shuffle = true } = {}) {
  const go = (how, name, text, title = tt(text)) => h('button', { class: 'icon-only', title, 'aria-label': title, onclick: () => act(() => queueRef(ref, how)) }, icon(name));
  return [
    go('now', 'play', 'library.playNowIn'),
    go('next', 'next', 'library.playNext'),
    go('end', 'queue', 'library.addToEndOfPlayer'),
    shuffle && go('shuffle', 'shuffle', 'library.shufflePlay'),
  ];
}

// Play now is `replace` then play; shuffle play is `replace`, the queue's
// shuffle, then play.
async function queueRef(ref, how) {
  const zone = needTarget();
  const body = how === 'end' ? ref : how === 'next' ? { ...ref, next: true } : { ...ref, replace: true };
  const r = await api('POST', `zones/${zone}/queue/items`, body);
  if (how === 'now' || how === 'shuffle') {
    await api('PATCH', `zones/${zone}/queue`, { shuffle: how === 'shuffle' });
    await api('POST', `zones/${zone}/play`);
  }
  return queuedFlash(r);
}

// What an add to a queue answered, flashed: how many, and what was refused.
function queuedFlash(r) {
  const refused = (r?.refused ?? []).map((x) => `${(x.track ?? x.entry?.item)?.title ?? '?'}: ${x.reason}`);
  flash([r?.queued != null ? tt('library.queued', { n: r.queued }) : tt('library.queuedDone'), r?.grouped && tt('library.asOneBook'), r?.truncated && tt('library.truncated'), refused.length && `${tt('library.refused')} ${refused.join(' | ')}`]
    .filter(Boolean).join(' · '), refused.length > 0);
  return r;
}

async function mbPlaylists(place) {
  if (place.trail.length) {
    const id = Number(place.trail.at(-1).node);
    const entries = await api('GET', `playlists/${id}/entries`);
    const tracks = entries.filter((e) => media(e, 'track')).map((e) => e.item);
    const books = entries.filter((e) => media(e, 'group'));
    return [mbCrumbs(place), h('div', { class: 'row' }, refActions({ playlist: id })),
      entries.length ? h('div', {},
        books.map((e) => h('div', { class: 'nodeRow' }, artSlot(e.item.artworkId), h('span', { class: 'grow' }, itemLabel(e.item)),
          h('span', { class: 'num small' }, fmtDur(e.item.duration)))),
        tracks.length && mbTracks(tracks)) : h('p', { class: 'muted' }, tt('library.empty'))];
  }
  const lists = await api('GET', 'playlists');
  // As the library's tracks are listed: its buttons slide out under the pointer.
  return [mbCrumbs(place), lists.length ? h('table', { class: 'small rows lib-tracks' }, lists.map((p) => {
    const ref = { playlist: p.id };
    // A click on the row opens its tracks; its buttons only do their own.
    return h('tr', { ...dragItem(ref), class: 'opens', onclick: () => mbGo({ provider: PLAYLISTS, trail: [{ node: String(p.id), title: p.name }] }) },
      dragGrip('td'),
      h('td', {}, artSlot(null, 'playlists')),
      h('td', { class: 'row-text' }, h('div', { class: 'track-lines', title: `${p.name} [#${p.id}]` },
        h('div', { class: 'line' }, h('a', { href: '#', onclick: (e) => e.preventDefault() }, p.name)),
        h('div', { class: 'muted line' }, tt('library.tracksCount', { n: p.count })))),
      h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
        h('span', { class: 'row-tools', onclick: (e) => e.stopPropagation() }, refButtons(ref, 'row-play'),
          h('button', { title: tt('library.shufflePlay'), 'aria-label': tt('library.shufflePlay'), onclick: () => act(() => queueRef(ref, 'shuffle')) }, icon('shuffle'))))));
  })) : h('p', { class: 'muted' }, tt('library.noPlaylists'))];
}

// Favourites as the library's tracks are listed. A track goes anywhere as
// itself; a book or a station as the favourite (a playlist takes neither).
async function mbFavorites(place) {
  const favorites = await api('GET', `favorites${mbScopeQuery(place)}`);
  return [mbCrumbs(place), favorites.length ? h('table', { class: 'small rows lib-tracks' }, favorites.map((f) => h('tr', dragItem({ favorite: f.id }),
    dragGrip('td'),
    h('td', {}, infoArt(f.item, artSlot(f.item.artworkId, media(f, 'station') ? 'radio' : 'library'))),
    h('td', { class: 'row-text' }, queueLines(f.item)),
    h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
      userPill(f.user),
      h('span', { class: 'num' }, fmtDur(f.item.duration)),
      h('span', { class: 'row-tools' }, refButtons(media(f, 'track') ? { tracks: [f.item] } : { favorite: f.id }, 'row-play'),
        media(f, 'track') && likeButton('track', f.item),
        favoriteButton(favoriteKey(f), () => (media(f, 'group') ? addBookFavorite(f.item.source)
          : addFavorite(media(f, 'track'), media(f, 'station'))), Boolean(media(f, 'group')))))))))
    : h('p', { class: 'muted' }, tt('library.noFavourites'))];
}

// The listener's likes, newest first, as the Liked page has them: tracks and
// stations as rows, albums and artists as nodes to open; books and songs
// heard on the radio as the Liked page draws them.
async function mbLiked(place) {
  const list = (mbListener(place) === null ? [...likedItems.values()] : await api('GET', `likes${mbScopeQuery(place)}`))
    .sort((a, b) => b.id - a.id);
  const kind = (k) => list.filter((l) => likedKind(l) === k).map((l) => l.item);
  // A radio song found as a track is listed as that track, its heart the song's.
  const radio = new Map(list.filter(foundTrack).map((l) => [l.found.track, l]));
  // Tracks, books and radio songs in one list, newest first.
  const rest = (l) => likedKind(l) === 'audiobook' || (radioSong(l) && !foundTrack(l));
  const rows = list.flatMap((l) => {
    if (rest(l)) return [likedRow(l)];
    const t = likedKind(l) === 'track' ? foundTrack(l) ?? l.item : null;
    if (!t || t.providerId === 'radio-metadata') return [];
    const row = mbTrackRow(t, radio);
    if (l.user) row.querySelector('.row-end').prepend(userPill(l.user));
    return [row];
  });
  const nodes = list.filter((l) => ['album', 'artist'].includes(likedKind(l))).map(({ item: x }) => ({
    kind: x.kind, providerId: x.providerId, nodeId: x.providerItemId, title: x.title ?? x.name,
    subtitle: x.artist, artworkId: x.artworkId, playable: x.kind === 'album',
  }));
  const section = (title, n, kids) => (n ? [h('h4', {}, `${tt(title)} (${n})`), kids] : []);
  const out = [mbCrumbs(place),
    ...(rows.length ? [h('table', { class: 'small rows lib-tracks' }, rows)] : []),
    ...section('library.albumsAndArtists', nodes.length, h('div', { class: 'covers' }, nodes.map(mbCover))),
    ...section('library.stations', kind('station').length, stationTable(kind('station')))];
  if (out.length === 1) out.push(h('p', { class: 'muted' }, tt('library.nothingLikedYet')));
  return out;
}

// What the house played lately (GET history), newest first.
async function mbHistory(place) {
  const plays = await api('GET', `history?limit=${MB_LIMIT}${mbScopeQuery(place, '&')}`);
  const rows = plays.items.length ? mbTracks(plays.items.map((p) => p.track)) : null;
  // Who listened, beside each play.
  if (rows) plays.items.forEach((p, i) => p.user && rows.querySelectorAll('tr')[i]?.querySelector('.row-end')?.prepend(userPill(p.user)));
  return [mbCrumbs(place), rows ?? h('p', { class: 'muted' }, tt('library.nothingPlayedYet'))];
}

// What the AI proposes for the listener (GET ai/proposals): first its kinds
// (songs, artists, albums and books when there are any), each opening its
// list. Tracks and books as rows with why, albums and artists as covers.
// Fresh picks asks for a new run; ✕ is not proposed again.
const PROPOSAL_KINDS = [['tracks', 'library.proposalSongs', 'playlists'], ['artists', 'common.artists', 'rooms'], ['albums', 'common.albums', 'library'], ['audiobooks', 'library.proposalBooks', 'library']]; // titles translated where shown
async function mbProposals(place) {
  const listener = mbListener(place);
  // All listeners: each one's kinds under their name, opening their list.
  if (listener === '*') {
    const scopes = ['', ...S.users.map((u) => u.name)];
    const views = await Promise.all(scopes.map((u) => api('GET', `ai/proposals?user=${enc(u)}`)));
    return [mbCrumbs(place), ...scopes.flatMap((u, i) => [h('h4', {}, `👤 ${listenerTitle(u)}`),
      proposalKinds(views[i], [{ node: `@${u}`, title: listenerTitle(u) }])])];
  }
  const v = await api('GET', `ai/proposals${listener !== null ? `?user=${enc(listener)}` : proposalsQuery()}`);
  if (listener === null) proposals = v;
  const base = listener === null ? [] : [place.trail[0]];
  const run = v.run, running = run?.state === 'running';
  const dismiss = (p) => h('button', {
    class: 'danger', title: tt('library.notProposedAgain'), 'aria-label': tt('library.dismiss'),
    onclick: () => act(() => api('DELETE', `ai/proposals/${p.id}`), 'library.dismissed').then(mbLoad),
  }, icon('close'));
  // With its reason as the second line and ✕ among its buttons.
  const why = (row, p) => {
    if (p.reason) row.querySelector('.track-lines').append(h('div', { class: 'muted line', title: p.reason }, p.reason));
    row.querySelector('.row-tools').append(dismiss(p));
    return row;
  };
  const book = (p) => {
    const x = p.item, b = bookRef(x.reference), ref = b && { node: { provider: b.providerId, id: b.providerItemId } };
    const grip = dragGrip('td');
    if (!ref) grip.style.visibility = 'hidden';
    return h('tr', ref ? dragItem(ref) : {}, grip,
      h('td', {}, artSlot(x.artworkId)),
      h('td', { class: 'row-text' }, h('div', { class: 'track-lines', title: `${x.title} [${x.reference}]` },
        h('div', { class: 'line' }, '📖 ', x.title),
        h('div', { class: 'muted line' }, x.author ?? ''))),
      h('td', { class: 'queue-actions' }, h('div', { class: 'row-end' },
        h('span', { class: 'row-tools' }, ref && refButtons(ref, 'row-play'), b && likeButton('audiobook', b)))));
  };
  const node = (kind) => ({ item: x }) => ({
    kind, providerId: x.providerId, nodeId: x.providerItemId, title: x.title ?? x.name,
    subtitle: x.artist, artworkId: x.artworkId, playable: kind === 'album',
  });
  const kind = place.trail[base.length]?.node;
  if (kind) {
    const list = v[kind] ?? [];
    const items = kind === 'tracks' ? [h('table', { class: 'small rows lib-tracks' }, list.map((p) => why(mbTrackRow(p.item), p)))]
      : kind === 'audiobooks' ? [h('table', { class: 'small rows lib-tracks' }, list.map((p) => why(book(p), p)))]
        : [h('div', { class: 'covers' }, list.map(node(kind === 'albums' ? 'album' : 'artist')).map(mbCover))];
    return [mbCrumbs(place), ...(list.length ? items : [h('p', { class: 'muted' }, tt('library.noneProposed'))])];
  }
  const owner = listener === null ? ownerOf() : listener ? { user: listener } : {};
  const out = [mbCrumbs(place),
    h('div', { class: 'row' },
      h('button', {
        type: 'button', class: 'primary', disabled: running || !v.aiAvailable, title: v.aiAvailable ? tt('library.askAiFor') : tt('library.noAi', { reason: v.reason }),
        onclick: () => act(() => api('POST', 'ai/proposals/refresh', owner), 'library.aiIsChoosing').then(mbLoad),
      }, icon('sparkle'), ` ${tt('library.freshPicks')}`),
      run?.error && h('span', { class: 'bad small' }, run.error)),
    ...(running ? [h('div', { class: 'proposals-running', role: 'status' }, h('span', { class: 'spinner', 'aria-hidden': 'true' }), tt('library.aiIsChoosingProposals'))] : []),
    proposalKinds(v, base)];
  if (!PROPOSAL_KINDS.some(([k]) => v[k].length) && !running) out.push(h('p', { class: 'muted' }, tt(v.aiAvailable ? 'library.noProposalsYet' : 'library.noProposalsConnect')));
  return out;
}

// A proposals view's kinds as rows that open their list (Books only when
// there are some), under `trail`: a listener's root, or none.
function proposalKinds(v, trail) {
  return h('table', { class: 'small rows lib-tracks' }, PROPOSAL_KINDS.filter(([k]) => k !== 'audiobooks' || v.audiobooks.length).map(([k, text, iconName]) => {
    const title = tt(text);
    const open = () => mbGo({ provider: PROPOSALS, trail: [...trail, { node: k, title }] });
    return h('tr', { class: 'opens', onclick: open },
      h('td', {}, artSlot(null, iconName)),
      h('td', { class: 'row-text' }, h('div', { class: 'track-lines' },
        h('div', { class: 'line' }, h('a', { href: '#', onclick: (e) => e.preventDefault() }, title)),
        h('div', { class: 'muted line' }, tt('library.proposed', { n: v[k].length })))));
  }));
}

// --- status ---------------------------------------------------------------------

function renderStatus() {
  const st = S.status;
  if (!st) return;
  renderAiStatus(); // its counts: status ai.proposals
  if (page === 'services') renderTidalState(st);
  if (page === 'ai') renderAi();
  if (page === 'settings') loadCores().catch(() => {}); // a server before core discovery answers 404
}

// --- system: the machines' figures (server phase 18) -----------------------------

// The client devices' machine models and network links, on the clients page.
function onSystem(view) {
  S.system = view;
  if (page === 'rooms') renderClients();
}

// Signal strength as people read it. -67 dBm is the usual floor for a link
// that streams without retries piling up.
// Lit arcs of the Wi-Fi icon, on wifiSignal's bands: good or better 3, fair 2, weak 1, poor 0.
const wifiBars = (dbm) => (dbm >= -67 ? 3 : dbm >= -75 ? 2 : dbm >= -82 ? 1 : 0);
const wifiSignal = (dbm) => `${dbm} dBm (${tt(dbm >= -55 ? 'system.excellent' : dbm >= -67 ? 'system.good' : dbm >= -75 ? 'system.fair' : dbm >= -82 ? 'system.weak' : 'system.poor')})`;

// --- discovered clients (core discovery.clients, client.attach/detach) ---------------

// Where a client stands, from the core's view: a room here, pointed nowhere,
// pointed at another core, or not saying (a client from before the field).
function attachment(c) {
  if (c.connected) return ['here', 'good', tt('clients.attachedHere')];
  if (c.server === '') return ['none', 'warn', tt('clients.unattached')];
  if (c.server == null) return ['unknown', 'muted', tt('clients.notSaidOlder')];
  if (c.attachedHere) return ['pending', 'warn', tt('clients.attachedHereNo')];
  return ['elsewhere', 'muted', tt('clients.attachedTo', { server: c.server })];
}

async function loadDiscovered() {
  try {
    discovered = await api('GET', 'clients/discovered', undefined, { quiet: true });
    discoveredError = '';
  } catch (e) {
    discovered = null;
    discoveredError = e.message;
  }
  renderClients();
}
// "seen" stays current while the page is open.
setInterval(() => { if (page === 'rooms' && !document.hidden) loadDiscovered(); }, 5000);
// The clients an attach or detach is under way for: their button spins until
// the core reports the client where it was pointed -- a session here after an
// attach, a beacon naming no core of ours after a detach -- or POINT_WAIT_MS
// passes. Between the two the client is "attached here, no session".
const pointing = new Set();
const POINT_WAIT_MS = 10000;
async function pointClient(client, call, done, attach) {
  pointing.add(client);
  renderClients();
  try {
    if (await act(call, done) === undefined) return; // refused: act said why
    const until = Date.now() + POINT_WAIT_MS;
    for (;;) {
      await loadDiscovered();
      const c = discovered?.clients.find((x) => x.client === client);
      const state = c ? attachment(c)[0] : 'gone';
      const settled = attach ? state === 'here' : state !== 'here' && state !== 'pending';
      if (settled || Date.now() > until) break;
      await new Promise((resolve) => { setTimeout(resolve, 500); });
    }
  } finally {
    pointing.delete(client);
    renderClients();
  }
}
function attachClient(client, state) {
  if (state === 'elsewhere' && !confirm(tt('clients.isAttachedTo', { name: client }))) return;
  pointClient(client, () => api('POST', `clients/${enc(client)}/attach`), (r) => tt('clients.pointedAt', { name: client, server: r.server }), true);
}
function detachClient(client) {
  pointClient(client, () => api('POST', `clients/${enc(client)}/detach`), tt('clients.detached', { name: client }), false);
}

// TIDAL sign-in by device code. The applications it signs in and plays through
// are the server's config file's (providers.tidal); nothing here sets them.
let tidalLoginTimer, tidalLoginPopup;
let tidalLoginGeneration = 0;
function showTidalLogin(login) {
  patch($('#tidalLoginLink'),
    h('a', { href: login.authorizeUrl, target: '_blank', rel: 'noreferrer' }, tt('services.openTidalSign')),
    ` ${tt('services.andApproveCode')} `, h('strong', {}, login.userCode), `. ${tt('services.waitingForApproval')}`);
}
async function pollTidalLogin(generation = tidalLoginGeneration, resuming = false) {
  try {
    const result = await api('GET', 'providers/tidal/login', undefined, { quiet: true });
    if (generation !== tidalLoginGeneration) return;
    renderTidalState({ providers: [{ id: 'tidal', ...result.status }] });
    if (result.login) {
      showTidalLogin(result.login);
      tidalLoginTimer = setTimeout(() => pollTidalLogin(generation), 1500);
    } else if (!resuming) {
      patch($('#tidalLoginLink'), result.status.state === 'ready' ? tt('services.accountLinked') : result.status.detail.split(';')[0]);
      if (result.status.state === 'ready') tidalLoginPopup?.close();
    }
  } catch (e) {
    if (generation !== tidalLoginGeneration) return;
    if (!resuming) {
      patch($('#tidalLoginLink'), tt('services.couldNotCheck'));
      tidalLoginTimer = setTimeout(() => pollTidalLogin(generation), 3000);
    }
  }
}
$('#tidalLogin').addEventListener('click', () => {
  if ($('#tidalLogin').disabled) return;
  const popup = window.open('', '_blank');
  if (popup) popup.opener = null;
  const generation = ++tidalLoginGeneration;
  clearTimeout(tidalLoginTimer);
  $('#tidalLogin').disabled = true;
  act(async () => {
    try {
      const login = await api('POST', 'providers/tidal/login');
      if (generation !== tidalLoginGeneration) { popup?.close(); return; }
      tidalLoginPopup = popup;
      showTidalLogin(login);
      if (popup) popup.location.href = login.authorizeUrl;
      pollTidalLogin(generation);
    } catch (e) { popup?.close(); throw e; }
    finally { $('#tidalLogin').disabled = false; }
  });
});

// Artwork (GET artwork): MusicBrainz and fanart.tv on or off, fanart.tv's
// key (never answered: hasKey, keyOrigin), and the add-in's progress.
function showArtwork(a) {
  const mb = $('#artMusicBrainz');
  const fanart = $('#artFanart');
  mb.disabled = fanart.disabled = !a;
  if (!a) { $('#artProgress').textContent = tt('services.notAnsweredBy'); $('#artStore').textContent = ''; return a; }
  mb.checked = a.musicBrainz.enabled;
  fanart.checked = a.fanartTv.enabled;
  $('#artMusicBrainzOrigin').textContent = tt('services.from', { origin: a.musicBrainz.origin });
  const f = a.fanartTv;
  $('#artFanartState').textContent = `${f.hasKey ? tt('services.keyFrom', { origin: f.keyOrigin }) : tt('services.noKey')} · ${tt(f.active ? 'services.asked' : 'services.notAsked')}`;
  $('#artFanartRemove').disabled = f.keyOrigin !== 'api';
  const p = a.progress;
  $('#artProgress').textContent = p ? tt('services.trackSLooked', p) : tt('services.unknown');
  // The artwork store (server phase 40): absent from a server before it.
  const s = a.store;
  $('#artStore').textContent = s ? tt('services.pictureSTo', { ...s, size: fmtBytes(s.bytes) }) : tt('services.unknown');
  return a;
}
async function loadArtwork() {
  try {
    return showArtwork(await api('GET', 'artwork', undefined, { quiet: true }));
  } catch {
    return showArtwork(null); // a server before the route answers 404
  }
}
// Many pictures may be found at once: the card is read again once they settle.
let artworkTimer;
function loadArtworkSoon() {
  clearTimeout(artworkTimer);
  artworkTimer = setTimeout(loadArtwork, 1000);
}
const artworkForm = $('#artworkForm');
artworkForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const key = artworkForm.elements.key.value;
  artworkForm.elements.key.value = '';
  act(async () => showArtwork(await api('PUT', 'artwork/fanarttv/key', { key })), () => 'fanart.tv key set');
});
$('#artFanartRemove').addEventListener('click', () => act(async () => showArtwork(await api('DELETE', 'artwork/fanarttv/key')),
  (a) => `${tt('services.fanartTvKeyRemoved')}; ${a.fanartTv.hasKey ? tt('services.nowS', { origin: a.fanartTv.keyOrigin }) : tt('services.noneLeft')}`));
$('#artMusicBrainz').addEventListener('change', (e) => act(async () =>
  showArtwork(await api('PATCH', 'artwork', { musicBrainz: e.target.checked })),
(a) => `MusicBrainz ${tt(a.musicBrainz.enabled ? 'services.on' : 'common.off')}`).then((a) => { if (!a) act(loadArtwork); e.target.blur(); }));
$('#artFanart').addEventListener('change', (e) => act(async () =>
  showArtwork(await api('PATCH', 'artwork', { fanartTv: e.target.checked })),
(a) => `fanart.tv ${tt(a.fanartTv.enabled ? 'services.on' : 'common.off')}`).then((a) => { if (!a) act(loadArtwork); e.target.blur(); }));

// --- settings: the server core and Tidal's account link ----------------------------

// The cores whose beacons the server hears, and which one it uses.
async function loadCores() {
  const r = await api('GET', 'cores', undefined, { quiet: true });
  $('#coreSelected').textContent = r.selected ? tt('settings.selected', { id: r.selected }) : tt('settings.configured', { address: r.configured });
  $('#useConfigured').disabled = r.selected == null;
  const state = (c) => c.coreId !== r.selected ? h('button', { onclick: () => selectCore(c.coreId) }, tt('settings.use'))
    : r.connected === `${c.host}:${c.controlPort}` ? h('span', { class: 'good' }, tt('settings.inUse')) : h('span', { class: 'warn' }, tt('settings.connecting'));
  patch($('#cores'),
    h('div', { class: 'small' }, `${tt('settings.connectedTo')} `, r.connected ?? h('span', { class: 'bad' }, tt('settings.nothing')),
      r.selected && !r.cores.some((c) => c.coreId === r.selected) && h('span', { class: 'warn' }, ` · ${tt('settings.isNotAnnouncing', { id: r.selected })}`)),
    r.cores.length
      ? h('table', { class: 'small' },
        h('tr', {}, h('th', {}, tt('settings.coreId')), h('th', {}, tt('settings.address')), h('th', {}, tt('settings.version')), h('th', {}, '')),
        r.cores.map((c) => h('tr', { class: c.coreId === r.selected ? 'current' : '' },
          h('td', {}, c.coreId), h('td', {}, `${c.host}:${c.controlPort}`), h('td', {}, c.version ?? '—'), h('td', {}, state(c)))))
      : h('p', { class: 'muted' }, tt('settings.noCoreBeacon')));
  return r;
}
function selectCore(coreId) {
  if (!confirm(coreId ? tt('settings.useCoreEvery', { id: coreId }) : tt('settings.goBackTo'))) return;
  act(() => api('PUT', 'core/selection', { coreId }), coreId ? tt('settings.coreSelected', { id: coreId }) : 'settings.configuredCoreAddress')
    .then(() => act(loadCores));
}
$('#reloadCores').addEventListener('click', () => act(loadCores));

// The core's audio section (GET core/audio). The keys the core names `live`
// are edited here; the rest size its memory or its resampler and are only
// read when it starts, so they are shown and not offered.
const CORE_AUDIO_HINTS = { // translated where shown
  startDelayMs: 'settings.coreHint.startDelayMs',
  minimumStartBufferMs: 'settings.coreHint.minimumStartBufferMs',
  clientReadyTimeoutMs: 'settings.coreHint.clientReadyTimeoutMs',
  seekFlushLeadMs: 'settings.coreHint.seekFlushLeadMs',
  seekFillLeadMs: 'settings.coreHint.seekFillLeadMs',
  memoryLimitMb: 'settings.coreHint.memoryLimitMb',
};
async function loadCoreAudio() {
  let r;
  try {
    r = await api('GET', 'core/audio', undefined, { quiet: true });
  } catch (e) {
    $('#coreAudioState').textContent = tt('settings.unavailable', { error: e.message });
    $('#coreAudioForm').replaceChildren();
    $('#coreAudioFixed').replaceChildren();
    return;
  }
  $('#coreAudioState').textContent = '';
  $('#coreAudioForm').replaceChildren(
    ...r.live.flatMap((key) => [
      h('label', { for: `coreAudio-${key}` }, key),
      // The range is shown, not enforced: the core checks it (server phase 35), and
      // an out-of-range value is sent so its refusal can be read.
      h('input', { id: `coreAudio-${key}`, name: key, type: 'number', step: 1, value: String(r.audio[key]), 'aria-describedby': CORE_AUDIO_HINTS[key] && `coreAudio-${key}-hint` }),
      CORE_AUDIO_HINTS[key] && h('p', { class: 'hint', id: `coreAudio-${key}-hint` }, tt(CORE_AUDIO_HINTS[key]),
        key === 'memoryLimitMb' && r.memoryLimit && ` — ${tt('settings.nowMbMb', { mb: r.memoryLimit.mb, used: r.memoryLimit.usedMb, min: r.memoryLimit.minMb, max: r.memoryLimit.maxMb ?? tt('settings.noUpperBound') })}`),
    ].filter(Boolean)),
    h('span', {}), h('div', { class: 'form-actions start' }, h('button', { class: 'primary' }, tt('settings.apply'))));
  $('#coreAudioFixed').replaceChildren(h('table', { class: 'small' },
    Object.entries(r.audio).filter(([key]) => !r.live.includes(key))
      .map(([key, value]) => h('tr', {}, h('td', {}, key), h('td', { class: 'mono' }, String(value))))));
  // A core before the resampler list offers only the configured one.
  const resamplers = r.resamplers ?? [r.audio.resampler].filter(Boolean);
  $('#resampler').replaceChildren(...resamplers.map((id) => h('option', { value: id }, id)));
  $('#resampler').value = r.audio.resampler ?? '';
  showResamplerHint();
}
// What the selected resampler means for drift correction (core: variableRatio).
const RESAMPLER_HINTS = { // translated where shown
  soxr: 'settings.resamplerHint.soxr',
  r8brain: 'settings.resamplerHint.r8brain',
};
function showResamplerHint() {
  const hint = tt(RESAMPLER_HINTS[$('#resampler').value] ?? '');
  $('#resamplerHint').textContent = hint;
  $('#resamplerHint').className = $('#resampler').value === 'r8brain' ? 'hint warn' : 'hint';
}
$('#resampler').addEventListener('change', showResamplerHint);
$('#coreAudioForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const body = {};
  for (const input of e.target.querySelectorAll('input')) body[input.name] = Number(input.value);
  act(() => api('PATCH', 'core/audio', body), (r) => tt(r.changed ? 'settings.coreAudioChanged' : 'settings.coreAudioUnchanged')).then(() => act(loadCoreAudio));
});
$('#useConfigured').addEventListener('click', () => selectCore(null));

// Keep the account status compact; the server's full detail remains available on hover.
function renderTidalState(st) {
  const p = st.providers.find((x) => x.id === 'tidal');
  const label = { ready: 'services.tidal.ready', authRequired: 'services.tidal.authRequired', unavailable: 'services.tidal.unavailable', disabled: 'services.tidal.disabled' };
  patch($('#tidalState'), p
    ? h('span', { class: p.state === 'ready' ? 'good' : 'warn', title: p.detail }, tt(label[p.state] ?? p.state))
    : h('span', { class: 'muted' }, tt('services.tidalIsNot')));
}

$('#setResampler').addEventListener('click', () => act(() => api('PUT', 'core/resampler', { resampler: $('#resampler').value }), (r) => `${tt('services.resampler', { name: r.resampler })}${r.changed ? ` ${tt('services.changed')}` : ''}`
  + (r.renegotiated?.length ? `; ${tt('services.reNegotiating', { rooms: r.renegotiated.join(', ') })}` : '')
  + (r.variableRatio ? '' : `; ${tt('services.noSoftwareDrift')}`)));

// --- AI: providers, keys, models and settings (server phase 21); browser login (phase 22)

const AI = {
  providers: [], settings: null,
  models: {},        // id -> [{id, name}] as the vendor lists them
  filter: {},        // id -> the model filter as typed
  refusal: {},       // id -> the last refusal of a key, model or login call, verbatim
  loggingIn: null,   // the provider whose login window is open
  waitUntil: {},     // id -> when to stop polling after "log in on this machine"
  api: null,         // the server's API base, from the proxy's /gui/config
};
const aiChannel = 'BroadcastChannel' in window ? new BroadcastChannel('vx.ai') : null;

async function loadAi() {
  [AI.providers, AI.settings] = await Promise.all([api('GET', 'ai/providers'), api('GET', 'ai/settings')]);
  renderAi();
}

// The GUI's own page, and the server's callback, as the vendor must be told them.
const guiRedirect = () => new URL('ai-login.html', location.href).href;
const serverRedirect = () => (AI.api ? `${AI.api}/api/v1/ai/login/callback` : 'the server\'s /api/v1/ai/login/callback');

function renderAi() {
  const usage = S.status?.ai;
  patch($('#aiUsage'), usage ? tt('ai.loginsInProgress', { n: usage.pendingLogins ?? '—', max: usage.maxPendingLogins ?? '—', models: usage.maxModels }) : '');
  render('ai', $('#aiProviders'), () => (AI.providers.length ? AI.providers.map(aiCard) : [h('p', { class: 'muted' }, tt('ai.noAiProviders'))]));
  if (!AI.settings) return;
  const form = $('#aiSettingsForm').elements;
  if (!$('#aiSettingsForm').contains(document.activeElement)) {
    patch(form.defaultProvider, h('option', { value: '' }, tt('ai.none')), AI.providers.map((p) => h('option', { value: p.id }, p.id)));
    form.defaultProvider.value = AI.settings.defaultProvider ?? '';
    form.defaultModel.value = AI.settings.defaultModel ?? '';
    form.reasoningAgent.value = AI.settings.reasoning?.agent ?? 'default';
    form.reasoningSuggestions.value = AI.settings.reasoning?.suggestions ?? 'default';
    form.webLookup.value = AI.settings.webLookup ?? 'off';
    const speech = AI.settings.speech ?? {};
    patch(form.speechProvider, h('option', { value: '' }, tt('ai.none')),
      AI.providers.filter((p) => p.capabilities?.includes('transcription') || p.id === speech.provider).map((p) => h('option', { value: p.id }, p.id)));
    form.speechRecognition.value = speech.recognition ?? 'browser';
    form.speechProvider.value = speech.provider ?? '';
    form.speechModel.value = speech.model ?? '';
    form.speechLanguage.value = speech.language ?? '';
    form.readReplies.checked = Boolean(speech.readReplies);
    for (const limit of ['timeoutSeconds', 'maxToolCalls', 'tokenBudget', 'maxConcurrent'])
      form[limit].value = AI.settings.agent?.[limit] ?? '';
  }
  patch($('#aiModelList'), (AI.models[form.defaultProvider.value] ?? []).map((m) => h('option', { value: m.id }, m.name ?? '')));
  patch($('#aiSpeechModels'), (AI.models[form.speechProvider.value] ?? []).map((m) => h('option', { value: m.id }, m.name ?? '')));
  renderProposals();
}

const tri = (v) => h('span', { class: v == null ? 'muted' : v ? 'good' : 'bad' }, v == null ? '—' : v ? '✓' : '✗');

// --- AI token usage per day, by what asked (GET ai/usage) ---------------------

// Labels by key, translated when the page starts (i18n.js before this script).
const USAGE_ORIGINS = [['proposals', 'usage.origin.proposals', '#2a7ab0'], ['autoFill', 'usage.origin.autoFill', '#1f9e89'],
  ['agent', 'usage.origin.agent', '#7b4fc9'], ['manual', 'usage.origin.manual', '#d0731c'], ['other', 'usage.origin.other', '#8a8f98']].map(([id, label, color]) => [id, tt(label), color]);
const USAGE_RANGES = [[7, 'usage.range7Days'], [30, 'usage.range30Days'], [90, 'usage.range90Days'], [0, 'usage.rangeAll']].map(([days, label]) => [days, tt(label)]);
const aiUsage = { days: 30, rows: [] };

// yyyy-MM-dd of `from` (today) plus `offset` days, in this browser's time zone.
function localDay(offset = 0, from = new Date()) {
  const d = new Date(from);
  d.setDate(d.getDate() + offset);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

async function loadAiUsage() {
  const query = aiUsage.days ? `?from=${localDay(1 - aiUsage.days)}` : '';
  aiUsage.rows = await api('GET', `ai/usage${query}`, undefined, { quiet: true });
  renderAiUsage();
}

let aiUsageTimer = 0; // the calls just made are read again once they settle
function aiUsageChanged() {
  if (page !== 'aiStatus') return;
  clearTimeout(aiUsageTimer);
  aiUsageTimer = setTimeout(() => act(loadAiUsage), 1500);
}

function renderAiUsage() {
  const fmt = (n) => Number(n).toLocaleString();
  const tokens = (r) => r.inputTokens + r.outputTokens;
  patch($('#aiUsageRange'), USAGE_RANGES.map(([days, label]) => h('button', {
    class: aiUsage.days === days ? 'on' : '', 'aria-pressed': String(aiUsage.days === days),
    onclick: () => { aiUsage.days = days; act(loadAiUsage); },
  }, label)));

  const rows = aiUsage.rows;
  const origins = USAGE_ORIGINS.filter(([id]) => id !== 'other' || rows.some((r) => r.origin === 'other'));
  // Every day of the range, the empty ones included.
  const byDay = new Map();
  const dayOf = (day) => byDay.get(day) ?? byDay.set(day, { day, calls: 0, failures: 0 }).get(day);
  const first = aiUsage.days ? localDay(1 - aiUsage.days) : rows[0]?.day;
  if (first) for (let d = first; d <= localDay(); d = localDay(1, `${d}T12:00`)) dayOf(d);
  for (const r of rows) {
    const day = dayOf(r.day);
    day[r.origin] = (day[r.origin] ?? 0) + tokens(r);
    day.calls += r.calls;
    day.failures += r.failures;
  }
  const days = [...byDay.values()].sort((a, b) => a.day.localeCompare(b.day));
  const total = (d) => origins.reduce((sum, [id]) => sum + (d[id] ?? 0), 0);
  const max = Math.max(1, ...days.map(total));

  patch($('#aiUsageChart'), days.length ? [
    h('div', { class: 'usage-bars', role: 'img', 'aria-label': tt('usage.tokensPerDay') }, days.map((d) => h('div', {
      class: 'usage-day',
      title: [d.day, ...origins.filter(([id]) => d[id]).map(([id, label]) => `${label}: ${fmt(d[id])}`), `${tt('usage.total')}: ${fmt(total(d))}`].join('\n'),
    }, origins.filter(([id]) => d[id]).map(([id, , color]) => h('span', { style: `height:${(d[id] / max) * 100}%;background:${color}` }))))),
    h('div', { class: 'usage-axis small muted' }, h('span', {}, days[0].day), h('span', {}, tt('usage.maxTokensDay', { n: fmt(max) })), h('span', {}, days.at(-1).day)),
    h('div', { class: 'usage-legend small' }, origins.map(([, label, color]) => h('span', {}, h('i', { style: `background:${color}` }), label))),
  ] : h('p', { class: 'muted' }, tt('usage.noAiCalls')));

  // Newest day first, days without calls left out, the range's totals last.
  const used = days.filter((d) => d.calls).reverse();
  const sum = { day: tt('usage.total'), calls: 0, failures: 0 };
  for (const d of used) for (const k of [...origins.map(([id]) => id), 'calls', 'failures']) sum[k] = (sum[k] ?? 0) + (d[k] ?? 0);
  const line = (d, tag = 'td') => h('tr', {}, h(tag, {}, d.day), origins.map(([id]) => h(tag, { class: 'num' }, d[id] ? fmt(d[id]) : '')),
    h(tag, { class: 'num' }, fmt(total(d))), h(tag, { class: 'num' }, fmt(d.calls)),
    h(tag, { class: d.failures ? 'num bad' : 'num' }, d.failures ? fmt(d.failures) : ''));
  patch($('#aiUsageTable'), used.length ? h('table', {},
    h('thead', {}, h('tr', {}, h('th', {}, tt('usage.day')), origins.map(([, label]) => h('th', { class: 'num' }, label)),
      h('th', { class: 'num' }, tt('usage.total')), h('th', { class: 'num' }, tt('usage.calls')), h('th', { class: 'num' }, tt('usage.failed')))),
    h('tbody', {}, used.map((d) => line(d))), h('tfoot', {}, line(sum, 'th'))) : '');

  const counts = ['calls', 'failures', 'inputTokens', 'outputTokens', 'reasoningTokens', 'webSearches'];
  const models = new Map();
  for (const r of rows) {
    const key = `${r.provider}\u0000${r.model}`;
    const m = models.get(key) ?? models.set(key, { provider: r.provider, model: r.model, ...Object.fromEntries(counts.map((k) => [k, 0])) }).get(key);
    for (const k of counts) m[k] += r[k];
  }
  const labels = ['usage.calls', 'usage.failed', 'usage.in', 'usage.out', 'usage.reasoning', 'usage.webSearches'];
  patch($('#aiUsageModels'), models.size ? h('table', {},
    h('thead', {}, h('tr', {}, h('th', {}, tt('usage.provider')), h('th', {}, tt('usage.model')), labels.map((label) => h('th', { class: 'num' }, tt(label))))),
    h('tbody', {}, [...models.values()].sort((a, b) => tokens(b) - tokens(a)).map((m) => h('tr', {},
      h('td', {}, m.provider), h('td', { class: 'mono' }, m.model), counts.map((k) => h('td', { class: 'num' }, fmt(m[k])))))))
    : h('p', { class: 'muted' }, tt('usage.nothingInThis')));
}

// --- proposals (server phase 38): the stored lists for the listener's scope ------

const PROPOSAL_FIELDS = ['intervalHours', 'debounceMinutes', 'maxRunsPerDay', 'tracks', 'albums', 'artists', 'audiobooks', 'keep'];
let proposals = null; // GET ai/proposals

async function loadProposals() {
  proposals = await api('GET', `ai/proposals${proposalsQuery()}`, undefined, { quiet: true });
  renderProposals();
  renderHome();
  renderAiStatus();
}

// Diagnostics › AI: the proposals' background runs, the last run and the
// counts since the server started; the lists are in the library.
function renderAiStatus() {
  $('#aiStatusScope').textContent = S.user || tt('common.household');
  if (!proposals) return;
  const v = proposals, run = v.run, stats = S.status?.ai?.proposals;
  const when = (t) => (t ? new Date(t).toLocaleString() : '—');
  patch($('#aiStatus'),
    run?.state === 'running' && h('div', { class: 'proposals-running', role: 'status' }, h('span', { class: 'spinner', 'aria-hidden': 'true' }),
      tt('proposals.aiIsCalculating', { id: run.id, trigger: run.trigger, when: when(run.started) })),
    h('div', { class: 'row small' },
      h('span', { class: 'pill ' + (v.background.enabled ? 'good' : 'muted') }, v.background.enabled ? tt('proposals.backgroundOnEvery', { n: v.background.intervalHours }) : tt('proposals.backgroundOff')),
      v.background.nextRunAt && h('span', { class: 'muted' }, tt('proposals.next', { when: when(v.background.nextRunAt) })),
      h('span', { class: 'pill ' + (v.aiAvailable ? 'good' : 'bad'), title: v.reason ?? '' }, v.aiAvailable ? tt('proposals.aiAvailable') : tt('proposals.noAi', { reason: v.reason }))),
    stats ? h('div', { class: 'small', title: 'GET status: ai.proposals' }, tt('proposals.runsFailedNot', stats))
      : h('p', { class: 'muted small' }, tt('proposals.noCountsYet')),
    run ? h('div', { class: 'small' },
      h('span', { class: 'pill ' + ({ ok: 'good', partial: 'warn', failed: 'bad', running: 'warn' }[run.state] ?? '') }, tv('state', run.state)),
      ` ${tt('proposals.run', { id: run.id })} · ${run.trigger} · ${tt('proposals.started', { when: when(run.started) })}${run.finished ? ` · ${tt('proposals.finished', { when: when(run.finished) })}` : ''} · ${tt('proposals.notFound', { n: run.notFound })}`,
      run.usage && ` · ${tt('proposals.inOutTokens', { in: run.usage.inputTokens, out: run.usage.outputTokens })}`,
      run.error && h('div', { class: 'bad' }, run.error))
      : h('p', { class: 'muted small' }, tt('proposals.noRunYet')));
}

function renderProposals() {
  const form = $('#proposalsForm').elements;
  const p = AI.settings?.proposals ?? {};
  if (AI.settings && !$('#proposalsForm').contains(document.activeElement)) {
    form.enabled.checked = Boolean(p.enabled);
    for (const k of PROPOSAL_FIELDS) form[k].value = p[k] ?? { intervalHours: 24, debounceMinutes: 10, maxRunsPerDay: 4, tracks: 25, albums: 10, artists: 10, audiobooks: 5, keep: 200 }[k];
    form.request.value = p.request ?? '';
  }
}

$('#proposalsForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const F = e.target.elements;
  const body = { enabled: F.enabled.checked, request: F.request.value };
  // Keep only to a server that has it (it says so in its settings).
  for (const k of PROPOSAL_FIELDS) if (k !== 'keep' || 'keep' in (AI.settings?.proposals ?? {})) body[k] = Number(F[k].value);
  act(() => api('PATCH', 'ai/settings', { proposals: body }), 'proposals.proposalsSaved').then(() => act(loadAi));
});

function aiCard(p) {
  const path = `ai/providers/${enc(p.id)}`;
  const fixed = p.origin === 'config';
  const oauth = p.authMethods.includes('oauth');
  const use = S.status?.ai?.providers?.find((u) => u.id === p.id);
  const change = (fn, done) => act(fn, done).then(() => act(loadAi));
  const filter = (AI.filter[p.id] ?? '').toLowerCase();
  const models = AI.models[p.id];
  // Folded by default; render() keeps an opened one open.
  return h('details', { class: 'card', 'data-key': `aiProvider:${p.id}` },
    h('summary', {}, p.displayName, h('span', { class: 'muted' }, p.id),
      p.authMethods.map((m) => h('span', { class: 'pill' }, m)),
      (p.capabilities ?? []).map((c) => h('span', { class: 'pill good', title: tt('ai.capabilityRequestMay') }, c)),
      fixed && h('span', { class: 'pill warn', title: tt('ai.keyIsFrom') }, 'config')),
    p.detail && h('div', { class: 'small ' + (p.authorised === false || p.reachable === false ? 'bad' : 'muted') }, linkify(p.detail)),
    h('div', { class: 'kv small' },
      kv('ai.enabled', h('label', {}, h('input', {
        type: 'checkbox', checked: p.enabled,
        onchange: (e) => {
          const off = new Set(AI.settings?.disabledProviders ?? []);
          if (e.target.checked) off.delete(p.id); else off.add(p.id);
          change(() => api('PATCH', 'ai/settings', { disabledProviders: [...off] }), tt(e.target.checked ? 'ai.providerEnabled' : 'ai.switchedOff', { id: p.id }));
        },
      }))),
      kv('ai.configured', tri(p.configured)),
      kv('ai.authorisedReachable', [tri(p.authorised), ' · ', tri(p.reachable)], 'ai.whatItsLast'),
      kv('ai.credential', [`hasKey ${p.hasKey} · origin ${p.origin}`, oauth && ` · hasToken ${p.hasToken}`,
        'hasClient' in p && ` · hasClient ${p.hasClient}`]),
      use && kv('ai.usage', `calls ${use.calls} · failures ${use.failures} · timeouts ${use.timeouts} · tokens in ${use.inputTokens} / out ${use.outputTokens} (reasoning ${use.reasoningTokens ?? '—'}) · web lookups ${use.webLookups ?? '—'} (searches ${use.webSearches ?? '—'}) · models cached ${use.cachedModels}`)),
    AI.refusal[p.id] && h('div', { class: 'small bad' }, AI.refusal[p.id]),
    p.authMethods.includes('apiKey') && h('form', { class: 'row', autocomplete: 'off', title: fixed ? tt('ai.keyIsFromConfig') : '', onsubmit(e) {
      e.preventDefault();
      const key = e.target.elements.key.value;
      e.target.elements.key.value = '';
      delete AI.models[p.id];
      aiCall(p.id, () => api('PUT', `${path}/key`, { key }), 'ai.keySet');
    } },
      h('input', { name: 'key', type: 'password', placeholder: tt(p.hasKey ? 'ai.replaceKey' : 'ai.apiKey'), size: 22, autocomplete: 'new-password', disabled: fixed }),
      h('button', { disabled: fixed }, tt('ai.setKey')),
      p.hasKey && h('button', { type: 'button', class: 'danger', disabled: fixed, onclick: () => { delete AI.models[p.id]; aiCall(p.id, () => api('DELETE', `${path}/key`), 'ai.keyRemoved'); } }, tt('ai.removeKey'))),
    oauth && h('div', { class: 'row' },
      h('button', { onclick: () => aiLogin(p.id, false), title: tt('ai.returnsTo', { uri: guiRedirect() }) }, tt(p.hasToken || p.hasKey ? 'ai.logInAgain' : 'ai.logIn')),
      h('button', { onclick: () => aiLogin(p.id, true), title: tt('ai.returnsTo', { uri: serverRedirect() }) }, tt('ai.logInOn')),
      p.hasToken && h('button', { class: 'danger', onclick: () => aiCall(p.id, () => api('DELETE', `${path}/key`), 'ai.loggedOut') }, tt('ai.logOut')),
      AI.waitUntil[p.id] && h('span', { class: 'muted small' }, tt('ai.waitingForLogin'))),
    'hasClient' in p && h('details', { 'data-key': `aiClient:${p.id}`, open: !p.hasClient },
      h('summary', {}, `OAuth client (${tt(p.hasClient ? 'common.set' : 'ai.neededToLog')})`),
      h('form', { class: 'row', autocomplete: 'off', onsubmit(e) {
        e.preventDefault();
        const F = e.target.elements, body = { clientId: F.clientId.value.trim() };
        if (F.clientSecret.value) body.clientSecret = F.clientSecret.value;
        F.clientSecret.value = '';
        aiCall(p.id, () => api('PUT', `${path}/client`, body), 'ai.oauthClientSet').then((r) => { if (r) F.clientId.value = ''; });
      } },
        h('input', { name: 'clientId', placeholder: tt('ai.clientId'), size: 22 }),
        h('input', { name: 'clientSecret', type: 'password', placeholder: tt('ai.clientSecret'), size: 14, autocomplete: 'new-password' }),
        h('button', {}, tt('common.set')),
        p.hasClient && h('button', { type: 'button', class: 'danger', onclick: () => aiCall(p.id, () => api('DELETE', `${path}/client`), 'ai.oauthClientRemoved') }, tt('ai.remove'))),
      h('div', { class: 'small' }, tt('ai.redirectUrisTo'),
        [guiRedirect(), serverRedirect()].map((uri) => h('div', {}, h('code', {}, uri), ' ',
          h('button', { type: 'button', onclick: () => navigator.clipboard?.writeText(uri).then(() => flash('ai.copied')) }, tt('ai.copy')))))),
    h('div', { class: 'row' },
      h('button', { onclick: () => aiModels(p.id, false) }, tt('ai.listModels')),
      h('button', { onclick: () => aiModels(p.id, true) }, tt('ai.refresh')),
      models && h('input', { placeholder: tt('ai.filterModels', { n: models.length }), size: 18, value: AI.filter[p.id] ?? '', oninput(e) {
        AI.filter[p.id] = e.target.value;
        const f = e.target.value.toLowerCase();
        for (const tr of e.target.closest('.card').querySelectorAll('tr[data-model]')) tr.hidden = !tr.dataset.model.includes(f);
      } })),
    models && h('div', { class: 'json' }, h('table', { class: 'small' },
      h('tr', {}, h('th', {}, 'id'), h('th', {}, 'name')),
      models.map((m) => {
        const text = `${m.id} ${m.name ?? ''}`.toLowerCase();
        return h('tr', { 'data-model': text, hidden: Boolean(filter) && !text.includes(filter) }, h('td', { class: 'mono' }, m.id), h('td', {}, m.name ?? ''));
      }))));
}

// A call on one provider: its refusal stays on the card verbatim, and the
// list is read again, since the server's view of the vendor may have changed.
async function aiCall(id, fn, done) {
  try {
    const answer = await fn();
    delete AI.refusal[id];
    flash(done);
    return answer;
  } catch (e) {
    AI.refusal[id] = `${e.data?.error ?? e.status}: ${e.data?.message ?? e.message}`;
    flash(e.message, true);
    return undefined;
  } finally {
    await act(loadAi);
  }
}

function aiModels(id, refresh) {
  return aiCall(id, async () => {
    AI.models[id] = await api('GET', `ai/providers/${enc(id)}/models${refresh ? '?refresh=true' : ''}`);
    return AI.models[id];
  }, tt('ai.modelsOfListed', { id }));
}

// The page is the app the server's phase 22 describes: it starts the login and
// opens the vendor's page. The window is opened at once, while the click still
// counts as one, and sent to the vendor once the server has answered.
async function aiLogin(id, here) {
  const win = window.open('about:blank', 'vx-ai-login');
  AI.loggingIn = id;
  const answer = await aiCall(id, () => api('POST', `ai/providers/${enc(id)}/login`, here ? undefined : { redirectUri: guiRedirect() }),
    tt(here ? 'ai.loginStartedFinish' : 'ai.loginStarted'));
  if (!answer) { win?.close(); return; }
  if (win) win.location.href = answer.authorizeUrl;
  else flash('ai.browserBlockedLogin', true);
  if (here) {
    // The server's callback answers the browser, not this page: watch the list.
    const before = AI.providers.find((p) => p.id === id);
    AI.waitUntil[id] = Date.parse(answer.expiresAt);
    const poll = async () => {
      await act(loadAi);
      const now = AI.providers.find((p) => p.id === id);
      const changed = now && before && (now.hasToken !== before.hasToken || now.hasKey !== before.hasKey);
      if (changed || Date.now() > AI.waitUntil[id]) { delete AI.waitUntil[id]; renderAi(); return; }
      setTimeout(poll, 2000);
    };
    setTimeout(poll, 2000);
  }
}

// ai-login.html says how the login it finished went.
if (aiChannel) {
  aiChannel.onmessage = (e) => {
    const id = e.data?.provider ?? AI.loggingIn;
    if (id) {
      if (e.data?.error) AI.refusal[id] = `login: ${e.data.error}: ${e.data.message ?? ''}`; else delete AI.refusal[id];
    }
    flash(e.data?.error ? tt('ai.loginRefused', { error: e.data.error }) : tt('ai.loggedIn', { id }), Boolean(e.data?.error));
    act(loadAi);
  };
}

$('#aiSettingsForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const F = e.target.elements;
  act(() => api('PATCH', 'ai/settings', {
    defaultProvider: F.defaultProvider.value, defaultModel: F.defaultModel.value.trim(),
    reasoning: { agent: F.reasoningAgent.value, suggestions: F.reasoningSuggestions.value },
    webLookup: F.webLookup.value,
    speech: { recognition: F.speechRecognition.value, provider: F.speechProvider.value, model: F.speechModel.value.trim(),
      language: F.speechLanguage.value.trim(), readReplies: F.readReplies.checked },
    // Numbers as typed; the server refuses one out of range by name.
    agent: Object.fromEntries(['timeoutSeconds', 'maxToolCalls', 'tokenBudget', 'maxConcurrent']
      .filter((k) => F[k].value !== '').map((k) => [k, Number(F[k].value)])),
  }), 'ai.aiSettingsSaved')
    .then(() => { document.activeElement?.blur(); return act(loadAi); });
});
$('#aiSettingsForm').elements.defaultProvider.addEventListener('change', () => renderAi());
$('#aiSettingsForm').elements.speechProvider.addEventListener('change', () => renderAi());

// --- assistant: a chat with the agent (server phase 25), kept as ai/chats ----------

// The chat shown (id null: a new one, made by its first message) and, while
// a command runs, its turn: the agent's tool calls arrive on the event stream.
const CHAT = { list: [], id: store.get('vx.chat', '') || null, title: '', turns: [], pending: null, filter: '', loaded: false, choice: '', reasoning: '' };

async function loadChats() {
  CHAT.list = await api('GET', 'ai/chats', undefined, { quiet: true });
  if (!CHAT.loaded) {
    CHAT.loaded = true;
    if (CHAT.id && CHAT.list.some((c) => c.id === CHAT.id)) await openChat(CHAT.id);
    else if (CHAT.id) newChat();
  }
  renderChatList();
}

async function openChat(id) {
  const chat = await api('GET', `ai/chats/${enc(id)}`);
  CHAT.id = chat.id;
  CHAT.title = chat.title;
  CHAT.turns = chat.turns;
  // The chat goes on with the model and reasoning it last used.
  const last = chat.turns.findLast((t) => t.model);
  CHAT.choice = last ? `${last.provider}\n${last.model}` : '';
  CHAT.reasoning = chat.turns.at(-1)?.reasoning ?? '';
  renderChatModels();
  store.set('vx.chat', chat.id);
  showChatSide(false);
  renderChat(true);
  renderChatList();
}

function newChat() {
  Object.assign(CHAT, { id: null, title: '', turns: [], choice: '', reasoning: '' });
  renderChatModels();
  store.set('vx.chat', '');
  showChatSide(false);
  renderChat(true);
  renderChatList();
  $('#promptForm').elements.text.focus();
}

// Grouped as a chat window does: today, yesterday, the week, older.
function renderChatList() {
  const day = (d) => new Date(d).setHours(0, 0, 0, 0);
  const today = day(Date.now());
  const group = (c) => {
    const days = Math.round((today - day(c.updated)) / 864e5);
    return days < 1 ? 'chat.today' : days < 2 ? 'chat.yesterday' : days < 7 ? 'chat.previous7Days' : 'chat.older';
  };
  const filter = CHAT.filter.trim().toLowerCase();
  const shown = CHAT.list.filter((c) => !filter || c.title.toLowerCase().includes(filter));
  const when = (c) => (group(c) === 'chat.today' ? new Date(c.updated).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })
    : new Date(c.updated).toLocaleDateString(undefined, { day: 'numeric', month: 'short' }));
  patch($('#chatList'), shown.length ? [...Map.groupBy(shown, group)].map(([name, chats]) => [
    h('div', { class: 'chat-group' }, tt(name)),
    chats.map((c) => h('button', {
      type: 'button', class: `chat-item${c.id === CHAT.id ? ' active' : ''}`, title: c.title,
      'aria-current': c.id === CHAT.id ? 'true' : false, onclick: () => act(() => openChat(c.id)),
    }, h('span', { class: 'chat-item-title' }, c.title), h('span', { class: 'chat-item-meta' }, when(c)))),
  ]) : h('p', { class: 'muted small chat-empty' }, tt(filter ? 'chat.noChatMatches' : 'chat.noChatsYet')));
}

// The model's answer in light Markdown: paragraphs, lists, headings, code
// blocks, `code` and **bold**. Built as nodes, never as HTML.
const LIST_ITEM = /^\s*([-*•]|\d+[.)])\s+/;
function chatMarkdown(text) {
  const inline = (line) => line.split(/(`[^`]+`|\*\*[^*]+\*\*)/).map((part) =>
    part.length > 1 && part.startsWith('`') && part.endsWith('`') ? h('code', {}, part.slice(1, -1))
      : part.length > 3 && part.startsWith('**') && part.endsWith('**') ? h('strong', {}, part.slice(2, -2)) : part);
  const out = [];
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  for (let i = 0; i < lines.length;) {
    const line = lines[i];
    if (line.startsWith('```')) {
      const code = [];
      for (i++; i < lines.length && !lines[i].startsWith('```'); i++) code.push(lines[i]);
      i++;
      out.push(h('pre', {}, h('code', {}, code.join('\n'))));
    } else if (LIST_ITEM.test(line)) {
      const ordered = /^\s*\d/.test(line);
      const items = [];
      for (; i < lines.length && LIST_ITEM.test(lines[i]); i++) items.push(h('li', {}, inline(lines[i].replace(LIST_ITEM, ''))));
      out.push(h(ordered ? 'ol' : 'ul', {}, items));
    } else if (/^#{1,6}\s/.test(line)) {
      out.push(h('p', {}, h('strong', {}, inline(line.replace(/^#+\s*/, '')))));
      i++;
    } else if (!line.trim()) {
      i++;
    } else {
      const para = [];
      for (; i < lines.length && lines[i].trim() && !lines[i].startsWith('```') && !/^#{1,6}\s/.test(lines[i]) && !LIST_ITEM.test(lines[i]); i++) para.push(lines[i]);
      out.push(h('p', {}, para.map((l, j) => [j > 0 && h('br', {}), inline(l)])));
    }
  }
  return out;
}

// A tool call: what was asked (or why it failed) once done, its time while live.
function chatToolCall(c, done) {
  const detail = done ? c.summary : `${c.ms} ms`;
  return h('details', { class: 'chat-tool' },
    h('summary', {}, h('span', { class: `chat-dot ${c.ok ? 'good' : 'bad'}`, title: tt(c.ok ? 'chat.ok' : 'chat.failed') }), h('span', { class: 'mono' }, c.tool),
      h('span', { class: 'chat-tool-summary' }, detail)),
    h('pre', {}, detail));
}

const clock = (at) => new Date(at).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });

function chatTurn(t) {
  const n = (v) => (v ?? 0).toLocaleString();
  const usage = t.usage && tt(t.usage.reasoningTokens ? 'chat.inOutReasoning' : 'chat.inOutTokens',
    { in: n(t.usage.inputTokens), out: n(t.usage.outputTokens), reasoning: n(t.usage.reasoningTokens) });
  const meta = [[t.provider, t.model].filter(Boolean).join(' · '), t.reasoning && tt('chat.reasoningLevel', { level: t.reasoning }), usage, clock(t.at)].filter(Boolean);
  return [
    h('div', { class: 'chat-msg user' }, h('div', { class: 'chat-bubble' }, t.text)),
    h('div', { class: 'chat-msg assistant' },
      t.actions?.length > 0 && h('div', { class: 'chat-tools-used' }, t.actions.map((a) => chatToolCall(a, true))),
      t.error ? h('div', { class: 'chat-error' }, h('strong', {}, t.error), t.detail && `: ${t.detail}`)
        : h('div', { class: 'chat-text' }, t.reply ? chatMarkdown(t.reply) : h('p', { class: 'muted' }, tt('chat.noReply'))),
      t.stoppedBy && h('div', {}, h('span', { class: 'pill bad' }, tt('chat.stoppedBy', { reason: t.stoppedBy })), t.detail && h('span', { class: 'bad small' }, ` ${t.detail}`)),
      h('div', { class: 'chat-meta' }, meta.join('  ·  '))),
  ];
}

function renderChat(toEnd = false) {
  const box = $('#chatMessages');
  const atEnd = toEnd || box.scrollHeight - box.scrollTop - box.clientHeight < 80;
  const pending = CHAT.pending?.id === CHAT.id ? CHAT.pending : null;
  if (document.activeElement !== $('#chatTitle')) patch($('#chatTitle'), CHAT.title || tt('chat.newChat'));
  $('#chatRename').hidden = $('#chatDelete').hidden = !CHAT.id || !CHAT.turns.length;
  patch(box, !CHAT.turns.length && !pending
    ? h('div', { class: 'chat-welcome' }, h('div', { class: 'card-icon' }, icon('chat')), h('h2', {}, tt('chat.whatShouldHouse')),
      h('p', { class: 'muted' }, tt('chat.askInWords')))
    : [CHAT.turns.map(chatTurn),
      pending && [
        h('div', { class: 'chat-msg user' }, h('div', { class: 'chat-bubble' }, pending.text)),
        h('div', { class: 'chat-msg assistant' },
          pending.live.length > 0 && h('div', { class: 'chat-tools-used' }, pending.live.map((c) => chatToolCall(c, false))),
          h('div', { class: 'chat-thinking', role: 'status', 'aria-label': tt('chat.working') }, h('span', {}), h('span', {}), h('span', {}))),
      ]]);
  if (atEnd) box.scrollTop = box.scrollHeight;
  $('#chatStop').hidden = !CHAT.pending;
  $('#chatSend').hidden = Boolean(CHAT.pending);
}

// The model picker: the settings' default first, then every provider that
// is on and set up, with those of its models that take tool calls (the
// agent works through tools). The pick is the chat's own: AI settings stay.
async function loadChatModels() {
  if (!AI.settings) [AI.providers, AI.settings] = await Promise.all([api('GET', 'ai/providers', undefined, { quiet: true }), api('GET', 'ai/settings', undefined, { quiet: true })]);
  renderChatModels();
  // Each provider's models as they come: one that is not reachable keeps none of the others waiting.
  await Promise.all(AI.providers.filter((p) => p.enabled && p.configured && !AI.models[p.id]).map(async (p) => {
    try { AI.models[p.id] = await api('GET', `ai/providers/${enc(p.id)}/models`, undefined, { quiet: true }); } catch { return; /* not listed: its group stays out */ }
    renderChatModels();
  }));
}

function renderChatModels() {
  const [wantedProvider, wantedModel] = CHAT.choice.split('\n');
  const def = AI.settings?.defaultProvider ? `${AI.settings.defaultProvider} · ${AI.settings.defaultModel ?? '?'}` : tt('chat.noDefaultModel');
  const words = $('#chatModelSearch').value.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const matches = (...texts) => words.every((w) => texts.some((t) => t?.toLowerCase().includes(w)));
  const option = (value, label, detail) => h('button', {
    type: 'button', role: 'option', class: 'model-option', 'aria-selected': String(value === CHAT.choice), 'data-value': value,
    onclick: () => pickChatModel(value),
  }, h('span', { class: 'model-option-name' }, label), detail && h('span', { class: 'model-option-id' }, detail));
  let label = `${def} (${tt('chat.settings')})`;
  const groups = AI.providers.filter((p) => p.enabled && p.configured).map((p) => {
    const models = (AI.models[p.id] ?? []).filter((m) => m.tools !== false);
    // The chat's model stays pickable before the list arrives.
    if (wantedProvider === p.id && wantedModel && !models.some((m) => m.id === wantedModel)) models.unshift({ id: wantedModel });
    const name = p.displayName ?? p.id;
    const chosen = wantedProvider === p.id && models.find((m) => m.id === wantedModel);
    if (chosen) label = `${chosen.name ?? chosen.id} · ${name}`;
    const shown = models.filter((m) => matches(m.id, m.name, name, p.id));
    return shown.length > 0 && [h('div', { class: 'model-group' }, `${name} (${shown.length})`),
      shown.map((m) => option(`${p.id}\n${m.id}`, m.name ?? m.id, m.name && m.name !== m.id ? m.id : null))];
  });
  patch($('#chatModelLabel'), label);
  $('#chatModel').title = tt('chat.model', { name: label });
  patch($('#chatModelList'), matches(def, 'settings', 'default', tt('chat.settings')) && option('', `${def}`, tt('chat.aiSettingsDefault')), groups,
    !groups.some(Boolean) && words.length > 0 && h('p', { class: 'muted small model-none' }, tt('chat.noModelMatches')));
  renderChatReasoning();
}

function pickChatModel(value) {
  CHAT.choice = value;
  $('#chatModelPop').hidePopover();
  renderChatModels();
  $('#promptForm').elements.text.focus();
}

// The levels the chosen provider and model take; none where they take none.
function chatReasons() {
  const [chosenProvider, chosenModel] = CHAT.choice.split('\n');
  const provider = chosenModel ? chosenProvider : AI.settings?.defaultProvider;
  const model = chosenModel || AI.settings?.defaultModel;
  const p = AI.providers.find((x) => x.id === provider);
  return Boolean(p?.capabilities?.includes('reasoning') && AI.models[provider]?.find((m) => m.id === model)?.reasoning !== false);
}

function renderChatReasoning() {
  const select = $('#promptForm').elements.reasoning;
  const takes = chatReasons();
  select.hidden = !takes;
  const agent = AI.settings?.reasoning?.agent ?? 'default';
  select.options[0].textContent = tt('chat.reasoningSettingsLevel', { level: agent });
  if (!takes) CHAT.reasoning = '';
  select.value = CHAT.reasoning;
}

function showChatSide(open) {
  $('#chatSide').classList.toggle('open', open);
  $('#chatSideToggle').setAttribute('aria-expanded', String(open));
}

async function sendChat(text, spoken = false) {
  const F = $('#promptForm').elements;
  if (!CHAT.id) {
    CHAT.id = crypto.randomUUID();
    CHAT.title = text.replace(/\s+/g, ' ');
    store.set('vx.chat', CHAT.id);
  }
  // Who speaks: the listener shown, else the household.
  const body = { text, conversationId: CHAT.id, ...ownerOf() };
  const [provider, model] = CHAT.choice.split('\n');
  if (model) Object.assign(body, { provider, model });
  // A provider or model without reasoning is sent none: the settings' level would be refused.
  if (!chatReasons()) body.reasoning = 'default';
  else if (F.reasoning.value) body.reasoning = F.reasoning.value;
  const pending = CHAT.pending = { id: CHAT.id, text, live: [], abort: new AbortController() };
  const turn = { text, at: new Date().toISOString(), provider: body.provider, model: body.model, reasoning: body.reasoning, actions: [] };
  renderChat(true);
  try {
    const a = await api('POST', 'ai/commands', body, { signal: pending.abort.signal });
    Object.assign(turn, { reply: a.reply, actions: a.actions, usage: a.usage, stoppedBy: a.stoppedBy, detail: a.detail, provider: a.provider ?? turn.provider, model: a.model ?? turn.model });
  } catch (err) {
    Object.assign(turn, pending.abort.signal.aborted ? { error: tt('chat.stopped'), detail: tt('chat.byYouNot') }
      : { error: err.data?.error ?? String(err.status), detail: err.data?.message ?? err.message });
  }
  CHAT.pending = null;
  if (CHAT.id === pending.id) CHAT.turns.push(turn);
  renderChat();
  if (spoken) readAloud(turn.reply);
  act(loadChats);
}

$('#promptForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const F = e.target.elements;
  const text = F.text.value.trim();
  if (!text || CHAT.pending) return;
  F.text.value = '';
  F.text.style.height = '';
  sendChat(text);
});
// Enter sends, Shift+Enter is a new line.
$('#promptForm').elements.text.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); $('#promptForm').requestSubmit(); }
});
$('#promptForm').elements.text.addEventListener('input', (e) => {
  e.target.style.height = 'auto';
  e.target.style.height = `${Math.min(e.target.scrollHeight, 240)}px`;
});
// The picker opens above its button (the composer is at the foot), its search focused.
$('#chatModel').addEventListener('click', () => $('#chatModelPop').togglePopover());
$('#chatModelPop').addEventListener('toggle', (e) => {
  if (e.newState !== 'open') return;
  const pop = e.target, b = $('#chatModel').getBoundingClientRect();
  pop.style.left = `${Math.max(8, Math.min(innerWidth - pop.offsetWidth - 8, b.left))}px`;
  pop.style.top = `${Math.max(8, b.top - pop.offsetHeight - 8)}px`;
  $('#chatModelSearch').select();
  $('#chatModelList [aria-selected="true"]')?.scrollIntoView({ block: 'nearest' });
});
$('#chatModelSearch').addEventListener('input', () => renderChatModels());
// Enter takes the first match; the arrows walk the list.
$('#chatModelPop').addEventListener('keydown', (e) => {
  const items = [...$('#chatModelList').querySelectorAll('.model-option')];
  const at = items.indexOf(document.activeElement);
  if (e.key === 'Enter' && e.target === $('#chatModelSearch') && items[0]) { e.preventDefault(); items[0].click(); }
  else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const next = e.key === 'ArrowDown' ? at + 1 : at - 1;
    (next < 0 ? $('#chatModelSearch') : items[Math.min(next, items.length - 1)])?.focus();
  }
});
$('#promptForm').elements.reasoning.addEventListener('change', (e) => { CHAT.reasoning = e.target.value; });
$('#chatStop').addEventListener('click', () => CHAT.pending?.abort.abort());
$('#chatNew').addEventListener('click', newChat);
$('#chatSideToggle').addEventListener('click', () => showChatSide(!$('#chatSide').classList.contains('open')));
$('#chatSearch').addEventListener('input', (e) => { CHAT.filter = e.target.value; renderChatList(); });
$('#chatDelete').addEventListener('click', () => {
  if (!CHAT.id || !confirm(tt('chat.deleteChatConfirm', { title: CHAT.title }))) return;
  const id = CHAT.id;
  act(async () => { await api('DELETE', `ai/chats/${enc(id)}`); if (CHAT.id === id) newChat(); await loadChats(); }, 'chat.chatDeleted');
});
// The title is edited in place: Enter keeps it, Escape goes back.
$('#chatRename').addEventListener('click', () => {
  const title = $('#chatTitle');
  title.contentEditable = 'plaintext-only';
  title.focus();
  getSelection().selectAllChildren(title);
});
$('#chatTitle').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); e.target.blur(); }
  if (e.key === 'Escape') { e.target.textContent = CHAT.title; e.target.blur(); }
});
$('#chatTitle').addEventListener('blur', (e) => {
  e.target.contentEditable = 'false';
  const title = e.target.textContent.trim();
  if (!CHAT.id || !title || title === CHAT.title) { e.target.textContent = CHAT.title || tt('chat.newChat'); return; }
  const id = CHAT.id;
  act(async () => { await api('PATCH', `ai/chats/${enc(id)}`, { title }); if (CHAT.id === id) CHAT.title = title; await loadChats(); }, 'chat.chatRenamed')
    .then(() => renderChat());
});

// --- assistant: the microphone --------------------------------------------------------
// One click listens; what is said is sent when the speaker stops. The AI
// settings' speech section decides who hears it: the server, through a
// provider's speech model, or the browser's own recognition, which also
// stands in when no speech provider can be used or the server fails.

const VOICE = { busy: false, stop: null, serverFailed: false };
const Recognition = window.SpeechRecognition ?? window.webkitSpeechRecognition;
const speechOf = () => AI.settings?.speech ?? {};
const speechLanguage = () => speechOf().language || navigator.language;

// Whether the server can hear it: recognition "server" with a provider that is on, set up and transcribes.
function serverHears() {
  const s = speechOf();
  const p = AI.providers.find((x) => x.id === s.provider);
  return s.recognition === 'server' && Boolean(p?.enabled && p.configured && p.capabilities?.includes('transcription')) && !VOICE.serverFailed;
}

function renderMic() {
  const mic = $('#chatMic');
  mic.classList.toggle('listening', VOICE.busy);
  mic.setAttribute('aria-pressed', String(VOICE.busy));
  mic.title = tt(VOICE.busy ? 'voice.stopListening' : 'voice.talkToGaldrun');
}

async function listen() {
  if (VOICE.busy) { VOICE.stop?.(); return; }
  if (!isSecureContext) { flash('voice.microphoneNeedsHttps', true); return; }
  speechSynthesis.cancel();
  VOICE.busy = true;
  renderMic();
  try {
    const text = serverHears() ? await hearOnServer() : await hearInBrowser();
    if (text?.trim()) sendChat(text.trim(), true);
  } catch (e) {
    flash(e.message, true);
  } finally {
    VOICE.busy = false;
    VOICE.stop = null;
    $('#promptForm').elements.text.placeholder = TEXT_PLACEHOLDER;
    renderMic();
  }
}

const TEXT_PLACEHOLDER = $('#promptForm').elements.text.placeholder;

// The browser's recognition, its words shown as they come.
function hearInBrowser() {
  if (!Recognition) throw new Error(tt('voice.thisBrowserHas'));
  const box = $('#promptForm').elements.text;
  return new Promise((resolve, reject) => {
    const rec = new Recognition();
    rec.lang = speechLanguage();
    rec.interimResults = true;
    let heard = '';
    rec.onresult = (e) => { heard = [...e.results].map((r) => r[0].transcript).join(''); box.value = heard; };
    rec.onerror = (e) => (e.error === 'no-speech' || e.error === 'aborted' ? resolve('')
      : reject(new Error(e.error === 'not-allowed' ? tt('voice.microphoneIsBlocked') : tt('voice.speechRecognition', { error: e.error }))));
    rec.onend = () => { box.value = ''; resolve(heard); };
    VOICE.stop = () => rec.stop();
    box.placeholder = tt('voice.listening');
    rec.start();
  });
}

// Recorded here as 16 kHz WAV, heard by the server. It stops by itself
// after 1.5 s of quiet once something was said (8 s of nothing ends it), at
// 60 s, or on a second click. When the server fails, the browser hears it
// instead, from then on in this page.
async function hearOnServer() {
  const box = $('#promptForm').elements.text;
  const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true } })
    .catch(() => { throw new Error(tt('voice.microphoneIsBlocked')); });
  const ctx = new AudioContext();
  const source = ctx.createMediaStreamSource(stream);
  const tap = ctx.createScriptProcessor(4096, 1, 1);
  const chunks = [];
  let spoke = false, quietSince = performance.now();
  const started = performance.now();
  await new Promise((resolve) => {
    VOICE.stop = resolve;
    tap.onaudioprocess = (e) => {
      const samples = new Float32Array(e.inputBuffer.getChannelData(0));
      chunks.push(samples);
      const rms = Math.sqrt(samples.reduce((sum, v) => sum + v * v, 0) / samples.length);
      const now = performance.now();
      if (rms > 0.02) { spoke = true; quietSince = now; }
      if ((spoke && now - quietSince > 1500) || (!spoke && now - started > 8000) || now - started > 60000) resolve();
    };
    source.connect(tap);
    tap.connect(ctx.destination);
    box.placeholder = tt('voice.listening');
  });
  tap.disconnect();
  source.disconnect();
  for (const t of stream.getTracks()) t.stop();
  const rate = ctx.sampleRate;
  await ctx.close();
  if (!spoke) return '';
  box.placeholder = tt('voice.transcribing');
  try {
    return (await api('POST', 'ai/transcriptions', wav(chunks, rate, 16000))).text;
  } catch (e) {
    VOICE.serverFailed = true;
    flash(tt('voice.serverCouldNot', { error: e.data?.message ?? e.message }), true);
    return hearInBrowser();
  }
}

// Mono PCM 16-bit WAV at `to` Hz, averaged down from `from` Hz.
function wav(chunks, from, to) {
  const input = new Float32Array(chunks.reduce((n, c) => n + c.length, 0));
  let at = 0;
  for (const c of chunks) { input.set(c, at); at += c.length; }
  const step = from / to;
  const length = Math.floor(input.length / step);
  const view = new DataView(new ArrayBuffer(44 + length * 2));
  const text = (offset, s) => [...s].forEach((ch, i) => view.setUint8(offset + i, ch.charCodeAt(0)));
  text(0, 'RIFF'); view.setUint32(4, 36 + length * 2, true); text(8, 'WAVE');
  text(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, to, true); view.setUint32(28, to * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  text(36, 'data'); view.setUint32(40, length * 2, true);
  for (let i = 0; i < length; i++) {
    const a = Math.floor(i * step), b = Math.min(input.length, Math.floor((i + 1) * step));
    let sum = 0;
    for (let j = a; j < b; j++) sum += input[j];
    const v = Math.max(-1, Math.min(1, sum / Math.max(1, b - a)));
    view.setInt16(44 + i * 2, v < 0 ? v * 0x8000 : v * 0x7fff, true);
  }
  return new Blob([view], { type: 'audio/wav' });
}

// A reply to a spoken message, read aloud where the AI settings say so; the
// Markdown marks are not read.
function readAloud(text) {
  if (!speechOf().readReplies || !text || !('speechSynthesis' in window)) return;
  const said = new SpeechSynthesisUtterance(text.replace(/```[\s\S]*?```/g, '').replace(/[*`#>_]/g, ''));
  said.lang = speechLanguage();
  speechSynthesis.speak(said);
}

$('#chatMic').addEventListener('click', () => listen());

// An agent's tool call belongs to the command still waiting.
function onAgentTool(data) {
  if (data?.caller !== 'agent' || !CHAT.pending) return;
  CHAT.pending.live.push(data);
  renderChat();
}

// A command another window sent: the list, and the chat if it is this one.
function onCommandEnded(data) {
  if (page !== 'assistant' || CHAT.pending?.id === data?.conversationId) return;
  act(loadChats);
  if (data?.conversationId && data.conversationId === CHAT.id && !CHAT.pending) act(() => openChat(CHAT.id));
}

// --- event stream -----------------------------------------------------------------

const EVENT_TYPES = [
  'zone.sourceNeeded', 'zone.sourceStarted', 'zone.sourceFailed', 'zone.ended', 'zone.transport',
  'room.connected', 'room.disconnected', 'room.active', 'room.joined', 'room.left', 'room.notReady', 'room.fellBehind', 'room.leftBehind', 'room.input',
  'room.unstable', 'room.stable', // server phase 34
  // What a room's client reports -- re-sent capabilities, sound cards, buffer,
  // volume control -- is announced when it arrives, so nothing waits and re-reads.
  'room.capabilities', 'room.audioDevices', 'room.bufferConfig', 'room.volumeControl', 'room.powerState',
  'core.synchronised', 'core.eventsLost', 'queue.changed', 'queue.deleted', 'zone.volumeChanged', 'indexer.progress', 'library.folderChanged', 'events.lost',
  'system.metrics', 'rooms.sync',
  'ai.toolCalled', 'ai.commandStarted', 'ai.commandEnded', 'ai.request',
  'queue.autoFilled', 'zone.autoFillChanged', 'zone.metadataChanged', 'settings.playbackChanged', 'like.changed',
  'favorite.changed', 'zone.userChanged', 'room.userChanged', 'proposals.running', 'proposals.changed', // server phases 36 and 38
  'room.imageChanged', 'room.name', 'room.forgotten', // server phase 39
  'artwork.changed', // server phase 40
];
const evCounts = new Map();

let es, evRetry, eventsOpened = false;
function connectEvents() {
  clearTimeout(evRetry);
  es?.close();
  es = new EventSource('/api/v1/events');
  // The page polls nothing: what it shows is read at the start and again after
  // an event. A reconnect may have missed events, so everything is read again.
  es.onopen = () => {
    clearTimeout(evRetry);
    setConn({ events: tt('events.eventsLive'), eventsOk: true });
    if (eventsOpened) refreshSoon();
    eventsOpened = true;
  };
  // The browser's own retry can sit on a dead socket for a long time, so the
  // page builds a fresh connection itself instead of waiting for it.
  es.onerror = () => {
    setConn({ events: tt('events.eventsReconnecting'), eventsOk: false });
    clearTimeout(evRetry);
    evRetry = setTimeout(connectEvents, 2000);
  };
  for (const type of EVENT_TYPES) es.addEventListener(type, (e) => onEvent(type, e));
}

function onEvent(type, e) {
  const data = tryParse(e.data);
  // Every second: not logged or counted, and no refresh.
  if (type === 'system.metrics' || type === 'rooms.sync') {
    if (type === 'system.metrics') onSystem(data); else onRoomsSync(data);
    return;
  }
  evCounts.set(type, (evCounts.get(type) ?? 0) + 1);
  const total = [...evCounts.values()].reduce((a, b) => a + b, 0);
  $('#evCount').textContent = `(${total})`;
  $('#evCount').title = [...evCounts].map(([t, n]) => `${t}: ${n}`).join('\n');
  const who = [data?.zone, data?.client].filter(Boolean).join(' ');
  appendLine($('#events'), `${time()} #${type === 'events.lost' ? '-' : e.lastEventId} ${type} ${who} ${JSON.stringify(data).slice(0, 220)}`, data,
    type.endsWith('ost') || type === 'zone.sourceFailed' || type === 'room.fellBehind' || type === 'room.leftBehind' || type === 'room.unstable' ? 'bad' : '');
  if (type === 'ai.request') { appendAi(aiRequestLine(data), aiPrompt(data), data.ok ? '' : 'bad'); aiUsageChanged(); }
  else if (/^(ai|proposals)\./.test(type) || type === 'queue.autoFilled' || type === 'zone.autoFillChanged') appendAi(`${time()} #${e.lastEventId} ${type} ${who} ${JSON.stringify(data).slice(0, 220)}`, data);
  if (type === 'queue.changed') {
    const zone = S.zones.find((z) => z.zone === data.zone);
    if (zone) { zone.queue = data; received.set(data, performance.now()); renderZones(); } else refreshSoon();
  } else if (type === 'indexer.progress') {
    if (S.status?.library) { S.status.library.indexer = data; renderIndexer(); renderFolderProgress(); }
  } else if (type === 'library.folderChanged') {
    onFolderChanged(data);
  } else if (type === 'ai.toolCalled') {
    onAgentTool(data);
    refreshSoon();
  } else if (type === 'zone.autoFillChanged') {
    // Its settings, or a fill starting or ending (the button spins meanwhile).
    const zone = S.zones.find((z) => z.zone === data.zone);
    if (zone && data.autoFill) { zone.autoFill = data.autoFill; renderZones(); } else refreshSoon();
  } else if (type === 'queue.autoFilled') {
    onAutoFilled(data);
    refreshSoon();
  } else if (type === 'favorite.changed') {
    act(loadFavorites);
  } else if (type === 'proposals.running' || type === 'proposals.changed') {
    if (page === 'home' || page === 'aiStatus') act(loadProposals);
    if (MB.places[MB.at]?.provider === PROPOSALS) mbLoad();
    refreshSoon(); // status: ai.proposals
  } else if (type === 'like.changed') {
    onLikeChanged(data);
  } else if (type === 'artwork.changed') {
    artworkFound(data.id);
    if (page === 'services') loadArtworkSoon();
  } else if (type === 'settings.playbackChanged') {
    S.playback = data;
    renderPlayback();
  } else if (type === 'ai.commandStarted') {
    // The answer to the POST says the same; nothing to re-read.
  } else if (type === 'ai.commandEnded') {
    onCommandEnded(data);
  } else if (type === 'room.imageChanged') {
    roomImageVersion.set(data.client, Date.now());
    noRoomImage.delete(data.client);
    refreshSoon();
  } else if (type === 'room.input') {
    // Shown on the room at once; the refresh then shows what the server did.
    S.lastInput[data.client] = { ...data, at: time() };
    renderRooms();
    refreshSoon();
  } else {
    refreshSoon();
  }
}

// --- log tail (the proxy's /gui/logs) -----------------------------------------------

let logCount = 0;
// Each file's offset after what arrived. Clear keeps them (per viewer), so a
// reload starts behind the cleared lines; a reconnect resumes where it was.
const logOffsets = {};
const logsCleared = () => { try { return JSON.parse(store.get('vx.logsCleared', '{}')); } catch { return {}; } };

async function connectLogs() {
  let config;
  try { config = await (await fetch('/gui/config')).json(); } catch { return; }
  const logs = $('#logs');
  if (!config.logs.length) {
    appendLine(logs, tt('logs.noLogFiles'));
    return;
  }
  const partial = {};
  const from = encodeURIComponent(JSON.stringify({ ...logsCleared(), ...logOffsets }));
  const es = new EventSource(`/gui/logs?from=${from}`);
  // The browser's own retry would ask from the first offsets again.
  es.onerror = () => { es.close(); setTimeout(connectLogs, 2000); };
  es.addEventListener('log', (e) => {
    const { name, text, offset } = JSON.parse(e.data);
    if (offset != null) logOffsets[name] = offset;
    const lines = ((partial[name] ?? '') + text).split(/\r?\n/);
    partial[name] = lines.pop();
    for (const line of lines) {
      const cls = /\b(ERROR|error)\b/.test(line) ? 'bad' : /\b(WARN|warn)\b/.test(line) ? 'warn' : '';
      appendLine(logs, `[${name}] ${line}`, undefined, cls);
      $('#logCount').textContent = `(${++logCount})`;
      noteIndexLine(line);
    }
  });
}

// --- refresh ------------------------------------------------------------------------

// The header's one dot: green while the server answers and its events arrive,
// red when either fails; what each said is its tooltip.
const conn = { api: tt('header.api'), apiOk: null, events: tt('header.events'), eventsOk: null };
function setConn(change) {
  Object.assign(conn, change);
  const el = $('#conn'), text = `${conn.api} · ${conn.events}`;
  el.className = 'conn-dot' + (conn.apiOk === false || conn.eventsOk === false ? ' bad' : conn.apiOk && conn.eventsOk ? ' good' : '');
  el.title = text;
  el.setAttribute('aria-label', `${tt('header.connection')}: ${text}`);
}

let refreshTimer;
function refreshSoon() {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(() => refreshCore(true), 150);
}

async function refreshCore(quiet = true) {
  const [zones, rooms, status] = await Promise.allSettled([
    api('GET', 'zones', undefined, { quiet }), api('GET', 'rooms', undefined, { quiet }), api('GET', 'status', undefined, { quiet })]);
  if (status.status === 'fulfilled') {
    setConn({ api: tt('common.apiOk'), apiOk: true });
    S.status = status.value;
    renderStatus();
    renderIndexer();
    renderFolderProgress();
  } else {
    setConn({ api: `API ${status.reason.status === 502 ? tt('common.unreachableProxy') : status.reason.status || tt('common.unreachable')}`, apiOk: false });
  }
  // Both read before either renders: a zone card names its rooms, a room card its zone.
  if (rooms.status === 'fulfilled') { S.rooms = rooms.value; recordHistory(S.rooms); }
  if (zones.status === 'fulfilled') {
    const now = performance.now();
    for (const z of zones.value) if (z.queue) received.set(z.queue, now);
    S.zones = zones.value;
    renderZones();
  }
  if (rooms.status === 'fulfilled') renderRooms();
}


// --- pages ---------------------------------------------------------------------------
// One page at a time, named by the hash (#/zones). A page's data is read when
// it is shown; zones, rooms and the event stream stay live on every page.

const PAGES = [...document.querySelectorAll('main > .page')].map((p) => p.dataset.page);
const onShow = {
  home: () => { act(loadHome); if (homeDetail) loadHomeDetail(); },
  // Places the track on the picked zone, which may have changed elsewhere.
  zones: () => renderZoneTabs(),
  client: () => renderRooms(),
  rooms: () => { act(loadDiscovered); if (!S.system) api('GET', 'system', undefined, { quiet: true }).then(onSystem, () => {}); },
  folders: () => { act(loadFolders); act(loadFound); },
  listeners: () => act(loadUsers),
  ai: () => act(loadAi),
  aiStatus: () => { act(loadProposals); act(loadAiUsage); },
  assistant: () => { renderChat(); act(loadChats); act(loadChatModels); },
  services: () => { if (S.status) renderTidalState(S.status); act(loadArtwork); },
  // The GUI settings pick up a change made in another browser.
  settings: () => { loadGuiSettings(); act(loadCores); act(loadCoreAudio); loadPlayback(); },
  playlists: () => act(loadPlaylists),
  stations: () => act(loadStations),
};

function showPage() {
  // A page, and for #/client/<id> the client it shows.
  const [wanted, arg, ...rest] = location.hash.replace(/^#\/?/, '').split('/');
  const next = PAGES.includes(wanted) ? wanted : 'home';
  const client = next === 'client' ? decodeURIComponent(arg ?? '') : null;
  const view = next === 'home' && HOME_VIEWS[arg] ? arg : null;
  const detail = next === 'home' && arg === 'open' && rest.length === 2 ? rest.join('/') : null;
  const changed = next !== page || client !== roomOpen || view !== homeView || detail !== (homeDetail?.key ?? null);
  homeView = view;
  if (detail !== (homeDetail?.key ?? null)) {
    homeDetail = detail && { provider: decodeURIComponent(rest[0]), node: decodeURIComponent(rest[1]), key: detail, back: page != null };
  }
  // A client's page goes back to the page it was opened from (Clients
  // when it was opened by its address).
  if (next === 'client' && page && page !== 'client') {
    const back = $('.room-back');
    back.href = `#/${page}`;
    back.lastChild.textContent = tt(document.querySelector(`main > .page[data-page="${page}"]`).dataset.title);
  }
  page = next;
  roomOpen = client;
  for (const p of document.querySelectorAll('main > .page')) p.hidden = p.dataset.page !== page;
  // A client's page belongs to Clients in the menu.
  const nav = page === 'client' ? 'rooms' : page;
  for (const a of document.querySelectorAll('#sidebar a[href^="#/"]')) {
    if (a.getAttribute('href') === `#/${nav}`) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
  }
  // A folded group opens for the page shown in it.
  const folded = $('#sidebar a[aria-current]')?.closest('.nav-links.closed');
  if (folded) foldNavGroup(document.querySelector(`[aria-controls="${folded.id}"]`), true);
  const section = document.querySelector(`main > .page[data-page="${page}"]`);
  // The library goes with the Zones and the Playlists page, as it was left.
  if ((page === 'zones' || page === 'playlists') && $('#browserPanel').parentElement !== section) section.append($('#libraryToggle'), $('#browserPanel'));
  document.title = `${tt(section.dataset.title)} · Galdrun Server`;
  setNav(false);
  if (!changed) return;
  // Its device settings are read again each time a client's page opens.
  if (client) deviceSettings.delete(client);
  window.scrollTo(0, 0);
  onShow[page]?.();
}
window.addEventListener('hashchange', () => { showPage(); $('#main').focus({ preventScroll: true }); });

// Below 960px the sidebar is a drawer behind the menu button.
function setNav(open) {
  document.body.classList.toggle('nav-open', open);
  $('#menuToggle').setAttribute('aria-expanded', String(open));
  $('#menuToggle').setAttribute('aria-label', tt(open ? 'nav.closeMenu' : 'nav.openMenu'));
  $('#scrim').hidden = !open;
  if (open) $('#sidebar a[aria-current]')?.focus();
}
$('#menuToggle').addEventListener('click', () => setNav(!document.body.classList.contains('nav-open')));
// On a wide screen the sidebar's own button collapses it to its icons (kept
// per viewer); a link's name is then its title.
function collapseNav(collapsed) {
  document.body.classList.toggle('nav-collapsed', collapsed);
  store.set('vx.nav', collapsed ? 'collapsed' : 'open');
  const b = $('#navCollapse');
  b.setAttribute('aria-expanded', String(!collapsed));
  b.title = tt(collapsed ? 'nav.expandMenu' : 'nav.collapseMenu');
  b.setAttribute('aria-label', b.title);
}
for (const a of document.querySelectorAll('#sidebar a')) a.title ||= [...a.childNodes].filter((n) => n.nodeType === 3).map((n) => n.textContent).join('').trim();
collapseNav(store.get('vx.nav', 'open') === 'collapsed');

// Settings and Diagnostics fold up and down by their headings, kept per viewer.
function foldNavGroup(b, open) {
  b.setAttribute('aria-expanded', String(open));
  $(`#${b.getAttribute('aria-controls')}`).classList.toggle('closed', !open);
  store.set(`vx.navGroup.${b.dataset.group}`, open ? 'open' : 'closed');
}
for (const b of document.querySelectorAll('.nav-group-toggle')) {
  foldNavGroup(b, store.get(`vx.navGroup.${b.dataset.group}`, 'open') === 'open');
  b.addEventListener('click', () => foldNavGroup(b, b.getAttribute('aria-expanded') !== 'true'));
}
$('#navCollapse').addEventListener('click', () => collapseNav(!document.body.classList.contains('nav-collapsed')));
$('#scrim').addEventListener('click', () => setNav(false));
$('#sidebar').addEventListener('click', (e) => { if (e.target.closest('a')) setNav(false); });
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && document.body.classList.contains('nav-open')) { setNav(false); $('#menuToggle').focus(); }
});

// --- tabs ---------------------------------------------------------------------------

for (const nav of document.querySelectorAll('nav.tabs')) {
  const panel = document.getElementById(nav.dataset.for);
  nav.addEventListener('click', (e) => {
    const button = e.target.closest('button[data-tab]');
    if (!button || button.parentElement !== nav) return;
    for (const b of nav.querySelectorAll(':scope > button[data-tab]')) b.classList.toggle('on', b === button);
    for (const t of panel.querySelectorAll(':scope > .tab')) t.classList.toggle('on', t.dataset.tab === button.dataset.tab);
    store.set(`vx.tab.${nav.dataset.for}`, button.dataset.tab);
  });
  const saved = store.get(`vx.tab.${nav.dataset.for}`, '');
  nav.querySelector(`:scope > button[data-tab="${saved}"]`)?.click();
}

// --- start --------------------------------------------------------------------------

setConn({});
renderHeaderListener();
showPage();
refreshCore(false);
act(loadPlaylists);
act(loadFavorites);
act(loadStations);
act(loadFolders);
act(loadSources);
loadPlayback();
loadEqProfiles();
loadLikes().catch(() => {}); // a server before likes answers 404
loadUsers().catch(() => {}); // a server before phase 36 answers 404
pollTidalLogin(tidalLoginGeneration, true);
fetch('/gui/config').then((r) => r.json()).then((c) => { AI.api = c.api; }, () => {});
loadGuiSettings();
api('GET', 'system', undefined, { quiet: true }).then(onSystem, () => {}); // a server before phase 18 answers 404
renderIndexLog();
connectEvents();
connectLogs();

// A control that has done its job gives focus back, so the panel re-renders.
document.addEventListener('change', (e) => {
  if (e.target.closest('#zones, #rooms, #folders') && e.target.matches('select, input[type=checkbox], input[type=range]')) e.target.blur();
});
$('#reloadListeners').addEventListener('click', () => { act(loadUsers); refreshSoon(); });

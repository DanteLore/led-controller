'use strict';
const $ = id => document.getElementById(id);
const JSON_MAX = 8192;
const HISTORY_ROWS = 150, HISTORY_SAMPLE_MS = 40;   // 6 seconds of history

let lib = null;          // the library being edited, as plain JSON
let savedText = '';      // what the device has, to spot unsaved changes
let set = null;          // lib, read by patch.js: the last version without problems
let problem = '';
let selected = 0;
let device = null;       // last /api/state, or null if the controller can't be reached
let ledCount = 10;
let live = false, liveSince = 0;
let speedSteps = 0;
let tab = 'patches';     // or 'playlists': which kind the cards and editor show

const isPlaylist = p => !!p.playlist;
const inTab = p => isPlaylist(p) === (tab === 'playlists');

// ---- Talking to the controller -------------------------------------------

async function api(method, path, body) {
  const r = await fetch(path, { method, body: body === undefined ? undefined : JSON.stringify(body),
                                headers: body === undefined ? {} : { 'Content-Type': 'application/json' } });
  const text = await r.text();
  if (!r.ok) throw new Error(text || r.statusText);
  return text;
}

async function refreshState() {
  try {
    device = JSON.parse(await api('GET', '/api/state'));
  } catch (e) {
    device = null;
  }
  showDevice();
}

function showDevice() {
  const playing = $('now-playing');
  const b = $('brightness');
  if (!device) {
    playing.textContent = 'Not connected: preview only';
    b.disabled = true;
    return;
  }
  const name = device.modes[device.mode] || '';
  const editing = lib && lib.patches[selected] ? lib.patches[selected].name : '';
  playing.replaceChildren(document.createTextNode(device.draft ? editing : name));
  if (device.draft) playing.append(' ', Object.assign(document.createElement('span'), { className: 'tag edit', textContent: 'EDIT' }));
  if (document.activeElement !== b) b.value = device.brightness;
  b.disabled = false;
  $('brightness-value').textContent = device.brightness + '%';
  $('tempo').textContent = !device.beat_found ? 'no beat'
    : Math.round(device.bpm) + ' BPM, ' + (device.breakdown ? 'breakdown' : Math.round(100 * (device.beat_belief || 0)) + '% sure');
  // The device stops the draft when its button is pressed
  if (live && !device.draft && Date.now() - liveSince > 4000) setLive(false);
  updateCardStatus();
}

let draftTimer = null;
function sendDraft() {
  clearTimeout(draftTimer);
  if (!live || problem || !device) return;
  draftTimer = setTimeout(async () => {
    try {
      await api('POST', '/api/draft?index=' + selected, lib);
    } catch (e) {
      say('Strip preview failed: ' + e.message, 'bad');
    }
  }, 120);
}

async function setLive(on) {
  live = on;
  $('live').checked = on;
  if (on) {
    liveSince = Date.now();
    sendDraft();
  } else if (device) {
    clearTimeout(draftTimer);
    try { await api('DELETE', '/api/draft'); } catch (e) { /* the strip carries on */ }
  }
}

// ---- Library state ---------------------------------------------------------

function say(text, kind) {
  const m = $('message');
  m.textContent = text;
  m.className = kind || 'dim';
}

// Older files gave each patch a "knob"; the knobs now have fixed jobs, so it's dropped
function withoutKnobs(library) {
  for (const p of library.patches || []) if (p && typeof p === 'object') delete p.knob;
  return library;
}

function libText() {
  return JSON.stringify(lib);
}

// Call after every edit. structural = the form's shape changed, so rebuild it.
function changed(structural) {
  try {
    set = Patch.parse(lib);
    problem = '';
  } catch (e) {
    problem = e.message;
  }
  $('problem').textContent = problem ? "Can't use this yet: " + problem : '';
  const text = libText();
  const dirty = text !== savedText;
  const size = new TextEncoder().encode(text).length;
  $('size').textContent = (size / 1024).toFixed(1) + ' of 8 KB';
  $('size').className = size > JSON_MAX ? 'bad' : 'dim';
  $('save').disabled = !dirty || !!problem || !device || size > JSON_MAX;
  $('discard').disabled = !dirty;
  if (dirty) say(device ? 'Unsaved changes' : 'Unsaved changes (not connected, so they can only be downloaded)');
  else say(device ? 'Saved on the device' : '');
  if (structural) renderEditor();
  renderCards();
  if (!$('json-details').open || document.activeElement !== $('json')) $('json').value = JSON.stringify(lib, null, 2);
  sendDraft();
}

function select(i) {
  selected = Math.max(0, Math.min(i, lib.patches.length - 1));
  tab = isPlaylist(lib.patches[selected]) ? 'playlists' : 'patches';
  resetPreview();
  renderEditor();
  renderCards();
  sendDraft();
}

function uniqueName(base) {
  base = base.replace(/ \d+$/, '');   // a clone of "Embers 2" is "Embers 3", not "Embers 2 2"
  for (let n = 1; ; n++) {
    const name = n === 1 ? base : base.slice(0, 13) + ' ' + n;
    if (!lib.patches.some(p => p.name === name)) return name;
  }
}

function plainPatches() {
  return lib.patches.filter(p => !p.playlist).map(p => p.name);
}

// ---- Library cards ---------------------------------------------------------

const TAB_INTRO = {
  patches: 'Each patch is a look: oscillators for brightness, and a palette. Click one to edit it, ⧉ clones it, ▶ plays the saved version on the strip.',
  playlists: 'A playlist plays patches in turn, like Demo. Click one to edit it, ⧉ clones it, ▶ plays the saved version on the strip.',
};

function renderCards() {
  const indices = lib.patches.map((p, i) => i).filter(i => inTab(lib.patches[i]));
  document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
  $('patch-count').textContent = lib.patches.filter(p => !isPlaylist(p)).length;
  $('playlist-count').textContent = lib.patches.filter(isPlaylist).length;
  $('tab-intro').textContent = TAB_INTRO[tab];
  $('new-patch').hidden = tab !== 'patches';
  $('new-playlist').hidden = tab !== 'playlists';
  $('editor-section').hidden = !indices.length;

  const cards = $('cards');
  if (!indices.length) {
    cards.replaceChildren(el('div', { className: 'empty', textContent: 'No playlists yet. New playlist makes one.' }));
    return;
  }
  cards.replaceChildren(...indices.map(i => {
    const p = lib.patches[i];
    const card = el('div', { className: 'card' + (i === selected ? ' selected' : '') });
    card.onclick = () => select(i);
    const canvas = el('canvas');
    canvas.dataset.index = i;
    const clone = el('button', { className: 'small', textContent: '⧉', title: 'Clone', disabled: lib.patches.length >= Patch.PATCH_MAX });
    clone.onclick = e => { e.stopPropagation(); clonePatch(i); };
    const play = el('button', { className: 'small play', textContent: '▶', title: 'Play the saved version on the strip' });
    play.onclick = e => { e.stopPropagation(); playSaved(p.name); };
    const top = el('div', { className: 'row' }, el('span', { className: 'name', textContent: p.name }),
                   el('span', { className: 'spacer' }), clone, play);
    const tag = p.playlist ? p.playlist.length + (p.playlist.length === 1 ? ' step' : ' steps')
              : p.colour && Array.isArray(p.colour.palette) ? p.colour.palette.length + (p.colour.palette.length === 1 ? ' colour' : ' colours') : '';
    const info = el('div', { className: 'row' }, el('span', { className: 'tag', textContent: tag }), el('span', { className: 'playing' }));
    info.dataset.name = p.name;
    card.append(canvas, top, info);
    return card;
  }));
  updateCardStatus();
}

function updateCardStatus() {
  const savedNames = device ? device.modes : [];
  document.querySelectorAll('.card').forEach(card => {
    const name = card.querySelector('[data-name]').dataset.name;
    const playing = device && !device.draft && savedNames[device.mode] === name;
    card.querySelector('.playing').textContent = playing ? '● on the strip' : '';
    card.querySelector('.play').disabled = !device || savedNames.indexOf(name) < 0;
  });
}

document.querySelectorAll('#tabs button').forEach(b => b.onclick = () => {
  if (tab === b.dataset.tab) return;
  tab = b.dataset.tab;
  const first = lib.patches.findIndex(inTab);
  if (first >= 0) select(first);
  else renderCards();
});

async function playSaved(name) {
  const mode = device ? device.modes.indexOf(name) : -1;
  if (mode < 0) return;
  if (live) await setLive(false);
  try {
    await api('POST', '/api/state', { mode });
    setTimeout(refreshState, 150);
  } catch (e) {
    say(e.message, 'bad');
  }
}

// ---- Library actions -------------------------------------------------------

// Goes after the patch it was made from, or the last of its kind
function addPatch(patch, after) {
  if (lib.patches.length >= Patch.PATCH_MAX) return say('The device holds up to ' + Patch.PATCH_MAX + ' patches and playlists', 'bad');
  if (after === undefined) {
    after = -1;
    lib.patches.forEach((q, i) => { if (isPlaylist(q) === isPlaylist(patch)) after = i; });
    if (after < 0) after = lib.patches.length - 1;
  }
  lib.patches.splice(after + 1, 0, patch);
  selected = after + 1;
  tab = isPlaylist(patch) ? 'playlists' : 'patches';
  resetPreview();
  changed(true);
}

function clonePatch(i) {
  const copy = JSON.parse(JSON.stringify(lib.patches[i]));
  copy.name = uniqueName(copy.name);
  addPatch(copy, i);
}

// The next patch of the same kind before (-1) or after (+1) the selected one, or -1
function neighbour(by) {
  for (let i = selected + by; i >= 0 && i < lib.patches.length; i += by)
    if (isPlaylist(lib.patches[i]) === isPlaylist(lib.patches[selected])) return i;
  return -1;
}

$('new-patch').onclick = () => addPatch({ name: uniqueName('New'), glow: { a: { shape: 'sine', period: 4, scatter: 1 } },
                                          colour: { palette: ['#FF3A00', '#FFB040'] } });
$('new-playlist').onclick = () => {
  const names = plainPatches();
  if (!names.length) return say('Make a patch first: playlists play other patches', 'bad');
  addPatch({ name: uniqueName('Playlist'), playlist: [{ patch: names[0], secs: 30 }] });
};
$('duplicate').onclick = () => clonePatch(selected);
$('delete').onclick = () => {
  const p = lib.patches[selected];
  const users = lib.patches.filter(q => (q.playlist || []).some(s => s.patch === p.name)).map(q => q.name);
  const extra = users.length ? '\n\nIt will also be taken out of: ' + users.join(', ') : '';
  if (!confirm('Delete ' + p.name + '?' + extra)) return;
  for (const q of lib.patches) if (q.playlist) q.playlist = q.playlist.filter(s => s.patch !== p.name);
  if (lib.default === p.name) delete lib.default;
  const next = neighbour(1) >= 0 ? neighbour(1) - 1 : neighbour(-1);
  lib.patches.splice(selected, 1);
  selected = next >= 0 ? next : Math.min(selected, lib.patches.length - 1);
  resetPreview();
  changed(true);
};
function move(by) {
  const to = neighbour(by);
  const [p] = lib.patches.splice(selected, 1);
  selected = to;
  lib.patches.splice(selected, 0, p);
  changed(true);
}
$('move-up').onclick = () => move(-1);
$('move-down').onclick = () => move(1);

$('download').onclick = () => {
  const a = el('a', { download: 'led-patches.json',
                      href: URL.createObjectURL(new Blob([JSON.stringify(lib, null, 2) + '\n'], { type: 'application/json' })) });
  a.click();
  URL.revokeObjectURL(a.href);
};
$('upload').onclick = () => $('upload-file').click();
$('upload-file').onchange = async e => {
  const file = e.target.files[0];
  e.target.value = '';
  if (file) replaceLibrary(await file.text(), 'Loaded ' + file.name + ': not saved yet');
};
$('reset').onclick = async () => {
  if (!confirm('Replace the library with the built-in presets? Nothing changes on the device until you save.')) return;
  try {
    replaceLibrary(await api('GET', '/api/presets'), 'Presets loaded: not saved yet');
  } catch (e) {
    say("Couldn't fetch the presets: " + e.message, 'bad');
  }
};
$('json-apply').onclick = () => replaceLibrary($('json').value, 'JSON applied: not saved yet');


function replaceLibrary(text, message) {
  let obj;
  try {
    obj = JSON.parse(text);
    Patch.parse(obj);
  } catch (e) {
    return say("Can't use that: " + (e instanceof SyntaxError ? 'not valid JSON' : e.message), 'bad');
  }
  lib = withoutKnobs(obj);
  selected = 0;
  resetPreview();
  changed(true);
  say(message);
}

$('discard').onclick = () => {
  if (!confirm('Throw away your changes and go back to what the device has?')) return;
  lib = JSON.parse(savedText);
  selected = Math.min(selected, lib.patches.length - 1);
  resetPreview();
  changed(true);
};

$('save').onclick = async () => {
  $('save').disabled = true;
  say('Saving…');
  try {
    const text = libText();
    await api('POST', '/api/patches', lib);
    savedText = text;
    await refreshState();
    // Saving ends the draft; carry on showing this patch, now saved
    if (live) await api('POST', '/api/state', { mode: selected });
    changed(false);
    say('Saved on the device', 'good');
  } catch (e) {
    changed(false);
    say('Not saved: ' + e.message, 'bad');
  }
};

$('live').onchange = e => setLive(e.target.checked);

let brightnessTimer = null;
$('brightness').oninput = e => {
  $('brightness-value').textContent = e.target.value + '%';
  clearTimeout(brightnessTimer);
  brightnessTimer = setTimeout(() => api('POST', '/api/state', { brightness: Number(e.target.value) }).catch(() => {}), 100);
};

$('speed').oninput = e => {
  speedSteps = Number(e.target.value);
  $('speed-value').textContent = speedMultiplier().toFixed(2) + 'x';
};
function speedMultiplier() {
  return Math.pow(2, speedSteps / 5);   // same as the knob: every 5 clicks doubles
}

$('led-count').oninput = e => {
  const n = Math.round(Number(e.target.value));
  if (n >= 1 && n <= 300) { ledCount = n; resetPreview(); }
};

window.addEventListener('beforeunload', e => { if (lib && libText() !== savedText) e.preventDefault(); });

// ---- Start -----------------------------------------------------------------

async function start() {
  await refreshState();
  let text;
  try {
    text = await api('GET', '/api/patches');
  } catch (e) {
    try {
      text = await api('GET', '/api/presets');
    } catch (e2) {
      $('fields').textContent = "Couldn't load the patches. Open this page from the controller's address.";
      return;
    }
  }
  lib = withoutKnobs(JSON.parse(text));
  savedText = libText();
  if (device) ledCount = device.leds;
  $('led-count').value = ledCount;
  $('led-count-hint').textContent = device ? 'The strip has ' + device.leds : '';
  $('history-span').textContent = (HISTORY_ROWS * HISTORY_SAMPLE_MS / 1000) + ' seconds at 1x';
  if (device) selected = Math.min(device.mode, lib.patches.length - 1);
  tab = isPlaylist(lib.patches[selected]) ? 'playlists' : 'patches';
  // A draft left playing by an earlier visit has nothing to edit it now
  if (device && device.draft) {
    await api('DELETE', '/api/draft').catch(() => {});
    device.draft = false;
    showDevice();
  }
  changed(true);
  requestAnimationFrame(tick);
  setInterval(refreshState, 2000);
}
start();

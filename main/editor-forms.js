'use strict';
// Loaded before editor.js, whose state these use once the page is running

// ---- Form building ---------------------------------------------------------

function el(tag, props, ...children) {
  const e = Object.assign(document.createElement(tag), props || {});
  e.append(...children);
  return e;
}

function fieldset(title, ...children) {
  return el('fieldset', {}, el('legend', { textContent: title }), ...children);
}

// A labelled row; hint goes underneath
function field(label, control, hint) {
  const id = 'f' + Math.random().toString(36).slice(2);
  const target = control.matches && control.matches('input, select') ? control : control.querySelector('input, select');
  if (target) target.id = id;
  const row = el('div', { className: 'field' }, el('label', { htmlFor: id, textContent: label }), control);
  if (hint) row.append(el('div', { className: 'hint', textContent: hint }));
  return row;
}

// Values equal to the default are left out of the JSON, to keep it short
function store(obj, key, value, fallback) {
  if (value === fallback) delete obj[key];
  else obj[key] = value;
}

function slider(obj, key, fallback, min, max, step) {
  const value = key in obj ? obj[key] : fallback;
  const range = el('input', { type: 'range', min, max, step, value });
  const num = el('input', { type: 'number', step, value });
  range.oninput = () => { num.value = range.value; store(obj, key, Number(range.value), fallback); changed(false); };
  num.oninput = () => {
    if (num.value === '' || isNaN(Number(num.value))) return;
    range.value = num.value;
    store(obj, key, Number(num.value), fallback);
    changed(false);
  };
  return el('div', { className: 'row' }, range, num);
}

function dropdown(obj, key, fallback, options, structural) {
  const s = el('select');
  for (const [value, text] of options) s.append(el('option', { value, textContent: text }));
  s.value = key in obj ? obj[key] : fallback;
  s.onchange = () => { store(obj, key, s.value, fallback); changed(structural); };
  return s;
}

function checkbox(text, checked, onchange) {
  const box = el('input', { type: 'checkbox', checked });
  box.onchange = () => onchange(box.checked);
  return el('label', { className: 'check' }, box, text);
}

function oscEditor(title, o) {
  const shape = dropdown(o, 'shape', 'sine',
    [['sine', 'Sine: smooth'], ['triangle', 'Triangle'], ['saw', 'Saw: ramp up, drop'], ['square', 'Square: on/off'],
     ['noise', 'Noise: random drift'], ['decay', 'Decay: hit, then fade']], false);
  const onBeat = o.clock === 'beat';
  const clock = dropdown(o, 'clock', 'time', [['time', 'Seconds'], ['beat', 'Beats, from the mic']], true);
  const unit = onBeat ? ' beats' : ' s';

  const ranged = Array.isArray(o.period);
  const periodBox = el('div', { className: 'row' });
  const periodInput = (index) => {
    const value = ranged ? o.period[index] : (o.period === undefined ? 1 : o.period);
    const input = el('input', { type: 'number', min: 0, step: 0.1, value });
    input.oninput = () => {
      const v = Number(input.value);
      if (input.value === '' || isNaN(v)) return;
      if (ranged) o.period[index] = v; else store(o, 'period', v, 1);
      changed(false);
    };
    return input;
  };
  if (ranged) periodBox.append(periodInput(0), ' to ', periodInput(1), unit);
  else periodBox.append(periodInput(0), unit);
  const randomPeriod = checkbox('Each LED picks its own, in a range', ranged, on => {
    const p = ranged ? o.period[0] : (o.period === undefined ? 1 : o.period);
    o.period = on ? [p, Math.round(p * 300) / 100] : p;
    changed(true);
  });

  return fieldset(title,
    field('Shape', shape),
    field('Clock', clock, onBeat ? 'Follows the beat the mic hears: 1 = every beat, 4 = every bar. In the glow, it pulses only as hard as the music hits, so it goes quiet in a breakdown' : null),
    field('Period', periodBox, '0 holds still'),
    el('div', { className: 'field' }, el('span'), randomPeriod),
    field('Phase', slider(o, 'phase', 0, 0, 1, 0.01), 'Start point, in cycles'),
    field('Spread', slider(o, 'spread', 0, -4, 4, 0.05), 'Cycles across the strip, so the wave travels; minus goes the other way'),
    field('Scatter', slider(o, 'scatter', 0, 0, 1, 0.01), 'How much each LED starts at its own random point'));
}

function glowEditor(p) {
  const box = fieldset('Glow: brightness of each LED',
    checkbox('Brightness changes (otherwise always full)', !!p.glow, on => {
      if (on) p.glow = { a: { shape: 'sine', period: 4 } }; else delete p.glow;
      changed(true);
    }));
  if (!p.glow) return box;
  const g = p.glow;
  box.append(oscEditor('Oscillator A', g.a));
  box.append(checkbox('Add oscillator B', !!g.b, on => {
    if (on) g.b = { shape: 'sine', period: 7 }; else { delete g.b; delete g.mix; delete g.balance; }
    changed(true);
  }));
  if (g.b) {
    box.append(oscEditor('Oscillator B', g.b));
    box.append(field('Mix', dropdown(g, 'mix', 'blend', [['blend', 'Blend A and B'], ['multiply', 'Multiply: A × B']], true)));
    if ((g.mix || 'blend') === 'blend') box.append(field('Share of A', slider(g, 'balance', 0.5, 0, 1, 0.01)));
  }
  box.append(field('Power', slider(g, 'power', 1, 0.1, 6, 0.1), 'Above 1 lingers dim with brief swells; below 1 stays bright with brief dips'));
  box.append(field('Floor', slider(g, 'floor', 0, 0, 1, 0.01), 'Dimmest it gets'));
  box.append(field('Ceiling', slider(g, 'ceiling', 1, 0, 1, 0.01), 'Brightest it gets'));
  return box;
}

function colourEditor(p) {
  const c = p.colour;
  const swatches = el('div', { className: 'swatches' });
  c.palette.forEach((hex, i) => {
    const input = el('input', { type: 'color', value: hex.toLowerCase() });
    input.oninput = () => { c.palette[i] = input.value.toUpperCase(); changed(false); };
    const remove = el('button', { textContent: '×', title: 'Remove', disabled: c.palette.length === 1 });
    remove.onclick = () => { c.palette.splice(i, 1); changed(true); };
    swatches.append(el('div', { className: 'swatch' }, input, remove));
  });
  const add = el('button', { textContent: '+', title: 'Add a colour', disabled: c.palette.length >= Patch.PATCH_PALETTE_MAX });
  add.onclick = () => { c.palette.push(c.palette[c.palette.length - 1]); changed(true); };
  swatches.append(add);

  const box = fieldset('Colour',
    field('Palette', el('div', {}, swatches, el('canvas', { id: 'palette-bar' }))),
    field('Blend', dropdown(c, 'blend', 'smooth', [['smooth', 'Smooth'], ['steps', 'Hard steps']], true)));
  if ((c.blend || 'smooth') === 'smooth') {
    box.append(el('div', { className: 'field' }, el('span'),
      checkbox('Wrap: last colour blends back to the first', !!c.wrap, on => { store(c, 'wrap', on, false); changed(false); })));
  }
  box.append(field('Picked by', dropdown(c, 'source', 'glow',
    [['glow', 'Brightness: dim LEDs get the first colour'], ['heat', 'Heat: the beat lifts it up the palette'], ['osc', 'Its own oscillator']], true)));
  if (c.source === 'osc') {
    if (!c.osc) c.osc = { shape: 'saw', period: 10, spread: 1 };
    box.append(oscEditor('Colour oscillator', c.osc));
  }
  return box;
}

// Pointer events rather than HTML drag and drop, so it works with a finger too.
// A press that doesn't move is a tap. dropAt(index) gets the step position under
// the pointer when it's let go over the playlist.
function draggable(handle, label, onTap, dropAt) {
  handle.addEventListener('pointerdown', e => {
    if (e.button) return;
    e.preventDefault();
    const x0 = e.clientX, y0 = e.clientY;
    let ghost = null, at = null;
    const line = el('div', { className: 'drop-line' });
    handle.setPointerCapture(e.pointerId);
    const move = ev => {
      if (!ghost && Math.hypot(ev.clientX - x0, ev.clientY - y0) < 6) return;
      if (!ghost) document.body.append(ghost = el('div', { className: 'drag-ghost', textContent: label }));
      ghost.style.left = ev.clientX + 'px';
      ghost.style.top = ev.clientY + 'px';
      at = dropIndex(ev.clientX, ev.clientY, line);
    };
    const end = ev => {
      handle.removeEventListener('pointermove', move);
      handle.removeEventListener('pointerup', end);
      handle.removeEventListener('pointercancel', end);
      line.remove();
      if (ghost) ghost.remove();
      if (ev.type !== 'pointerup') return;
      if (!ghost) { if (onTap) onTap(); }
      else if (at !== null) dropAt(at);
    };
    handle.addEventListener('pointermove', move);
    handle.addEventListener('pointerup', end);
    handle.addEventListener('pointercancel', end);
  });
}

// Which gap between steps the pointer is nearest, with a line drawn there; null when away from the list
function dropIndex(x, y, line) {
  const list = $('steps');
  const r = list.getBoundingClientRect();
  if (x < r.left - 40 || x > r.right + 40 || y < r.top - 50 || y > r.bottom + 50) {
    line.remove();
    return null;
  }
  const rows = [...list.querySelectorAll('.step')];
  let at = rows.findIndex(row => { const b = row.getBoundingClientRect(); return y < b.top + b.height / 2; });
  if (at < 0) at = rows.length;
  const lineY = !rows.length ? r.top + r.height / 2
              : at < rows.length ? rows[at].getBoundingClientRect().top - 4
              : rows[rows.length - 1].getBoundingClientRect().bottom + 2;
  Object.assign(line.style, { left: r.left + 'px', width: r.width + 'px', top: lineY + 'px' });
  if (!line.isConnected) document.body.append(line);
  return at;
}

function addStep(p, name, at) {
  if (p.playlist.length >= Patch.PATCH_PLAYLIST_MAX) return say('A playlist holds up to ' + Patch.PATCH_PLAYLIST_MAX + ' steps', 'bad');
  p.playlist.splice(at, 0, { patch: name, secs: 30 });
  changed(true);
}

function moveStep(p, from, to) {
  if (to > from) to--;
  if (to === from) return;
  const [s] = p.playlist.splice(from, 1);
  p.playlist.splice(to, 0, s);
  changed(true);
}

function playlistEditor(p) {
  const names = plainPatches();
  const chips = el('div', { className: 'chips' });
  for (const name of names) {
    const canvas = el('canvas');
    canvas.dataset.index = lib.patches.findIndex(q => q.name === name);
    const chip = el('div', { className: 'chip', title: 'Drag into the playlist, or tap to add it at the end' }, canvas, name);
    draggable(chip, name, () => addStep(p, name, p.playlist.length), at => addStep(p, name, at));
    chips.append(chip);
  }
  const steps = el('div', { className: 'steps', id: 'steps' });
  if (!p.playlist.length) steps.append(el('div', { className: 'placeholder', textContent: 'Drag patches here' }));

  p.playlist.forEach((s, i) => {
    const handle = el('span', { className: 'handle', textContent: '⠿', title: 'Drag to reorder' });
    draggable(handle, s.patch, null, at => moveStep(p, i, at));
    const missing = names.indexOf(s.patch) < 0;
    const name = el('span', { className: 'step-name' + (missing ? ' bad' : ''), textContent: s.patch + (missing ? ' (missing)' : '') });
    const secs = el('input', { type: 'number', min: 0.1, step: 1, value: s.secs, title: 'Seconds' });
    secs.oninput = () => { if (secs.value !== '') { s.secs = Number(secs.value); changed(false); } };
    const ramp = Array.isArray(s.speed) ? s.speed : [s.speed === undefined ? 1 : s.speed, s.speed === undefined ? 1 : s.speed];
    const speedInput = (index) => {
      const input = el('input', { type: 'number', min: 0, max: 100, step: 0.1, value: ramp[index], title: index ? 'Speed at the end' : 'Speed at the start' });
      input.oninput = () => {
        if (input.value === '') return;
        ramp[index] = Number(input.value);
        if (ramp[0] === ramp[1]) store(s, 'speed', ramp[0], 1); else s.speed = ramp.slice();
        changed(false);
      };
      return input;
    };
    const remove = el('button', { className: 'small', textContent: '×', title: 'Remove step' });
    remove.onclick = () => { p.playlist.splice(i, 1); changed(true); };
    steps.append(el('div', { className: 'step' }, handle, name, el('span', { className: 'row' }, secs, 's'),
      el('span', { className: 'row dim ramp' }, 'speed', speedInput(0), '→', speedInput(1), 'x'), remove));
  });

  return fieldset('Playlist',
    el('div', { className: 'caption' }, el('span', { textContent: 'Patches: drag into the playlist, or tap to add at the end' })),
    names.length ? chips : el('div', { className: 'empty', textContent: 'Make a patch first: playlists play patches.' }),
    el('div', { className: 'caption', style: 'margin-top:14px' },
       el('span', { textContent: 'Steps, in order: drag ⠿ to reorder. Speed ramps from the first number to the second (1 = normal).' })),
    steps);
}

function renderEditor() {
  const p = lib.patches[selected];
  $('editor-title').textContent = 'Edit ' + p.name;
  const name = el('input', { type: 'text', value: p.name, maxLength: Patch.PATCH_NAME_MAX });
  name.oninput = () => {
    const old = p.name;
    p.name = name.value;
    // Playlists follow the rename
    for (const q of lib.patches) for (const s of q.playlist || []) if (s.patch === old) s.patch = p.name;
    if (lib.default === old) lib.default = p.name;
    $('editor-title').textContent = 'Edit ' + p.name;
    changed(false);
  };
  const general = fieldset('Name', field('Name', name, 'Shown on the device screen'),
    el('div', { className: 'field' }, el('span'),
      checkbox('Start on this when the device boots', lib.default === p.name, on => {
        if (on) lib.default = p.name; else delete lib.default;
        changed(false);
      })));
  const parts = [general];
  if (p.playlist) parts.push(playlistEditor(p));
  else parts.push(glowEditor(p), colourEditor(p));
  $('fields').replaceChildren(...parts);
  $('timeline-wrap').hidden = !p.playlist;
  $('move-up').disabled = neighbour(-1) < 0;
  $('move-down').disabled = neighbour(1) < 0;
  $('delete').disabled = lib.patches.length === 1;
  $('duplicate').disabled = lib.patches.length >= Patch.PATCH_MAX;
}

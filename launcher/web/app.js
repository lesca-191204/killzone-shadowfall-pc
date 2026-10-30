// Killzone Shadow Fall launcher page. kzsf_web.py describes the pages (state.pages) and answers the
// calls (window.pywebview.api); the preview (kzsf_web.py --preview) answers them itself.
'use strict';

const ICONS = {
  home: '<path d="M3 10.5 12 3l9 7.5"/><path d="M5 9v11h14V9"/><path d="M10 20v-6h4v6"/>',
  monitor: '<rect x="2.5" y="4" width="19" height="13" rx="2.5"/><path d="M8 21h8M12 17v4"/>',
  gamepad: '<path d="M7.5 7h9a5 5 0 0 1 4.9 6l-.8 3.9a2.6 2.6 0 0 1-4.5 1.2L14 16h-4l-2.1 2.1a2.6 2.6 0 0 1-4.5-1.2L2.6 13a5 5 0 0 1 4.9-6Z"/><path d="M8 10v4M6 12h4"/><circle cx="16" cy="11" r=".6"/><circle cx="17.5" cy="13" r=".6"/>',
  bolt: '<path d="M13 2 4 14h7l-1 8 9-12h-7l1-8Z"/>',
  sliders: '<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/>',
  save: '<path d="M5 3h11l3 3v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z"/><path d="M7 3v5h8V3M7 21v-7h10v7"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2.5"/><circle cx="9" cy="10" r="2"/><path d="m21 16-5-5-9 9"/>',
  file: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5M9 13h6M9 17h6"/>',
  check: '<circle cx="12" cy="12" r="9"/><path d="m8 12 3 3 5-6"/>',
  alert: '<path d="M12 3 2 20h20L12 3Z"/><path d="M12 10v4M12 17h0"/>',
  x: '<circle cx="12" cy="12" r="9"/><path d="m9 9 6 6M15 9l-6 6"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5h0"/>',
  reset: '<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/>',
  download: '<path d="M12 3v12M7 10l5 5 5-5M4 20h16"/>',
  heart: '<path d="M12 20s-7-4.4-9-9a4.8 4.8 0 0 1 9-3 4.8 4.8 0 0 1 9 3c-2 4.6-9 9-9 9Z"/>',
  chat: '<path d="M4 5h16v11H8l-4 4Z"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  users: '<circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><path d="M16 4.5a3.5 3.5 0 0 1 0 7M18 14a6 6 0 0 1 3.5 6"/>',
};

function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('fill', 'none');
  svg.setAttribute('stroke', 'currentColor');
  svg.setAttribute('stroke-width', '1.8');
  svg.setAttribute('stroke-linecap', 'round');
  svg.setAttribute('stroke-linejoin', 'round');
  svg.innerHTML = ICONS[name] || '';
  return svg;
}

// h('div.class', {attr}, children...)
function h(spec, attrs, ...children) {
  const [tag, ...classes] = spec.split('.');
  const el = document.createElement(tag || 'div');
  if (classes.length) el.className = classes.join(' ');
  if (attrs && (typeof attrs !== 'object' || attrs instanceof Node || Array.isArray(attrs))) {
    children.unshift(attrs);
    attrs = null;
  }
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'style') el.style.cssText = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const child of children.flat(Infinity)) {
    if (child == null || child === false) continue;
    el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return el;
}

// -- calls --------------------------------------------------------------------------------------

function previewApi() {
  const s = window.KZ_PREVIEW;
  const answers = {
    get_state: () => s,
    set: (key, value) => { s.values[key] = value; return value; },
    assign_key: (name, key) => s.reserved.includes(key) ? { ok: false } : (s.key_map[name] = key, { ok: true, key_map: s.key_map }),
    play: () => ({ playing: true }),
    cache_confirm_text: () => s.texts.clear_confirm,
    clear_cache: () => ({ ok: true }),
    choose_game: () => ({ game: s.game, art: s.art, icon: s.icon }),
    reset_controls: () => s, reset_all: () => s, set_language: () => s,
  };
  return new Proxy({}, { get: (_, name) => (...args) => Promise.resolve(answers[name] ? answers[name](...args) : null) });
}

const api = () => window.KZ_PREVIEW ? previewApi() : window.pywebview.api;

// -- state --------------------------------------------------------------------------------------

let S = null;
const ui = { page: 'home', sub: {}, status: null, cache: '', fsr4: null, capturing: null };
const T = (key) => (S.texts[key] ?? key);

function shown(item) {
  const cond = item.show_if;
  if (!cond) return true;
  const value = S.values[cond.key];
  if (cond.in) return cond.in.includes(value);
  if (cond.not) return !cond.not.includes(value);
  return true;
}

async function setValue(key, value) {
  S.values[key] = await api().set(key, value);
  if (S.pages.some(p => JSON.stringify(p).includes(`"show_if":{"key":"${key}"`))) renderPage();
}

// -- widgets ------------------------------------------------------------------------------------

function helpText(text) {
  if (!text) return null;
  const el = h('div.help', text);
  const more = h('button.more', { hidden: true, onclick: () => {
    el.classList.toggle('open');
    more.textContent = el.classList.contains('open') ? T('less') : T('more');
  } }, T('more'));
  requestAnimationFrame(() => { if (el.scrollHeight > el.clientHeight + 2) more.hidden = false; });
  return [el, more];
}

function selectControl(item) {
  const sel = h('select', { onchange: () => {
    const opt = item.options[sel.selectedIndex];
    setValue(item.key, opt[0]);
  } }, item.options.map(([v, label]) => h('option', { selected: String(S.values[item.key]) === String(v) }, label)));
  return h('div.select', sel);
}

function toggleControl(key) {
  const input = h('input', { type: 'checkbox', onchange: () => setValue(key, input.checked) });
  const v = S.values[key];
  input.checked = v === true || v === '1' || v === 1;
  return h('label.switch', input, h('span'));
}

function sliderControl(item) {
  const fmt = (v) => `${item.prefix || ''}${Number(v).toFixed(item.decimals || 0)}${item.unit || ''}`;
  const input = h('input', { type: 'range', min: item.min, max: item.max, step: item.step, value: S.values[item.key] });
  const pill = h('span.value-pill', fmt(input.value));
  const fill = () => input.style.setProperty('--fill', `${(input.value - item.min) * 100 / (item.max - item.min)}%`);
  let timer = null;
  input.addEventListener('input', () => {
    pill.textContent = fmt(input.value);
    fill();
    clearTimeout(timer);
    timer = setTimeout(() => setValue(item.key, Number(input.value)), 150);
  });
  fill();
  const reset = item.default != null ? h('button.icon-btn', { title: fmt(item.default), onclick: () => {
    input.value = item.default;
    input.dispatchEvent(new Event('input'));
  } }, icon('reset')) : null;
  return h('div.slider', input, pill, reset);
}

function fsr4Control() {
  const state = ui.fsr4 || (S.fsr4_ready ? { state: 'ready', text: T('fsr4_ready') } : { state: 'missing', text: T('fsr4_missing') });
  const chip = h(`span.status-chip${state.state === 'ready' ? '.ok' : state.state === 'error' ? '.error' : ''}`, state.text);
  const button = state.state === 'ready' || state.state === 'busy' ? null :
    h('button.btn', { onclick: () => { ui.fsr4 = { state: 'busy', text: '…' }; api().fsr4_download(); renderPage(); } },
      icon('download'), T('fsr4_download'));
  return [chip, button];
}

function row(item) {
  if (item.type === 'help') return h('div.row-help', helpText(item.help));
  let control;
  let wide = false;
  switch (item.type) {
    case 'select': control = selectControl(item); break;
    case 'toggle':
      control = [item.action ? h('button.btn.ghost', { onclick: () => action(item.action.id) }, item.action.label) : null,
                 toggleControl(item.key)];
      break;
    case 'slider': control = sliderControl(item); break;
    case 'fsr4': control = fsr4Control(); wide = true; break;
    case 'keys_info': control = h('div.keys-row', item.keys.map(k => h('span.keycap.static', k))); wide = true; break;
    case 'cache':
      control = [h('button.btn', { onclick: () => action('clear_cache') }, icon('trash'), item.button)];
      wide = true;
      return h('div.row.wide', h('div', h('div.row-label', item.label), helpText(ui.cacheText || '')), h('div.control', control));
    case 'button':
      control = h(`button.btn${item.danger ? '.danger' : ''}`, { onclick: () => action(item.id) }, icon(item.icon || 'reset'), item.button);
      return h('div.row.wide', h('div', h('div.row-label', item.label), helpText(item.help)), h('div.control', control));
  }
  if (item.type === 'toggle') wide = true;
  return h(`div.row${wide ? '.wide' : ''}`, h('div', h('div.row-label', item.label), helpText(item.help)), h('div.control', control));
}

function card(section) {
  const items = (section.items || []).filter(shown);
  const head = section.title ? h('div.card-head', h('div.card-title', section.title),
    section.subtitle ? h('div.card-sub', section.subtitle) : null) : null;
  const help = section.help ? h('div.card-help', helpText(section.help)) : null;
  const body = section.grid ? h('div.grid-toggles', items.map(row)) : items.map(row);
  return h('section.card', head, help, body);
}

// -- controls -----------------------------------------------------------------------------------

const KEY_CODES = {
  Space: 'Space', ShiftLeft: 'Left Shift', ShiftRight: 'Right Shift', ControlLeft: 'Left Ctrl', ControlRight: 'Right Ctrl',
  AltLeft: 'Left Alt', AltRight: 'Right Alt', Enter: 'Return', Tab: 'Tab', Backspace: 'Backspace', CapsLock: 'CapsLock',
  Comma: ',', Period: '.', Slash: '/', Semicolon: ';', Quote: "'", BracketLeft: '[', BracketRight: ']', Minus: '-',
  Equal: '=', Backquote: '`', Backslash: '\\', Home: 'Home', End: 'End', PageUp: 'PageUp', PageDown: 'PageDown',
  ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right', Insert: 'Insert', Delete: 'Delete',
  Escape: 'Escape', NumpadEnter: 'Keypad Enter', NumpadAdd: 'Keypad +', NumpadSubtract: 'Keypad -',
  NumpadMultiply: 'Keypad *', NumpadDivide: 'Keypad /', NumpadDecimal: 'Keypad .',
};
const MOUSE = ['Mouse Left', 'Mouse Middle', 'Mouse Right', 'Mouse 4', 'Mouse 5'];

function sdlKey(code) {
  if (KEY_CODES[code]) return KEY_CODES[code];
  let m;
  if ((m = /^Key([A-Z])$/.exec(code))) return m[1];
  if ((m = /^Digit(\d)$/.exec(code))) return m[1];
  if ((m = /^Numpad(\d)$/.exec(code))) return `Keypad ${m[1]}`;
  if ((m = /^F(\d{1,2})$/.exec(code)) && +m[1] >= 1 && +m[1] <= 12) return code;
  return null;
}

function psBadge(b) {
  const cls = { cross: '.cross', circle: '.circle', square: '.square', triangle: '.triangle' }[b.name] || '';
  const label = { options: 'OPT', touchpad_left: 'TP', touchpad_right: 'TP', move_forward: '↑', move_back: '↓',
                  move_left: '←', move_right: '→', swipe_up: 'TP↑', swipe_down: 'TP↓', swipe_left: 'TP←',
                  swipe_right: 'TP→' }[b.name] || b.label;
  return h(`span.ps${cls}${label.length > 2 ? '.wide' : ''}`, label);
}

function keysSection(section) {
  return [section.help ? h('div.banner.info', icon('info'), h('div', section.help)) : null,
    h('div.keys-grid', section.groups.map(group => h('section.card', h('div.card-head', h('div.card-title', group.title)),
      group.buttons.map(b => {
        const key = S.key_map[b.name] || b.key;
        const cap = h(`button.keycap${ui.capturing === b.name ? '.capturing' : ''}`, { 'data-name': b.name,
          onclick: (e) => { e.stopPropagation(); startCapture(b.name); } }, ui.capturing === b.name ? T('press_key') : key || '—');
        return h('div.bind', psBadge(b), h('span.bind-name', (S.texts.buttons || {})[b.name] || b.label), cap);
      }))))];
}

function padsSection(section) {
  return [section.help ? h('div.banner.info', icon('info'), h('div', section.help)) : null,
    h('div.keys-grid', section.groups.map(group => h('section.card', h('div.card-head', h('div.card-title', group.title)),
      group.buttons.map(b => {
        let control;
        if (b.fixed_pad) control = h('span.fixed', b.fixed_pad);
        else {
          const current = b.name in S.pad_map ? S.pad_map[b.name] : b.pad;
          const sel = h('select', { onchange: () => {
            const value = section.options[sel.selectedIndex][0];
            S.pad_map[b.name] = value;
            api().set_pad(b.name, value);
          } }, section.options.map(([v, label]) => h('option', { selected: v === current }, label)));
          control = h('div.select', sel);
        }
        return h('div.bind', psBadge(b), h('span.bind-name', (S.texts.buttons || {})[b.name] || b.label), control);
      }))))];
}

function startCapture(name) {
  ui.capturing = name;
  renderPage();
  // Armed once this click is over, or it would be taken as the new binding.
  setTimeout(() => { ui.armed = true; }, 150);
}

function endCapture() {
  ui.capturing = null;
  ui.armed = false;
  renderPage();
}

async function assign(key) {
  const name = ui.capturing;
  if (!name) return;
  const result = await api().assign_key(name, key);
  if (!result || !result.ok) {
    const cap = document.querySelector(`.keycap[data-name="${name}"]`);
    if (cap) { cap.classList.remove('denied'); void cap.offsetWidth; cap.classList.add('denied'); }
    toast(T('reserved_key'));
    return;
  }
  S.key_map = result.key_map;
  endCapture();
}

document.addEventListener('keydown', (e) => {
  if (!ui.capturing) return;
  e.preventDefault();
  e.stopPropagation();
  if (e.code === 'Escape') { endCapture(); return; }
  assign(sdlKey(e.code));
}, true);
document.addEventListener('mousedown', (e) => {
  if (!ui.capturing || !ui.armed) return;
  e.preventDefault();
  e.stopPropagation();
  assign(MOUSE[e.button]);
}, true);
document.addEventListener('wheel', (e) => {
  if (!ui.capturing || !ui.armed) return;
  e.preventDefault();
  assign(e.deltaY < 0 ? 'Wheel Up' : 'Wheel Down');
}, { capture: true, passive: false });
document.addEventListener('contextmenu', (e) => e.preventDefault());

// -- multiplayer (beta) -------------------------------------------------------------------------

function textInput(value, attrs) {
  return h('input.text', { type: 'text', value: value || '', spellcheck: 'false', autocomplete: 'off', ...attrs });
}

function onlineAnswer(r) {
  if (!r) return;
  if (r.error) message(r.error);
  const { error, ...info } = r;
  if (info.network || info.missing) S.online = info;
  renderPage();
}

function onlinePage() {
  const o = S.online || {};
  const out = [h('div.banner.warn', icon('alert'), h('div', T('online_warning')))];
  if (o.missing) return [...out, h('div.banner.warn', icon('x'), h('div', o.missing))];
  const net = o.network || {};
  const level = { radmin: 'ok', tailscale: 'ok', lan: 'warn' }[net.kind] || 'error';
  out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_network'))),
    h(`div.game-state.${level}`, icon(level === 'ok' ? 'check' : level === 'warn' ? 'alert' : 'x'), h('div', net.text)),
    h('div.card-help', helpText(T('online_net_help'))),
    h('div.page-actions.inline',
      h('button.btn.ghost', { onclick: () => api().online_info().then(onlineAnswer) }, icon('reset'), T('online_refresh')),
      h('button.btn', { onclick: () => api().open_url('radmin') }, icon('download'), T('online_get_radmin')))));
  const busy = o.busy || ui.onlineBusy;
  if (o.room) {
    const room = o.room;
    out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_room'))),
      h('div.room',
        h('div.room-code', room.code),
        h('button.btn', { onclick: () => { navigator.clipboard.writeText(room.code).then(() => toast(T('online_copied'))); } },
          icon('file'), T('online_copy'))),
      h('div.card-help', h('div.help', T(room.host ? 'online_you_host' : 'online_you_join').replace('{nickname}', room.nickname))),
      room.host ? [h('div.card-help', h('div.help', T('online_send'))),
        h('div.page-actions.inline', h('button.btn', { onclick: async () => {
          const r = await api().online_save_connection();
          if (r && r.text) toast(r.text);
        } }, icon('save'), T('online_save_connection')))] : null,
      h('div.page-actions.inline',
        h('button.btn.primary', { disabled: S.playing, onclick: async () => {
          const r = await api().online_play();
          if (r && r.error) { if (r.page) { ui.page = r.page; renderPage(); } message(r.error); return; }
          if (r && r.playing) { S.playing = true; renderBar(); renderPage(); }
        } }, T('online_play')),
        h('button.btn.danger', { disabled: S.playing, onclick: () => api().online_close().then(onlineAnswer) },
          icon('x'), T(room.host ? 'online_close' : 'online_leave')))));
    out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_steps'))),
      h('div.card-help', h('div.help', T('online_steps_help')))));
    return out;
  }
  const nick = textInput(o.nickname, { maxlength: 16, placeholder: 'Lesca' });
  out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_nickname'))),
    h('div.folder', nick), h('div.card-help', { style: 'padding-top:8px' }, helpText(T('online_nickname_help')))));
  const start = (call) => async () => {
    ui.onlineBusy = true;
    renderPage();
    onlineAnswer(await call());
  };
  out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_host'))),
    h('div.card-help', helpText(T('online_host_help'))),
    h('div.page-actions.inline', h('button.btn.primary', { disabled: busy,
      onclick: start(() => api().online_host(nick.value.trim())) }, busy ? T('online_working') : T('online_host_button')))));
  const code = textInput(ui.onlineCode, { maxlength: 16, placeholder: 'ABCD1234', style: 'text-transform:uppercase',
    oninput: () => { ui.onlineCode = code.value; } });
  out.push(h('section.card', h('div.card-head', h('div.card-title', T('online_join'))),
    h('div.card-help', helpText(T('online_join_help'))),
    h('div.folder', h('div.folder-path', { title: o.connection || '' }, o.connection || T('online_connection')),
      h('button.btn', { onclick: () => api().online_choose_connection().then(onlineAnswer) }, icon('folder'), T('online_choose'))),
    h('div.folder', code,
      h('button.btn.primary', { disabled: busy, onclick: start(() => api().online_join(code.value.trim(), nick.value.trim())) },
        busy ? T('online_working') : T('online_join_button')))));
  return out;
}

// -- pages --------------------------------------------------------------------------------------

function homePage() {
  const g = S.game;
  const hero = h('div.hero', S.art ? h('div.hero-img', { style: `background-image:url(${S.art})` }) : null,
    h('div.hero-text', h('h1.hero-title', 'KILLZONE'), h('div.hero-sub', 'SHADOW FALL'),
      h('div.hero-tag', h('b', 'PC'), T('home_sub'))));
  const stateIcon = { ok: 'check', warn: 'alert', error: 'x' }[g.level];
  const folder = h('section.card', h('div.card-head', h('div.card-title', T('game_folder'))),
    h('div.folder', h('div.folder-path', { title: g.game_dir || '' }, g.game_dir || '—'),
      h('button.btn', { onclick: () => chooseFolder('choose_game') }, icon('folder'), T('browse'))),
    h('div.card-help', { style: 'padding-top:8px;padding-bottom:0' }, helpText(T('game_folder_help'))),
    g.needs_update ? [h('div.card-head', h('div.card-title', T('update_folder'))),
      h('div.folder', h('div.folder-path', g.update_dir || '—'),
        h('button.btn', { onclick: () => chooseFolder('choose_update') }, icon('folder'), T('browse'))),
      h('div.card-help', { style: 'padding-top:8px;padding-bottom:0' }, helpText(T('update_folder_help')))] : null,
    h(`div.game-state.${g.level}`, icon(stateIcon), h('div', g.message)));
  const quick = h('section.card', h('div.card-head', h('div.card-title', T('quick_links'))),
    h('div.quick',
      h('button', { onclick: () => api().open('saves') }, icon('save'), T('open_saves')),
      h('button', { onclick: () => api().open('shots') }, icon('image'), T('open_shots')),
      h('button', { onclick: () => api().open('logs') }, icon('file'), T('open_logs'))));
  return [hero, folder, quick, h('div.page-note', T('home_hint'))];
}

function settingsPage(page) {
  const out = [];
  let sections = page.sections;
  if (page.tabs) {
    const current = ui.sub[page.id] || page.tabs[0].id;
    sections = page.tabs.find(t => t.id === current).sections;
  }
  if (page.warning) out.push(h('div.banner.warn', icon('alert'), h('div', page.warning)));
  for (const section of sections) {
    if (section.type === 'keys') out.push(keysSection(section));
    else if (section.type === 'pads') out.push(padsSection(section));
    else out.push(card(section));
  }
  if (page.actions) out.push(h('div.page-actions', page.actions.map(a =>
    h('button.btn.ghost', { onclick: () => action(a.id) }, icon('reset'), a.label))));
  if (page.note) out.push(h('div.page-note', page.note));
  return out;
}

function renderPage() {
  const scroll = document.querySelector('.scroll');
  if (!scroll) return render();
  const top = scroll.scrollTop;
  const page = S.pages.find(p => p.id === ui.page);
  const head = h('div.page-head', h('h1.page-title', page.type === 'home' ? '' : page.title),
    page.tabs ? h('div.subtabs', page.tabs.map(t => h(`button${(ui.sub[page.id] || page.tabs[0].id) === t.id ? '.active' : ''}`,
      { onclick: () => { ui.sub[page.id] = t.id; ui.capturing = null; renderPage(); } }, t.title))) : null);
  const update = S.update ? h('div.banner.update', icon('download'), h('div.grow', S.update.text),
    h('button.btn.ghost', { onclick: () => api().open_url('notes') }, T('update_notes')),
    h('button.btn.ghost', { onclick: () => { S.update = null; renderPage(); } }, T('update_later')),
    h('button.btn.primary', { onclick: () => action('install_update') }, T('update_now'))) : null;
  const content = h('div.content', page.type === 'home' ? null : head, update,
    page.type === 'home' ? homePage() : page.type === 'online' ? onlinePage() : settingsPage(page));
  scroll.replaceChildren(content);
  scroll.scrollTop = top;
  document.querySelectorAll('.nav-item').forEach(n => n.classList.toggle('active', n.dataset.page === ui.page));
  renderBar();
}

function renderBar() {
  const bar = document.querySelector('.actionbar');
  if (!bar) return;
  const g = S.game;
  let text = g.message;
  let dot = g.level;
  if (ui.status && ui.status.text) { text = ui.status.text; dot = ui.status.busy ? 'busy' : 'ok'; }
  else if (S.playing) { text = T('preparing'); dot = 'busy'; }
  bar.replaceChildren(
    h('div.status', h(`span.dot.${dot}`), h('span', text)),
    h('button.play', { disabled: S.playing || !g.ok || (ui.status && ui.status.busy), onclick: play },
      S.playing ? T('playing') : T('play')));
}

const theme = () => S.values.launcher_theme || 'vektan';

async function setTheme(id) {
  if (id === theme()) return;
  S.values.launcher_theme = id;
  document.documentElement.dataset.theme = id;
  render();
  S.values.launcher_theme = await api().set('launcher_theme', id);
}

function render() {
  document.documentElement.dataset.theme = theme();
  const app = document.getElementById('app');
  const nav = h('nav.nav', S.pages.map(p => h(`button.nav-item${p.id === ui.page ? '.active' : ''}`,
    { 'data-page': p.id, onclick: () => { ui.page = p.id; ui.capturing = null; document.querySelector('.scroll').scrollTop = 0; renderPage(); } },
    icon(p.icon), p.title, p.badge ? h('span.nav-badge', p.badge) : null)));
  const side = h('aside.sidebar',
    h('div.brand', h('div.brand-icon', { style: S.icon ? `background-image:url(${S.icon})` : '' }),
      h('div.brand-name', 'KILLZONE', h('small', 'SHADOW FALL'))),
    nav,
    h('div.sidebar-foot',
      h('div.foot-label', T('theme')),
      h('div.lang.theme', [['vektan', 'Vektan'], ['helghast', 'Helghast']].map(([id, name]) =>
        h(`button${theme() === id ? '.active' : ''}`, { onclick: () => setTheme(id) }, h(`span.swatch.${id}`), name))),
      h('div.lang', ['es', 'en'].map(code => h(`button${S.lang === code ? '.active' : ''}`,
        { onclick: () => setLanguage(code) }, code.toUpperCase()))),
      h('div.foot-label', T('support')),
      h('div.links',
        h('button.link-btn.paypal', { title: T('donate'), onclick: () => api().open_url('donate') }, icon('heart'), 'PayPal'),
        h('button.link-btn.discord', { title: T('discord_join'), onclick: () => api().open_url('discord') }, icon('chat'), 'Discord')),
      h('div.version', h('b', 'Lesca'), ` · v${S.version}`)));
  const main = h('main.main', h('div.scroll'), h('footer.actionbar'));
  app.replaceChildren(side, main);
  renderPage();
}

// -- actions ------------------------------------------------------------------------------------

function modal(text, buttons) {
  return new Promise(resolve => {
    const root = document.getElementById('modal-root');
    const close = (value) => { root.replaceChildren(); resolve(value); };
    root.replaceChildren(h('div.modal-back', { onclick: (e) => { if (e.target === e.currentTarget) close(null); } },
      h('div.modal', h('p', text), h('div.buttons', buttons.map(b =>
        h(`button.btn${b.primary ? '.primary' : b.danger ? '.danger' : ''}`, { onclick: () => close(b.value) }, b.label))))));
  });
}

const confirmBox = (text) => modal(text, [{ label: T('cancel'), value: false }, { label: T('yes'), value: true, primary: true }]);
const message = (text) => modal(text, [{ label: T('ok'), value: true, primary: true }]);

function toast(text) {
  document.querySelectorAll('.toast').forEach(t => t.remove());
  const el = h('div.toast', text);
  document.body.append(el);
  setTimeout(() => el.remove(), 2500);
}

async function action(id) {
  switch (id) {
    case 'clear_cache': {
      if (!await confirmBox(await api().cache_confirm_text())) return;
      const r = await api().clear_cache();
      if (r && r.error) message(r.error); else toast(T('cleared'));
      return;
    }
    case 'reset_all':
      if (!await confirmBox(T('reset_all_confirm'))) return;
      S = await api().reset_all();
      return renderPage();
    case 'reset_controls':
      S = await api().reset_controls();
      return renderPage();
    case 'check_updates_now':
      return api().check_updates(false);
    case 'open_bin':
      return api().open_bin();
    case 'create_shortcut': {
      const r = await api().create_shortcut();
      if (r && r.error) message(r.error); else if (r) toast(r.text);
      return;
    }
    case 'install_update': {
      if (!await confirmBox(S.update.confirm)) return;
      const r = await api().install_update();
      if (r && r.error) message(r.error);
      return;
    }
  }
}

async function chooseFolder(call) {
  const r = await api()[call]();
  if (!r) return;
  S.game = r.game;
  if ('art' in r) { S.art = r.art; S.icon = r.icon; return render(); }
  renderPage();
}

async function setLanguage(code) {
  if (code === S.lang) return;
  S = await api().set_language(code);
  document.documentElement.lang = code;
  render();
}

async function play() {
  const r = await api().play();
  if (r && r.error) {
    if (r.page) { ui.page = r.page; renderPage(); }
    message(r.error);
    return;
  }
  if (r && r.playing) { S.playing = true; renderBar(); }
}

// Events from kzsf_web.py (Api._emit).
window.kz = {
  event(kind, data) {
    if (!S) return;
    switch (kind) {
      case 'cache': ui.cache = data.size; ui.cacheText = data.text; if (ui.page === 'advanced') renderPage(); break;
      case 'fsr4': ui.fsr4 = data; if (data.state === 'ready') S.fsr4_ready = true; renderPage(); break;
      case 'update': S.update = data; renderPage(); break;
      case 'status': ui.status = data; renderBar(); break;
      case 'playing': S.playing = data.playing; renderBar(); if (ui.page === 'online') renderPage(); break;
      case 'online':
        ui.onlineBusy = false;
        if (data.error) message(data.error);
        { const { error, ...info } = data; S.online = info; }
        if (ui.page === 'online') renderPage();
        break;
      case 'state': { const playing = S.playing; S = data; S.playing = playing; render(); break; }
      case 'message':
        if (data.logs) modal(data.text, [{ label: T('close'), value: false }, { label: T('open_logs_q'), value: true, primary: true }])
          .then(v => v && api().open('run_logs'));
        else message(data.text);
        break;
    }
  },
};

async function start() {
  S = await api().get_state();
  document.documentElement.lang = S.lang;
  render();
  api().ready();
}

if (window.KZ_PREVIEW) start();
else if (window.pywebview && window.pywebview.api && window.pywebview.api.get_state) start();
else window.addEventListener('pywebviewready', start, { once: true });

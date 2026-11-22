// The Shelf for Heltec V4 (and V3): small touches on top of The Shelf's own page.
(function () {
  async function boardInfo() {
    try { return await fetch('/api/board/info').then(r => r.json()); } catch { return null; }
  }
  function fmt(b) { return b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.round(b / 1e3) + ' KB'; }
  async function decorate() {
    const info = await boardInfo();
    // On its own Wi-Fi the board has no internet clock: lend it this device's time.
    // the board has no clock of its own on its own Wi-Fi, and doesn't know your time zone: tell it (for its screen)
    const tz = -new Date().getTimezoneOffset();
    if (info && (!info.timeSet || info.tz !== tz)) fetch('/api/board/time', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ms: Date.now(), tz }) }).catch(() => {});
    const p = document.querySelector('.brand-text p');
    if (p) p.textContent = 'running on a Heltec V4';
    const net = document.getElementById('networkUrl');
    if (net && info) net.textContent = (info.hotspot || (info.ap && !info.ssid)) ? info.ip : info.host + '.local';
    const scroll = document.querySelector('.sidebar-scroll');
    if (scroll && !document.querySelector('.board-badge')) {
      const a = document.createElement('a');
      a.className = 'board-badge'; a.href = '/board/';
      a.innerHTML = '<span class="chip">V4</span><span><b>Heltec V4</b><small>' +
        (info ? fmt(info.total - info.used) + ' free on ' + (info.storage === 'microSD card' ? 'microSD' : 'the board') : 'Board settings') +
        ' · Wi-Fi &amp; settings</small></span>';
      scroll.appendChild(a);
    }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', decorate); else decorate();
})();

// ---- Read-only mode: a switch in the sidebar, and an unlock prompt when a change is refused ----
(function () {
  const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const _fetch = window.fetch.bind(window);
  let A = null, dialogOpen = false, dismissedAt = 0;
  const post = (u, d) => _fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(d || {}) });

  // A small password dialog. onSubmit returns an error message, or nothing when it worked.
  window.shelfAskPassword = function (title, sub, button, onSubmit) {
    const ov = document.createElement('div');
    ov.className = 'rm-overlay';
    ov.innerHTML = `<form class="rm-box rm-small"><h3>${esc(title)}</h3><p class="rm-sub">${sub}</p>
      <input type="password" class="rm-input" placeholder="Admin password" autocomplete="current-password">
      <div class="rm-err"></div>
      <div class="rm-row"><button type="button" class="coms-btn secondary rm-cancel">Cancel</button><button type="submit" class="coms-btn">${esc(button)}</button></div></form>`;
    document.body.appendChild(ov);
    const input = ov.querySelector('input'), err = ov.querySelector('.rm-err');
    const close = () => { ov.remove(); };
    ov.querySelector('.rm-cancel').onclick = close;
    ov.addEventListener('mousedown', e => { if (e.target === ov) close(); });
    ov.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
    ov.querySelector('form').onsubmit = async e => {
      e.preventDefault();
      err.textContent = 'Checking…';
      const msg = await onSubmit(input.value);
      if (msg) { err.textContent = msg; input.select(); } else close();
    };
    setTimeout(() => input.focus(), 30);
    return close;
  };

  // The switches at the top of the sidebar, like The Shelf on a computer: read-only mode, and
  // admin write mode in this browser. (Folder locks add their own switch below these.)
  function toggleRow(id, onChange) {
    let row = document.getElementById(id);
    if (row) return row;
    const group = document.querySelector('.power-toggle-group');
    if (!group) return null;
    row = document.createElement('div');
    row.className = 'power-toggle'; row.id = id;
    row.innerHTML = '<label class="toggle-switch"><input type="checkbox"><span class="toggle-slider"></span></label><span class="bt-label"></span>';
    const ro = document.getElementById('bdRoRow');
    if (id === 'bdAdminRow' && ro) ro.after(row); else group.prepend(row);
    row.querySelector('input').addEventListener('change', e => onChange(e.target.checked, e.target));
    return row;
  }
  function askPw(title, sub, button, url, body) {
    window.shelfAskPassword(title, sub, button, async pw => {
      const r = await post(url, Object.assign({ password: pw }, body || {}));
      if (!r.ok) { const j = await r.json().catch(() => ({})); return j.error || 'That didn’t work.'; }
      await afterChange(); return '';
    });
    const obs = new MutationObserver(() => { if (!document.querySelector('.rm-overlay')) { obs.disconnect(); load(); } });
    obs.observe(document.body, { childList: true });
  }
  async function afterChange() {
    await load();
    if (typeof loadAuthStatus === 'function') loadAuthStatus();
    if (window.shelfLocksRefresh) window.shelfLocksRefresh();
  }
  function renderSwitches() {
    // (Read-only mode, which stops changes, is on the board's settings page. The switch here is
    // Read control, from locks.js: chosen shelves need a password to look at.)
    const old = document.getElementById('bdRoRow'); if (old) old.remove();
    const ad = A.adminSet ? toggleRow('bdAdminRow', async (on, cb) => {
      if (on) { cb.checked = false; unlock(false); }
      else { await lock(); if (window.shelfLocksRefresh) window.shelfLocksRefresh(); }
    }) : null;
    if (ad) {
      ad.hidden = false;
      ad.querySelector('input').checked = !!A.unlocked;
      ad.querySelector('.bt-label').textContent = A.unlocked ? 'Admin: unlocked here' : 'Admin: locked';
      ad.title = 'Unlock with the admin password to make changes and set Read control from this browser (8 hours)';
    } else { const r = document.getElementById('bdAdminRow'); if (r) r.hidden = true; }
  }

  async function load() {
    try { A = await _fetch('/api/admin/status').then(r => r.json()); } catch { return; }
    renderSwitches();
    let pill = document.getElementById('roPill');
    const badge = document.querySelector('.board-badge');
    if (true) { if (pill) pill.remove(); return; }   // (the switches at the top of the sidebar do this now)
    if (!pill) {
      if (!badge) return;
      pill = document.createElement('button');
      pill.id = 'roPill'; pill.className = 'ro-pill';
      badge.insertAdjacentElement('afterend', pill);
      pill.addEventListener('click', () => A.unlocked ? lock() : unlock(false));
    }
    pill.classList.toggle('open', !!A.unlocked);
    pill.innerHTML = A.unlocked
      ? '<span class="ro-dot"></span><span><b>Write mode</b><small>Unlocked in this browser · tap to lock</small></span>'
      : '<span class="ro-dot"></span><span><b>Read-only</b><small>Tap and enter the admin password to make changes</small></span>';
  }
  async function lock() { await post('/api/admin/lock'); await afterChange(); }
  function unlock(blocked) {
    if (dialogOpen) return;
    dialogOpen = true;
    const closeOrig = window.shelfAskPassword(blocked ? 'This Shelf is read-only' : 'Unlock write mode',
      (blocked ? 'That change wasn’t saved. ' : '') + 'Enter the admin password to make changes from this browser for the next 8 hours.' + (blocked ? ' Then try it again.' : ''),
      'Unlock', async pw => {
        const r = await post('/api/admin/unlock', { password: pw });
        if (!r.ok) { const j = await r.json().catch(() => ({})); return j.error || 'Couldn’t unlock.'; }
        dialogOpen = false; await afterChange();
        return '';
      });
    const obs = new MutationObserver(() => { if (!document.querySelector('.rm-overlay')) { dialogOpen = false; dismissedAt = Date.now(); obs.disconnect(); } });
    obs.observe(document.body, { childList: true });
    void closeOrig;
  }
  function refused() { if (Date.now() - dismissedAt > 2500) unlock(true); }
  // Any refused change (fetch or upload) brings up the unlock prompt.
  // The Shelf's upload list only says "failed": show why, from the board's answer.
  function explainUpload(why) {
    setTimeout(() => {
      const st = [...document.querySelectorAll('#uploadList .status.err')].find(x => !x.dataset.why);
      if (!st) return;
      st.dataset.why = '1';
      st.title = why;
      const m = document.createElement('div');
      m.className = 'upload-why'; m.textContent = why;
      st.parentElement.appendChild(m);
    }, 60);
  }
  window.fetch = async function (...args) {
    const isUpload = String(args[0]).includes('/api/upload');
    let r;
    try { r = await _fetch(...args); }
    catch (e) { if (isUpload) explainUpload('The connection to the board dropped partway through. Try again, closer to the board or one file at a time.'); throw e; }
    if (r.status === 403 && r.headers.get('X-Shelf-ReadOnly')) { refused(); load(); }
    else if (isUpload && !r.ok) r.clone().json().then(j => explainUpload(j.error || ('The board answered ' + r.status))).catch(() => explainUpload('The board answered ' + r.status));
    return r;
  };
  const send = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.send = function (...a) {
    this.addEventListener('load', () => { if (this.status === 403 && this.getResponseHeader('X-Shelf-ReadOnly')) { refused(); load(); } });
    return send.apply(this, a);
  };
  window.shelfReadOnlyRefresh = load;
  const start = () => { load(); setInterval(load, 20000); };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => setTimeout(start, 300)); else setTimeout(start, 300);
})();

// ---- Meshed Comms: a simple text mesh between Shelf boards (900 MHz LoRa and 2.4 GHz) ----
(function () {
  let timer = null, last = '', drafting = false;
  const LEVEL_NAMES = ['Low', 'Normal', 'High'];
  const BAND_NAME = 'Meshed Comms';
  const NOLORA = false;                            // a plain ESP32: no LoRa radio, 2.4 GHz only (pack_www sets it)
  const boardLabel = b => (+b === 32 ? 'ESP32' : 'V' + (b || '?'));
  const sig = r => (r ? r + ' dBm' : '2.4 GHz');   // 0: heard over 2.4 GHz (no signal reading there)
  const ago = t => { const s = Math.max(0, (Date.now() - t) / 1000); return s < 60 ? 'just now' : s < 3600 ? Math.floor(s / 60) + ' min ago' : s < 86400 ? Math.floor(s / 3600) + ' h ago' : new Date(t).toLocaleDateString(); };
  const time = t => new Date(t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  const post = (u, d) => fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(d || {}) });

  function viewing() { return typeof state !== 'undefined' && state.view === 'coms'; }

  async function refresh(force) {
    if (!viewing()) { clearInterval(timer); timer = null; return; }
    let c; try { c = await fetch('/api/coms').then(r => r.json()); } catch { return; }
    const sig = JSON.stringify([c.enabled, c.radio, c.receipts, c.bandLora, c.bandWifi, c.lora, c.wifi, c.log.length, c.log.length && c.log[c.log.length - 1].id, c.neighbors.length, c.error, c.heard, c.relayed, c.synced, c.receivedBy,
      (c.devices || []).map(d => [d.node, d.name, d.hops, d.batt, d.ro, d.admin, Math.floor((Date.now() - d.at) / 60000)])]);
    if (!force && sig === last) return;
    last = sig;
    render(c);
  }

  function render(c) {
    const content = el('#content');
    const keep = content.querySelector('#comsInput');
    const draft = keep ? keep.value : '';
    const hadFocus = keep && document.activeElement === keep;
    const neigh = c.neighbors.filter(n => n.node !== c.node);
    const devs = (c.devices || []).filter(d => d.node !== c.node).sort((a, b) => b.at - a.at);
    const log = c.log.slice().sort((a, b) => a.t - b.t);
    let day = '';
    const msgs = log.map(m => {
      const d = new Date(m.t).toDateString(); let sep = '';
      if (d !== day) { day = d; sep = `<div class="coms-day">${escapeHtml(d === new Date().toDateString() ? 'Today' : new Date(m.t).toLocaleDateString([], { weekday: 'long', month: 'short', day: 'numeric' }))}</div>`; }
      const got = (c.receivedBy || {})[m.id] || [];
      const meta = m.mine ? `${time(m.t)} · ${got.length ? '\u2713 Received by ' + got.map(escapeHtml).join(', ') : 'sent'}` : `${time(m.t)} · ${m.hops ? m.hops + (m.hops === 1 ? ' hop' : ' hops') : 'direct'}${m.rssi !== undefined ? ' · ' + sig(m.rssi) : ''}${m.synced ? ' · caught up' : ''}`;
      return sep + `<div class="coms-msg ${m.mine ? 'mine' : ''}"><div class="who">${escapeHtml(m.mine ? 'You' : (m.from || 'Unknown'))}</div><div class="txt">${escapeHtml(m.text)}</div><div class="meta">${meta}</div></div>`;
    }).join('');
    content.innerHTML = `
      <div class="coms">
        <div class="coms-card">
          <div class="coms-top">
            <label class="toggle-switch"><input type="checkbox" id="comsToggle" ${c.enabled ? 'checked' : ''}><span class="toggle-slider"></span></label>
            <span class="power-label">${c.enabled ? (c.radio ? BAND_NAME + ' is on' : 'Starting the radio…') : BAND_NAME + ' is off'}</span>
            <span class="sub">${c.enabled ? 'This board is on the mesh and relays messages for everyone else on it.' : 'Turn it on to join the mesh. Any Shelf board with Coms on can talk, and each one relays for the others.'}</span>
          </div>
          ${c.enabled ? `
          <div class="coms-chips">
            <span class="coms-chip">You are <b>${escapeHtml(c.name)}</b></span>
            ${c.hasLora === false ? `<span class="coms-chip" title="Boards hear each other over 2.4 GHz only on the same Wi-Fi channel">2.4 GHz${c.wifi ? ' · channel ' + c.channel : ' (off)'}</span>` : `
            <button class="coms-chip coms-band ${c.bandLora !== false ? 'on' : ''}" id="comsBandLora" title="Tap to switch the 900 MHz LoRa radio ${c.bandLora !== false ? 'off' : 'on'}">${c.bandLora !== false ? '\u2713' : '\u2715'} ${c.freq.toFixed(1)} MHz${c.bandLora !== false && !c.lora ? ' (starting)' : ''}</button>
            <button class="coms-chip coms-band ${c.bandWifi !== false ? 'on' : ''}" id="comsBandWifi" title="Tap to switch 2.4 GHz ${c.bandWifi !== false ? 'off' : 'on'}. Boards hear each other over 2.4 GHz only on the same Wi-Fi channel">${c.bandWifi !== false ? '\u2713' : '\u2715'} 2.4 GHz${c.bandWifi !== false && c.wifi ? ' \u00b7 channel ' + c.channel : ''}</button>`}
            <span class="coms-chip">Heard <b>${c.heard}</b></span>
            <span class="coms-chip">Relayed <b>${c.relayed}</b></span>
            <span class="coms-chip">Caught up <b>${c.synced}</b></span>
            <button class="coms-chip coms-receipts ${c.receipts ? 'on' : ''}" id="comsReceipts" title="When on, this board tells senders it got their message">${c.receipts ? '\u2713 Sending \u201creceived\u201d' : 'Not sending \u201creceived\u201d'}</button>
          </div>
          <div class="coms-devs-head"><span>Boards on the mesh</span><button class="coms-chip coms-who" id="comsWho" title="Ask every board on the mesh to say hello">Refresh</button></div>
          <div class="coms-devs">${devs.length ? devs.map(d => `<button class="coms-dev" data-node="${d.node}">
              <span class="cd-chip">${boardLabel(d.board)}</span>
              <span class="cd-main"><b>${escapeHtml(d.name || d.node)}</b><small>${d.hops ? d.hops + (d.hops === 1 ? ' hop' : ' hops') : 'direct · ' + sig(d.rssi)} · ${d.batt < 0 ? 'USB' : d.batt + '%'} · ${ago(d.at)}${d.ro ? ' · 🔒' : ''}</small></span>
              <span class="cd-go">${d.admin ? 'Manage ›' : '<span class="cd-none" title="Set an admin password on that board to manage it from here">No password</span>'}</span></button>`).join('')
            : '<div class="coms-neigh" style="margin:0">No boards yet. Each board says hello when it comes on and every 15 minutes. Tap Refresh to ask now.</div>'}</div>
          <div class="coms-neigh">${neigh.length ? 'In range: ' + neigh.map(n => `<b style="color:var(--text)">${escapeHtml(n.name || n.node)}</b> (${sig(n.rssi)}, ${ago(n.at)})`).join(', ') : 'No other boards heard yet. They show up here as soon as one transmits.'}</div>
          <details style="margin-top:10px"><summary style="cursor:pointer;color:var(--text-muted);font-size:13px">Settings</summary>
            <div class="coms-settings">
              <label>Your name on the mesh<input id="comsName" maxlength="16" value="${escapeHtml(c.name)}"></label>
              ${c.hasLora === false ? '' : `<label>Transmit power<select id="comsLevel">${LEVEL_NAMES.map((l, i) => `<option value="${i}" ${i === c.level ? 'selected' : ''}>${l} (~${(c.levels || [11, 20, 27])[i]} dBm)</option>`).join('')}</select></label>`}
              <button class="coms-btn secondary" id="comsSave">Save</button>
              <button class="coms-btn secondary" id="comsClear">Clear log</button>
            </div>
          </details>` : ''}
          ${c.error ? `<div class="coms-err">${escapeHtml(c.error)}</div>` : ''}
        </div>
        ${c.enabled ? `
          <div class="coms-log" id="comsLog">${msgs || '<div class="coms-off" style="margin:auto;text-align:center">No messages in the last while.<br>Anything said on the mesh shows up here, and when this board joins, nearby boards fill in the last 12 hours.</div>'}</div>
          <form class="coms-compose" id="comsForm"><input id="comsInput" maxlength="180" placeholder="Message everyone on the mesh…" autocomplete="off"><button type="submit">Send</button></form>
          <div class="coms-count" id="comsCount"></div>
        ` : `<div class="coms-card coms-off">
            ${NOLORA ? `<b>How it works:</b> Shelf boards talk to each other directly over 2.4 GHz (ESP-NOW, the ESP32's own board-to-board radio frames), with no router in between. Phones can stay joined to a board's own Wi-Fi the whole time. Each board re-sends what it hears, so messages hop from board to board as far as the mesh reaches, and a board that joins late is caught up on the last 12 hours by its neighbors. Heltec V3 and V4 boards join in too and carry messages on to their 900 MHz mesh.<br><br>
            Boards hear each other only on the same Wi-Fi channel. Boards on their own Wi-Fi all use channel 1; a board joined to a home Wi-Fi uses that router's channel. Range is about that of Wi-Fi: a house, or a few hundred feet in the open.`
            : `<b>How it works:</b> every Heltec V3 and V4 running The Shelf has a 900 MHz LoRa radio, and every Shelf board (plain ESP32 boards too) also talks over 2.4 GHz. With Coms on, boards in range hear each other directly, and each board re-sends what it hears, on both, so messages hop from board to board as far as the mesh reaches. A board that joins late is caught up on the last 12 hours by its neighbors. No internet or Wi-Fi network needed between boards.<br><br>
            Fit the LoRa antenna before switching it on. It uses its own channel (915.0 MHz), separate from Meshtastic. Over 2.4 GHz, boards hear each other only on the same Wi-Fi channel (channel 1 on their own Wi-Fi).`}</div>`}
      </div>`;
    const logEl = content.querySelector('#comsLog');
    if (logEl) logEl.scrollTop = logEl.scrollHeight;
    content.querySelector('#comsToggle').addEventListener('change', async e => { await post('/api/coms/toggle', { on: e.target.checked }); last = ''; setTimeout(() => refresh(true), 400); });
    [['#comsBandLora', 'lora', c.bandLora !== false], ['#comsBandWifi', 'wifi', c.bandWifi !== false]].forEach(([id, key, on]) => {
      const b = content.querySelector(id);
      if (b) b.addEventListener('click', async () => {
        const r = await post('/api/coms/settings', { [key]: !on });
        if (!r.ok) { const j = await r.json().catch(() => ({})); alert(j.error || 'That didn\u2019t work.'); }
        last = ''; setTimeout(() => refresh(true), 400);
      });
    });
    const input = content.querySelector('#comsInput');
    if (input) {
      input.value = draft;
      if (hadFocus) input.focus();
      const count = () => { const n = new TextEncoder().encode(input.value).length; content.querySelector('#comsCount').textContent = n ? `${n}/180` : ''; };
      input.addEventListener('input', count); count();
      content.querySelector('#comsForm').addEventListener('submit', async e => {
        e.preventDefault();
        const text = input.value.trim(); if (!text) return;
        if (new TextEncoder().encode(text).length > 180) { alert('That’s too long for one radio message (180 characters max).'); return; }
        input.value = '';
        const r = await post('/api/coms/send', { text });
        if (!r.ok) { const j = await r.json().catch(() => ({})); alert(j.error || 'Couldn’t send.'); input.value = text; }
        last = ''; refresh(true);
      });
    }
    const who = content.querySelector('#comsWho');
    if (who) who.addEventListener('click', async () => { who.textContent = 'Asking…'; await post('/api/coms/remote/who'); setTimeout(() => { who.textContent = 'Refresh'; }, 8000); });
    content.querySelectorAll('.coms-dev').forEach(b => b.addEventListener('click', () => {
      const d = devs.find(x => x.node === b.dataset.node);
      if (d) openRemote(d);
    }));
    const rc = content.querySelector('#comsReceipts');
    if (rc) rc.addEventListener('click', async () => { await post('/api/coms/settings', { receipts: !c.receipts }); last = ''; refresh(true); });
    const save = content.querySelector('#comsSave');
    if (save) save.addEventListener('click', async () => { await post('/api/coms/settings', { name: content.querySelector('#comsName').value, level: Number(content.querySelector('#comsLevel').value) }); last = ''; refresh(true); });
    const clear = content.querySelector('#comsClear');
    if (clear) clear.addEventListener('click', async () => { if (confirm('Clear the message log on this board?')) { await post('/api/coms/clear'); last = ''; refresh(true); } });
  }

  // ---- Remote admin over 900MHz: connect to a board with its admin password ----
  const SHELVES = ['Pictures', 'Music', 'MIDI', 'Videos', 'Games', 'Programs', 'Books'];
  function openRemote(dev) {
    const R = { dev, session: null, busy: null, st: null, out: '', err: '' };
    const ov = document.createElement('div');
    ov.className = 'rm-overlay';
    document.body.appendChild(ov);
    const close = () => { if (R.session) post('/api/coms/remote/disconnect', { session: R.session }); ov.remove(); };
    ov.addEventListener('mousedown', e => { if (e.target === ov) close(); });
    ov.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
    const nm = escapeHtml(dev.name || dev.node);
    const up = s => s > 86400 ? Math.floor(s / 86400) + 'd ' + Math.floor(s % 86400 / 3600) + 'h' : Math.floor(s / 3600) + 'h ' + Math.floor(s % 3600 / 60) + 'm';
    const fmtB = b => b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.round(b / 1e3) + ' KB';

    async function waitFor(req, label) {
      const t0 = Date.now();
      R.busy = label; draw();
      for (;;) {
        await new Promise(r => setTimeout(r, 1000));
        if (!document.body.contains(ov)) return null;
        let j; try { j = await fetch('/api/coms/remote/result?req=' + req).then(r => r.json()); } catch { continue; }
        if (j.state === 'waiting') { R.busy = `${label}… ${Math.round((Date.now() - t0) / 1000)} s`; drawBusy(); continue; }
        R.busy = null;
        return j;
      }
    }
    async function cmd(n, arg, label, confirmText) {
      if (R.busy) return;
      if (confirmText && !confirm(confirmText)) return;
      R.err = '';
      const r = await post('/api/coms/remote/cmd', { session: R.session, cmd: n, arg: arg == null ? '' : String(arg) });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) { R.err = j.error || 'Couldn’t send that.'; if (r.status === 401) R.session = null; draw(); return; }
      const t0 = Date.now();
      const res = await waitFor(j.req, label + ' — waiting for ' + (dev.name || 'it'));
      if (!res) return;
      if (res.state === 'failed') R.err = res.error;
      else if (res.status) R.st = res.status;
      if (res.text) R.out = res.text + (n === 10 ? ` (answered in ${Math.round((Date.now() - t0) / 1000)} s)` : '');
      draw();
    }
    async function connect(pw) {
      R.err = '';
      const r = await post('/api/coms/remote/connect', { node: dev.node, password: pw });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) { R.err = j.error || 'Couldn’t connect.'; draw(); return; }
      R.session = j.session;
      const res = await waitFor(j.req, 'Asking ' + (dev.name || 'it'));
      if (!res) return;
      if (res.state === 'failed') { R.err = res.error; post('/api/coms/remote/disconnect', { session: R.session }); R.session = null; }
      else R.st = res.status;
      draw();
    }
    function drawBusy() { const b = ov.querySelector('.rm-busy'); if (b) b.textContent = R.busy || ''; else draw(); }
    function draw() {
      const s = R.st;
      let body;
      if (!R.session || !s) {
        body = `<p class="rm-sub">Sends a signed request over ${NOLORA ? '2.4 GHz' : 'the mesh'}${dev.hops ? `, through ${dev.hops} other ${dev.hops === 1 ? 'board' : 'boards'}` : ''}. Nothing is encrypted, so anyone listening can read the commands and replies, but only someone with that board's admin password can give them.</p>
          ${R.busy ? '' : `<input type="password" class="rm-input" id="rmPw" placeholder="${nm}'s admin password" autocomplete="current-password">
          <div class="rm-row"><button type="button" class="coms-btn secondary" data-close>Cancel</button><button class="coms-btn" id="rmGo">Connect</button></div>`}`;
      } else {
        const kv = [['Board', `${escapeHtml(boardLabel(s.b))} · v${escapeHtml(s.fw)}`], ['Battery', s.bat < 0 ? 'On USB power' : s.bat + '%'], ['Up for', s.up != null ? up(s.up) : '—'],
          ['Wi-Fi', escapeHtml(s.w || '—')], ['Address', escapeHtml(s.ip || '—') + (s.cl ? ` · ${s.cl} connected` : '')], ['Storage', `${s.st === 'SD' ? 'microSD' : 'Board flash'}` + (s.fm != null ? ` · ${fmtB(s.fm * 1e6)} free` : '')],
          ['Mode', s.ro ? '🔒 Read-only' : 'Anyone can make changes'], ['Screen', s.scr ? 'On' : 'Asleep'],
          ['On the mesh', `${escapeHtml(s.cm || dev.name || '')} · ${LEVEL_NAMES[s.lv]} (~${(s.b === '3' ? [11, 17, 22] : [11, 20, 27])[s.lv]} dBm)` + (s.mb != null ? ` · knows ${s.mb} ${s.mb === 1 ? 'board' : 'boards'}` : '')],
          ['Heard / relayed', (s.hd != null ? `${s.hd} / ${s.rl}` : '—') + (s.rc ? ' · sends receipts' : '')]];
        const dis = R.busy ? 'disabled' : '';
        body = `<div class="rm-kv">${kv.map(r => `<div><span>${r[0]}</span><span>${r[1]}</span></div>`).join('')}</div>
          <div class="rm-grid">
            <div class="rm-sec"><label>Mode</label><div class="rm-seg"><button ${dis} class="${s.ro ? 'on' : ''}" data-c="2" data-a="1">Read-only</button><button ${dis} class="${s.ro ? '' : 'on'}" data-c="2" data-a="0">Anyone can change</button></div></div>
            <div class="rm-sec"><label>Screen</label><div class="rm-seg"><button ${dis} class="${s.scr ? 'on' : ''}" data-c="4" data-a="1">Wake</button><button ${dis} class="${s.scr ? '' : 'on'}" data-c="4" data-a="0">Sleep</button></div></div>
            <div class="rm-sec"><label>Transmit power</label><div class="rm-seg">${LEVEL_NAMES.map((l, i) => `<button ${dis} class="${i === s.lv ? 'on' : ''}" data-c="5" data-a="${i}">${l}</button>`).join('')}</div></div>
            <div class="rm-sec"><label>"Received" receipts</label><div class="rm-seg"><button ${dis} class="${s.rc ? 'on' : ''}" data-c="6" data-a="1">On</button><button ${dis} class="${s.rc ? '' : 'on'}" data-c="6" data-a="0">Off</button></div></div>
            <div class="rm-sec"><label>Name on the mesh</label><div class="rm-inline"><input id="rmName" maxlength="16" value="${escapeHtml(s.cm || '')}"><button ${dis} class="coms-btn secondary" id="rmRename">Rename</button></div></div>
            <div class="rm-sec"><label>Look in a shelf</label><div class="rm-inline"><select id="rmShelf">${SHELVES.map(x => `<option>${x}</option>`).join('')}</select><button ${dis} class="coms-btn secondary" id="rmList">List files</button></div></div>
          </div>
          ${R.out ? `<div class="rm-out">${escapeHtml(R.out)}</div>` : ''}
          <div class="rm-row rm-wrap">
            <button ${dis} class="coms-btn secondary" data-c="1">Refresh</button>
            <button ${dis} class="coms-btn secondary" data-c="10">Ping</button>
            <button ${dis} class="coms-btn secondary" data-c="7" data-confirm="Switch ${nm} to its own Wi-Fi? It restarts, and stays on the mesh.">Its own Wi-Fi</button>
            <button ${dis} class="coms-btn secondary" data-c="8" data-confirm="Switch ${nm} back to its home Wi-Fi? It restarts, and stays on the mesh.">Home Wi-Fi</button>
            <button ${dis} class="coms-btn secondary rm-bad" data-c="3" data-confirm="Restart ${nm}?">Restart</button>
            <span style="flex:1"></span>
            <button class="coms-btn secondary" data-close>Disconnect</button>
          </div>`;
      }
      ov.innerHTML = `<div class="rm-box"><div class="rm-head"><span class="cd-chip">V${dev.board || '?'}</span><h3>${nm}</h3><button class="rm-x" data-close title="Close">&times;</button></div>
        ${body}<div class="rm-busy">${R.busy ? escapeHtml(R.busy) : ''}</div><div class="rm-err">${escapeHtml(R.err)}</div></div>`;
      ov.querySelectorAll('[data-close]').forEach(b => b.onclick = close);
      const go = ov.querySelector('#rmGo'), pw = ov.querySelector('#rmPw');
      if (go) { go.onclick = () => { if (pw.value) connect(pw.value); }; pw.onkeydown = e => { if (e.key === 'Enter' && pw.value) connect(pw.value); }; setTimeout(() => pw.focus(), 30); }
      ov.querySelectorAll('[data-c]').forEach(b => b.onclick = () => cmd(Number(b.dataset.c), b.dataset.a, b.textContent.trim(), b.dataset.confirm));
      const rn = ov.querySelector('#rmRename'); if (rn) rn.onclick = () => cmd(9, ov.querySelector('#rmName').value.trim(), 'Renaming');
      const ls = ov.querySelector('#rmList'); if (ls) ls.onclick = () => { R.out = ''; cmd(11, ov.querySelector('#rmShelf').value, 'Listing ' + ov.querySelector('#rmShelf').value); };
    }
    draw();
  }

  window.loadComsView = function () {
    setToolHeader(BAND_NAME, 'A simple radio mesh between Shelf boards', null, null);
    last = '';
    refresh(true);
    clearInterval(timer);
    timer = setInterval(() => { if (document.activeElement && document.activeElement.id === 'comsName') return; refresh(false); }, 2000);
  };
})();

// ---- Pictures, cover art and thumbnails: a few at a time ----
// The shelf pages ask for every picture on screen at once. A board sends them slowly, and one
// without PSRAM (a plain ESP32) runs out of memory sending several big photos together. Here the
// pictures wait in a queue and load as they come near the screen: one at a time on a board without
// PSRAM, four at a time otherwise.
(function () {
  let limit = 2, active = 0;
  const q = [];
  fetch('/api/board/info').then(r => r.json()).then(i => { limit = i.psram ? 4 : 1; pump(); }).catch(() => {});
  function pump() {
    while (active < limit && q.length) {
      const img = q.shift();
      if (!img.isConnected || img.getAttribute('src')) continue;
      active++;
      let finished = false;
      const done = () => { if (finished) return; finished = true; active--; clearTimeout(t); setTimeout(pump, 0); };
      const t = setTimeout(done, 20000);                       // a stuck picture doesn't hold up the rest
      img.addEventListener('load', done, { once: true });
      img.addEventListener('error', done, { once: true });
      img.src = img.dataset.qsrc;
    }
  }
  const io = 'IntersectionObserver' in window ? new IntersectionObserver(es => {
    for (const e of es) if (e.isIntersecting) { io.unobserve(e.target); q.push(e.target); }
    pump();
  }, { rootMargin: '400px' }) : null;
  const watch = img => { if (img.getAttribute('src')) return; if (io) io.observe(img); else { q.push(img); pump(); } };
  new MutationObserver(ms => {
    for (const m of ms) for (const n of m.addedNodes) {
      if (n.nodeType !== 1) continue;
      if (n.matches && n.matches('img[data-qsrc]')) watch(n);
      if (n.querySelectorAll) n.querySelectorAll('img[data-qsrc]').forEach(watch);
    }
  }).observe(document.documentElement, { childList: true, subtree: true });
})();

// ---- Cover art and thumbnails: try again when the board was too busy ----
// A shelf asks for a picture per song or game, all at once. A board without PSRAM (a plain
// ESP32) can't always serve them all together, so a picture that fails or gets cut off is
// asked for again a moment later, up to 4 times, spread out so they don't all come back at once.
(function () {
  const isArt = img => /\/data-files\/(album-art|video-thumbs|file-thumbs)\//.test(img.getAttribute('src') || '');
  document.addEventListener('error', e => {
    const img = e.target;
    if (!(img instanceof HTMLImageElement) || !isArt(img)) return;
    const n = +(img.dataset.artRetry || 0);
    if (n >= 4) return;
    img.dataset.artRetry = n + 1;
    setTimeout(() => {
      try { const u = new URL(img.getAttribute('src'), location.href); u.searchParams.set('r', n + 1); img.src = u.pathname + u.search; } catch (x) {}
    }, 700 + Math.random() * 1500 * (n + 1));
  }, true);
})();

// ---- Bluetooth (plain ESP32 boards only): pair a headset and play the Music shelf on it ----
// The board itself plays the songs from its card to the headset, so nothing has to stay open
// on the phone. Only MP3 and WAV songs can go to the headset.
(function () {
  let timer = null, songs = null, filter = '', lastSig = '';
  const post = (u, d) => fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(d || {}) }).then(r => r.json()).catch(() => ({}));
  const viewing = () => typeof state !== 'undefined' && state.view === 'bluetooth';
  const playable = n => /\.(mp3|wav)$/i.test(n);
  const pathOf = f => decodeURIComponent(String(f.url || '').replace(/^\/shared/, '')) || ('/Music/' + f.name);
  const mmss = s => { s = Math.max(0, Math.round(s || 0)); return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };
  const bars = rssi => rssi > -60 ? '▂▄▆█' : rssi > -72 ? '▂▄▆' : rssi > -84 ? '▂▄' : '▂';
  const songName = p => (p || '').split('/').pop().replace(/\.[^.]+$/, '');

  async function loadSongs() {
    try { songs = (await fetch('/api/files?category=Music').then(r => r.json())).sort((a, b) => a.name.localeCompare(b.name)); } catch { songs = []; }
    drawSongs();
  }
  function shown() { const q = filter.trim().toLowerCase(); return (songs || []).filter(f => playable(f.name) && (!q || f.name.toLowerCase().includes(q))); }
  function drawSongs() {
    const box = document.getElementById('btSongs'); if (!box) return;
    if (!songs) { box.innerHTML = '<div class="bt-empty">Loading songs…</div>'; return; }
    const list = shown(), other = (songs || []).filter(f => !playable(f.name)).length;
    box.innerHTML = (list.length ? list.map((f, i) => `<button class="bt-song" data-i="${i}"><span class="bt-play">▶</span><span>${escapeHtml(f.name.replace(/\.[^.]+$/, ''))}</span></button>`).join('')
      : `<div class="bt-empty">${songs.length ? 'No songs match.' : 'No songs on the Music shelf yet.'}</div>`) +
      (other ? `<div class="bt-note">${other} song${other === 1 ? '' : 's'} in other formats (M4A, FLAC, MIDI…) can only play in the browser.</div>` : '');
    box.querySelectorAll('.bt-song').forEach(b => b.onclick = () => play(+b.dataset.i, false));
  }
  async function play(index, shuffle) {
    const list = shown(); if (!list.length) return;
    await post('/api/bt/play', { paths: list.map(pathOf), index, shuffle });
    refresh(true);
  }

  function render(b) {
    const content = el('#content');
    if (!content.querySelector('.bt')) {
      content.innerHTML = `<div class="bt coms">
        <div class="coms-card" id="btHead"></div>
        <div class="coms-card" id="btNow"></div>
        <div class="coms-card"><div class="bt-row"><b>Music</b><span class="bt-grow"></span>
          <button class="tool-action-btn tool-action-btn-secondary" id="btAll">Play all</button><button class="tool-action-btn tool-action-btn-secondary" id="btShuf">Shuffle all</button></div>
          <input type="search" id="btFilter" class="bt-search" placeholder="Find a song" value="${escapeHtml(filter)}">
          <div id="btSongs" class="bt-songs"></div></div></div>`;
      content.querySelector('#btFilter').oninput = e => { filter = e.target.value; drawSongs(); };
      content.querySelector('#btAll').onclick = () => play(0, false);
      content.querySelector('#btShuf').onclick = () => play(0, true);
      drawSongs();
    }
    // headset
    const head = content.querySelector('#btHead');
    let h = `<div class="coms-top"><label class="toggle-switch"><input type="checkbox" id="btPower" ${b.on ? 'checked' : ''}><span class="toggle-slider"></span></label>
      <span class="power-label">${!b.on ? 'Bluetooth is off' : b.connected ? 'Connected to ' + escapeHtml(b.headset.name || 'your headset') : b.held ? 'Not connected' : b.target ? 'Connecting…' : b.saved.mac ? 'Looking for ' + escapeHtml(b.saved.name || 'your headset') + '…' : 'Looking for headsets…'}</span>
      <span class="sub">${b.on ? 'The board plays your music straight to a Bluetooth headset or speaker.' : 'Turn it on to play music from this board on a Bluetooth headset or speaker. The board restarts to switch it on or off.'}</span></div>`;
    if (b.on && b.started) {
      if (b.connected) h += `<div class="bt-row"><button class="tool-action-btn tool-action-btn-secondary" id="btDisc">Disconnect</button><button class="tool-action-btn tool-action-btn-secondary" id="btForget">Forget this headset</button></div>`;
      else {
        h += `<p class="bt-help">Put the headset in pairing mode (usually: hold its power button until the light flashes). It shows up here in a few seconds; tap it to connect.${b.saved.mac ? ' It reconnects to <b>' + escapeHtml(b.saved.name || b.saved.mac) + '</b> on its own when that one is on.' : ''}</p>`;
        h += (b.found.length ? `<div class="bt-found">${b.found.map(d => `<button class="bt-dev" data-mac="${d.mac}"><span>${escapeHtml(d.name)}</span><span class="bt-sig">${bars(d.rssi)}</span><span class="bt-act">${b.target === d.mac ? 'Connecting…' : 'Connect'}</span></button>`).join('')}</div>`
          : '<div class="bt-empty">Nothing found yet…</div>');
        h += `<div class="bt-row"><button class="tool-action-btn tool-action-btn-secondary" id="btScan">Search again</button>${b.saved.mac ? '<button class="tool-action-btn tool-action-btn-secondary" id="btForget">Forget ' + escapeHtml(b.saved.name || 'saved headset') + '</button>' : ''}</div>`;
      }
    }
    if (b.error) h += `<div class="bt-note">${escapeHtml(b.error)}</div>`;
    head.innerHTML = h;
    head.querySelector('#btPower').onchange = async e => {
      head.querySelector('.power-label').textContent = 'Restarting the board…';
      await post('/api/bt/power', { on: e.target.checked });
      setTimeout(() => location.reload(), 9000);
    };
    const on = (id, f) => { const x = head.querySelector(id); if (x) x.onclick = f; };
    on('#btDisc', async () => { await post('/api/bt/disconnect'); refresh(true); });
    on('#btForget', async () => { await post('/api/bt/forget'); refresh(true); });
    on('#btScan', async () => { await post('/api/bt/scan'); refresh(true); });
    head.querySelectorAll('.bt-dev').forEach(x => x.onclick = async () => { await post('/api/bt/connect', { mac: x.dataset.mac }); refresh(true); });
    // now playing
    const p = b.player, now = content.querySelector('#btNow');
    const track = p.track ? songName(p.track) : '';
    now.innerHTML = `<div class="bt-row"><b>${track ? escapeHtml(track) : 'Nothing playing'}</b><span class="bt-grow"></span><span class="bt-time">${track ? mmss(p.pos) + (p.dur ? ' / ' + mmss(p.dur) : '') : ''}</span></div>
      ${track ? `<div class="bt-sub">${p.index + 1} of ${p.count}${p.shuffle ? ' · shuffled' : ''}${b.connected ? '' : ' · waiting for the headset'}</div>` : `<div class="bt-sub">${b.connected ? 'Pick a song below.' : 'Connect a headset, then pick a song below.'}</div>`}
      <div class="bt-ctl"><button id="btPrev" title="Previous">⏮</button><button id="btPP" class="bt-big" title="Play/pause">${p.playing ? '❚❚' : '▶'}</button><button id="btNext" title="Next">⏭</button><button id="btStop" title="Stop">■</button>
      <span class="bt-vol">🔊 <input type="range" id="btVol" min="0" max="100" value="${p.volume}"></span></div>`;
    const ctl = c => async () => { await post('/api/bt/control', { cmd: c }); refresh(true); };
    now.querySelector('#btPrev').onclick = ctl('prev');
    now.querySelector('#btNext').onclick = ctl('next');
    now.querySelector('#btStop').onclick = ctl('stop');
    now.querySelector('#btPP').onclick = ctl(p.playing ? 'pause' : 'resume');
    let vt = 0; now.querySelector('#btVol').oninput = e => { clearTimeout(vt); const v = +e.target.value; vt = setTimeout(() => post('/api/bt/control', { volume: v }), 150); };
  }

  async function refresh(force) {
    if (!viewing()) { clearInterval(timer); timer = null; return; }
    let b; try { b = await fetch('/api/bt').then(r => r.json()); } catch { return; }
    const sig = JSON.stringify([b, Math.round(b.player.pos)]);
    if (!force && sig === lastSig) return;
    lastSig = sig;
    if (document.activeElement && document.activeElement.id === 'btVol') return;   // don't move the slider under a finger
    render(b);
  }

  window.loadBtView = function () {
    setToolHeader('Bluetooth', 'Play music from this board on a Bluetooth headset', null, null);
    el('#content').innerHTML = '';
    lastSig = ''; songs = null;
    refresh(true); loadSongs();
    clearInterval(timer);
    timer = setInterval(() => refresh(false), 1500);
  };
})();

// ---- iPhone/iPad: visualizer bars that start quickly, and lock-screen controls that stay ----
// The Shelf draws the bars on iOS from the song itself: it downloads the whole file a second
// time and decodes it. From the board that takes a while, and when it's too slow The Shelf
// falls back to a hidden second <audio> routed through Web Audio, which is what makes iOS lose
// the lock-screen play/pause/next controls. The hidden copy is never used here.
//
// Instead an MP3 is downloaded in pieces, and what has arrived so far is decoded from the
// start of the song each time: first ~4 seconds' worth, then three times as much, and so on,
// so the bars start within seconds and keep ahead of the music. Safari is fussy: a piece cut
// from the middle of an MP3 usually won't decode at all (most frames borrow bytes from the
// frame before), which is why bars used to stop after the first few seconds. Decoding from
// the start, cut on a whole frame, always works. The ID3 tag (cover art can make it big) is
// skipped. Other formats are fetched whole and decoded once. Pieces are kept under 256 KB, so a
// plain ESP32 never counts them as a second stream of the song (it limits those), and a piece that
// fails is asked for again rather than ending the bars for the song.
(function () {
  if (typeof IS_IOS === 'undefined' || !IS_IOS || typeof preparePcmViz !== 'function') return;
  try { ensureShadowVisualizer = function () {}; } catch (e) {}           // never the hidden second player
  const NET = 224 * 1024, FIRST = 96 * 1024, GROW = 3, WHOLE_MAX = 40 * 1048576;
  const isMidiUrl = u => /\.(mid|midi|kar|rmi)$/i.test(String(u).split('?')[0]);
  const isMp3Url = u => /\.mp3$/i.test(decodeURIComponent(String(u).split('?')[0]));
  const BR = { 1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],   // MPEG-1 Layer III
               2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160] };      // MPEG-2/2.5 Layer III
  const SR = { 3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000] };
  function frameLen(b, i) {                                                  // an MP3 frame header at b[i]: its length, or 0
    if (i + 4 > b.length || b[i] !== 0xFF || (b[i + 1] & 0xE0) !== 0xE0) return 0;
    const ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3;
    if (ver === 1 || layer !== 1 || bri === 0 || bri === 15 || sri === 3) return 0;     // Layer III only
    const kbps = BR[ver === 3 ? 1 : 2][bri], sr = SR[ver][sri], pad = (b[i + 2] >> 1) & 1;
    return ver === 3 ? Math.floor(144000 * kbps / sr) + pad : Math.floor(72000 * kbps / sr) + pad;
  }
  // first frame: a header whose next header is where it says
  function firstFrame(b, from) {
    for (let i = from; i + 4 <= b.length; i++) { const n = frameLen(b, i); if (n >= 24 && frameLen(b, i + n)) return i; }
    return -1;
  }
  function mono(dec) {
    const m = new Float32Array(dec.length);
    for (let c = 0; c < dec.numberOfChannels; c++) { const ch = dec.getChannelData(c); for (let i = 0; i < m.length; i++) m[i] += ch[i] / dec.numberOfChannels; }
    return m;
  }

  preparePcmViz = async function () {
    const src = audio.src;
    if (!src || pcmViz.src === src) return;
    const st = { src, data: null, rate: PCM_VIZ_RATE, ready: false, fallback: false };
    pcmViz = st; pcmVizSmoothed.fill(0);
    if (isMidiUrl(src)) return;                                              // MIDI: bars come from the synth (midi.js)
    const Offline = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    const live = () => pcmViz === st;
    const decode = async bytes => mono(await decodeToBuffer(new Offline(1, 1, PCM_VIZ_RATE), bytes));
    try {
      if (!isMp3Url(src)) {                                                  // whole file, one decode
        const r = await fetch(src);
        if (!r.ok || (+r.headers.get('Content-Length') || 0) > WHOLE_MAX) return;
        const bytes = await r.arrayBuffer();
        if (!live()) return;
        st.data = await decode(bytes);
        if (live()) st.ready = true;
        return;
      }
      let buf = new Uint8Array(1 << 20), have = 0, pos = 0, total = Infinity;
      let start = -1, walked = -1, frames = 0, next = FIRST, failed = 0;
      while (true) {
        if (!live()) return;
        if (pos < total) {
          let r, piece;
          for (let tries = 0; ; tries++) {                                   // a busy board can drop a piece: ask again
            try {
              r = await fetch(src, { headers: { Range: 'bytes=' + pos + '-' + (pos + NET - 1) } });
              if (r.status !== 206 && r.status !== 200) throw new Error('fetch ' + r.status);
              piece = new Uint8Array(await r.arrayBuffer());
              break;
            } catch (e) {
              if (tries >= 6 || !live()) throw e;
              await new Promise(res => setTimeout(res, 1000 * (tries + 1)));
            }
          }
          if (r.status === 200) { total = piece.length; pos = 0; have = 0; }
          else { const cr = r.headers.get('Content-Range'); total = cr ? (+cr.split('/')[1] || Infinity) : Infinity; }
          if (!piece.length) total = pos;
          if (have + piece.length > buf.length) { const nb = new Uint8Array(Math.max(have + piece.length, buf.length * 2)); nb.set(buf.subarray(0, have)); buf = nb; }
          buf.set(piece, have); have += piece.length; pos += piece.length;
        }
        const last = pos >= total;
        if (start < 0) {                                                     // skip the ID3 tag, find the first frame
          let skip = 0;
          if (have >= 10 && buf[0] === 0x49 && buf[1] === 0x44 && buf[2] === 0x33)
            skip = 10 + ((buf[6] & 127) << 21 | (buf[7] & 127) << 14 | (buf[8] & 127) << 7 | (buf[9] & 127)) + (buf[5] & 0x10 ? 10 : 0);
          if (have < skip + 4096 && !last) continue;
          start = firstFrame(buf.subarray(0, have), skip);
          if (start < 0) { if (last) return; start = -1; continue; }
          walked = start;
        }
        // walk whole frames as far as they've arrived
        while (true) { const n = frameLen(buf, walked); if (!n || walked + n > have) break; walked += n; frames++; }
        const ready = walked - start;
        if (ready < next && !last) continue;
        try {
          const m = await decode(buf.slice(start, walked).buffer);            // from the start of the song, every time
          if (!live()) return;
          st.data = m; st.rate = PCM_VIZ_RATE; st.ready = true;
          next = Math.min(Math.max(next * GROW, ready + FIRST), ready + 1536 * 1024);   // fresh bars at least every 1.5 MB
        } catch (e) { failed++; next = ready + FIRST; }                      // Safari balked: try again with a bit more
        if (last) {
          // the whole song is in: one last pass if frames ended early (a glitch or a tag mid-file)
          if (walked < have - 256) { const more = firstFrame(buf.subarray(0, have), walked); if (more > 0) { st.data = await decode(buf.slice(start, have).buffer).catch(() => st.data); } }
          return;
        }
      }
    } catch (e) { /* no bars for this song; playback and the lock screen are unaffected */ }
  };
})();

// ---- "Get album art" on a board ----
// The board can't look art up online the way The Shelf on a computer does (it's often on its own
// Wi-Fi, with no internet). It shows the art that's saved inside each song, so this button
// explains that and points to Music Standardizer on the Programs shelf, which adds cover art to
// the files themselves on a computer.
(function () {
  function openArtHelp() {
    const ov = document.createElement('div');
    ov.className = 'rm-overlay';
    ov.innerHTML = `<div class="rm-box rm-small art-help" role="dialog" aria-label="Album art">
      <div class="rm-head"><h3>Album art</h3><button class="rm-x" data-close aria-label="Close">&times;</button></div>
      <p class="rm-sub">The board shows the cover art that's saved inside each song. To add art to songs that don't have it:</p>
      <ol>
        <li>On your computer, run <b>Music Standardizer</b> from the <b>Programs</b> shelf on your music. It looks up the cover art and saves it into each file.</li>
        <li>Copy the songs back onto the board's Music shelf (upload them, or copy them onto the microSD card).</li>
        <li>The board reads the art from them by itself: within a minute or two for uploaded songs, or after a restart for songs copied onto the card.</li>
      </ol>
      <p class="rm-sub">Already have art in The Shelf on your computer? <b>Settings</b> &rsaquo; <b>Thumbnails from your computer</b> copies it straight across.</p>
      <div class="rm-row rm-wrap" id="artLinks"><span class="rm-sub" style="margin:0">Looking for Music Standardizer…</span></div>
      <div class="rm-row"><button class="coms-btn" data-close>OK</button></div>
    </div>`;
    document.body.appendChild(ov);
    const close = () => ov.remove();
    ov.addEventListener('click', e => { if (e.target === ov || e.target.closest('[data-close]')) close(); });
    fetch('/api/files?category=Programs').then(r => r.json()).then(list => {
      const hits = list.filter(f => /music.?standardi[sz]er/i.test(f.name));
      const box = ov.querySelector('#artLinks');
      box.innerHTML = hits.length
        ? hits.map(f => `<a class="coms-btn" href="${f.url}" download>Download ${escapeHtml(f.name)}</a>`).join('')
        : '<span class="rm-sub" style="margin:0">Music Standardizer isn\'t on this board\'s Programs shelf yet.</span>';
    }).catch(() => { ov.querySelector('#artLinks').innerHTML = ''; });
  }
  function hook() {
    const btn = document.getElementById('fetchArtBtn');
    if (!btn) return;
    // capture phase: runs instead of The Shelf's own online lookup
    btn.addEventListener('click', e => { e.stopImmediatePropagation(); e.preventDefault(); openArtHelp(); }, true);
    btn.title = 'Where album art comes from, and how to add it';
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', hook); else hook();
})();

// ---- Busy board: ask again ----
// A board without PSRAM (a plain ESP32) sometimes answers "short of memory" (503) when several
// things land at once, such as a phone opening the Music shelf while pictures are loading.
// Instead of the page sitting there, reading requests are asked for again a moment later.
(function () {
  const real = window.fetch.bind(window);
  window.fetch = async function (input, init) {
    const method = ((init && init.method) || (input && input.method) || 'GET').toUpperCase();
    const url = typeof input === 'string' ? input : (input && input.url) || '';
    let res = await real(input, init);
    if (method !== 'GET' || !/\/api\//.test(url)) return res;
    for (let i = 1; res.status === 503 && i <= 5; i++) {
      await new Promise(r => setTimeout(r, 600 * i + Math.random() * 400));
      res = await real(input, init);
    }
    return res;
  };
})();

// ---- Check for updates, the board's way ----
// The computer's Shelf updates itself straight from GitHub. A board gets its updates from the
// installer site (the same files its /update page uses): this checks there for newer firmware,
// and "Update now" opens the board's update page, which downloads and installs it.
(function () {
  const BASE = 'https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/';
  const newer = (a, b) => { a = String(a).split('.').map(Number); b = String(b).split('.').map(Number); for (let i = 0; i < 3; i++) { if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0); } return false; };
  window.checkShelfUpdate = checkShelfUpdate = async function () {
    const box = document.getElementById('shelfUpdateBox');
    if (!box) return;
    box.hidden = false;
    const msg = document.getElementById('shelfUpdateMsg'), btn = document.getElementById('shelfUpdateBtn');
    msg.textContent = 'Checking for newer firmware…'; btn.hidden = true;
    let info;
    try { info = await fetch('/api/board/update/info', { cache: 'no-store' }).then(r => r.json()); }
    catch { msg.textContent = "Couldn't ask the board which version it's running."; return; }
    try {
      const latest = await fetch(BASE + 'ota/' + info.id + '.json', { cache: 'no-store' }).then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });
      if (newer(latest.version, info.fw)) {
        msg.textContent = `v${latest.version} is ready to install. This board has v${info.fw}.`;
        btn.hidden = false; btn.disabled = false;
      } else {
        msg.textContent = `Up to date (v${info.fw}).`;
      }
    } catch {
      msg.textContent = `This board has v${info.fw}. Couldn't reach the internet from this browser to check for a newer one. On the board's own Wi-Fi, use its update page with a file instead.`;
      btn.hidden = false; btn.disabled = false; btn.textContent = 'Open the update page';
    }
  };
  function hook() {
    const old = document.getElementById('shelfUpdateBtn');
    if (!old) return;
    const btn = old.cloneNode(true);   // drops The Shelf's own handler (it updates a computer)
    old.replaceWith(btn);
    btn.addEventListener('click', () => { location.href = '/update'; });
    const side = document.getElementById('sidebarUpdateBtn');
    if (side) side.title = 'Check for newer firmware for this board';
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', hook); else hook();
})();

// ---- The board's screen shows the picture on show ----
// Songs, games, programs and videos tell the board through their play count; pictures in the
// slideshow don't have one, so each slide's name is passed on here (only pictures from the
// Pictures shelf, not the album-covers slideshow).
(function () {
  if (typeof showSlide !== 'function') return;
  const real = showSlide;
  window.showSlide = showSlide = function () {
    real.apply(this, arguments);
    try {
      if (state.slideshow.list) return;
      const f = slideList()[state.slideshow.index];
      if (!f) return;
      fetch('/api/board/showing', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ cat: 'Pictures', name: f.name }) }).catch(() => {});
    } catch (e) {}
  };
})();

// ---- Weather, the board's way (as TidyOS does it) ----
// The Shelf asks the browser where it is, but browsers only answer secure (https) pages and a
// board is plain http, so the Home page's weather never showed. Here the browser does the
// fetching instead, over https: where the board is (its settings page, or worked out from the
// internet address), then the weather from Open-Meteo, shown on Home and handed to the board
// for its screen (the 4-day temperature graph). Every 30 minutes while The Shelf is open.
(function () {
  const WMO = { 0: ['Clear sky', '☀️'], 1: ['Mainly clear', '🌤️'], 2: ['Partly cloudy', '⛅'], 3: ['Overcast', '☁️'], 45: ['Fog', '🌫️'], 48: ['Freezing fog', '🌫️'],
    51: ['Light drizzle', '🌦️'], 53: ['Drizzle', '🌦️'], 55: ['Heavy drizzle', '🌧️'], 56: ['Light freezing drizzle', '🌧️'], 57: ['Freezing drizzle', '🌧️'],
    61: ['Light rain', '🌧️'], 63: ['Rain', '🌧️'], 65: ['Heavy rain', '🌧️'], 66: ['Light freezing rain', '🌧️'], 67: ['Freezing rain', '🌧️'],
    71: ['Light snow', '🌨️'], 73: ['Snow', '🌨️'], 75: ['Heavy snow', '❄️'], 77: ['Snow grains', '🌨️'], 80: ['Light rain showers', '🌦️'],
    81: ['Rain showers', '🌧️'], 82: ['Heavy rain showers', '⛈️'], 85: ['Light snow showers', '🌨️'], 86: ['Heavy snow showers', '❄️'],
    95: ['Thunderstorm', '⛈️'], 96: ['Thunderstorm with hail', '⛈️'], 99: ['Severe thunderstorm', '⛈️'] };
  let now = null, at = 0;
  const j = u => fetch(u, { cache: 'no-store' }).then(r => { if (!r.ok) throw r.status; return r.json(); });
  async function where() {
    try { const b = await j('/api/board/location'); if (b.lat != null) return { lat: b.lat, lon: b.lon, fromBoard: true }; } catch (e) {}
    const tries = [['https://ipwho.is/', x => x.success !== false && x.latitude != null ? { lat: x.latitude, lon: x.longitude, place: [x.city, x.region].filter(Boolean).join(', ') } : null],
      ['https://ipapi.co/json/', x => x.latitude != null ? { lat: x.latitude, lon: x.longitude, place: [x.city, x.region].filter(Boolean).join(', ') } : null],
      ['http://ip-api.com/json/?fields=status,city,regionName,lat,lon', x => x.status === 'success' ? { lat: x.lat, lon: x.lon, place: [x.city, x.regionName].filter(Boolean).join(', ') } : null]];
    for (const [u, f] of tries) { try { const g = f(await j(u)); if (g) return g; } catch (e) {} }
    return null;
  }
  async function refresh() {
    const g = await where();
    if (!g) return false;
    const w = await j(`https://api.open-meteo.com/v1/forecast?latitude=${g.lat}&longitude=${g.lon}&current=temperature_2m,weather_code,is_day&hourly=temperature_2m&past_days=4&forecast_days=1&temperature_unit=fahrenheit&timeformat=unixtime`);
    now = { f: w.current.temperature_2m, code: w.current.weather_code, day: w.current.is_day };
    at = Date.now();
    fetch('/api/board/wxdata', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ lat: g.fromBoard ? null : g.lat, lon: g.fromBoard ? null : g.lon, place: g.place || '', time: w.hourly.time, temp: w.hourly.temperature_2m, now }) }).catch(() => {});
    return true;
  }
  function show() {
    const p = document.getElementById('weatherPill');
    if (!p || !now) return;
    const [label, icon0] = WMO[now.code] || ['', '🌡️'];
    const icon = now.code <= 1 && !now.day ? '🌙' : icon0;
    p.innerHTML = `${icon} <strong>${Math.round(now.f)}&deg;F</strong> ${escapeHtml(label)}`;
    p.title = `${Math.round((now.f - 32) * 5 / 9)}°C — ${label}`;
  }
  async function tick() { try { if (await refresh()) show(); } catch (e) {} }
  if (typeof loadWeatherWidget === 'function') {
    window.loadWeatherWidget = loadWeatherWidget = async function () {
      if (now && Date.now() - at < 30 * 60000) { show(); return; }
      try {
        if (!(await refresh())) throw 0;
        show();
      } catch (e) {                                   // this browser can't reach the internet: ask the board
        try {
          const d = await j('/api/weather');
          const p = document.getElementById('weatherPill');
          if (p) { p.innerHTML = `${d.icon} <strong>${d.tempF}&deg;F</strong> ${escapeHtml(d.condition)}`; p.title = `${d.tempC}°C — ${d.condition}`; }
        } catch (e2) { const p = document.getElementById('weatherPill'); if (p) p.remove(); }
      }
    };
  }
  setTimeout(tick, 4000);
  setInterval(tick, 30 * 60000);
})();

// ---- GPS (Heltec V4 with Heltec's GPS kit on its GNSS connector) ----
// A map of where the board is and where it's been (OpenStreetMap, when this browser has internet;
// otherwise the trail drawn on its own), and everything the GPS knows: height, speed, heading,
// the satellites, the ham radio grid square, the time from the sky.
(function () {
  let timer = null, map = null, marker = null, line = null, ring = null, trail = [], lastT = 0, follow = true, leafletState = 0;
  const C = { mph: k => k * 0.621371, ft: m => m * 3.28084 };
  const dirs = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
  const compass = d => dirs[Math.round(((d % 360) + 360) % 360 / 22.5) % 16];
  const dms = (v, pos, neg) => { const a = Math.abs(v), d = Math.floor(a), mf = (a - d) * 60, m = Math.floor(mf), s = ((mf - m) * 60).toFixed(1); return `${d}° ${m}′ ${s}″ ${v >= 0 ? pos : neg}`; };
  function grid(lat, lon) {
    const x = lon + 180, y = lat + 90, A = 'A'.charCodeAt(0), a = 'a'.charCodeAt(0);
    return String.fromCharCode(A + Math.floor(x / 20), A + Math.floor(y / 10)) + Math.floor(x / 2 % 10) + Math.floor(y % 10) +
      String.fromCharCode(a + Math.floor(x * 12 % 24), a + Math.floor(y * 24 % 24));
  }
  function sunTimes(lat, lon) {                     // sunrise and sunset today (NOAA), local time
    const d = new Date(), n = Math.floor((Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) - Date.UTC(d.getFullYear(), 0, 0)) / 864e5);
    const g = 2 * Math.PI / 365 * (n - 1), r = Math.PI / 180;
    const eq = 229.18 * (0.000075 + 0.001868 * Math.cos(g) - 0.032077 * Math.sin(g) - 0.014615 * Math.cos(2 * g) - 0.040849 * Math.sin(2 * g));
    const dec = 0.006918 - 0.399912 * Math.cos(g) + 0.070257 * Math.sin(g) - 0.006758 * Math.cos(2 * g) + 0.000907 * Math.sin(2 * g) - 0.002697 * Math.cos(3 * g) + 0.00148 * Math.sin(3 * g);
    const c = (Math.cos(90.833 * r) / (Math.cos(lat * r) * Math.cos(dec)) - Math.tan(lat * r) * Math.tan(dec));
    if (c > 1 || c < -1) return null;
    const ha = Math.acos(c) / r, noon = 720 - 4 * lon - eq;
    const t = mins => { const x = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()) + mins * 60000); return x.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }); };
    return [t(noon - 4 * ha), t(noon + 4 * ha)];
  }
  function loadLeaflet() {
    if (window.L || leafletState) return;
    leafletState = 1;
    const css = document.createElement('link'); css.rel = 'stylesheet'; css.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'; document.head.appendChild(css);
    const s = document.createElement('script'); s.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
    s.onload = () => { leafletState = 2; last = null; };
    s.onerror = () => { leafletState = 3; };
    document.head.appendChild(s);
  }
  let last = null;
  const viewing = () => typeof state !== 'undefined' && state.view === 'gps';
  const post = (u, d) => fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(d || {}) });

  function drawTrailCanvas(cv, g) {                 // no map tiles (no internet here): the trail on its own
    const ctx = cv.getContext('2d'), w = cv.width = cv.clientWidth * devicePixelRatio, h = cv.height = cv.clientHeight * devicePixelRatio;
    ctx.fillStyle = '#0d1420'; ctx.fillRect(0, 0, w, h);
    const pts = trail.length ? trail.map(p => [p[0], p[1]]) : g.fix ? [[g.lat, g.lon]] : [];
    if (!pts.length) return;
    let la0 = Math.min(...pts.map(p => p[0])), la1 = Math.max(...pts.map(p => p[0])), lo0 = Math.min(...pts.map(p => p[1])), lo1 = Math.max(...pts.map(p => p[1]));
    const k = Math.cos((la0 + la1) / 2 * Math.PI / 180), pad = 0.0015;
    la0 -= pad; la1 += pad; lo0 -= pad / k; lo1 += pad / k;
    const sc = Math.min(w / ((lo1 - lo0) * k), h / (la1 - la0)) * 0.9;
    const X = lo => w / 2 + (lo - (lo0 + lo1) / 2) * k * sc, Y = la => h / 2 - (la - (la0 + la1) / 2) * sc;
    ctx.strokeStyle = 'rgba(255,255,255,.08)'; ctx.lineWidth = 1;
    for (let i = 1; i < 6; i++) { ctx.beginPath(); ctx.moveTo(w * i / 6, 0); ctx.lineTo(w * i / 6, h); ctx.moveTo(0, h * i / 6); ctx.lineTo(w, h * i / 6); ctx.stroke(); }
    ctx.strokeStyle = '#f2b441'; ctx.lineWidth = 3 * devicePixelRatio; ctx.beginPath();
    pts.forEach((p, i) => i ? ctx.lineTo(X(p[1]), Y(p[0])) : ctx.moveTo(X(p[1]), Y(p[0]))); ctx.stroke();
    if (g.fix) { ctx.fillStyle = '#5ac8ff'; ctx.beginPath(); ctx.arc(X(g.lon), Y(g.lat), 7 * devicePixelRatio, 0, 7); ctx.fill(); }
    ctx.fillStyle = '#7f93a8'; ctx.font = `${12 * devicePixelRatio}px sans-serif`; ctx.fillText('No map tiles without internet: your trail is shown on its own', 10 * devicePixelRatio, h - 10 * devicePixelRatio);
  }

  function render(g) {
    const content = el('#content');
    if (!g.supported) {
      content.innerHTML = `<div class="coms"><div class="coms-card coms-off"><b>No GPS on this board.</b><br>The GPS tab is for a Heltec V4 with Heltec's GPS kit plugged into its GNSS connector. The V4-R8 can't power the kit (its pin is used by the board's memory).</div></div>`;
      return;
    }
    const f = g.fix, mph = g.kmh != null ? C.mph(g.kmh) : null, sun = f ? sunTimes(g.lat, g.lon) : null;
    const acc = g.hdop != null ? Math.max(2, Math.round(g.hdop * 3)) : null;
    const status = !g.on ? 'GPS is off' : !g.talking ? 'Waiting for the GPS…' : f ? `Fix: ${g.sats} satellites` : `Looking for satellites (${g.view} in view)…`;
    const card = (t, v, s) => `<div class="gps-card"><div class="gps-t">${t}</div><div class="gps-v">${v}</div>${s ? `<div class="gps-s">${s}</div>` : ''}</div>`;
    const head = `
      <div class="coms-card">
        <div class="coms-top">
          <label class="toggle-switch"><input type="checkbox" id="gpsToggle" ${g.on ? 'checked' : ''} ${g.pins ? '' : 'disabled'}><span class="toggle-slider"></span></label>
          <span class="power-label">${escapeHtml(status)}</span>
          <span class="sub">${!g.pins ? 'The microSD card is wired to the GNSS connector’s pins (42 · 41 · 40 · 39). Move it to CS 4, MISO 6, CLK 47, MOSI 48 (see the installer page) to use the GPS kit.'
            : !g.on ? 'Turn it on with Heltec’s GPS kit plugged into the V4’s GNSS connector. The first fix takes a minute or two outdoors.'
            : !g.talking ? 'Nothing from the GPS yet. Check the kit is plugged in all the way.' : f ? 'The board’s location, earth map and weather follow the GPS while it has a fix.' : 'Best outdoors or by a window, with open sky. It can take a few minutes the first time.'}</span>
        </div>
      </div>`;
    if (!document.getElementById('gpsMap')) {
      content.innerHTML = `<div class="coms gps"><div id="gpsHead"></div>
        <div class="coms-card gps-mapcard"><div id="gpsMap"></div><canvas id="gpsCanvas"></canvas>
          <div class="gps-maprow"><label class="gps-follow"><input type="checkbox" id="gpsFollow" checked> Keep the map on the board</label>
          <a class="coms-chip" href="/api/gps/track.gpx" download>Download trail (GPX)</a><button class="coms-chip" id="gpsClear">New trail</button></div></div>
        <div class="gps-grid" id="gpsGrid"></div></div>`;
      content.querySelector('#gpsFollow').onchange = e => { follow = e.target.checked; };
      content.querySelector('#gpsClear').onclick = async () => { if (!confirm('Start a new trail? The current one is forgotten (download it first to keep it).')) return; await post('/api/gps/clear'); trail = []; lastT = 0; if (line) line.setLatLngs([]); };
      map = null; marker = null; line = null; ring = null;
    }
    document.getElementById('gpsHead').innerHTML = head;
    const tg = document.getElementById('gpsToggle');
    tg.onchange = async e => { const r = await post('/api/gps/power', { on: e.target.checked }); if (!r.ok) { const j = await r.json().catch(() => ({})); alert(j.error || 'That didn’t work.'); } refresh(); };
    document.getElementById('gpsGrid').innerHTML = f ? [
      card('Position', `${g.lat.toFixed(5)}, ${g.lon.toFixed(5)}`, `${dms(g.lat, 'N', 'S')} · ${dms(g.lon, 'E', 'W')}<br><a href="https://www.openstreetmap.org/?mlat=${g.lat}&mlon=${g.lon}#map=16/${g.lat}/${g.lon}" target="_blank" rel="noopener">OpenStreetMap</a> · <a href="https://www.google.com/maps?q=${g.lat},${g.lon}" target="_blank" rel="noopener">Google Maps</a>`),
      card('Altitude', g.altM != null ? `${Math.round(C.ft(g.altM)).toLocaleString()} ft` : '--', g.altM != null ? `${Math.round(g.altM)} m above sea level` : ''),
      card('Speed', mph != null ? `${mph < 10 ? mph.toFixed(1) : Math.round(mph)} mph` : '--', `${g.kmh != null ? g.kmh.toFixed(1) + ' km/h · ' : ''}top ${Math.round(C.mph(g.maxKmh))} mph`),
      card('Heading', g.course != null && g.kmh > 2 ? `<span class="gps-arrow" style="transform:rotate(${g.course}deg)">↑</span> ${Math.round(g.course)}° ${compass(g.course)}` : 'Standing still', ''),
      card('Grid square', grid(g.lat, g.lon), 'Maidenhead locator, for ham radio'),
      card('Satellites', `${g.sats} used · ${g.view} in view`, acc ? `Accurate to about ±${acc} m (HDOP ${g.hdop.toFixed(1)})` : ''),
      card('Trail', `${(g.distKm * 0.621371).toFixed(2)} mi`, `${trail.length} points kept (one every 10 s on the move)`),
      card('Sun', sun ? `☀ ${sun[0]} · ${sun[1]}` : '--', 'Sunrise and sunset here today'),
      card('Time from the satellites', g.utc ? new Date(g.utc).toLocaleTimeString() : '--', g.utc ? new Date(g.utc).toUTCString().slice(17, 25) + ' UTC' : '')
    ].join('') : `<div class="gps-card" style="grid-column:1/-1"><div class="gps-t">Satellites</div><div class="gps-v">${g.on ? g.view + ' in view' : '--'}</div><div class="gps-s">${g.on && g.talking ? 'Heard ' + g.sentences + ' messages from the GPS so far.' : ''}</div></div>`;
    // the map
    const mapEl = document.getElementById('gpsMap'), cv = document.getElementById('gpsCanvas');
    if (window.L && leafletState === 2) {
      cv.style.display = 'none'; mapEl.style.display = 'block';
      if (!map) {
        map = L.map(mapEl, { zoomControl: true }).setView(f ? [g.lat, g.lon] : [42.33, -83.05], f ? 16 : 3);
        L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 19, attribution: '&copy; OpenStreetMap contributors' }).addTo(map);
        line = L.polyline(trail.map(p => [p[0], p[1]]), { color: '#f2b441', weight: 4 }).addTo(map);
      }
      if (f) {
        if (!marker) { marker = L.circleMarker([g.lat, g.lon], { radius: 8, color: '#fff', weight: 2, fillColor: '#3aa0ff', fillOpacity: 1 }).addTo(map); ring = L.circle([g.lat, g.lon], { radius: acc || 10, color: '#3aa0ff', weight: 1, fillOpacity: 0.12 }).addTo(map); }
        marker.setLatLng([g.lat, g.lon]); ring.setLatLng([g.lat, g.lon]); if (acc) ring.setRadius(acc);
        if (follow) map.panTo([g.lat, g.lon], { animate: true });
      }
      line.setLatLngs(trail.map(p => [p[0], p[1]]));
    } else {
      mapEl.style.display = 'none'; cv.style.display = 'block';
      drawTrailCanvas(cv, g);
    }
  }
  async function refresh() {
    if (!viewing()) { clearInterval(timer); timer = null; return; }
    let g; try { g = await fetch('/api/gps?since=' + lastT, { cache: 'no-store' }).then(r => r.json()); } catch { return; }
    if (g.track && g.track.length) { trail = trail.concat(g.track); lastT = g.last; if (trail.length > 2000) trail = trail.slice(-2000); }
    render(g);
  }
  window.loadGpsView = function () {
    setToolHeader('GPS', 'Where the board is, from Heltec’s GPS kit', null, null);
    el('#content').innerHTML = '';
    trail = []; lastT = 0; map = null;
    loadLeaflet();
    refresh();
    clearInterval(timer);
    timer = setInterval(refresh, 2000);
  };
})();

// ---- News headlines for the board's screen ----
// The browser fetches the top headlines (NPR, or BBC News) as TidyOS does and hands them to the
// board, which also fetches them itself every 30 minutes when it has internet.
(function () {
  const FEEDS = [['https://feeds.npr.org/1001/rss.xml', 'NPR'], ['https://feeds.bbci.co.uk/news/rss.xml', 'BBC News']];
  async function tick() {
    for (const [u, name] of FEEDS) {
      try {
        const j = await fetch('https://api.rss2json.com/v1/api.json?rss_url=' + encodeURIComponent(u), { cache: 'no-store' }).then(r => r.json());
        if (j.status !== 'ok' || !j.items || !j.items.length) continue;
        const titles = j.items.slice(0, 6).map(x => String(x.title || '').trim()).filter(Boolean);
        await fetch('/api/board/news', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ titles, source: name }) });
        return;
      } catch (e) {}
    }
  }
  setTimeout(tick, 8000);
  setInterval(tick, 30 * 60000);
})();

// ---- Meshed File Transfer: send a file (up to 5 MB) to a board in range ----
// A little file manager to pick it (pictures as pictures, songs with their album art, PDFs,
// HTML games and programs with their thumbnails), the boards this one hears, and the progress.
(function () {
  let timer = null, cat = 'Pictures', files = [], picked = null, to = '', filter = '', lastSig = '';
  const SHELVES = [['Pictures', '🖼️'], ['Music', '🎵'], ['MIDI', '🎹'], ['Books', '📕'], ['Games', '🎮'], ['Programs', '🧰'], ['Videos', '🎬']];
  const ICON = f => {
    const e = (f.ext || '').toLowerCase();
    if (e === 'pdf') return '📕'; if (e === 'html' || e === 'htm') return cat === 'Games' ? '🎮' : '🌐';
    if (e === 'zip') return '🗜️'; if (e === 'exe' || e === 'msi') return '💾'; if (['mp3', 'm4a', 'wav', 'ogg', 'flac'].includes(e)) return '🎵';
    if (['mid', 'midi', 'kar'].includes(e)) return '🎹'; if (['mp4', 'webm', 'mov', 'mkv'].includes(e)) return '🎬';
    if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp'].includes(e)) return '🖼️'; if (['txt', 'md', 'epub'].includes(e)) return '📄';
    return '📦';
  };
  const fmt = b => b >= 1048576 ? (b / 1048576).toFixed(b >= 10485760 ? 0 : 1) + ' MB' : Math.max(1, Math.round(b / 1024)) + ' KB';
  const dur = s => s < 60 ? Math.max(1, Math.round(s)) + ' s' : s < 3600 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h';
  const viewing = () => typeof state !== 'undefined' && state.view === 'xfer';
  const post = (u, d) => fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(d || {}) });
  const RATE = { lora: 1800, wifi: 15000 };                 // bytes a second, roughly, for the estimate

  async function loadFiles() {
    files = [];
    try { files = await fetch('/api/files?category=' + encodeURIComponent(cat)).then(r => r.json()); } catch (e) {}
    if (!Array.isArray(files)) files = files.files || [];
    renderFiles();
  }
  function renderFiles() {
    const box = document.getElementById('xfFiles'); if (!box) return;
    const q = filter.toLowerCase();
    const list = files.filter(f => !q || f.name.toLowerCase().includes(q)).sort((a, b) => a.name.localeCompare(b.name));
    box.innerHTML = list.length ? list.map((f, i) => {
      const big = f.size > 5 * 1048576;
      const pic = cat === 'Pictures' && f.size < 3 * 1048576 ? f.url : (f.artUrl || f.thumbUrl || '');
      return `<button class="xf-file ${picked && picked.name === f.name ? 'on' : ''} ${big ? 'big' : ''}" data-i="${files.indexOf(f)}" ${big ? 'disabled title="Over 5 MB: too big to send"' : ''}>
        <span class="xf-thumb">${pic ? `<img data-qsrc="${escapeHtml(pic)}" alt="">` : `<span>${ICON(f)}</span>`}</span>
        <span class="xf-name">${escapeHtml(f.name)}</span><span class="xf-size">${big ? 'over 5 MB' : fmt(f.size)}</span></button>`;
    }).join('') : `<div class="coms-neigh" style="margin:8px 0">${files.length ? 'Nothing matches.' : 'Nothing on this shelf yet.'}</div>`;
    box.querySelectorAll('.xf-file').forEach(b => {
      b.onclick = () => { picked = files[+b.dataset.i]; renderFiles(); renderSend(); };
      b.ondblclick = () => { const f = files[+b.dataset.i]; if (f && f.url) window.open(f.url, '_blank'); };   // a look before sending
    });
  }
  let st = null;
  function renderSend() {
    const el2 = document.getElementById('xfSend'); if (!el2 || !st) return;
    const est = picked ? picked.size / (st.lora ? RATE.lora : RATE.wifi) : 0;
    el2.innerHTML = `<select id="xfTo">${st.peers.length ? st.peers.map(p => `<option value="${p.node}" ${p.node === to ? 'selected' : ''}>${escapeHtml(p.name)} (${p.rssi ? p.rssi + ' dBm' : '2.4 GHz'}, heard ${dur(p.ago)} ago)</option>`).join('') : '<option value="">No boards heard yet</option>'}</select>
      <button class="coms-btn" id="xfGo" ${picked && st.peers.length && !st.sending && !st.receiving ? '' : 'disabled'}>Send${picked ? ' ' + escapeHtml(picked.name.length > 28 ? picked.name.slice(0, 26) + '…' : picked.name) : ''}</button>
      <div class="coms-neigh" style="width:100%;margin:6px 0 0">${picked ? `${fmt(picked.size)}: about ${dur(est)} over ${st.lora ? '900 MHz' : '2.4 GHz'} (faster over 2.4 GHz when an ESP32 is on either end).` : 'Pick a file below, up to 5 MB.'}</div>`;
    const sel = el2.querySelector('#xfTo'); sel.onchange = () => { to = sel.value; };
    if (!to && st.peers.length) to = st.peers[0].node;
    el2.querySelector('#xfGo').onclick = async () => {
      const r = await post('/api/coms/files/send', { node: to || sel.value, cat, name: picked.name });
      if (!r.ok) { const j = await r.json().catch(() => ({})); alert(j.error || 'That didn’t work.'); }
      refresh(true);
    };
  }
  function render(s) {
    st = s;
    const content = el('#content');
    if (!document.getElementById('xfWrap')) {
      content.innerHTML = `<div class="coms" id="xfWrap">
        <div class="coms-card" id="xfTop"></div>
        <div class="coms-card" id="xfProg" style="display:none"></div>
        <div class="coms-card"><div class="xf-head"><b>Send a file</b><span class="coms-neigh" style="margin:0">to a board this one hears directly</span></div>
          <div class="xf-send" id="xfSend"></div>
          <div class="xf-shelves" id="xfShelves">${SHELVES.map(([c, i]) => `<button class="coms-chip ${c === cat ? 'on' : ''}" data-c="${c}">${i} ${c}</button>`).join('')}</div>
          <input id="xfFilter" class="xf-filter" placeholder="Find a file… (double-click one to open it)" autocomplete="off">
          <div class="xf-files" id="xfFiles"><div class="coms-neigh">Loading…</div></div></div>
        <div class="coms-card"><b>Sent and received</b><div id="xfHist"></div></div>
        <div class="coms-card coms-off" style="font-size:13px"><b>How it works:</b> the two boards agree over Meshed Comms, then move to a fast link of their own: on 900 MHz, 923.3 MHz at its fastest setting (about 2 KB a second: a 1 MB picture in about 9 minutes, 5 MB in about 45, with less range than chat: hundreds of yards to a mile or so), or over 2.4 GHz when an ESP32 is involved (much faster, about Wi-Fi's range). Chat pauses on those two boards meanwhile. Missing pieces are sent again, the whole file is checked at the end, and it lands on the same shelf on the other board.</div>
      </div>`;
      content.querySelectorAll('#xfShelves .coms-chip').forEach(b => b.onclick = () => { cat = b.dataset.c; picked = null; content.querySelectorAll('#xfShelves .coms-chip').forEach(x => x.classList.toggle('on', x === b)); loadFiles(); renderSend(); });
      const fi = content.querySelector('#xfFilter'); fi.oninput = () => { filter = fi.value; renderFiles(); };
      loadFiles();
    }
    const top = document.getElementById('xfTop');
    top.innerHTML = `<div class="coms-top">
        <label class="toggle-switch"><input type="checkbox" id="xfAccept" ${s.accept ? 'checked' : ''}><span class="toggle-slider"></span></label>
        <span class="power-label">${s.accept ? 'Taking files from other boards' : 'Not taking files'}</span>
        <span class="sub">${!s.enabled ? 'Meshed Comms is off: switch it on in the Meshed Comms tab to send or receive files.' : `Files that arrive go on the same shelf they came from. Links here: ${[s.lora && '900 MHz', s.wifi && '2.4 GHz'].filter(Boolean).join(' and ') || 'none yet'}.`}</span></div>`;
    top.querySelector('#xfAccept').onchange = async e => { await post('/api/coms/files/settings', { accept: e.target.checked }); refresh(true); };
    const pg = document.getElementById('xfProg');
    if (s.sending || s.receiving) {
      const pct = s.size ? Math.min(100, s.bytes * 100 / s.size) : 0, rate = s.secs > 2 && s.bytes ? s.bytes / s.secs : 0;
      pg.style.display = 'block';
      pg.innerHTML = `<div class="xf-head"><b>${s.sending ? 'Sending' : 'Receiving'} ${escapeHtml(s.name)}</b><span class="coms-neigh" style="margin:0">${s.sending ? 'to' : 'from'} ${escapeHtml(s.peer)}${s.band ? ' · ' + s.band : ''}</span></div>
        <div class="xf-bar"><div style="width:${pct.toFixed(1)}%"></div></div>
        <div class="xf-head" style="margin-top:6px"><span class="coms-neigh" style="margin:0">${escapeHtml(s.status)} · ${fmt(s.bytes)} of ${fmt(s.size)} (${pct.toFixed(0)}%)${rate ? ` · ${fmt(rate)}/s · about ${dur((s.size - s.bytes) / rate)} to go` : ''}</span>
        <button class="coms-btn secondary" id="xfStop">Stop</button></div>`;
      pg.querySelector('#xfStop').onclick = () => post('/api/coms/files/cancel').then(() => refresh(true));
    } else if (s.status) {
      pg.style.display = 'block';
      pg.innerHTML = `<div class="coms-neigh" style="margin:0">${s.lastOk ? '✅' : '⚠️'} ${escapeHtml(s.status)}</div>`;
    } else pg.style.display = 'none';
    document.getElementById('xfHist').innerHTML = s.history.length ? s.history.map(h => `<div class="xf-hist">
        <span>${h.ok ? (h.sent ? '⬆️' : '⬇️') : '⚠️'}</span>
        <span><b>${escapeHtml(h.name)}</b><small>${h.sent ? 'to' : 'from'} ${escapeHtml(h.peer)} · ${fmt(h.size)}${h.ok ? ' · ' + dur(h.secs) : ''} · ${escapeHtml(h.note)}</small></span></div>`).join('')
      : '<div class="coms-neigh">Nothing yet.</div>';
    renderSend();
  }
  async function refresh(force) {
    if (!viewing()) { clearInterval(timer); timer = null; return; }
    let s; try { s = await fetch('/api/coms/files', { cache: 'no-store' }).then(r => r.json()); } catch (e) { return; }
    const sig = JSON.stringify([s.phase, s.bytes, s.status, s.history.length, s.accept, s.peers.map(p => p.node + p.name), s.enabled]);
    if (!force && sig === lastSig) return;
    lastSig = sig;
    if (document.activeElement && document.activeElement.id === 'xfTo') return;
    render(s);
  }
  window.loadXferView = function () {
    setToolHeader('Meshed File Transfer', 'Send a file, up to 5 MB, to a board in range', null, null);
    el('#content').innerHTML = ''; lastSig = ''; picked = null; filter = '';
    refresh(true);
    clearInterval(timer);
    timer = setInterval(() => refresh(false), 1500);
  };
})();

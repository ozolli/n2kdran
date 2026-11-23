/*
 * n2kdran.js — n2kdran dans un navigateur : même cœur C (libn2k en WebAssembly),
 * même écran que l'appli Android (portage de DashboardView.kt, Engine.kt, Cells.kt,
 * SailChart.kt). Les flux arrivent par WebSocket depuis relay.py.
 *
 * Gestes (comme sur la tablette, à la souris ou au doigt) : appui long = mode nuit ;
 * double appui = mise au point ; triple appui sur une case = choix de la donnée ;
 * appui sur les indicateurs (bas gauche) = réglages ; appui sur le point rouge d'une
 * alerte AIS = acquitter, puis passer à la suivante.
 */
'use strict';

// ============================================================================
// Cœur C (WebAssembly)
// ============================================================================
class Core {
  constructor(M) {
    this.M = M;
    M._w_init();
    const n = M._w_item_count();
    this.names = [];
    for (let i = 0; i < n; i++) this.names.push(M.UTF8ToString(M._w_item_name(i)));
    this.index = new Map(this.names.map((s, i) => [s, i]));
    this.values = new Float64Array(n);
    this.status = new Uint8Array(n);
    this.enc = new TextEncoder();
    this.inPtr = M._w_in();
    this.inSize = M._w_in_size();
  }
  idx(name) { return this.index.has(name) ? this.index.get(name) : -1; }

  /** Pousse du texte (lignes séparées par LF). */
  feed(text, now, navOnly) {
    if (text.length > this.inSize - 1024) {           // gros bloc : par paquets de lignes
      const lines = text.split('\n');
      let part = '';
      for (const l of lines) {
        if (part.length + l.length + 1 > this.inSize - 1024) { this.feedRaw(part, now, navOnly); part = ''; }
        part += l + '\n';
      }
      if (part) this.feedRaw(part, now, navOnly);
    } else this.feedRaw(text, now, navOnly);
  }
  feedRaw(text, now, navOnly) {
    const b = this.enc.encode(text);
    const n = Math.min(b.length, this.inSize);
    this.M.HEAPU8.set(b.subarray(0, n), this.inPtr);
    this.M._w_feed(n, now, navOnly ? 1 : 0);
  }

  snapshot(now) {
    this.M._w_snapshot(now);
    const n = this.names.length;
    this.values.set(new Float64Array(this.M.HEAPF64.buffer, this.M._w_values(), n));
    this.status.set(new Uint8Array(this.M.HEAPU8.buffer, this.M._w_status(), n));
  }
  setStaleMs(ms) { this.M._w_set_stale(ms); }
  setAisIgnoreStill(on) { this.M._w_ais_still(on ? 1 : 0); }

  /** Installe une polaire (texte) ; null si OK, sinon le message d'erreur. */
  setPolar(text) {
    const b = this.enc.encode(text);
    if (b.length > this.inSize) return 'polaire trop grande';
    this.M.HEAPU8.set(b, this.inPtr);
    const e = this.M._w_set_polar(b.length);
    return e ? this.M.UTF8ToString(e) : null;
  }
  vmgProfile(out) {
    if (!this.M._w_vmg_profile()) return false;
    out.set(new Float64Array(this.M.HEAPF64.buffer, this.M._w_profile(), 181));
    return true;
  }
  wpName(now) { const p = this.M._w_wp_name(now); return p ? this.M.UTF8ToString(p) : null; }
  stats() { return Array.from(new Float64Array(this.M.HEAPF64.buffer, this.M._w_stats(), 4)); }
  aisReset() { this.M._w_ais_reset(); }

  _skip(skip) {
    const a = Array.from(skip).slice(0, 64);
    const v = new Uint32Array(this.M.HEAPU32.buffer, this.M._w_skip(), 64);
    a.forEach((m, i) => { v[i] = m >>> 0; });
    return a.length;
  }
  _ais(level) {
    if (!level) return null;
    const v = new Float64Array(this.M.HEAPF64.buffer, this.M._w_ais_v(), 7);
    const name = this.M.UTF8ToString(this.M._w_ais_name()).trim();
    return { level: v[0], mmsi: v[1], name: name || null, range: v[2], bearing: v[3],
             cpa: v[4], tcpa: v[5], est: v[6] !== 0,
             get isMob() { return this.level === 972 || this.level === MOB_N2K; } };
  }
  aisThreat(now, cpa, tcpa, range, skip = []) {
    const n = this._skip(skip);
    return this._ais(this.M._w_ais_threat(now, cpa, tcpa, range, n));
  }
  aisDistress(now, skip = []) {
    const n = this._skip(skip);
    return this._ais(this.M._w_ais_distress(now, n));
  }
  /** Hommes à la mer en cours (le cœur rend les MOB en premier). */
  aisMobs(now) {
    const out = [];
    while (out.length < 16) {
      const d = this.aisDistress(now, out.map(t => t.mmsi));
      if (!d || !d.isMob) break;
      out.push(d);
    }
    return out;
  }
}
const MOB_N2K = 127233;
const OK = 2, NEVER = 0;

// ============================================================================
// Réglages (stockage du navigateur)
// ============================================================================
const DEFAULT_COLUMNS = [
  ['bsp', 'cible', 'vmgpct', 'twa', 'tws'],
  ['sog', 'awa', 'aws', 'gite', 'fond'],
  ['bords', 'ttw', 'dtw', 'wpt', 'fond'],
];
const Settings = {
  _d: { staleS: 3, headingRef: 'V', sailRose: true, transducerDepth: 0, nightDim: 60,
        aisCpa: 0.5, aisTcpa: 20, aisRange: 15, aisStill: false, columns: DEFAULT_COLUMNS,
        polarName: null, polarText: null, sailName: null, saildef: null, sailselect: null },
  _v: {},
  load() {
    let s = {};
    try { s = JSON.parse(localStorage.getItem('n2kdran') || '{}') || {}; } catch (e) { s = {}; }
    this._v = Object.assign({}, this._d, s);
    if (!Array.isArray(this._v.columns) || this._v.columns.length !== 3 ||
        this._v.columns.some(c => !Array.isArray(c) || c.length !== 5))
      this._v.columns = DEFAULT_COLUMNS.map(c => c.slice());
  },
  save() { try { localStorage.setItem('n2kdran', JSON.stringify(this._v)); } catch (e) { /* stockage refusé */ } },
  get(k) { return this._v[k]; },
  set(k, v) { this._v[k] = v; this.save(); },
};

// ============================================================================
// Cases
// ============================================================================
const CELLS = [
  ['bsp', 'BSP', 'BSP', 'kn', 'SPEED'],
  ['cible', 'CIBLE', 'TARGET_BSP', 'kn', 'SPEED'],
  ['sog', 'SOG', 'SOG', 'kn', 'SPEED'],
  ['vmg', 'VMG', 'VMG', 'kn', 'SPEED_ABS'],
  ['vmgpct', 'VMG', 'VMG_PCT', '%', 'PCT'],
  ['polpct', 'POLAIRE', 'POLAR_PCT', '%', 'PCT'],
  ['twa', 'TWA', 'TWA', '°', 'ANGLE_SIDE'],
  ['awa', 'AWA', 'AWA', '°', 'ANGLE_SIDE'],
  ['tws', 'TWS', 'TWS', 'kn', 'SPEED'],
  ['aws', 'AWS', 'AWS', 'kn', 'SPEED'],
  ['twd', 'TWD', 'TWD', '°', 'DIR'],
  ['twsavg', "TWS 10'", 'TWS_AVG', 'kn', 'SPEED'],
  ['twdavg', "TWD 10'", 'TWD_AVG', '°', 'DIR'],
  ['angopt', 'ANGLE OPT', 'TARGET_TWA', '°', 'ANGLE'],
  ['cap', 'CAP', 'HDG_TRUE', '°', 'DIR'],
  ['cog', 'COG', 'COG', '°', 'DIR'],
  ['gite', 'GÎTE', 'ROLL', '°', 'SIGNED_ANGLE'],
  ['tangage', 'TANGAGE', 'PITCH', '°', 'SIGNED_ANGLE'],
  ['barre', 'BARRE', 'RUDDER', '°', 'SIGNED_ANGLE'],
  ['fond', 'FOND', 'DEPTH', 'm', 'DEPTH'],
  ['loch', 'LOCH', 'LOG', 'NM', 'DIST'],
  ['journalier', 'JOURNALIER', 'TRIP', 'NM', 'DIST'],
  ['courant', 'COURANT', 'DRIFT', 'kn', 'SPEED'],
  ['dircourant', 'DIR. COURANT', 'SET', '°', 'DIR'],
  ['derive', 'DÉRIVE', 'LEEWAY', '°', 'ANGLE1'],
  ['teau', 'T° EAU', 'SEA_TEMP', '°C', 'TEMP'],
  ['tair', 'T° AIR', 'AIR_TEMP', '°C', 'TEMP'],
  ['pression', 'PRESSION', 'PRESSURE', 'hPa', 'PRESS'],
  ['voile', 'VOILE', '', '', 'SAIL'],
  ['sailchart', 'SAIL', '', '', 'SAILCHART'],
  ['bords', 'BORDS', 'TTW_PORT', '', 'PS'],
  ['ttw', 'TTW', 'TTW', '', 'TIME'],
  ['dtw', 'DTW', 'DTW_SAIL', 'NM', 'DIST'],
  ['dtwdirect', 'DTW DIRECT', 'DTW', 'NM', 'DIST'],
  ['wpt', 'WPT', 'BTW', '°', 'DIR'],
  ['vmc', 'VMC', 'WCV', 'kn', 'SPEED'],
  ['xte', 'XTE', 'XTE', 'NM', 'XTE'],
  ['sats', 'SATELLITES', 'GNSS_SATS', '', 'INT'],
  ['hdop', 'HDOP', 'HDOP', '', 'NUM1'],
  ['ais', 'AIS', '', '', 'AIS'],
].map(([key, label, item, unit, kind]) => ({ key, label, item, unit, kind }));
const CELL_BY_KEY = new Map(CELLS.map(c => [c.key, c]));
const cellDef = k => CELL_BY_KEY.get(k) || CELLS[0];

// ============================================================================
// Sail chart qtVlm (portage de SailChart.kt)
// ============================================================================
const SAIL_PALETTE = [
  0x00FF00, 0x4D8DFF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xB4F4FF, 0x4CB050, 0xFF2020, 0x803200, 0x800080,
  0x008080, 0xFF6600, 0x00FF66, 0x6600FF, 0x66FF00, 0xFF0066, 0x0066FF, 0xCC0000, 0x00CC00, 0x0000CC,
  0xCCCC00, 0xCC00CC, 0x00CCCC, 0x990000, 0x009900, 0x000099, 0x999900, 0x990099, 0x009999, 0xFF3300,
  0x33FF00, 0x0033FF, 0xFF0033, 0x00FF33, 0x3300FF, 0xCC3300, 0x33CC00, 0x0033CC, 0xCC0033, 0x00CC33,
  0x3300CC, 0xFF6699, 0x66FF99, 0x9966FF, 0x99FF66, 0xFF9966, 0x6699FF, 0xDD0000, 0x00DD00, 0x0000DD,
].map(c => (0xFF000000 | c) >>> 0);
const SAIL_NONE = 0xFF505050;

class SailChart {
  constructor(names, twa, tws, grid) { this.names = names; this.twa = twa; this.tws = tws; this.grid = grid; }
  static parse(saildef, sailselect) {
    const names = new Map();
    for (const raw of saildef.split(/\r?\n/)) {
      const l = raw.trim();
      if (!l) continue;
      const i = l.indexOf(';');
      const code = i > 0 ? parseInt(l.slice(0, i).trim(), 10) : NaN;
      if (i < 0 || !Number.isInteger(code) || !/^\s*-?\d+\s*$/.test(l.slice(0, i)))
        throw new Error(`saildef : ligne invalide « ${l} »`);
      names.set(code, l.slice(i + 1).trim());
    }
    if (!names.size) throw new Error('saildef vide');
    const cells = l => l.trim().split(/[;\t]/).map(s => s.trim());
    const lines = sailselect.split(/\r?\n/).map(s => s.trim()).filter(s => s);
    if (lines.length < 2) throw new Error('sailselect : pas de données');
    const tws = cells(lines[0]).slice(1).map(s => {
      const x = Number(s); if (s === '' || isNaN(x)) throw new Error(`sailselect : TWS invalide « ${s} »`); return x;
    });
    const twa = [], grid = [];
    for (const l of lines.slice(1)) {
      const c = cells(l);
      if (c.length !== tws.length + 1) throw new Error(`sailselect : nombre de colonnes incohérent (« ${l.slice(0, 30)}… »)`);
      const a = Number(c[0]);
      if (c[0] === '' || isNaN(a)) throw new Error(`sailselect : TWA invalide « ${c[0]} »`);
      twa.push(a);
      grid.push(c.slice(1).map(s => {
        if (!/^-?\d+$/.test(s)) throw new Error(`sailselect : code invalide « ${s} »`); return parseInt(s, 10);
      }));
    }
    return new SailChart(names, twa, tws, grid);
  }
  _frac(axis, x) {
    if (x <= axis[0]) return 0;
    for (let k = 0; k < axis.length - 1; k++)
      if (x <= axis[k + 1]) return k + (x - axis[k]) / (axis[k + 1] - axis[k]);
    return axis.length - 1;
  }
  /** Code pour |TWA| (°) et TWS (kn) : vote gaussien (σ = 1 maille) des points voisins. */
  codeAt(twaDeg, twsKn) {
    if (!this.twa.length || !this.tws.length) return null;
    const fi = this._frac(this.twa, Math.min(180, Math.max(0, Math.abs(twaDeg))));
    const fj = this._frac(this.tws, Math.max(0, twsKn));
    const i0 = Math.round(fi), j0 = Math.round(fj);
    const votes = new Map();
    for (let i = i0 - 3; i <= i0 + 3; i++) {
      if (i < 0 || i >= this.twa.length) continue;
      for (let j = j0 - 3; j <= j0 + 3; j++) {
        if (j < 0 || j >= this.tws.length) continue;
        const di = i - fi, dj = j - fj;
        const w = Math.exp(-(di * di + dj * dj) / 2);
        const c = this.grid[i][j];
        votes.set(c, (votes.get(c) || 0) + w);
      }
    }
    let best = null, bw = -1;
    for (const [c, w] of votes) if (w > bw) { bw = w; best = c; }
    return best !== null ? best : this.grid[i0][j0];
  }
  name(code) { return code == null ? null : (this.names.has(code) ? this.names.get(code) : null); }
  isNone(code) { const n = this.names.get(code); return n != null && n.trim().toUpperCase() === 'N/A'; }
  color(code) {
    if (code == null || this.isNone(code)) return SAIL_NONE;
    return SAIL_PALETTE[code - 1] !== undefined ? SAIL_PALETTE[code - 1] : SAIL_NONE;
  }
  static luminance(c) {
    return 0.299 * ((c >>> 16) & 255) / 255 + 0.587 * ((c >>> 8) & 255) / 255 + 0.114 * (c & 255) / 255;
  }
}

// ============================================================================
// Moteur : liaisons, historiques, voilure, alarmes AIS (portage d'Engine.kt)
// ============================================================================
const Engine = {
  core: null,
  source: '',                 // source N2K annoncée par le relais
  n2kState: 'DISCONNECTED',   // CONNECTING, CONNECTED, DISCONNECTED (relais ↔ source)
  wsUp: false,                // page ↔ relais
  lastError: null,
  lastLineMs: 0,
  navLastMs: 0,
  navPackets: 0,
  polarMsg: '', sailMsg: '',
  sailChart: null, sailCode: null, sailCandidate: null, sailCandidateT: 0,
  rudHist: [], windHist: [], windLastT: 0,
  aisAcked: new Set(), aisSkipped: new Set(),
  aisCpaM: 0.5 * 1852, aisTcpaS: 20 * 60, aisRangeM: 15 * 1852,

  async applySettings() {
    const c = this.core;
    c.setStaleMs(Settings.get('staleS') * 1000);
    c.setAisIgnoreStill(Settings.get('aisStill'));
    this.aisCpaM = Settings.get('aisCpa') * 1852;
    this.aisTcpaS = Settings.get('aisTcpa') * 60;
    this.aisRangeM = Settings.get('aisRange') * 1852;
    try {
      let name = Settings.get('polarName'), text = Settings.get('polarText');
      if (!name || !text) { name = 'CM50 (défaut)'; text = await fetchText('CM50.pol'); }
      const e = c.setPolar(text);
      this.polarMsg = e ? `polaire ${name} : ${e}` : `polaire ${name}`;
    } catch (e) { this.polarMsg = 'polaire : ' + e.message; }
    try {
      let name = Settings.get('sailName'), def = Settings.get('saildef'), sel = Settings.get('sailselect');
      if (!name || !def || !sel) {
        name = 'CM50 (défaut)';
        [def, sel] = await Promise.all([fetchText('CM50.saildef'), fetchText('CM50.sailselect')]);
      }
      this.sailChart = SailChart.parse(def, sel);
      this.sailMsg = 'voiles ' + name;
    } catch (e) { this.sailChart = null; this.sailMsg = 'voiles : ' + e.message; }
    this.sailCode = null; this.sailCandidate = null;
  },

  connect() {
    const base = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host;
    const open = (path, onText, onState) => {
      let ws;
      const start = () => {
        ws = new WebSocket(base + path);
        ws.onopen = () => onState(true);
        ws.onmessage = ev => onText(ev.data);
        ws.onclose = () => { onState(false); setTimeout(start, 2000); };
        ws.onerror = () => ws.close();
      };
      start();
    };
    open('/n2k', text => {
      const now = performance.now();
      if (text.startsWith('#')) {
        if (text.startsWith('#C')) { this.n2kState = 'CONNECTED'; this.lastError = null; }
        else if (text.startsWith('#D')) { this.n2kState = 'DISCONNECTED'; this.lastError = text.slice(3); }
        else if (text.startsWith('#S')) this.source = text.slice(3);
        return;
      }
      this.n2kState = 'CONNECTED';
      this.lastLineMs = now;
      this.core.feed(text, now, false);
    }, up => { this.wsUp = up; this.n2kState = up ? 'CONNECTING' : 'DISCONNECTED'; if (!up) this.lastError = 'relais injoignable'; });
    open('/nav', text => {
      const now = performance.now();
      this.navLastMs = now; this.navPackets++;
      this.core.feed(text.split('\n').map(s => s.trim()).filter(s => s).join('\n'), now, true);
    }, () => {});
  },

  updateSail(now, twa, tws) {
    const chart = this.sailChart;
    if (!chart || twa == null || tws == null) { this.sailCode = null; this.sailCandidate = null; return; }
    const code = chart.codeAt(twa * DEG, tws * KN);
    if (this.sailCode == null) { this.sailCode = code; this.sailCandidate = code; return; }
    if (code !== this.sailCandidate) { this.sailCandidate = code; this.sailCandidateT = now; }
    if (code !== this.sailCode && now - this.sailCandidateT >= 3000) this.sailCode = code;
  },

  /** Échantillonnage à 5 Hz : barre (2 min), vent (10 min), voilure, alarmes AIS. */
  tick() {
    const c = this.core, now = performance.now();
    c.snapshot(now);
    const ok = n => { const i = c.idx(n); return i >= 0 && c.status[i] === OK ? c.values[i] : null; };
    const rud = ok('RUDDER');
    if (rud != null) this.rudHist.push([now, rud * DEG]);
    while (this.rudHist.length && now - this.rudHist[0][0] > 120000) this.rudHist.shift();
    if (now - this.windLastT >= 1000) {
      this.windLastT = now;
      this.windHist.push({ t: now, twa: ok('TWA'), awa: ok('AWA'), twd: ok('TWD') });
      while (this.windHist.length && now - this.windHist[0].t > WIND_HISTORY_MS) this.windHist.shift();
    }
    this.updateSail(now, ok('TWA'), ok('TWS'));
    const skip = [...this.aisSkipped];
    const mobs = c.aisMobs(now);
    if (mobs.length) {
      this.distressAlarm(now, mobs.find(m => !this.aisAcked.has(m.mmsi)) || null);
      this.dangerBeep(now, null);
    } else {
      const distress = c.aisDistress(now, [...this.aisAcked]);
      let threat = c.aisThreat(now, this.aisCpaM, this.aisTcpaS, this.aisRangeM, skip);
      if (threat && threat.level !== 2) threat = null;
      if (this.aisAcked.size && !c.aisDistress(now) &&
          (c.aisThreat(now, this.aisCpaM, this.aisTcpaS, this.aisRangeM) || {}).level !== 2) {
        this.aisAcked.clear(); this.aisSkipped.clear();         // plus aucune alerte : réarmement
      }
      this.distressAlarm(now, distress);
      this.dangerBeep(now, threat && !this.aisAcked.has(threat.mmsi) ? threat : null);
    }
  },

  /** Appui sur le point rouge : acquitte, ou passe à la suivante si déjà acquittée (sauf MOB). */
  aisTap(mmsi) {
    const mob = this.core.aisMobs(performance.now()).some(t => t.mmsi === mmsi);
    if (this.aisAcked.has(mmsi)) { if (!mob) this.aisSkipped.add(mmsi); }
    else this.aisAcked.add(mmsi);
  },

  // --- Sons (Web Audio : le navigateur n'en joue qu'après un premier clic dans la page) ---
  audio: null, alarmOn: false, alarmLastT: 0, beepLastT: 0,
  tone(freqs, dur, gain = 0.3) {
    const a = this.audio;
    if (!a || a.state !== 'running') return;
    const t0 = a.currentTime;
    const g = a.createGain();
    g.gain.value = gain;
    g.connect(a.destination);
    freqs.forEach(([f, start, len]) => {
      const o = a.createOscillator();
      o.type = 'square'; o.frequency.value = f;
      o.connect(g); o.start(t0 + start); o.stop(t0 + start + Math.min(len, dur));
    });
  },
  distressAlarm(now, d) {
    this.alarmOn = !!d;
    if (!d || now - this.alarmLastT < 1500) return;
    this.alarmLastT = now;
    // Deux tons alternés, façon alarme d'urgence.
    const seq = [];
    for (let k = 0; k < 8; k++) seq.push([k % 2 ? 1500 : 1000, k * 0.125, 0.12]);
    this.tone(seq, 1.0, 0.35);
  },
  dangerBeep(now, t) {
    if (this.alarmOn || !t || now - this.beepLastT < 3000) return;
    this.beepLastT = now;
    this.tone([[1200, 0, 0.15], [1200, 0.25, 0.15]], 0.4, 0.25);
  },

  windHistory() { return this.windHist; },
  rudderRange() {
    if (this.rudHist.length < 2) return null;
    let lo = Infinity, hi = -Infinity;
    for (const [, d] of this.rudHist) { if (d < lo) lo = d; if (d > hi) hi = d; }
    return [lo, hi];
  },
};
const WIND_HISTORY_MS = 600000;

async function fetchText(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url} : ${r.status}`);
  return r.text();
}

// ============================================================================
// Dessin (portage de DashboardView.kt) — repère virtuel 1280 × 800
// ============================================================================
const PI = Math.PI;
const DEG = 180 / PI;
const KN = 3600 / 1852;
const VW = 1280, VH = 800, CX = 640, CY = 400;
const K = 800 / 1080;
const CARD_S = 512 / 437;
const R_CARD_IN = 381 * CARD_S * K;
const R_CARD_OUT = 512 * K;
const R_GAP_IN = 515 * K;
const R_GAP_OUT = 540 * K;
const HDG_DY = -(409 * CARD_S - 406);
const R_DOTS = 409 * CARD_S * K;
const NEEDLE_TIP = 540 - 409 * CARD_S;
const NEEDLE_BASE = NEEDLE_TIP + 99.7;
const R_SAIL = R_CARD_IN - 4;
const ROSE_RES = 2;
const SAIL_ROSE_ALPHA = 0x80;
const ROSE_S = 0.97;
const R_TRAIL_OUT = (540 - NEEDLE_BASE) * K;
const R_TRAIL_IN = R_CARD_IN * 0.2;
const TRAIL_W = 4;
const TWD_SHIFT_FULL = 10;
const OPP_SECTOR_ALPHA = 0x90;
const OPP_SECTOR_W = 26;
const R_TAG = R_CARD_IN - OPP_SECTOR_W / 2;
const LAY_HIST_ALPHA = 0xA0;
const LAY_BAND_ALPHA = 0x2C;
const WP_SEE_THROUGH = 75;
const R_AIS_DOT = (R_GAP_IN + R_GAP_OUT) / 2;
const BAN_L = 310, BAN_R = 970, BAN_T = 290, BAN_B = 510;
const BTN_X = 292, BTN_Y = 40, BTN_R = 24;
const CH_W = 220, CH_H = 106, CH_TWA0 = 40, CH_TWA1 = 180;
const STEPS = 1440, SMOOTH = 12;
const CURRENT_COL = 0xB300B3B3;
const FONT = '"Roboto", "Noto Sans", "DejaVu Sans", Arial, sans-serif';

const P = {
  bg: 0xFF000000, fg: 0xFFFFFFFF, label: 0xFFB0B0B0, stale: 0xFF505050, grid: 0xFF303030,
  stbd: 0xFF2ECC40, port: 0xFFFF4136, ok: 0xFF2ECC40, bad: 0xFFFF4136,
};

/** Couleur ARGB (entier) → CSS ; `alpha` (0–255) remplace l'opacité si donné. */
function css(c, alpha) {
  const a = alpha === undefined ? (c >>> 24) & 255 : alpha;
  return `rgba(${(c >>> 16) & 255},${(c >>> 8) & 255},${c & 255},${a / 255})`;
}
function lerpColor(a, b, t) {
  const ch = (x, sh) => (x >>> sh) & 255;
  const mix = sh => Math.round(ch(a, sh) + (ch(b, sh) - ch(a, sh)) * t);
  return ((0xFF << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0)) >>> 0;
}
const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
const norm360 = d => ((d % 360) + 360) % 360;
const fmt1 = x => x.toFixed(1);
const pad3 = n => String(n).padStart(3, '0');
function angDiff(a, b) {
  let d = (a - b) % (2 * PI);
  if (d > PI) d -= 2 * PI;
  if (d <= -PI) d += 2 * PI;
  return d;
}
function fmtTime(sec) {
  const t = Math.max(0, Math.round(sec));
  return t < 3600 ? `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`
                  : `${Math.floor(t / 3600)}h${String(Math.floor((t % 3600) / 60)).padStart(2, '0')}`;
}
function fmtDist(m) {
  if (m < 1000) return `${Math.round(m)} m`;
  if (m < 10 * 1852) return `${(m / 1852).toFixed(2)} NM`;
  return fmt1(m / 1852) + ' NM';
}
const px = (a, r) => CX + r * Math.sin(a);
const py = (a, r) => CY - r * Math.cos(a);
const arcStart = a => a - PI / 2;          // angle écran (0 = haut) → angle canvas (0 = 3 h)

class Dashboard {
  constructor(canvas, core) {
    this.cv = canvas;
    this.c = canvas.getContext('2d');
    this.core = core;
    this.headingRef = 'V';
    this.transducerDepth = 0;
    this.sailRose = true;
    this.columns = DEFAULT_COLUMNS;
    this.dim = 0;
    this.aisCpaNm = 0.5; this.aisTcpaMin = 20; this.aisRangeNm = 15;
    this.aisThreat = null; this.aisDistress = null; this.aisMob = false; this.aisMobs = [];
    this.aisDots = [];
    this.hdg = 0;
    this.profile = new Float64Array(181);
    this.base = new Uint32Array(STEPS);
    this.smooth = new Uint32Array(STEPS);
    this.circleOk = false;
    this.boat = new Image();
    this.boat.src = 'cm50_outline.png';
    this.chartBmpOf = null; this.chartBmp = null;
    this.roseBmpOf = null; this.roseBmp = null;
    this.trail = document.createElement('canvas');
    const ix = n => core.idx(n);
    Object.assign(this, {
      iVar: ix('VARIATION'), iHdg: ix('HDG_TRUE'), iCog: ix('COG'), iTwd: ix('TWD'), iTwa: ix('TWA'),
      iAwa: ix('AWA'), iSet: ix('SET'), iDrift: ix('DRIFT'), iBtw: ix('BTW'), iTgt: ix('TARGET_TWA'),
      iTwaPct: ix('TWA_VMG_PCT'), iRudder: ix('RUDDER'), iLs: ix('LAY_STBD_GND'), iLp: ix('LAY_PORT_GND'),
      iLsMin: ix('LAY_STBD_GND_MIN'), iLsMax: ix('LAY_STBD_GND_MAX'), iLpMin: ix('LAY_PORT_GND_MIN'),
      iLpMax: ix('LAY_PORT_GND_MAX'), iLsLo: ix('LAY_STBD_GND_LO'), iLsHi: ix('LAY_STBD_GND_HI'),
      iLpLo: ix('LAY_PORT_GND_LO'), iLpHi: ix('LAY_PORT_GND_HI'), iOppTwa: ix('OPP_TWA_GND'),
      iTwdAvg: ix('TWD_AVG'), iDepthOff: ix('DEPTH_OFFSET'), iDtw: ix('DTW'), iTtwS: ix('TTW_STBD'),
      iTtwP: ix('TTW_PORT'), iTws: ix('TWS'),
    });
  }

  v(i) { return i >= 0 && this.core.status[i] === OK ? this.core.values[i] : null; }
  wpActive() { return this.v(this.iDtw) != null && this.v(this.iBtw) != null; }
  sa(b) { return b - this.hdg; }

  // --- Outils canvas ---
  font(size) { this.c.font = `bold ${size}px ${FONT}`; }
  text(s, x, y, align, color) {
    const c = this.c;
    c.textAlign = align; c.fillStyle = css(color); c.fillText(s, x, y);
  }
  rot(a) { const c = this.c; c.translate(CX, CY); c.rotate(a); c.translate(-CX, -CY); }
  line(x0, y0, x1, y1) { const c = this.c; c.beginPath(); c.moveTo(x0, y0); c.lineTo(x1, y1); c.stroke(); }
  circle(x, y, r) { const c = this.c; c.beginPath(); c.arc(x, y, r, 0, 2 * PI); }
  stroke(color, w, cap = 'butt') { const c = this.c; c.strokeStyle = css(color); c.lineWidth = w; c.lineCap = cap; }
  /** Ajuste la taille si le texte dépasse `max`. */
  fit(s, size, max) {
    this.font(size);
    const w = this.c.measureText(s).width;
    if (w > max) this.font(size * max / w);
  }

  // --- Image complète ---
  draw() {
    const cv = this.cv, c = this.c;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.round(cv.clientWidth * dpr), h = Math.round(cv.clientHeight * dpr);
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    c.setTransform(1, 0, 0, 1, 0, 0);
    c.fillStyle = css(P.bg); c.fillRect(0, 0, w, h);
    const s = Math.min(w / VW, h / VH);
    this.scale = s;
    c.setTransform(s, 0, 0, s, (w - VW * s) / 2, (h - VH * s) / 2);
    c.textBaseline = 'alphabetic';
    const now = performance.now();
    const core = this.core;
    const skip = [...Engine.aisSkipped];
    this.aisThreat = core.aisThreat(now, this.aisCpaNm * 1852, this.aisTcpaMin * 60, this.aisRangeNm * 1852, skip);
    this.aisMobs = core.aisMobs(now);
    this.aisMob = this.aisMobs.length > 0;
    this.aisDistress = this.aisMobs[0] || core.aisDistress(now, skip) || core.aisDistress(now);
    this.drawColumn(this.columns[0], 0);
    this.drawColumn(this.columns[this.wpActive() ? 2 : 1], VW - 240);
    this.drawRose();
    const d = this.aisDistress;
    if (d && (this.aisMob || !Engine.aisAcked.has(d.mmsi))) this.drawDistress(d);
    c.setTransform(1, 0, 0, 1, 0, 0);
    if (this.dim > 0) { c.fillStyle = `rgba(0,0,0,${this.dim})`; c.fillRect(0, 0, w, h); }
  }

  /** Coordonnées virtuelles d'un point de l'écran (pixels CSS). */
  toVirtual(x, y) {
    const cv = this.cv, s = Math.min(cv.clientWidth / VW, cv.clientHeight / VH);
    return [(x - (cv.clientWidth - VW * s) / 2) / s, (y - (cv.clientHeight - VH * s) / 2) / s];
  }

  // --- Colonnes de cases ---
  drawColumn(keys, x0) {
    const c = this.c;
    this.stroke(P.grid, 2);
    c.strokeRect(x0 + 1, 1, 238, keys.length * 160 - 2);
    for (let k = 1; k < keys.length; k++) this.line(x0, k * 160, x0 + 240, k * 160);
    keys.forEach((key, k) => {
      const cell = cellDef(key);
      const y0 = k * 160;
      const value = this.v(this.core.idx(cell.item));
      this.font(26);
      this.text(cell.label, x0 + 14, y0 + 34, 'left', P.label);
      const lw = c.measureText(cell.label).width;
      if (cell.kind === 'ANGLE_SIDE' && value != null) {
        c.fillStyle = css(value >= 0 ? P.stbd : P.port);
        this.circle(x0 + 26 + lw, y0 + 25, 9); c.fill();
      }
      if (cell.kind === 'XTE' && value != null && Math.abs(value) >= 0.5) {
        c.fillStyle = css(value < 0 ? P.stbd : P.port);
        this.circle(x0 + 26 + lw, y0 + 25, 9); c.fill();
      }
      const unit = cell.kind === 'XTE' && value != null && Math.abs(value) < 185.2 ? 'm' : cell.unit;
      this.text(unit, x0 + 226, y0 + 34, 'right', P.label);

      if (cell.kind === 'AIS') return this.drawAisCell(x0, y0);
      if (cell.kind === 'SAIL') return this.drawSailCell(x0, y0);
      if (cell.kind === 'SAILCHART') return this.drawSailChartCell(x0, y0);
      if (cell.kind === 'PS') {
        this.font(56);
        const tp = this.v(this.iTtwP), ts = this.v(this.iTtwS);
        this.text('P ' + (tp != null ? fmtTime(tp) : '---'), x0 + 16, y0 + 92, 'left', tp == null ? P.stale : P.port);
        this.text('S ' + (ts != null ? fmtTime(ts) : '---'), x0 + 16, y0 + 148, 'left', ts == null ? P.stale : P.stbd);
        return;
      }
      let color = P.fg, str;
      if (value == null) { color = P.stale; str = '---'; }
      else switch (cell.kind) {
        case 'SPEED': str = fmt1(value * KN); break;
        case 'SPEED_ABS': str = fmt1(Math.abs(value) * KN); break;
        case 'PCT': str = String(Math.round(value)); break;
        case 'DEPTH': {
          const off = this.v(this.iDepthOff);
          str = fmt1(value + (off != null && off > 0 ? off : this.transducerDepth)); break;
        }
        case 'DIR': str = pad3(Math.round(norm360(value * DEG)) % 360); break;
        case 'ANGLE_SIDE': case 'ANGLE': str = String(Math.round(Math.abs(value * DEG))); break;
        case 'ANGLE1': str = fmt1(Math.abs(value * DEG)); break;
        case 'SIGNED_ANGLE': { const r = Math.round(value * DEG); str = (r >= 0 ? '+' : '') + r; break; }
        case 'TIME': str = fmtTime(value); break;
        case 'DIST': { const n = value / 1852; str = n < 10 ? n.toFixed(2) : fmt1(n); break; }
        case 'TEMP': str = fmt1(value - 273.15); break;
        case 'PRESS': str = String(Math.round(value / 100)); break;
        case 'NUM1': str = fmt1(value); break;
        case 'INT': str = String(Math.round(value)); break;
        case 'XTE': {
          const x = Math.abs(value);
          if (x < 185.2) str = String(Math.round(x));
          else { const n = x / 1852; str = n < 10 ? n.toFixed(2) : fmt1(n); }
          break;
        }
        default: str = '';
      }
      this.fit(str, 104, 216);
      this.text(str, x0 + 120, y0 + 138, 'center', color);
    });
  }

  dashes(x0, y0) { this.font(104); this.text('---', x0 + 120, y0 + 138, 'center', P.stale); }

  drawAisCell(x0, y0) {
    const t = this.aisThreat;
    if (!t) return this.dashes(x0, y0);
    const id = t.name || String(t.mmsi);
    this.fit(id, 24, 150);
    this.text(id, x0 + 226, y0 + 34, 'right', P.label);
    const col = t.level === 2 ? P.bad : P.fg;
    const min = t.tcpa / 60;
    for (const [str, y] of [[fmtDist(t.cpa), 92], [min < 1 ? '< 1 min' : `${Math.round(min)} min`, 148]]) {
      this.fit(str, 56, 208);
      this.text(str, x0 + 16, y0 + y, 'left', col);
    }
  }

  drawDistress(t) {
    const c = this.c;
    const ringing = this.aisMob ? this.aisMobs.some(m => !Engine.aisAcked.has(m.mmsi)) : !Engine.aisAcked.has(t.mmsi);
    const on = !ringing || Math.floor(performance.now() / 500) % 2 === 0;
    c.beginPath(); c.roundRect(BAN_L, BAN_T, BAN_R - BAN_L, BAN_B - BAN_T, 24);
    c.fillStyle = css(on ? 0xFFE00000 : 0xFF700000); c.fill();
    this.stroke(0xFFFFFFFF, 4); c.stroke();
    const kind = { 972: 'MOB AIS', 974: 'EPIRB AIS', [MOB_N2K]: 'HOMME À LA MER' }[t.level] || 'AIS-SART';
    const max = BAN_R - BAN_L - 40;
    this.fit(kind, 64, max); this.text(kind, CX, 360, 'center', 0xFFFFFFFF);
    const who = t.name || (t.level === MOB_N2K ? 'alerte MOB du bus N2K' : String(t.mmsi));
    this.fit(who, 40, max); this.text(who, CX, 418, 'center', 0xFFFFFFFF);
    const where = this.noDir(t) ? 'position inconnue'
      : (t.est ? 'position inconnue  ≈ ' : '') + fmtDist(t.range) + '  ' +
        pad3(Math.round(norm360(t.bearing * DEG)) % 360) + '°';
    this.fit(where, 52, max); this.text(where, CX, 482, 'center', 0xFFFFFFFF);
  }

  /** Décalage vertical pour centrer un texte sur y (équivalent de descent + ascent d'Android). */
  midY(s) {
    const m = this.c.measureText(s);
    const a = m.fontBoundingBoxAscent, d = m.fontBoundingBoxDescent;
    return a !== undefined ? (a - d) / 2 : 0;
  }

  drawSailCell(x0, y0) {
    const c = this.c, chart = Engine.sailChart, code = Engine.sailCode;
    const name = chart ? chart.name(code) : null;
    if (!chart || code == null || name == null) return this.dashes(x0, y0);
    if (chart.isNone(code)) {
      this.font(70);
      this.text('N/A', x0 + 120, y0 + 99 + this.midY('N/A'), 'center', P.label);
      return;
    }
    const col = chart.color(code);
    c.beginPath(); c.roundRect(x0 + 10, y0 + 48, 220, 102, 14); c.fillStyle = css(col); c.fill();
    this.fit(name, 70, 200);
    this.text(name, x0 + 120, y0 + 99 + this.midY(name), 'center',
              SailChart.luminance(col) > 0.55 ? 0xFF000000 : 0xFFFFFFFF);
  }

  drawSailChartCell(x0, y0) {
    const c = this.c, chart = Engine.sailChart;
    if (!chart) return this.dashes(x0, y0);
    const twsMax = chart.tws.length ? chart.tws[chart.tws.length - 1] : 30;
    if (this.chartBmpOf !== chart) {
      const cv = document.createElement('canvas'); cv.width = CH_W; cv.height = CH_H;
      const cx = cv.getContext('2d'), img = cx.createImageData(CH_W, CH_H);
      for (let x = 0; x < CH_W; x++) for (let y = 0; y < CH_H; y++) {
        const twa = CH_TWA0 + (x + 0.5) / CH_W * (CH_TWA1 - CH_TWA0);
        const tws = twsMax * (1 - (y + 0.5) / CH_H);
        const code = chart.codeAt(twa, tws);
        const col = code == null || chart.isNone(code) ? 0xFF000000 : chart.color(code);
        const o = (y * CH_W + x) * 4;
        img.data[o] = (col >>> 16) & 255; img.data[o + 1] = (col >>> 8) & 255; img.data[o + 2] = col & 255; img.data[o + 3] = 255;
      }
      cx.putImageData(img, 0, 0);
      this.chartBmp = cv; this.chartBmpOf = chart;
    }
    const left = x0 + 10, top = y0 + 46;
    c.drawImage(this.chartBmp, left, top);
    this.stroke(P.grid, 1); c.strokeRect(left, top, CH_W, CH_H);
    const code = Engine.sailCode;
    const nm = chart.name(code);
    if (nm && code != null && !chart.isNone(code)) { this.font(24); this.text(nm, x0 + 228, y0 + 34, 'right', chart.color(code)); }
    const twa = this.v(this.iTwa), tws = this.v(this.iTws);
    if (twa == null || tws == null) return;
    const a = clamp(Math.abs(twa) * DEG, CH_TWA0, CH_TWA1);
    const k = clamp(tws * KN, 0, twsMax);
    const cx = left + (a - CH_TWA0) / (CH_TWA1 - CH_TWA0) * CH_W;
    const cy = top + (1 - k / twsMax) * CH_H;
    for (const [w, col] of [[6, 0xFF000000], [3, 0xFFFFFFFF]]) {
      this.stroke(col, w);
      this.line(cx - 9, cy - 9, cx + 9, cy + 9); this.line(cx - 9, cy + 9, cx + 9, cy - 9);
    }
  }

  // --- Rose ---
  drawCard(a) {
    const c = this.c;
    c.save(); this.rot(a);
    let ring = 0xFFF4F4F4;
    const th = this.aisThreat;
    if (!this.aisMob && th && th.level === 2 && !Engine.aisAcked.has(th.mmsi)) {
      const t = (1 - Math.cos(2 * PI * (performance.now() % 1000) / 1000)) / 2;
      ring = lerpColor(0xFFF4F4F4, 0xFFFF2020, t);
    }
    this.stroke(ring, R_CARD_OUT - R_CARD_IN);
    this.circle(CX, CY, (R_CARD_IN + R_CARD_OUT) / 2); c.stroke();
    this.stroke(0xFF606060, 1.5);
    this.circle(CX, CY, R_CARD_IN); c.stroke();
    this.circle(CX, CY, R_CARD_OUT); c.stroke();
    for (let d = 0; d < 360; d += 5) {
      if (d % 30 === 0) continue;
      const t = d / DEG, long = d % 10 === 0;
      this.stroke(long ? 0xFF404040 : 0xFF909090, long ? 3 : 2, 'round');
      const r1 = R_CARD_OUT - 4, r0 = r1 - (long ? 16 : 9);
      this.line(px(t, r0), py(t, r0), px(t, r1), py(t, r1));
    }
    c.lineCap = 'butt';
    for (let d = 0; d < 360; d += 30) {
      const s = { 0: 'N', 90: 'E', 180: 'S', 270: 'W' }[d] || String(d);
      const size = d % 90 === 0 ? 34 : 28;
      this.font(size);
      c.save(); this.rot(d / DEG);
      this.text(s, CX, CY - R_DOTS + size * 0.36, 'center', 0xFF000000);
      c.restore();
    }
    c.restore();
  }

  drawCog(a) {
    const c = this.c;
    c.save(); this.rot(a);
    const r1 = (R_GAP_IN + R_GAP_OUT) / 2, r0 = R_CARD_IN - 32, half = 8;
    c.beginPath(); c.moveTo(CX - half, CY - r1); c.lineTo(CX + half, CY - r1); c.lineTo(CX, CY - r0); c.closePath();
    c.fillStyle = css(0xFFB2458A); c.fill();
    this.stroke(0xFF000000, 1.5); c.lineJoin = 'round'; c.stroke(); c.lineJoin = 'miter';
    c.restore();
  }

  drawHdgBox() {
    const c = this.c;
    const top = CY - 400 + (95 + HDG_DY) * K, bot = CY - 400 + (172 + HDG_DY) * K;
    const l = CX - 65 * K, r = CX + 65 * K;
    c.beginPath(); c.roundRect(l, top, r - l, bot - top, 10);
    c.fillStyle = css(0xFFFFFFFF); c.fill();
    this.stroke(0xFF000000, 3); c.stroke();
  }

  drawNeedle(a, col, letter) {
    const c = this.c;
    const tip = CY - R_DOTS, base = tip + 99.7 * K, half = 46 * K;
    c.save(); this.rot(a);
    c.beginPath(); c.moveTo(CX, tip); c.lineTo(CX - half, base); c.lineTo(CX + half, base); c.closePath();
    c.fillStyle = css(col); c.fill();
    this.stroke(0xFF000000, 1.5); c.lineJoin = 'round'; c.stroke(); c.lineJoin = 'miter';
    this.font(26); c.textAlign = 'center';
    const ty = base - 38 * K + 9;
    this.stroke(0xFF000000, 3); c.strokeText(letter, CX, ty);
    c.fillStyle = css(0xFFFFFFFF); c.fillText(letter, CX, ty);
    c.restore();
  }

  drawWpMark(a, alpha = 255) {
    const c = this.c, h = 47, half = 20;
    c.save(); this.rot(a);
    c.beginPath(); c.moveTo(CX, CY - R_DOTS); c.lineTo(CX - half, CY - R_DOTS + h); c.lineTo(CX + half, CY - R_DOTS + h); c.closePath();
    c.fillStyle = css(0xFFFFD228, alpha); c.fill();
    c.strokeStyle = css(0xFF000000, alpha); c.lineWidth = 1.2; c.stroke();
    c.restore();
  }

  drawBoat(rot) {
    const c = this.c, b = this.boat;
    if (!b.complete || !b.naturalWidth) return;
    c.save(); this.rot(rot);
    const k = K * CARD_S;
    const l = Math.trunc(CX + (380 - 540) * k), t = Math.trunc(CY + (225 - 540) * k);
    const r = Math.trunc(CX + (699 - 540) * k), bt = Math.trunc(CY + (850 - 540) * k);
    c.drawImage(b, l, t, r - l, bt - t);
    c.restore();
  }

  drawSailRose(boatRot) {
    const chart = Engine.sailChart;
    if (!chart || !chart.tws.length) return;
    const twsMax = chart.tws[chart.tws.length - 1];
    if (this.roseBmpOf !== chart) {
      const n = Math.trunc(2 * R_SAIL / ROSE_RES);
      const cv = document.createElement('canvas'); cv.width = n; cv.height = n;
      const cx = cv.getContext('2d'), img = cx.createImageData(n, n);
      const half = n / 2;
      for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
        const dx = x + 0.5 - half, dy = half - (y + 0.5);
        const r = Math.hypot(dx, dy) / half;
        if (r > 1) continue;
        const twa = Math.abs(Math.atan2(dx, dy)) * DEG;
        const code = chart.codeAt(twa, r * twsMax);
        if (code == null || chart.isNone(code)) continue;
        const col = chart.color(code), o = (y * n + x) * 4;
        img.data[o] = (col >>> 16) & 255; img.data[o + 1] = (col >>> 8) & 255; img.data[o + 2] = col & 255;
        img.data[o + 3] = SAIL_ROSE_ALPHA;
      }
      cx.putImageData(img, 0, 0);
      this.roseBmp = cv; this.roseBmpOf = chart;
    }
    const c = this.c;
    c.save(); this.rot(boatRot);
    c.imageSmoothingEnabled = true;
    c.drawImage(this.roseBmp, CX - R_SAIL, CY - R_SAIL, 2 * R_SAIL, 2 * R_SAIL);
    this.stroke(0x60FFFFFF, 1);
    for (let k = 10; k < twsMax; k += 10) { this.circle(CX, CY, k / twsMax * R_SAIL); c.stroke(); }
    c.restore();
  }

  drawSailRoseCross(boatRot) {
    const chart = Engine.sailChart;
    if (!chart || !chart.tws.length) return;
    const twsMax = chart.tws[chart.tws.length - 1];
    const twa = this.v(this.iTwa), tws = this.v(this.iTws);
    if (twa == null || tws == null) return;
    const r = clamp(tws * KN, 0, twsMax) / twsMax * R_SAIL;
    const a = twa + boatRot, x = px(a, r), y = py(a, r);
    for (const [w, col] of [[5, 0xFF000000], [2.5, 0xFFFFFFFF]]) {
      this.stroke(col, w);
      this.line(x - 8, y - 8, x + 8, y + 8); this.line(x - 8, y + 8, x + 8, y - 8);
    }
  }

  drawSailButton() {
    if (!Engine.sailChart) return;
    const c = this.c, led = this.sailRose ? 0xFF30E030 : 0xFFE02020;
    for (const [w, a] of [[12, 0x30], [8, 0x50]]) {
      c.strokeStyle = css(led, a); c.lineWidth = w;
      this.circle(BTN_X, BTN_Y, BTN_R - 5); c.stroke();
    }
    c.fillStyle = css(0xFF1A1B1E); this.circle(BTN_X, BTN_Y, BTN_R); c.fill();
    this.stroke(led, 4); this.circle(BTN_X, BTN_Y, BTN_R - 5); c.stroke();
    const g = c.createRadialGradient(BTN_X - 6, BTN_Y - 8, 0, BTN_X - 6, BTN_Y - 8, BTN_R);
    g.addColorStop(0, css(0xFF4A4D52)); g.addColorStop(1, css(0xFF2A2C30));
    c.fillStyle = g; this.circle(BTN_X, BTN_Y, BTN_R - 8); c.fill();
  }

  drawSailName() {
    const chart = Engine.sailChart, code = Engine.sailCode;
    if (!chart || code == null) return;
    const none = chart.isNone(code);
    const name = none ? 'N/A' : chart.name(code);
    if (name == null) return;
    this.font(34);
    this.text(name, CX + 392, CY - 400 + 40, 'right', none ? P.label : chart.color(code));
  }

  drawRose() {
    const c = this.c;
    const hT = this.v(this.iHdg), cog = this.v(this.iCog), variation = this.v(this.iVar);
    let up = hT, label = hT, boatRot = 0;
    if (this.headingRef === 'M') label = hT != null && variation != null ? hT - variation : null;
    else if (this.headingRef === 'C') {
      up = cog != null ? cog : hT;
      label = cog;
      if (hT != null && cog != null) boatRot = hT - cog;
    }
    this.hdg = up != null ? up : 0;
    const cardTop = this.headingRef === 'M' && label != null ? label : this.hdg;

    c.save();
    c.translate(CX, CY); c.scale(ROSE_S, ROSE_S); c.translate(-CX, -CY);
    if (this.sailRose) this.drawSailRose(boatRot);
    this.drawBoat(boatRot);
    this.drawCard(-cardTop);

    this.circleOk = false;
    c.save(); this.rot(boatRot);
    if (this.core.vmgProfile(this.profile)) this.drawVmgCircle();
    this.drawOptimumTicks();
    c.restore();

    this.drawLaylines();
    c.save(); this.rot(boatRot); this.drawRudder(); c.restore();
    this.drawCurrent();
    this.drawWindTrails();
    if (this.sailRose) this.drawSailRoseCross(boatRot);

    if (cog != null) this.drawCog(this.sa(cog));
    const awa = this.v(this.iAwa);
    if (awa != null) this.drawNeedle(awa + boatRot, 0xFF4A78B0, 'A');
    const twa = this.v(this.iTwa);
    if (twa != null) {
      const col = this.circleOk ? this.circleColorAt(twa) : this.twaColor(twa, this.v(this.iTwaPct), this.v(this.iTgt));
      this.drawNeedle(twa + boatRot, col, 'T');
    }
    const btw = this.v(this.iBtw);
    if (btw != null) this.drawWpMark(this.sa(btw));

    this.drawHdgBox();
    const box = label == null ? '---' : pad3(Math.round(norm360(label * DEG)) % 360);
    this.font(40);
    this.text(box, CX, CY - 400 + (132 + HDG_DY) * K + 15, 'center', label == null ? 0xFF808080 : 0xFF000000);
    this.font(15);
    this.text(this.headingRef, CX - 400 + 600 * K, CY - 400 + (166 + HDG_DY) * K, 'right', 0xFF000000);
    if (cog != null) {
      const a = this.sa(cog);
      if (Math.abs(Math.sin(a)) < 0.2 && Math.cos(a) > 0) {
        c.save(); c.globalAlpha = WP_SEE_THROUGH / 255; this.drawCog(a); c.restore();
      }
    }
    if (btw != null) {
      const a = this.sa(btw);
      if (Math.abs(Math.sin(a)) < 0.2 && Math.cos(a) > 0) this.drawWpMark(a, WP_SEE_THROUGH);
    }
    this.drawAisDots();
    c.restore();

    this.drawSailName();
    this.drawSailButton();
    this.drawStatus();
  }

  twaColor(twa, pct, target) {
    if (pct == null || target == null) return 0xFF00FF00;
    const a = Math.abs(twa), upwind = a < PI / 2;
    return this.vmgColor(pct, upwind === (a < target));
  }
  vmgColor(pct, redSide) {
    const t = clamp((99 - pct) / 9, 0, 1);
    return lerpColor(0xFF00FF00, redSide ? 0xFFFF0000 : 0xFF0000FF, t);
  }
  pctAt(t) {
    const k = clamp(Math.trunc(t), 0, 179), f = clamp(t - k, 0, 1);
    return this.profile[k] * (1 - f) + this.profile[k + 1] * f;
  }

  drawVmgCircle() {
    const c = this.c, prof = this.profile;
    let kUp = 0, kDn = 180;
    for (let k = 0; k <= 89; k++) if (prof[k] > prof[kUp]) kUp = k;
    for (let k = 90; k <= 180; k++) if (prof[k] > prof[kDn]) kDn = k;
    const step = 360 / STEPS;
    for (let i = 0; i < STEPS; i++) {
      const t = Math.abs(((i + 0.5) * step + 540) % 360 - 180);
      const redSide = t < 90 ? t < kUp : t > kDn;
      this.base[i] = this.vmgColor(this.pctAt(t), redSide);
    }
    const n = 2 * SMOOTH + 1;
    for (let i = 0; i < STEPS; i++) {
      let r = 0, g = 0, b = 0;
      for (let d = -SMOOTH; d <= SMOOTH; d++) {
        const col = this.base[(i + d + STEPS) % STEPS];
        r += (col >>> 16) & 255; g += (col >>> 8) & 255; b += col & 255;
      }
      this.smooth[i] = ((0xFF << 24) | (Math.trunc(r / n) << 16) | (Math.trunc(g / n) << 8) | Math.trunc(b / n)) >>> 0;
    }
    this.circleOk = true;
    c.lineWidth = R_GAP_OUT - R_GAP_IN; c.lineCap = 'butt';
    const rr = (R_GAP_IN + R_GAP_OUT) / 2, sweep = step * 1.3 / DEG;
    for (let i = 0; i < STEPS; i++) {
      const s = (i * step - 90) / DEG;
      c.strokeStyle = css(this.smooth[i]);
      c.beginPath(); c.arc(CX, CY, rr, s, s + sweep); c.stroke();
    }
  }

  drawOptimumTicks() {
    const t = this.v(this.iTgt);
    if (t == null) return;
    for (const [w, col] of [[3, 0xFF00BF00], [1, 0xFF008000]]) {
      this.stroke(col, w);
      for (const a of [t, -t]) this.line(px(a, R_GAP_IN), py(a, R_GAP_IN), px(a, R_GAP_OUT), py(a, R_GAP_OUT));
    }
  }

  circleColorAt(a) {
    const i = ((Math.round(a * DEG / (360 / STEPS)) % STEPS) + STEPS) % STEPS;
    return this.smooth[i];
  }

  /** Secteur depuis le centre entre deux angles écran (ordre quelconque, < 180°). */
  wedge(a0, a1, color, alpha, r) {
    const c = this.c;
    let start = a0, sweep = norm360((a1 - a0) * DEG);
    if (sweep > 180) { start = a1; sweep = 360 - sweep; }
    c.fillStyle = css(color, alpha);
    c.beginPath(); c.moveTo(CX, CY); c.arc(CX, CY, r, arcStart(start), arcStart(start) + sweep / DEG); c.closePath(); c.fill();
  }

  drawLaylines() {
    const c = this.c, r = R_CARD_IN;
    const twa = this.v(this.iTwa), onStbd = twa != null && twa > 0;
    const lsMin = this.v(this.iLsMin), lsMax = this.v(this.iLsMax), lpMin = this.v(this.iLpMin), lpMax = this.v(this.iLpMax);
    if (lsMin != null && lsMax != null) this.wedge(this.sa(lsMin), this.sa(lsMax), P.stbd, LAY_HIST_ALPHA, r);
    if (lpMin != null && lpMax != null) this.wedge(this.sa(lpMin), this.sa(lpMax), P.port, LAY_HIST_ALPHA, r);
    const lo = this.v(onStbd ? this.iLpLo : this.iLsLo);
    const hi = this.v(onStbd ? this.iLpHi : this.iLsHi);
    const mid = this.v(onStbd ? this.iLp : this.iLs);
    const color = onStbd ? P.port : P.stbd;
    if (twa != null && lo != null && hi != null && mid != null)
      for (let k = 1; k <= 8; k++) {
        const f = k / 8;
        this.wedge(this.sa(mid + f * angDiff(lo, mid)), this.sa(mid + f * angDiff(hi, mid)), color, LAY_BAND_ALPHA, r);
      }
    const ls = this.v(this.iLs), lp = this.v(this.iLp);
    if (ls != null) { this.stroke(P.stbd, 5); this.line(CX, CY, px(this.sa(ls), r), py(this.sa(ls), r)); }
    if (lp != null) { this.stroke(P.port, 5); this.line(CX, CY, px(this.sa(lp), r), py(this.sa(lp), r)); }
    if (twa != null && mid != null) {
      const o = this.v(this.iOppTwa);
      if (o != null) {
        const d = angDiff(o, mid);
        if (Math.abs(d) > 0.5 / DEG) {
          c.strokeStyle = css(color, OPP_SECTOR_ALPHA); c.lineWidth = OPP_SECTOR_W; c.lineCap = 'butt';
          const s = arcStart(this.sa(mid));
          c.beginPath(); c.arc(CX, CY, R_TAG, s, s + d, d < 0); c.stroke();
        }
      }
    }
  }

  /**
   * Traînée du TWD sur 10 min (angle = valeur passée, distance au centre = âge). Segments
   * opaques dans un calque à part, puis opacité appliquée selon le rayon (= l'âge) : pas
   * de points plus clairs aux jonctions des segments.
   */
  drawWindTrails() {
    const hist = Engine.windHistory();
    if (hist.length < 2) return;
    const now = performance.now();
    const avg = this.v(this.iTwdAvg), twa = this.v(this.iTwa);
    const side = twa == null ? 0 : Math.sign(twa);
    const color = smp => {
      const d = smp.twd;
      if (avg == null || d == null || side === 0) return P.fg;
      const t = clamp(angDiff(d, avg) * side * DEG / TWD_SHIFT_FULL, -1, 1);
      return t >= 0 ? lerpColor(0xFFFFFFFF, 0xFF00FF00, t) : lerpColor(0xFFFFFFFF, 0xFFFF2020, -t);
    };
    // Calque : carré de la rose (800 × 800 virtuels) à la résolution de l'écran.
    const m = this.c.getTransform();
    const sc = Math.hypot(m.a, m.b);
    const n = Math.max(1, Math.ceil(800 * sc));
    const tv = this.trail;
    if (tv.width !== n) { tv.width = n; tv.height = n; }
    const t = tv.getContext('2d');
    t.setTransform(1, 0, 0, 1, 0, 0); t.clearRect(0, 0, n, n);
    t.setTransform(n / 800, 0, 0, n / 800, -(CX - 400) * n / 800, -(CY - 400) * n / 800);
    t.lineWidth = TRAIL_W; t.lineCap = 'round';
    for (let i = 1; i < hist.length; i++) {
      const h0 = hist[i - 1], h1 = hist[i];
      if (h0.twd == null || h1.twd == null) continue;
      const a0 = this.sa(h0.twd), a1 = this.sa(h1.twd);
      if (Math.abs(angDiff(a1, a0)) > PI / 2) continue;
      const f0 = clamp((now - h0.t) / WIND_HISTORY_MS, 0, 1), f1 = clamp((now - h1.t) / WIND_HISTORY_MS, 0, 1);
      const r0 = R_TRAIL_OUT - f0 * (R_TRAIL_OUT - R_TRAIL_IN), r1 = R_TRAIL_OUT - f1 * (R_TRAIL_OUT - R_TRAIL_IN);
      const x0 = px(a0, r0), y0 = py(a0, r0), x1 = px(a1, r1), y1 = py(a1, r1);
      const g = t.createLinearGradient(x0, y0, x1, y1);
      g.addColorStop(0, css(color(h0), 255)); g.addColorStop(1, css(color(h1), 255));
      t.strokeStyle = g;
      t.beginPath(); t.moveTo(x0, y0); t.lineTo(x1, y1); t.stroke();
    }
    // Opacité : 1 au bord (maintenant), 0,2 au plus ancien.
    t.globalCompositeOperation = 'destination-in';
    const g = t.createRadialGradient(CX, CY, R_TRAIL_IN, CX, CY, R_TRAIL_OUT);
    g.addColorStop(0, 'rgba(0,0,0,0.2)'); g.addColorStop(1, 'rgba(0,0,0,1)');
    t.fillStyle = g; t.fillRect(CX - 400, CY - 400, 800, 800);
    t.globalCompositeOperation = 'source-over';
    this.c.drawImage(tv, CX - 400, CY - 400, 800, 800);
  }

  drawRudder() {
    const c = this.c, r = 205, arc = 28, range = 15, k = arc / range;
    this.stroke(P.label, 3);
    c.beginPath(); c.arc(CX, CY, r, (90 - arc) / DEG, (90 + arc) / DEG); c.stroke();
    for (let d = -15; d <= 15; d += 5) {
      const a = PI - d * k / DEG, len = d % 15 === 0 ? 18 : 11;
      this.stroke(P.label, d === 0 ? 4 : 2);
      this.line(px(a, r), py(a, r), px(a, r - len), py(a, r - len));
    }
    const rr = Engine.rudderRange();
    if (rr) {
      const lo = clamp(rr[0], -range, range), hi = clamp(rr[1], -range, range);
      const rh = r + 9;
      this.stroke(0xFF2ECC40, 8);
      const a0 = 90 - hi * k, sweep = Math.max((hi - lo) * k, 1.5);
      c.beginPath(); c.arc(CX, CY, rh, a0 / DEG, (a0 + sweep) / DEG); c.stroke();
    }
    const rud = this.v(this.iRudder);
    if (rud == null) return;
    const deg = clamp(rud * DEG, -range, range), a = PI - deg * k / DEG;
    c.beginPath();
    c.moveTo(px(a, r - 2), py(a, r - 2));
    c.lineTo(px(a + 0.07, r - 30), py(a + 0.07, r - 30));
    c.lineTo(px(a - 0.07, r - 30), py(a - 0.07, r - 30));
    c.closePath(); c.fillStyle = css(rud >= 0 ? P.stbd : P.port); c.fill();
    this.font(30);
    this.text(String(Math.round(Math.abs(rud * DEG))), CX, CY + r - 42, 'center', P.fg);
  }

  drawCurrent() {
    const c = this.c, set = this.v(this.iSet), drift = this.v(this.iDrift);
    if (set == null || drift == null) return;
    const kn = drift * KN;
    if (kn < 0.1) return;
    const len = (Math.min(kn, 4) / 3 * 200 + 70) * 1.3, w = len * 0.56;
    c.save(); this.rot(this.sa(set));
    const top = CY - len / 2, bot = CY + len / 2;
    c.beginPath(); c.moveTo(CX, top); c.lineTo(CX + w / 2, bot); c.lineTo(CX, bot - len * 0.27); c.lineTo(CX - w / 2, bot); c.closePath();
    c.fillStyle = css(CURRENT_COL); c.fill();
    c.restore();
    const str = fmt1(kn);
    this.font(39); c.textAlign = 'center';
    this.stroke(0xFF000000, 6); c.lineJoin = 'round'; c.strokeText(str, CX, CY + 14); c.lineJoin = 'miter';
    c.fillStyle = css(P.fg); c.fillText(str, CX, CY + 14);
  }

  drawStatus() {
    const c = this.c, now = performance.now();
    const conn = Engine.wsUp && Engine.n2kState === 'CONNECTED';
    const fresh = conn && now - Engine.lastLineMs < 3000;
    c.fillStyle = css(fresh ? P.ok : P.bad); this.circle(262, VH - 22, 9); c.fill();
    this.font(24);
    const msg = fresh ? 'N2K' : !Engine.wsUp ? 'N2K : relais coupé' : conn ? 'N2K : muet' : 'N2K : coupé';
    this.text(msg, 280, VH - 13, 'left', P.label);
    const navFresh = Engine.navLastMs !== 0 && now - Engine.navLastMs < 10000;
    c.fillStyle = css(navFresh ? P.ok : P.stale); this.circle(262, VH - 57, 9); c.fill();
    this.text('qtVlm', 280, VH - 48, 'left', P.label);
    const name = this.core.wpName(now);
    if (name) { this.fit(name, 30, 190); this.text(name, VW - 252, VH - 13, 'right', P.fg); }
  }

  noDir(t) { return isNaN(t.bearing) || (t.est && t.range < 20); }

  drawAisDots() {
    const c = this.c, now = performance.now();
    this.aisDots = [];
    const list = this.aisMob ? this.aisMobs
      : [this.aisDistress, this.aisThreat && this.aisThreat.level === 2 ? this.aisThreat : null].filter(Boolean);
    for (const t of list) {
      if (this.noDir(t) || this.aisDots.some(d => d[0] === t.mmsi)) continue;
      const a = this.sa(t.bearing), x = px(a, R_AIS_DOT), y = py(a, R_AIS_DOT);
      const acked = Engine.aisAcked.has(t.mmsi);
      const r = acked ? 14 : 14 + 6 * (1 - Math.cos(2 * PI * (now % 1000) / 1000)) / 2;
      c.fillStyle = css(0xFFFF2020); this.circle(x, y, r); c.fill();
      this.stroke(0xFFFFFFFF, 3); c.stroke();
      this.aisDots.push([t.mmsi, CX + (x - CX) * ROSE_S, CY + (y - CY) * ROSE_S]);
    }
  }

  // --- Zones d'appui (coordonnées virtuelles) ---
  cellAt(vx, vy) {
    if (vy < 0 || vy >= VH) return null;
    const idx = clamp(Math.trunc(vy / 160), 0, 4);
    if (vx >= 0 && vx <= 240) return [0, idx];
    if (vx >= VW - 240 && vx <= VW) return [this.wpActive() ? 2 : 1, idx];
    return null;
  }
  aisAlertAt(vx, vy) {
    let best = null, bd = Infinity;
    for (const d of this.aisDots) { const e = Math.hypot(d[1] - vx, d[2] - vy); if (e < bd) { bd = e; best = d; } }
    if (best && bd < 50) return best[0];
    const d = this.aisDistress;
    if (d && this.noDir(d) && vx >= BAN_L && vx <= BAN_R && vy >= BAN_T && vy <= BAN_B) return d.mmsi;
    return null;
  }
  isSailButton(vx, vy) { return vy >= 0 && vy <= 90 && vx >= 240 && vx <= 350; }
  isStatusArea(vx, vy) { return vy > VH - 100 && vx >= 240 && vx <= 440; }
}

// ============================================================================
// Page : rendu, gestes, mise au point, réglages
// ============================================================================
let dash, core;

function applyDisplaySettings() {
  dash.columns = Settings.get('columns');
  dash.headingRef = Settings.get('headingRef');
  dash.sailRose = Settings.get('sailRose');
  dash.transducerDepth = Settings.get('transducerDepth');
  dash.aisCpaNm = Settings.get('aisCpa');
  dash.aisTcpaMin = Settings.get('aisTcpa');
  dash.aisRangeNm = Settings.get('aisRange');
  if (night) dash.dim = Settings.get('nightDim') / 100;
}

let night = false;
function setNight(on) {
  night = on;
  dash.dim = on ? Math.min(0.95, Settings.get('nightDim') / 100) : 0;
}

// --- Appuis : 1 = action du point touché, 2 = mise au point, 3 = choix de la case ; appui long = nuit ---
const TAP_WINDOW_MS = 350, TAP_SLOP = 40, LONG_MS = 500;
let tapCount = 0, tapX = 0, tapY = 0, tapTimer = 0, longTimer = 0, longDone = false, downX = 0, downY = 0;

function onTapAction() {
  const n = tapCount;
  tapCount = 0;
  const [vx, vy] = dash.toVirtual(tapX, tapY);
  if (n >= 3) { const cell = dash.cellAt(vx, vy); if (cell) chooseCell(cell[0], cell[1]); return; }
  if (n === 2) { toggleDebug(); return; }
  const m = dash.aisAlertAt(vx, vy);
  if (m != null) Engine.aisTap(m);
  else if (dash.isSailButton(vx, vy)) { Settings.set('sailRose', !Settings.get('sailRose')); dash.sailRose = Settings.get('sailRose'); }
  else if (dash.isStatusArea(vx, vy)) openSettings();
}

function setupInput(cv) {
  cv.addEventListener('pointerdown', e => {
    unlockAudio();
    downX = e.clientX; downY = e.clientY; longDone = false;
    clearTimeout(longTimer);
    longTimer = setTimeout(() => { longDone = true; tapCount = 0; clearTimeout(tapTimer); setNight(!night); }, LONG_MS);
  });
  cv.addEventListener('pointermove', e => {
    if (Math.abs(e.clientX - downX) > TAP_SLOP || Math.abs(e.clientY - downY) > TAP_SLOP) clearTimeout(longTimer);
  });
  cv.addEventListener('pointerup', e => {
    clearTimeout(longTimer);
    if (longDone) return;
    const x = e.clientX, y = e.clientY;
    if (tapCount > 0 && (Math.abs(x - tapX) > TAP_SLOP || Math.abs(y - tapY) > TAP_SLOP)) tapCount = 0;
    if (tapCount === 0) { tapX = x; tapY = y; }
    tapCount++;
    clearTimeout(tapTimer);
    tapTimer = setTimeout(onTapAction, TAP_WINDOW_MS);
  });
  cv.addEventListener('pointercancel', () => clearTimeout(longTimer));
  cv.addEventListener('contextmenu', e => e.preventDefault());
}

function unlockAudio() {
  try {
    if (!Engine.audio) Engine.audio = new (window.AudioContext || window.webkitAudioContext)();
    if (Engine.audio.state === 'suspended') Engine.audio.resume();
  } catch (e) { /* pas de son */ }
  requestWakeLock();
}

let wakeLock = null;
async function requestWakeLock() {
  if (wakeLock || !('wakeLock' in navigator)) return;
  try {
    wakeLock = await navigator.wakeLock.request('screen');
    wakeLock.addEventListener('release', () => { wakeLock = null; });
  } catch (e) { wakeLock = null; }
}
document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') requestWakeLock(); });

// --- Mise au point (double appui) ---
function displayUnit(name) {
  const deg = 180 / PI;
  if (name === 'LAT' || name === 'LON') return [1, 0, '°'];
  if (name === 'ROT') return [deg * 60, 0, '°/min'];
  if (['SOG', 'BSP', 'AWS', 'TWS', 'GWS', 'DRIFT', 'WCV', 'TWS_AVG', 'VMG', 'POLAR_BSP', 'TARGET_BSP'].includes(name)) return [KN, 0, 'kn'];
  if (['LOG', 'TRIP', 'DTW', 'DTW_SAIL'].includes(name)) return [1 / 1852, 0, 'NM'];
  if (name.startsWith('TTW')) return [1 / 60, 0, 'min'];
  if (name.endsWith('_TEMP')) return [1, -273.15, '°C'];
  if (name === 'PRESSURE') return [0.01, 0, 'hPa'];
  if (name.endsWith('_PCT') || name === 'HUMIDITY') return [1, 0, '%'];
  if (['DEPTH', 'DEPTH_OFFSET', 'XTE'].includes(name)) return [1, 0, 'm'];
  if (['GNSS_METHOD', 'GNSS_SATS', 'HDOP', 'PDOP'].includes(name)) return [1, 0, ''];
  return [deg, 0, '°'];
}
function toggleDebug() {
  const d = document.getElementById('debug');
  d.hidden = !d.hidden;
  if (!d.hidden) renderDebug();
}
function renderDebug() {
  const d = document.getElementById('debug');
  if (d.hidden) return;
  const now = performance.now();
  const st = core.stats();
  const age = Engine.lastLineMs ? `${Math.round(now - Engine.lastLineMs)} ms` : '—';
  let s = `n2kdran web  source ${Engine.source || '?'}  ${Engine.wsUp ? Engine.n2kState : 'relais coupé'}  dernière ligne ${age}`;
  if (Engine.lastError) s += `  (${Engine.lastError})`;
  s += `\nlignes ${st[0]}  rejetées ${st[1]}  fp ${st[2]}/${st[3]}  ${Engine.polarMsg}  ${Engine.sailMsg}`;
  s += `\nqtVlm UDP : ${Engine.navPackets} datagrammes   son : ${Engine.audio ? Engine.audio.state : 'cliquer dans la page'}\n\n`;
  const rows = core.names.map((_, i) => i).filter(i => core.status[i] !== NEVER);
  const third = Math.ceil(rows.length / 3);
  const cell = i => {
    const name = core.names[i], [k, off, unit] = displayUnit(name);
    const v = core.values[i] * k + off;
    const val = core.status[i] === OK ? v.toFixed(2).padStart(9) : '      ---';
    return name.slice(0, 16).padEnd(16) + val + ' ' + unit.padEnd(5);
  };
  for (let r = 0; r < third; r++) {
    const line = [];
    for (let c = 0; c < 3; c++) { const k = r + c * third; if (k < rows.length) line.push(cell(rows[k])); }
    s += line.join('  ') + '\n';
  }
  d.textContent = s;
}

// --- Choix de la donnée d'une case (triple appui) ---
function chooseCell(col, idx) {
  const dlg = document.getElementById('cellDlg');
  document.getElementById('cellTitle').textContent = col === 2 ? `Case ${idx + 1} (colonne WP)` : `Case ${idx + 1}`;
  const list = document.getElementById('cellList');
  list.innerHTML = '';
  const cur = Settings.get('columns')[col][idx];
  for (const d of CELLS) {
    const b = document.createElement('button');
    b.textContent = d.unit ? `${d.label} (${d.unit})` : d.label;
    if (d.key === cur) b.className = 'current';
    b.onclick = () => {
      const cols = Settings.get('columns').map(c => c.slice());
      cols[col][idx] = d.key;
      Settings.set('columns', cols);
      dash.columns = cols;
      dlg.close();
    };
    list.appendChild(b);
  }
  dlg.showModal();
}

// --- Réglages (appui sur les indicateurs) ---
function openSettings() {
  const dlg = document.getElementById('settingsDlg');
  const f = document.getElementById('settingsForm');
  f.staleS.value = Settings.get('staleS');
  f.headingRef.value = Settings.get('headingRef');
  f.transducerDepth.value = Settings.get('transducerDepth');
  f.nightDim.value = Settings.get('nightDim');
  f.aisCpa.value = Settings.get('aisCpa');
  f.aisTcpa.value = Settings.get('aisTcpa');
  f.aisRange.value = Settings.get('aisRange');
  f.aisStill.checked = Settings.get('aisStill');
  document.getElementById('srcInfo').textContent = Engine.source || 'inconnue';
  document.getElementById('polarInfo').textContent = Engine.polarMsg;
  document.getElementById('sailInfo').textContent = Engine.sailMsg;
  dlg.showModal();
}

function setupSettings() {
  const dlg = document.getElementById('settingsDlg');
  const f = document.getElementById('settingsForm');
  const num = (el, lo, hi, def) => { const x = Number(el.value); return isFinite(x) ? clamp(x, lo, hi) : def; };
  f.addEventListener('submit', async e => {
    e.preventDefault();
    Settings.set('staleS', Math.round(num(f.staleS, 1, 60, 3)));
    Settings.set('headingRef', f.headingRef.value);
    Settings.set('transducerDepth', num(f.transducerDepth, 0, 5, 0));
    Settings.set('nightDim', Math.round(num(f.nightDim, 0, 90, 60)));
    Settings.set('aisCpa', num(f.aisCpa, 0.05, 5, 0.5));
    Settings.set('aisTcpa', Math.round(num(f.aisTcpa, 1, 120, 20)));
    Settings.set('aisRange', num(f.aisRange, 0, 100, 15));
    Settings.set('aisStill', f.aisStill.checked);
    await Engine.applySettings();
    applyDisplaySettings();
    dlg.close();
  });
  document.getElementById('cancelSettings').onclick = () => dlg.close();
  document.getElementById('polarFile').onchange = async e => {
    const file = e.target.files[0];
    if (!file) return;
    const text = await file.text();
    const err = core.setPolar(text);
    if (err) { document.getElementById('polarInfo').textContent = `${file.name} : ${err}`; return; }
    Settings.set('polarName', file.name); Settings.set('polarText', text);
    await Engine.applySettings();
    document.getElementById('polarInfo').textContent = Engine.polarMsg;
  };
  document.getElementById('polarReset').onclick = async () => {
    Settings.set('polarName', null); Settings.set('polarText', null);
    await Engine.applySettings();
    document.getElementById('polarInfo').textContent = Engine.polarMsg;
  };
  document.getElementById('sailFiles').onchange = async e => {
    const files = [...e.target.files];
    const def = files.find(x => x.name.toLowerCase().endsWith('.saildef'));
    const sel = files.find(x => x.name.toLowerCase().endsWith('.sailselect'));
    const info = document.getElementById('sailInfo');
    if (!def || !sel) { info.textContent = 'choisir les deux fichiers : .saildef et .sailselect'; return; }
    const [a, b] = await Promise.all([def.text(), sel.text()]);
    try { SailChart.parse(a, b); } catch (er) { info.textContent = er.message; return; }
    Settings.set('sailName', def.name.replace(/\.saildef$/i, '')); Settings.set('saildef', a); Settings.set('sailselect', b);
    await Engine.applySettings();
    info.textContent = Engine.sailMsg;
  };
  document.getElementById('sailReset').onclick = async () => {
    Settings.set('sailName', null); Settings.set('saildef', null); Settings.set('sailselect', null);
    await Engine.applySettings();
    document.getElementById('sailInfo').textContent = Engine.sailMsg;
  };
  document.getElementById('fullscreen').onclick = () => {
    if (document.fullscreenElement) document.exitFullscreen(); else document.documentElement.requestFullscreen();
  };
  document.getElementById('resetCells').onclick = () => {
    Settings.set('columns', DEFAULT_COLUMNS.map(c => c.slice()));
    dash.columns = Settings.get('columns');
  };
}

// --- Démarrage ---
async function main() {
  Settings.load();
  const M = await N2kModule();
  core = new Core(M);
  Engine.core = core;
  await Engine.applySettings();
  const cv = document.getElementById('screen');
  dash = new Dashboard(cv, core);
  applyDisplaySettings();
  setupInput(cv);
  setupSettings();
  // L'écran de mise au point recouvre la rose : il reçoit lui-même le double appui de retour.
  document.getElementById('debug').addEventListener('dblclick', e => {
    e.preventDefault(); window.getSelection().removeAllRanges(); toggleDebug();
  });
  document.addEventListener('keydown', e => {
    if (document.querySelector('dialog[open]')) return;
    if (e.key === 'f' || e.key === 'F11') { e.preventDefault(); document.getElementById('fullscreen').click(); }
    else if (e.key === 'n') setNight(!night);
    else if (e.key === 'd') toggleDebug();
  });
  Engine.connect();
  setInterval(() => Engine.tick(), 200);
  const frame = () => {
    core.snapshot(performance.now());
    dash.draw();
    renderDebug();
  };
  setInterval(frame, 100);
  window.addEventListener('resize', frame);         // pas d'image déformée en attendant la suivante
}
main().catch(e => {
  document.body.textContent = 'n2kdran : ' + e.message;
  document.body.style.color = '#fff';
});

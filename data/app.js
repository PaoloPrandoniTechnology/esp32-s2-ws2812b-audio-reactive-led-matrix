/*
 * app.js - Client dei "Giochi di Luce" 8x32
 *
 * Responsabilita':
 *   - Collegarsi al WebSocket del firmware (/ws) con riconnessione automatica.
 *   - Costruire la griglia degli effetti leggendo /effects.
 *   - Inviare i comandi JSON "set" quando l'utente muove gli slider,
 *     sceglie una palette, attiva il random o seleziona un effetto.
 *   - Aggiornare la UI dallo stato ricevuto (messaggi "status") e
 *     alimentare il VU-meter (messaggi "audio").
 *   - Persistere i parametri nel localStorage del browser.
 *
 * I limiti in PARAM_CONFIG devono restare coerenti con include/config.h.
 */

"use strict";

// ============================================================
// LIMITI PARAMETRI (allineati a include/config.h)
// ============================================================
const PARAM_CONFIG = {
    brightness:  { min: 5,   max: 255, def: 110 },
    speed:       { min: 1,   max: 100, def: 50  },
    sensitivity: { min: 0,   max: 100, def: 50  },
    gainDb:      { min: 0,   max: 36,  def: 12  },
    palette:     { min: 0,   max: 8,   def: 0   }
};

const STORE_KEY     = "giochiluce_8x32_v1";
const SLIDER_KEYS   = ["brightness", "speed", "sensitivity", "gainDb"];

// ============================================================
// STATO LOCALE
// ============================================================
let fxNames      = [];      // nomi effetti da /effects
let fxCount      = 0;
let firstAudioFx = 9999;    // primo indice effetto audio-reattivo
let palNames     = [];      // nomi palette da /effects
let ws           = null;
let wsReady      = false;
let reconnectTmr = null;
let pendingSet   = null;    // comando "set" accorpato
let sendTmr      = null;
let storedParams = null;    // parametri salvati nel browser

// ============================================================
// RIFERIMENTI DOM
// ============================================================
const $ = (id) => document.getElementById(id);
const el = {
    fxGrid:   $("fxGrid"),
    prevBtn:  $("prevBtn"),
    nextBtn:  $("nextBtn"),
    randBtn:  $("randBtn"),
    palette:  $("palette"),
    wsdot:    $("wsdot"),
    wstxt:    $("wstxt"),
    barLevel: $("barLevel"),  valLevel:  $("valLevel"),
    barBass:  $("barBass"),   valBass:   $("valBass"),
    barMid:   $("barMid"),    valMid:    $("valMid"),
    barTreble:$("barTreble"), valTreble: $("valTreble"),
    sigTxt:   $("sigTxt"),
    beatTxt:  $("beatTxt")
};

// ============================================================
// PERSISTENZA (localStorage)
// ============================================================
function loadStored() {
    try {
        const raw = localStorage.getItem(STORE_KEY);
        if (raw) storedParams = JSON.parse(raw);
    } catch (e) {
        storedParams = null;
    }
}

function saveStored(patch) {
    storedParams = Object.assign(storedParams || {}, patch);
    try { localStorage.setItem(STORE_KEY, JSON.stringify(storedParams)); } catch (e) {}
}

// ============================================================
// WEBSOCKET
// ============================================================
function wsConnect() {
    if (reconnectTmr) { clearTimeout(reconnectTmr); reconnectTmr = null; }
    const proto = (location.protocol === "https:") ? "wss" : "ws";
    try {
        ws = new WebSocket(proto + "://" + location.host + "/ws");
    } catch (e) {
        scheduleReconnect();
        return;
    }

    ws.onopen = () => {
        wsReady = true;
        setConnState(true);
        // Applica i parametri salvati nel browser (se presenti), altrimenti richiedi lo stato.
        if (storedParams) {
            sendWS(Object.assign({ cmd: "set" }, storedParams));
        } else {
            sendWS({ cmd: "get" });
        }
    };

    ws.onclose  = () => { wsReady = false; setConnState(false); scheduleReconnect(); };
    ws.onerror  = () => { wsReady = false; setConnState(false); };

    ws.onmessage = (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); } catch (e) { return; }
        if (msg.type === "status")      applyStatus(msg);
        else if (msg.type === "audio")  applyAudio(msg);
    };
}

function scheduleReconnect() {
    if (reconnectTmr) return;
    reconnectTmr = setTimeout(() => { reconnectTmr = null; wsConnect(); }, 2000);
}

function sendWS(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(obj));
}

// Accorpa piu' variazioni ravvicinate (slider trascinato) in un solo "set".
function queueSet(patch) {
    pendingSet = Object.assign(pendingSet || {}, patch);
    saveStored(patch);
    if (sendTmr) return;
    sendTmr = setTimeout(() => {
        sendTmr = null;
        const p = pendingSet; pendingSet = null;
        sendWS(Object.assign({ cmd: "set" }, p));
    }, 60);
}

function setConnState(on) {
    el.wsdot.className = "dot " + (on ? "on" : "off");
    el.wstxt.textContent = on ? "connesso" : "disconnesso";
}

// ============================================================
// STATO RICEVUTO DAL FIRMWARE
// ============================================================
function applyStatus(s) {
    if (typeof s.fxCount === "number" && s.fxCount !== fxCount) {
        fxCount = s.fxCount;
        if (!fxNames.length) buildFallbackGrid();
    }
    if (typeof s.effect === "number")      highlightFx(s.effect);
    if (typeof s.random === "boolean")     el.randBtn.classList.toggle("active", s.random);
    if (typeof s.brightness === "number")  setSlider("brightness", s.brightness);
    if (typeof s.speed === "number")       setSlider("speed", s.speed);
    if (typeof s.sensitivity === "number") setSlider("sensitivity", s.sensitivity);
    if (typeof s.gainDb === "number")      setSlider("gainDb", s.gainDb);
    if (typeof s.palette === "number")     el.palette.value = String(s.palette);
}

function applyAudio(a) {
    setBar(el.barLevel,  el.valLevel,  a.level);
    setBar(el.barBass,   el.valBass,   a.bass);
    setBar(el.barMid,    el.valMid,    a.mid);
    setBar(el.barTreble, el.valTreble, a.treble);

    el.sigTxt.textContent = a.signal ? "Mic: segnale attivo" : "Mic: silenzio";
    el.sigTxt.style.color = a.signal ? "var(--ok)" : "var(--text-dim)";

    el.beatTxt.textContent = "Beat: " + (a.beats | 0);
    el.beatTxt.classList.toggle("beat-on", (a.level | 0) > 55);
}

function setBar(bar, val, v) {
    const n = Math.max(0, Math.min(100, v | 0));
    bar.style.width = n + "%";
    bar.style.background = n > 85 ? "var(--clip)" : (n > 65 ? "var(--warn)" : "var(--ok)");
    val.textContent = n;
}

// ============================================================
// COSTRUZIONE UI EFFETTI
// ============================================================
function buildFxGrid() {
    el.fxGrid.innerHTML = "";
    for (let i = 0; i < fxNames.length; i++) {
        const b = document.createElement("button");
        b.className = "fx-btn";
        b.dataset.idx = String(i);
        b.textContent = fxNames[i];
        if (i >= firstAudioFx) {
            const t = document.createElement("span");
            t.className = "tag";
            t.textContent = "audio";
            b.appendChild(t);
        }
        b.addEventListener("click", () => selectEffect(i));
        el.fxGrid.appendChild(b);
    }
}

function buildFallbackGrid() {
    el.fxGrid.innerHTML = "";
    for (let i = 0; i < fxCount; i++) {
        const b = document.createElement("button");
        b.className = "fx-btn";
        b.dataset.idx = String(i);
        b.textContent = "Effetto " + i;
        b.addEventListener("click", () => selectEffect(i));
        el.fxGrid.appendChild(b);
    }
}

function highlightFx(idx) {
    const list = el.fxGrid.children;
    for (let i = 0; i < list.length; i++) {
        list[i].classList.toggle("active", i === idx);
    }
    window.__curFx = idx;
}

function selectEffect(idx) {
    if (idx < 0) idx = 0;
    if (fxCount && idx >= fxCount) idx = fxCount - 1;
    highlightFx(idx);
    queueSet({ effect: idx });
}

// ============================================================
// SLIDER
// ============================================================
function setSlider(key, value) {
    const c = PARAM_CONFIG[key];
    const v = Math.max(c.min, Math.min(c.max, value | 0));
    $(key).value = String(v);
    $(key + "Val").textContent = String(v);
}

function initSliders() {
    SLIDER_KEYS.forEach((key) => {
        const c = PARAM_CONFIG[key];
        const inp = $(key);
        inp.min = c.min; inp.max = c.max; inp.step = 1;
        inp.value = String(c.def);
        $(key + "Val").textContent = String(c.def);
        inp.addEventListener("input", () => {
            const v = parseInt(inp.value, 10) || c.def;
            $(key + "Val").textContent = String(v);
            queueSet({ [key]: v });
        });
    });
}

// ============================================================
// PALETTE
// ============================================================
function buildPalettes() {
    el.palette.innerHTML = "";
    for (let i = 0; i < palNames.length; i++) {
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = palNames[i];
        el.palette.appendChild(o);
    }
    el.palette.value = String(PARAM_CONFIG.palette.def);
    el.palette.addEventListener("change", () => {
        queueSet({ palette: parseInt(el.palette.value, 10) || 0 });
    });
}

// ============================================================
// PULSANTI EFFETTO PRECEDENTE / SUCCESSIVO / RANDOM
// ============================================================
function initFxNav() {
    el.prevBtn.addEventListener("click", () => {
        const n = fxCount || (parseInt(el.fxGrid.lastElementChild?.dataset.idx, 10) + 1) || 1;
        let i = (window.__curFx || 0) - 1;
        if (i < 0) i = n - 1;
        selectEffect(i);
    });
    el.nextBtn.addEventListener("click", () => {
        const n = fxCount || (parseInt(el.fxGrid.lastElementChild?.dataset.idx, 10) + 1) || 1;
        let i = (window.__curFx || 0) + 1;
        if (i >= n) i = 0;
        selectEffect(i);
    });
    el.randBtn.addEventListener("click", () => {
        const on = !el.randBtn.classList.contains("active");
        el.randBtn.classList.toggle("active", on);
        queueSet({ random: on });
    });
}

// ============================================================
// AVVIO
// ============================================================
async function fetchEffects() {
    try {
        const r = await fetch("/effects", { cache: "no-store" });
        if (!r.ok) throw new Error("http " + r.status);
        const d = await r.json();
        fxNames      = Array.isArray(d.effects)  ? d.effects  : [];
        palNames     = Array.isArray(d.palettes) ? d.palettes : [];
        firstAudioFx = (typeof d.firstAudioFx === "number") ? d.firstAudioFx : 9999;
        fxCount      = (typeof d.fxCount === "number") ? d.fxCount : fxNames.length;
        buildFxGrid();
        buildPalettes();
    } catch (e) {
        // Rete assente o filesystem non caricato: UI comunque funzionante.
        console.warn("Impossibile leggere /effects:", e);
        buildFallbackGrid();
    }
}

async function main() {
    loadStored();
    initSliders();
    initFxNav();
    await fetchEffects();
    wsConnect();
}

main();

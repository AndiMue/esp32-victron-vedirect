// VE.Direct WebUI – Stufen W1–W3 (System), W4a (Projekt-Reiter)
// Reines JavaScript, keine externen Bibliotheken (funktioniert auch im AP ohne Internet).
"use strict";

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
function toast(text, isError = false) {
  const t = $("toast");
  t.textContent = text;
  t.className = "toast" + (isError ? " error" : "");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.add("hidden"), 4000);
}

async function api(path, data) {
  const options = data
    ? { method: "POST", body: new URLSearchParams(data) }
    : { method: "GET" };
  const res = await fetch(path, options);
  let json = {};
  try { json = await res.json(); } catch (e) { /* keine JSON-Antwort */ }
  if (!res.ok) {
    throw new Error(json.error || ("Fehler " + res.status));
  }
  return json;
}

function formatUptime(s) {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return (d ? d + " d " : "") + h + " h " + m + " min";
}

const stateText = {
  access_point: "Nur Access Point (kein WLAN eingerichtet)",
  connecting:   "Verbinde mit WLAN …",
  connected:    "Mit WLAN verbunden",
  fallback:     "WLAN nicht erreichbar – Access Point aktiv, neuer Versuch läuft",
};

// ---------------------------------------------------------------------------
// Browser-Speicher (nur Komfort: zuletzt gewählter Reiter usw.).
// Kann gesperrt sein (privates Fenster) -> dann einfach Standardwerte.
// ---------------------------------------------------------------------------
function loadPref(key, fallback) {
  try { return localStorage.getItem("vedirect." + key) || fallback; } catch (e) { return fallback; }
}
function savePref(key, value) {
  try { localStorage.setItem("vedirect." + key, value); } catch (e) { /* egal */ }
}

// ---------------------------------------------------------------------------
// Reiter (W4a): Hauptreiter + Unterreiter in "System" und (W4b) "Einstellungen".
// Jeder Hauptreiter mit Unterreitern merkt sich seinen eigenen Unterreiter.
// ---------------------------------------------------------------------------
let currentTab = "";
let currentSub = "";   // Unterreiter in "System" (für den Neustart nach Updates usw.)

const subDefaults = { system: "info", settings: "control" };

/// Unterreiter "name" im Hauptreiter "tab" anzeigen.
function showSubIn(tab, name) {
  const section = $("tab-" + tab);
  if (!section.querySelector("#sub-" + name)) name = subDefaults[tab];
  if (tab === "system") currentSub = name;
  savePref("sub." + tab, name);
  section.querySelectorAll(".subtab").forEach((b) => b.classList.toggle("active", b.dataset.sub === name));
  section.querySelectorAll(".subpanel").forEach((p) => p.classList.toggle("hidden", p.id !== "sub-" + name));
  if (name === "wifi") loadWifi();
  if (name === "mqtt") loadMqtt();
  if (name === "device") loadClock();
  if (name === "control") loadSettings();
  if (name === "shuntset") readHex("shunt");
  if (name === "mpptset") readHex("mppt");
}

/// Unterreiter im Sammelreiter "System" (wird auch von anderer Stelle genutzt).
function showSub(name) { showSubIn("system", name); }

function showTab(name) {
  if (!$("tab-" + name)) name = "overview";
  currentTab = name;
  savePref("tab", name);
  document.querySelectorAll(".tab").forEach((b) => b.classList.toggle("active", b.dataset.tab === name));
  document.querySelectorAll(".panel").forEach((p) => p.classList.toggle("hidden", p.id !== "tab-" + name));
  if (subDefaults[name]) showSubIn(name, loadPref("sub." + name, loadPref(name === "system" ? "sub" : "-", subDefaults[name])));
  if (name === "overview") refreshLive();
  if (name === "shunt" || name === "mppt" || name === "history") refreshDevices();
  if (name === "history") readHex("mppt");
  if (name === "faults") refreshFaults();
}

document.querySelectorAll(".tab").forEach((btn) => {
  btn.addEventListener("click", () => showTab(btn.dataset.tab));
});
document.querySelectorAll(".subtab").forEach((btn) => {
  const tab = btn.closest(".panel").id.replace("tab-", "");
  btn.addEventListener("click", () => showSubIn(tab, btn.dataset.sub));
});
showTab(loadPref("tab", "overview"));

// ---------------------------------------------------------------------------
// "Passwort anzeigen": Checkbox mit data-show="feld1,feld2"
// ---------------------------------------------------------------------------
document.querySelectorAll("input[data-show]").forEach((cb) => {
  cb.addEventListener("change", () => {
    cb.dataset.show.split(",").forEach((id) => {
      $(id).type = cb.checked ? "text" : "password";
    });
  });
});

// ---------------------------------------------------------------------------
// Status (alle 3 s)
// ---------------------------------------------------------------------------
let lastStatus = null;
async function refreshStatus() {
  try {
    const s = await api("/api/status");
    const w = s.wifi;
    $("deviceName").textContent = s.device;
    $("stState").textContent = stateText[w.state] || w.state;
    $("stError").textContent = w.lastError || "–";
    lastStatus = s;
    $("stSsid").textContent = w.ssid || "–";
    $("stIp").textContent = w.ip || "–";
    $("stRssi").textContent = w.connected ? w.rssi + " dBm" : "–";
    $("stAp").textContent = w.apActive
      ? w.apSsid + " (" + w.apIp + ", " + w.apClients + " Gerät(e))"
      : "aus";
    $("stHost").textContent = "http://" + w.hostname + ".local";
    $("stFw").textContent = s.firmware;
    $("stWebui").textContent = s.webui;
    $("upFw").textContent = s.firmware;
    $("upWebui").textContent = s.webui;
    $("upTrial").textContent = s.update.trial
      ? "Neue Firmware läuft auf Probe (wird nach 60 s bestätigt)"
      : "Firmware bestätigt";
    $("rollbackWarning").classList.toggle("hidden", !s.update.rolledBack);
    if (s.mqtt) showMqttStatus(s.mqtt);
    $("stUptime").textContent = formatUptime(s.uptimeS);
    $("stHeap").textContent = Math.round(s.freeHeap / 1024) + " KB";
    if (s.memory) {
      // W5: kleinste Werte seit dem Start; unter der Warngrenze rot
      const m = s.memory;
      $("stHeapMin").textContent = Math.round(m.minFreeHeap / 1024) + " KB (kleinster Wert seit Start)";
      $("stHeapMin").style.color = m.minFreeHeap < m.heapWarn ? "var(--danger)" : "";
      $("stStackMin").textContent = m.minStack + " Byte (Task " + m.minStackTask + ")";
      $("stStackMin").style.color = m.minStack < m.stackWarn ? "var(--danger)" : "";
    }
    $("pwWarning").classList.toggle("hidden", !s.defaultPassword);
    showFaultSummary(s.faults);
    sendBrowserTimeIfNeeded(s.clock);
    if (s.clock) {
      $("stClock").textContent = s.clock.valid
        ? s.clock.local + " (" + (clockSourceText[s.clock.source] || s.clock.source) + ")"
        : "nicht gestellt";
    }
  } catch (e) {
    $("stState").textContent = "Keine Verbindung zum Gerät";
  }
}

// Markierung am Reiter "Fehler" und Banner bei kritischen Fehlern (alle Reiter)
function showFaultSummary(f) {
  if (!f) return;
  $("faultBadge").textContent = f.count;
  // Jeder aktive Fehler = rot, unabhängig von der Schwere (Projektinhaber 09.11.2026)
  $("faultBadge").classList.toggle("hidden", f.count === 0);
  $("critBanner").classList.toggle("hidden", f.severity !== "critical");
}
$("critLink").addEventListener("click", (ev) => { ev.preventDefault(); showTab("faults"); });

// ---------------------------------------------------------------------------
// Aktualisierung (W4a): ein Takt für alles, im Browser wählbar (Standard 5 s).
// Läuft nicht, solange die Seite im Hintergrund ist.
// ---------------------------------------------------------------------------
let refreshTimer = null;

async function refreshAll() {
  if (document.hidden) return;
  await refreshStatus();
  if (currentTab === "overview") await refreshLive();
  if (currentTab === "shunt" || currentTab === "mppt" || currentTab === "history") await refreshDevices();
  if (currentTab === "faults") await refreshFaults();
}

function startRefresh() {
  const s = parseInt(loadPref("refresh", "5"), 10) || 5;
  $("refreshSel").value = String(s);
  clearInterval(refreshTimer);
  refreshTimer = setInterval(refreshAll, s * 1000);
}

$("refreshSel").addEventListener("change", () => {
  savePref("refresh", $("refreshSel").value);
  startRefresh();
});
document.addEventListener("visibilitychange", () => { if (!document.hidden) refreshAll(); });

// ---------------------------------------------------------------------------
// Übersicht (W4a Schritt 2)
// ---------------------------------------------------------------------------
function num(v, digits) {
  return v.toLocaleString("de-DE", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}
function fmt(v, digits, unit) {
  return v === null || v === undefined ? "–" : num(v, digits) + " " + unit;
}
function formatTtg(b) {
  if (b.timeToGoInfinite) return "∞ (keine Entladung)";
  if (b.timeToGoMin === null) return "–";
  const h = Math.floor(b.timeToGoMin / 60);
  return (h ? h + " h " : "") + (b.timeToGoMin % 60) + " min";
}

const linkText = { ok: "● ok", instabil: "◐ instabil", defekt: "⊗ fehlt / ausgefallen" };

function relayInfo(i, r, lim, live) {
  const parts = [];
  const on = r["r" + i] === "ON";
  const wanted = r["r" + i + "Wanted"];
  const soc = live.battery.soc;
  if (r.override === "ON") {
    parts.push("Override: Sperren aufgehoben");
  } else if (r["r" + i + "Locked"]) {
    parts.push(lim.requiresSoc && soc === null
      ? "🔒 gesperrt: kein gültiger SOC (Shunt fehlt)"
      : "🔒 Einschalten gesperrt bis SOC " + num(lim.release, 0) + " %");
  }
  if (wanted && !on && r.override !== "ON") parts.push("Einschalten gewünscht, wartet auf ausreichenden SOC");
  parts.push("Schwellen: gesperrt unter " + num(lim.lock, 0) + " %, aus unter " + num(lim.off, 0) +
             " %, frei ab " + num(lim.release, 0) + " %");
  if (r["r" + i + "Reason"]) parts.push("Letzte Änderung: " + r["r" + i + "Reason"]);
  return parts.join("<br>");
}

function buildRelayRows(live) {
  const box = $("ovRelays");
  if (box.children.length) return;
  for (let i = 1; i <= live.limits.length; i++) {
    const div = document.createElement("div");
    div.className = "relay";
    div.innerHTML =
      '<div class="relay-head">' +
        '<span class="relay-name" id="rlName' + i + '">R' + i + '</span>' +
        '<span class="pill" id="rlState' + i + '">–</span>' +
        '<span class="relay-buttons" id="rlBtns' + i + '">' +
          '<button class="small" data-relay="' + i + '" data-on="ON">Ein</button>' +
          '<button class="small secondary" data-relay="' + i + '" data-on="OFF">Aus</button>' +
        '</span>' +
      '</div>' +
      '<div class="relay-info" id="rlInfo' + i + '"></div>';
    box.appendChild(div);
  }
  box.querySelectorAll("button[data-relay]").forEach((b) => {
    b.addEventListener("click", () => sendRelay(b.dataset.relay, b.dataset.on));
  });
}

function showLive(live) {
  const b = live.battery, p = live.pv, r = live.relays, s = live.system;

  // Batterie
  $("ovSoc").textContent = b.soc === null ? "– – %" : num(b.soc, 1) + " %";
  let dir = "";
  if (b.current !== null) dir = b.current > 0.05 ? "▲ lädt" : (b.current < -0.05 ? "▼ entlädt" : "o Ruhe");
  $("ovDir").textContent = dir;
  const star = b.voltageSource === "mppt" ? " *" : "";
  $("ovVoltage").textContent = fmt(b.voltage, 2, "V") + star;
  $("ovVoltHint").classList.toggle("hidden", b.voltageSource !== "mppt");
  $("ovCurrent").textContent = fmt(b.current, 2, "A");
  $("ovPower").textContent = fmt(b.power, 0, "W");
  $("ovTtg").textContent = formatTtg(b);
  $("ovConsumed").textContent = fmt(b.consumedAh, 1, "Ah");

  // Solar
  $("ovPvPower").textContent = fmt(p.power, 0, "W");
  $("ovPvVoltage").textContent = fmt(p.voltage, 2, "V");
  $("ovPvState").textContent = p.state || "–";
  $("ovPvYield").textContent = fmt(p.yieldTodayKWh, 2, "kWh");
  $("ovPvError").textContent = p.errorCode === null ? "–" : (p.errorCode === 0 ? "keiner" : "Code " + p.errorCode);

  // Relais
  buildRelayRows(live);
  for (let i = 1; i <= live.limits.length; i++) {
    const st = r["r" + i];
    $("rlName" + i).textContent = "R" + i + " " + (r["r" + i + "Name"] || "");
    const pill = $("rlState" + i);
    pill.textContent = st === "ON" ? "EIN" : (st === "OFF" ? "AUS" : "deaktiviert");
    pill.className = "pill " + (st === "ON" ? "on" : (st === "OFF" ? "off" : "disabled"));
    $("rlBtns" + i).classList.toggle("hidden", st === "DISABLED");
    $("rlInfo" + i).innerHTML = st === "DISABLED" ? "In dieser Anlage nicht verwendet." : relayInfo(i, r, live.limits[i - 1], live);
  }
  const ovr = r.override === "ON";
  $("ovOverrideState").textContent = ovr ? "AKTIV" : "AUS";
  $("ovOverrideState").className = "pill " + (ovr ? "warn" : "off");
  $("ovOverrideBanner").classList.toggle("hidden", !ovr);

  // Verbindung
  $("ovLinkShunt").textContent = linkText[s.shunt] || s.shunt;
  $("ovLinkMppt").textContent = linkText[s.mppt] || s.mppt;
  $("ovUpdated").textContent = "Stand: " + new Date().toLocaleTimeString("de-DE");
}

async function refreshLive() {
  try {
    showLive(await api("/api/live"));
  } catch (e) {
    $("ovUpdated").textContent = "Keine Verbindung zum Gerät";
  }
}

let commandBusy = false;
async function sendCommand(path, data) {
  if (commandBusy) return;
  commandBusy = true;
  document.querySelectorAll("#tab-overview button").forEach((b) => (b.disabled = true));
  try {
    const res = await api(path, data);
    toast(res.result, !res.ok);
    if (res.live) showLive(res.live);
  } catch (e) {
    toast(e.message, true);
  }
  document.querySelectorAll("#tab-overview button").forEach((b) => (b.disabled = false));
  commandBusy = false;
}

function sendRelay(relay, on) {
  sendCommand("/api/relay", { relay: relay, on: on });
}

$("ovOverrideOn").addEventListener("click", () => {
  if (!confirm("Override einschalten?\n\nAlle Schutzsperren (SOC-Schwellen, fehlender Shunt) werden aufgehoben. " +
               "Nur noch das BMS schützt den Akku vor Tiefentladung. Die Verantwortung liegt beim Benutzer.")) return;
  sendCommand("/api/override", { on: "ON" });
});
$("ovOverrideOff").addEventListener("click", () => sendCommand("/api/override", { on: "OFF" }));

// ---------------------------------------------------------------------------
// Shunt, MPPT, Historie (W4a Schritt 3): alle Felder in Klartext
// ---------------------------------------------------------------------------
const StaleAfterS = 10;   // älter = grau mit "vor x s"

function escapeHtml(t) {
  return String(t).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

function fieldRows(fields) {
  if (!fields.length) return '<tr><td class="muted">Keine Werte empfangen.</td></tr>';
  return fields.map((f) => {
    const stale = f.ageS > StaleAfterS;
    return '<tr' + (stale ? ' class="stale"' : '') + '>' +
      '<td>' + escapeHtml(f.name) + '</td>' +
      '<td>' + escapeHtml(f.value) + (stale ? ' <span class="lbl">(vor ' + f.ageS + ' s)</span>' : '') + '</td></tr>';
  }).join("");
}

function grid(rows) {
  return rows.map(([k, v]) => "<dt>" + escapeHtml(k) + "</dt><dd>" + escapeHtml(v) + "</dd>").join("");
}

function renderDevice(prefix, d) {
  $(prefix + "Missing").classList.toggle("hidden", d.present);
  if (!d.present) {
    $(prefix + "Head").innerHTML = "";
    $(prefix + "Fields").innerHTML = "";
    $(prefix + "Link").innerHTML = "";
    return;
  }
  $(prefix + "Head").innerHTML = grid([
    ["Gerät", d.device],
    ["Anschluss", d.uart],
    ["Letzter Datenblock", "vor " + num(d.lastFrameAgoMs / 1000, 1) + " s"],
  ]);
  $(prefix + "Fields").innerHTML = fieldRows(d.fields.filter((f) => !f.history));
  const l = d.link;
  $(prefix + "Link").innerHTML = grid([
    ["Zustand", l.unstable ? "◐ instabil" : "● ok"],
    ["Prüfsummenfehler", String(l.checksumErrors)],
    ["Verworfene Blöcke", String(l.discarded)],
    ["Ausfälle", String(l.outages) + (l.lastOutageMs ? " (letzter " + num(l.lastOutageMs / 1000, 0) + " s)" : "")],
    ["Als instabil eingestuft", l.unstableCount + "×"],
    ["Verlorene Meldungen", String(l.queueDrops)],
  ]);
}

function renderHistory(id, d) {
  $(id).innerHTML = d.present
    ? fieldRows(d.fields.filter((f) => f.history))
    : '<tr><td class="muted">Gerät nicht erkannt.</td></tr>';
}

async function refreshDevices() {
  try {
    if (currentTab === "shunt" || currentTab === "history") {
      const s = await api("/api/device?role=shunt");
      if (currentTab === "shunt") renderDevice("shunt", s); else renderHistory("histShunt", s);
    }
    if (currentTab === "mppt" || currentTab === "history") {
      const m = await api("/api/device?role=mppt");
      if (currentTab === "mppt") renderDevice("mppt", m); else renderHistory("histMppt", m);
    }
  } catch (e) {
    toast("Keine Verbindung zum Gerät", true);
  }
}

// ---------------------------------------------------------------------------
// Fehler (W4a Schritt 4): aktive Fehler, Neustart-Grund, Ereignisliste
// ---------------------------------------------------------------------------
const severityText = { critical: "kritisch", warning: "Warnung", info: "Info", none: "" };

function formatAgo(s) {
  if (s < 60) return "vor " + s + " s";
  if (s < 3600) return "vor " + Math.floor(s / 60) + " min";
  const h = Math.floor(s / 3600);
  if (h < 48) return "vor " + h + " h " + Math.floor((s % 3600) / 60) + " min";
  return "vor " + Math.floor(h / 24) + " d " + (h % 24) + " h";
}

function sevPill(sev) {
  return '<span class="sev ' + sev + '">' + escapeHtml(severityText[sev] || sev) + "</span>";
}

async function refreshFaults() {
  try {
    const f = await api("/api/faults");
    $("ftActive").innerHTML = f.active.length
      ? f.active.map((a) => "<li>" + sevPill(a.severity) + " " + escapeHtml(a.text) + "</li>").join("")
      : '<li class="ok">✓ Keine aktiven Fehler</li>';
    const dev = [];
    if (f.pvError) dev.push(["MPPT meldet", f.pvError]);
    if (f.batteryAlarm) dev.push(["Shunt-Alarm", f.batteryAlarm]);
    $("ftDevice").innerHTML = grid(dev);
    $("ftReset").innerHTML = sevPill(f.reset.severity) + " " + escapeHtml(f.reset.text);
    $("ftUptime").textContent = formatUptime(f.uptimeS) + (f.bootTime ? " (seit " + f.bootTime + ")" : "");
    $("ftEvents").innerHTML = f.events.length
      ? f.events.map((e) =>
          // W4e: mit gültiger Uhr Datum/Uhrzeit (Zeitpunkt beim Darüberfahren: "vor …")
          '<tr><td class="when"' + (e.time ? ' title="' + formatAgo(e.ageS) + '">' + e.time : ">" + formatAgo(e.ageS)) +
          "</td><td>" + sevPill(e.severity) +
          "</td><td>" + escapeHtml(e.text) + "</td></tr>").join("")
      : '<tr><td class="muted">Keine Ereignisse.</td></tr>';
  } catch (e) {
    toast("Keine Verbindung zum Gerät", true);
  }
}

// ---------------------------------------------------------------------------
// Einstellungen (W4a Schritt 5)
// Wird NICHT im Takt aktualisiert, sonst würden Eingaben überschrieben.
// "var" statt "let": showTab() läuft schon oben beim Laden der Datei.
// ---------------------------------------------------------------------------
var settingsInstallation = "";

function onOffText(v) { return v ? "ein" : "aus"; }

function numField(i, key, label, unit, value, preset, step, max) {
  return '<label>' + label + ' (' + unit + ')' +
    '<input type="number" id="set' + key + i + '" min="0" max="' + max + '" step="' + step +
    '" value="' + value + '" inputmode="decimal">' +
    '<small>Voreinstellung: ' + preset + ' ' + unit + '</small></label>';
}

function relaySettingsCard(i, r, maxMinOffS) {
  const head = '<div class="relay-head"><span class="relay-name">R' + (i + 1) + " " + escapeHtml(r.name) +
    "</span>" + (r.custom ? '<span class="pill warn">geändert</span>' : '<span class="pill off">Voreinstellung</span>') +
    "</div>";
  if (!r.enabled) {
    return '<div class="relay"><div class="relay-head"><span class="relay-name">R' + (i + 1) + " " +
      escapeHtml(r.name) + '</span><span class="pill disabled">deaktiviert</span></div>' +
      '<div class="relay-info">In dieser Anlage nicht verwendet.</div></div>';
  }
  const p = r.preset;
  const noSoc = r.requiresSoc
    ? (r.forceOffWithoutSoc ? "gesperrt und ausgeschaltet" : "gesperrt")
    : "Zustand bleibt";
  return '<div class="relay" data-relay="' + (i + 1) + '">' + head +
    '<div class="setgrid">' +
    numField(i, "Lock", "Sperre unter", "%", r.lock, p.lock, "0.1", 100) +
    numField(i, "Off", "Aus unter", "%", r.off, p.off, "0.1", 100) +
    numField(i, "Release", "Frei ab", "%", r.release, p.release, "0.1", 100) +
    numField(i, "MinOff", "Mindest-Auszeit", "s", r.minOffS, p.minOffS, "1", maxMinOffS) +
    "</div>" +
    '<label>Zustand nach Neustart<select id="setStartup' + i + '">' +
    '<option value="OFF"' + (r.startup ? "" : " selected") + ">aus</option>" +
    '<option value="ON"' + (r.startup ? " selected" : "") + ">ein (sobald freigegeben)</option>" +
    "</select><small>Voreinstellung: " + onOffText(p.startup) + "</small></label>" +
    '<label class="check"><input type="checkbox" id="setAuto' + i + '"' + (r.auto ? " checked" : "") + ">" +
    "<span>Nach Schutzabschaltung automatisch wieder ein (wenn der SOC wieder reicht)" +
    "<small>Voreinstellung: " + (p.auto ? "ja" : "nein") + "</small></span></label>" +
    '<div class="relay-info">Ohne gültigen SOC (Shunt fehlt): ' + noSoc + " (fest).</div>" +
    '<div class="relay-buttons"><button class="small" data-save="' + i + '">Speichern</button>' +
    (r.custom ? '<button class="small secondary" data-reset="' + i + '">Auf Voreinstellung</button>' : "") +
    "</div></div>";
}

function showSettings(s) {
  settingsInstallation = s.installation;
  $("setInstallation").value = s.installation;
  $("setRelays").innerHTML = s.relays.map((r, i) => relaySettingsCard(i, r, s.maxMinOffS)).join("");
  document.querySelectorAll("#setRelays button[data-save]").forEach((btn) => {
    btn.addEventListener("click", () => saveRelaySettings(parseInt(btn.dataset.save, 10), btn));
  });
  document.querySelectorAll("#setRelays button[data-reset]").forEach((btn) => {
    btn.addEventListener("click", () => resetRelaySettings(parseInt(btn.dataset.reset, 10), btn));
  });
}

// Ein einzelnes Relais auf die Voreinstellung der Anlage zurücksetzen
async function resetRelaySettings(i, btn) {
  if (!confirm("R" + (i + 1) + " auf die Voreinstellung zurücksetzen?\n" +
               "Die Werte wirken sofort; das Relais kann dadurch abschalten.")) return;
  btn.disabled = true;
  try {
    const res = await api("/api/settings/defaults", { relay: i + 1 });
    toast("R" + (i + 1) + ": " + res.result, !res.ok);
    showSettings(res.settings);
  } catch (e) {
    toast("R" + (i + 1) + ": " + e.message, true);
    btn.disabled = false;
  }
}

async function loadSettings() {
  try {
    showSettings(await api("/api/settings"));
  } catch (e) {
    toast("Einstellungen konnten nicht geladen werden: " + e.message, true);
  }
}

async function saveRelaySettings(i, btn) {
  const data = {
    relay: i + 1,
    lock: $("setLock" + i).value,
    off: $("setOff" + i).value,
    release: $("setRelease" + i).value,
    minOffS: $("setMinOff" + i).value,
    startup: $("setStartup" + i).value,
    auto: $("setAuto" + i).checked ? "ON" : "OFF",
  };
  btn.disabled = true;
  try {
    const res = await api("/api/settings/relay", data);
    toast("R" + (i + 1) + ": " + res.result, !res.ok);
    showSettings(res.settings);
  } catch (e) {
    toast("R" + (i + 1) + ": " + e.message, true);
    btn.disabled = false;
  }
}

$("setDefaultsBtn").addEventListener("click", async () => {
  if (!confirm("Alle Relais-Einstellungen auf die Voreinstellung der Anlage zurücksetzen?\n" +
               "Die Werte wirken sofort; ein Relais kann dadurch abschalten.")) return;
  try {
    const res = await api("/api/settings/defaults", {});
    toast(res.result, !res.ok);
    showSettings(res.settings);
  } catch (e) {
    toast(e.message, true);
  }
});

$("setInstallBtn").addEventListener("click", async () => {
  const value = $("setInstallation").value;
  if (value === settingsInstallation) { toast("Anlage ist bereits ausgewählt."); return; }
  const name = $("setInstallation").selectedOptions[0].textContent;
  if (!confirm("Anlage wechseln auf „" + name + "“?\n\n" +
               "• Alle geänderten Relais-Einstellungen werden auf die Voreinstellung zurückgesetzt.\n" +
               "• Das Gerät startet neu, alle Relais fallen dabei kurz ab.")) return;
  try {
    const res = await api("/api/settings/installation", { value: value });
    toast(res.result);
    if (res.restart) {
      $("setInstallBtn").disabled = true;
      setTimeout(() => location.reload(), 15000);
    }
  } catch (e) {
    toast(e.message, true);
  }
});

// ---------------------------------------------------------------------------
// W4b: Daten per HEX-Protokoll (nur lesen). Gelesen wird beim Öffnen des
// Reiters und per Knopf, NICHT im Aktualisierungstakt (Projektinhaber).
// showTab() läuft schon oben beim Laden der Datei, also bevor Variablen
// hier unten belegt sind. Deshalb steht der Zustand als Eigenschaft an der
// (vorab bekannten) Funktion readHex selbst.
// ---------------------------------------------------------------------------
function wait(ms) { return new Promise((res) => setTimeout(res, ms)); }

/// Neu einlesen anfordern und warten, bis das Gerät fertig ist (max. 20 s).
async function readHex(role) {
  const hexReading = readHex.running || (readHex.running = {});   // je Rolle: läuft gerade
  if (hexReading[role]) return;
  hexReading[role] = true;
  // Ein Lesen füllt alle Anzeigen dieser Rolle (MPPT: Historie + Einstellungen)
  const render = (h) => {
    if (role === "mppt") renderMpptHistory(h);
    renderHexSettings(role, h);
  };
  try {
    const r = await api("/api/hex/refresh", { role: role });
    if (!r.ok) {
      render({ present: false });
      return;
    }
    render(null);   // "wird gelesen …"
    for (let i = 0; i < 40; i++) {
      await wait(500);
      const h = await api("/api/hex?role=" + role);
      if (!h.present || (!h.pending && !h.busy)) {
        render(h);
        return;
      }
    }
    render(await api("/api/hex?role=" + role));
  } catch (e) {
    toast("Keine Verbindung zum Gerät", true);
  } finally {
    hexReading[role] = false;
  }
}

function fmtKWh(v) { return v === null || v === undefined ? "–" : num(v, 2) + " kWh"; }
function fmtV(v)   { return v === null || v === undefined ? "–" : num(v, 2) + " V"; }
function fmtMin(m) { return m >= 60 ? Math.floor(m / 60) + " h " + (m % 60) + " min" : m + " min"; }

/// Historientag als Text. today = {y, m, d} des Geräts (nur bei gültiger Uhr),
/// dann mit Wochentag und Datum, z. B. "Mo 09.11.".
function dayLabel(ago, today) {
  if (!today) {
    if (ago === 0) return "heute";
    if (ago === 1) return "gestern";
    return "vor " + ago + " Tagen";
  }
  const dt = new Date(today.y, today.m - 1, today.d - ago);   // reine Kalenderrechnung
  const wd = ["So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"][dt.getDay()];
  const date = String(dt.getDate()).padStart(2, "0") + "." + String(dt.getMonth() + 1).padStart(2, "0") + ".";
  if (ago === 0) return "heute, " + date;
  if (ago === 1) return "gestern, " + date;
  return wd + " " + date;
}

const hexStateText = {
  nodata: "keine Daten",
  unknown: "nicht verfügbar",
  noanswer: "keine Antwort",
  missing: "nicht gelesen",
};

/// Anzeige der MPPT-Historie. h = null: wird gerade gelesen.
function renderMpptHistory(h) {
  const status = $("hxStatus");
  if (h === null) {
    status.textContent = "Wird aus dem MPPT gelesen …";
    $("hxRefresh").disabled = true;
    return;
  }
  $("hxRefresh").disabled = false;
  const clear = () => { $("hxTotal").innerHTML = ""; $("hxChart").innerHTML = ""; $("hxDays").innerHTML = ""; };

  if (!h.present) {
    status.textContent = "MPPT nicht erkannt.";
    clear();
    return;
  }
  if (h.pending || h.busy) {
    status.textContent = "Lesen dauert länger als erwartet – bitte später erneut versuchen.";
    return;
  }
  if (h.job.aborted) {
    status.textContent = "Der MPPT (" + h.uart + ") antwortet nicht auf HEX-Anfragen – Historie nicht verfügbar. " +
      "(Bei einem Simulator, der nur Text sendet, ist das normal.)";
    clear();
    return;
  }
  status.textContent = h.device + " (" + h.uart + "), gelesen " + formatAgo(h.job.ageS) +
    (h.job.failed ? ", " + h.job.failed + " Werte ohne Antwort" : "");

  // Gesamtwerte
  const t = h.history.total;
  if (t.state === "ok") {
    const rows = [
      ["Gesamtertrag", fmtKWh(t.userYieldKWh) + " (System: " + fmtKWh(t.systemYieldKWh) + ")"],
      ["Panelspannung max.", fmtV(t.panelMaxV)],
      ["Batteriespannung max./min.", fmtV(t.batteryMaxV) + " / " + fmtV(t.batteryMinV)],
      ["Tage mit Daten", String(t.daysAvailable)],
    ];
    if (t.errors.length) rows.push(["Letzte Fehler", t.errors.join(", ")]);
    $("hxTotal").innerHTML = grid(rows);
  } else {
    $("hxTotal").innerHTML = grid([["Gesamtwerte", hexStateText[t.state] || t.state]]);
  }

  // Balkendiagramm Tagesertrag (ältester links, heute rechts)
  const days = h.history.days.slice().reverse();
  const max = Math.max(0.01, ...days.map((d) => (d.state === "ok" ? d.yieldKWh : 0)));
  $("hxChart").innerHTML = days.map((d) => {
    const v = d.state === "ok" ? d.yieldKWh : 0;
    const pct = Math.round((v / max) * 100);
    const tip = dayLabel(d.ago, h.today) + ": " + (d.state === "ok" ? fmtKWh(v) : (hexStateText[d.state] || d.state));
    return '<div class="bar' + (d.state === "ok" ? "" : " empty") + '" title="' + escapeHtml(tip) +
      '"><span style="height:' + pct + '%"></span></div>';
  }).join("") + '<div class="barlabels"><span>vor 30 Tagen</span><span>max. ' + fmtKWh(max) + "</span><span>heute</span></div>";

  // Tabelle (heute oben)
  const head = "<tr><th>Tag</th><th>Ertrag</th><th>P max</th><th>Batterie min–max</th>" +
    "<th>Bulk / Abs. / Erh.</th><th>Fehler</th></tr>";
  $("hxDays").innerHTML = head + h.history.days.map((d) => {
    if (d.state !== "ok") {
      return '<tr class="stale"><td>' + dayLabel(d.ago, h.today) + '</td><td colspan="5">' +
        escapeHtml(hexStateText[d.state] || d.state) + "</td></tr>";
    }
    return "<tr><td>" + dayLabel(d.ago, h.today) + "</td><td>" + fmtKWh(d.yieldKWh) + "</td><td>" + d.powerMaxW + " W</td>" +
      "<td>" + num(d.batteryMinV, 2) + "–" + num(d.batteryMaxV, 2) + " V</td>" +
      "<td>" + fmtMin(d.bulkMin) + " / " + fmtMin(d.absorptionMin) + " / " + fmtMin(d.floatMin) + "</td>" +
      '<td class="' + (d.errors.length ? "err" : "") + '">' + (d.errors.length ? escapeHtml(d.errors.join(", ")) : "–") + "</td></tr>";
  }).join("");
}

$("hxRefresh").addEventListener("click", () => readHex("mppt"));

/// Geräteeinstellungen (nur lesen) im Reiter Einstellungen → Shunt / MPPT.
/// h = null: wird gerade gelesen.
function renderHexSettings(role, h) {
  const prefix = role === "shunt" ? "hsShunt" : "hsMppt";
  const status = $(prefix + "Status");
  const table = $(prefix);
  const name = role === "shunt" ? "SmartShunt" : "MPPT";
  document.querySelectorAll('button.hexreload[data-role="' + role + '"]').forEach((b) => (b.disabled = h === null));
  if (h === null) {
    status.textContent = "Wird aus dem " + name + " gelesen …";
    return;
  }
  if (!h.present) {
    status.textContent = name + " nicht erkannt.";
    table.innerHTML = "";
    return;
  }
  if (h.pending || h.busy) {
    status.textContent = "Lesen dauert länger als erwartet – bitte später erneut versuchen.";
    return;
  }
  if (h.job.aborted) {
    status.textContent = "Der " + name + " (" + h.uart + ") antwortet nicht auf HEX-Anfragen – Einstellungen nicht verfügbar. " +
      "(Bei einem Simulator, der nur Text sendet, ist das normal.)";
    table.innerHTML = "";
    return;
  }
  status.textContent = h.device + " (" + h.uart + "), gelesen " + formatAgo(h.job.ageS);
  table.innerHTML = h.settings.length
    ? h.settings.map((s) => "<tr" + (s.state === "ok" ? "" : ' class="stale"') + "><td>" + escapeHtml(s.name) +
        "</td><td>" + escapeHtml(s.state === "ok" ? s.value : (hexStateText[s.state] || s.state)) + "</td></tr>").join("")
    : '<tr><td class="muted">Keine Einstellungen gelesen.</td></tr>';
}

document.querySelectorAll("button.hexreload").forEach((b) => {
  b.addEventListener("click", () => readHex(b.dataset.role));
});

// ---------------------------------------------------------------------------
// W4e: Uhr. Zustand als Eigenschaften an Funktionen (siehe readHex), weil
// refreshStatus()/showSubIn() schon beim Laden der Datei laufen.
// ---------------------------------------------------------------------------
const clockSourceText = {
  none: "noch nicht gestellt",
  rtc: "Echtzeituhr (RTC)",
  ntp: "Internet (NTP)",
  browser: "Uhrzeit des Browsers",
};

/// Hat das Gerät keine gültige Zeit, einmal pro Seitenaufruf die Browserzeit
/// schicken (Projektinhaber: automatisch, kein Knopf).
function sendBrowserTimeIfNeeded(clock) {
  if (!clock || clock.valid || sendBrowserTimeIfNeeded.sent) return;
  sendBrowserTimeIfNeeded.sent = true;
  api("/api/time", { unix: Math.floor(Date.now() / 1000) })
    .then((r) => { if (r.applied) toast("Uhrzeit des Geräts wurde vom Browser übernommen."); })
    .catch(() => { sendBrowserTimeIfNeeded.sent = false; });
}

function formatDuration(s) {
  if (s === null || s === undefined) return "";
  return " (" + formatAgo(s) + ")";
}

function showClock(c) {
  $("clkNow").textContent = c.valid ? c.local : "nicht gestellt";
  $("clkSource").textContent = (clockSourceText[c.source] || c.source) + (c.valid ? formatDuration(c.setAgoS) : "");
  $("clkRtc").textContent = c.rtc.found
    ? c.rtc.chip + (c.rtc.batteryLow ? " – Knopfzelle fehlt/schwach (Zeit geht bei Stromausfall verloren)" : " – Knopfzelle ok")
    : "nicht gefunden";
  $("clkTz").innerHTML = c.zones.map((z) => '<option value="' + escapeHtml(z.key) + '">' + escapeHtml(z.label) + "</option>").join("");
  $("clkTz").value = c.tz;
  $("clkServers").innerHTML = c.servers.map((s) => '<option value="' + escapeHtml(s) + '">').join("");
  $("clkNtp").value = c.ntp;
}

async function loadClock() {
  try {
    showClock(await api("/api/clock"));
  } catch (e) {
    toast("Uhr: " + e.message, true);
  }
}

$("clockForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  try {
    const r = await api("/api/clock", { tz: $("clkTz").value, ntp: $("clkNtp").value.trim() });
    toast("Uhr: " + r.result, !r.ok);
    showClock(r.clock);
  } catch (e) {
    toast("Uhr: " + e.message, true);
  }
});

// ---------------------------------------------------------------------------
// W5: Sicherung / Wiederherstellung (System → Gerät)
// ---------------------------------------------------------------------------
$("bkDownload").addEventListener("click", async () => {
  try {
    const res = await fetch("/api/backup");
    if (!res.ok) throw new Error("Fehler " + res.status);
    const text = await res.text();
    const d = new Date();
    const name = "vedirect-einstellungen-" + d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") +
      "-" + String(d.getDate()).padStart(2, "0") + ".json";
    const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    toast("Gespeichert als " + name);
  } catch (e) {
    toast("Sicherung: " + e.message, true);
  }
});

/// Gewählte Datei als Text lesen (für Prüfen und Wiederherstellen).
function readBackupFile() {
  const file = $("bkFile").files[0];
  if (!file) return Promise.reject(new Error("Bitte zuerst eine Datei auswählen."));
  if (file.size > 8192) return Promise.reject(new Error("Datei zu groß – das ist keine Sicherung dieses Geräts."));
  return file.text();
}

$("bkFile").addEventListener("change", () => $("bkPreview").classList.add("hidden"));

$("bkCheck").addEventListener("click", async () => {
  $("bkPreview").classList.add("hidden");
  try {
    const r = await api("/api/backup/check", { data: await readBackupFile() });
    $("bkSummary").innerHTML = grid(r.summary);
    $("bkPreview").classList.remove("hidden");
  } catch (e) {
    toast("Sicherung: " + e.message, true);
  }
});

$("bkRestore").addEventListener("click", async () => {
  if (!confirm("Alle Einstellungen durch die Sicherung ersetzen?\n\n" +
               "• Das Gerät startet danach neu, alle Relais fallen dabei kurz ab.\n" +
               "• Passwörter bleiben unverändert.")) return;
  try {
    const r = await api("/api/backup/restore", { data: await readBackupFile() });
    toast(r.result, !r.ok);
    $("bkRestore").disabled = true;
    setTimeout(() => location.reload(), 15000);
  } catch (e) {
    toast("Sicherung: " + e.message, true);
  }
});

// Start: sofort einmal alles holen, dann im gewählten Takt
startRefresh();
refreshAll();

// ---------------------------------------------------------------------------
// WLAN
// ---------------------------------------------------------------------------
async function loadWifi() {
  try {
    const w = await api("/api/wifi");
    $("wifiSsid").value = w.ssid;
    $("apAlways").checked = w.apAlways;
    $("wifiPassHint").textContent = w.passwordSet
      ? "Ein Passwort ist gespeichert. Leer lassen = behalten."
      : "Noch kein Passwort gespeichert.";
  } catch (e) {
    toast(e.message, true);
  }
}

// Nach dem Speichern auf das Ergebnis des Verbindungsversuchs warten
// (max. ca. 30 s) und Erfolg oder Fehlergrund melden.
async function waitForConnection(attemptBefore) {
  const until = Date.now() + 30000;
  while (Date.now() < until) {
    await new Promise((res) => setTimeout(res, 2000));
    let s;
    try { s = await api("/api/status"); } catch (e) { continue; }
    const w = s.wifi;
    if (w.attempt <= attemptBefore) continue;          // neuer Versuch noch nicht gestartet
    if (w.connected) {
      toast("Verbunden mit " + w.ssid + " – IP " + w.ip + ", erreichbar unter http://" + w.hostname + ".local");
      return;
    }
    if (w.lastError) {
      toast("Keine Verbindung: " + w.lastError, true);
      return;
    }
  }
  toast("Keine Verbindung innerhalb von 30 s. Gerät versucht es weiter.", true);
}

$("wifiForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const attemptBefore = lastStatus ? lastStatus.wifi.attempt : 0;
  try {
    await api("/api/wifi", {
      ssid: $("wifiSsid").value,
      password: $("wifiPass").value,
      apAlways: $("apAlways").checked ? "1" : "0",
    });
    $("wifiPass").value = "";
    toast("Gespeichert. Verbinde mit dem WLAN …");
    waitForConnection(attemptBefore);
  } catch (e) {
    toast(e.message, true);
  }
});

$("scanBtn").addEventListener("click", async () => {
  $("scanBtn").disabled = true;
  $("scanList").innerHTML = "<li>Suche läuft …</li>";
  try {
    let r;
    for (let i = 0; i < 20; i++) {          // max. ca. 10 s warten
      r = await api("/api/wifi/scan");
      if (!r.scanning) break;
      await new Promise((res) => setTimeout(res, 500));
    }
    const list = $("scanList");
    list.innerHTML = "";
    const seen = new Set();
    (r.networks || [])
      .sort((a, b) => b.rssi - a.rssi)
      .forEach((n) => {
        if (!n.ssid || seen.has(n.ssid)) return;
        seen.add(n.ssid);
        const li = document.createElement("li");
        li.textContent = n.ssid + "  (" + n.rssi + " dBm" + (n.open ? ", offen" : "") + ")";
        li.addEventListener("click", () => { $("wifiSsid").value = n.ssid; $("wifiPass").focus(); });
        list.appendChild(li);
      });
    if (!list.children.length) list.innerHTML = "<li>Keine Netzwerke gefunden</li>";
  } catch (e) {
    toast(e.message, true);
  }
  $("scanBtn").disabled = false;
});

$("wifiResetBtn").addEventListener("click", async () => {
  if (!confirm("WLAN-Zugangsdaten wirklich löschen?")) return;
  try {
    await api("/api/wifi/reset", {});
    toast("WLAN-Zugangsdaten gelöscht.");
    loadWifi();
  } catch (e) {
    toast(e.message, true);
  }
});

// ---------------------------------------------------------------------------
// MQTT (W3)
// ---------------------------------------------------------------------------
const mqttStateText = {
  disabled:   "aus",
  no_network: "wartet auf WLAN-Verbindung",
  connecting: "verbinde …",
  connected:  "verbunden",
  error:      "Fehler – neuer Versuch folgt",
};

function showMqttStatus(m) {
  const text = (mqttStateText[m.state] || m.state) + (m.broker && m.state !== "disabled" ? " (" + m.broker + ")" : "");
  $("stMqtt").textContent = text;
  $("mqState").textContent = text;
  $("mqError").textContent = m.error || "–";
  $("mqCounts").textContent = m.publishCount + " Nachrichten, " + m.receiveCount + " Befehle empfangen" +
    (m.publishCount ? ", zuletzt vor " + m.lastPublishAgoS + " s" : "");
  $("tpBase1").textContent = m.baseTopic || "vedirect";
}

async function loadMqtt() {
  try {
    const m = await api("/api/mqtt");
    $("mqEnabled").checked = m.enabled;
    $("mqHost").value = m.host;
    $("mqPort").value = m.port;
    $("mqUser").value = m.user;
    $("mqPass").value = "";
    $("mqPassHint").textContent = m.passwordSet
      ? "Ein Passwort ist gespeichert. Leer lassen = behalten."
      : "Kein Passwort gespeichert.";
    $("mqBase").value = m.baseTopic;
    $("mqInterval").value = m.intervalS;
    $("mqClient").value = m.clientId;
    showMqttStatus(m.status);
  } catch (e) {
    toast(e.message, true);
  }
}

$("mqttForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  try {
    await api("/api/mqtt", {
      enabled: $("mqEnabled").checked ? "1" : "0",
      host: $("mqHost").value,
      port: $("mqPort").value,
      user: $("mqUser").value,
      password: $("mqPass").value,
      baseTopic: $("mqBase").value,
      intervalS: $("mqInterval").value,
      clientId: $("mqClient").value,
    });
    $("mqPass").value = "";
    toast("Gespeichert. Verbinde mit dem Broker …");
    setTimeout(loadMqtt, 4000);
  } catch (e) {
    toast(e.message, true);
  }
});

$("mqTestBtn").addEventListener("click", async () => {
  try {
    await api("/api/mqtt/test", {});
    toast("Verbindung wird neu aufgebaut …");
    setTimeout(loadMqtt, 4000);
  } catch (e) {
    toast(e.message, true);
  }
});

$("mqResetBtn").addEventListener("click", async () => {
  if (!confirm("MQTT-Einstellungen wirklich löschen?")) return;
  try {
    await api("/api/mqtt/reset", {});
    toast("MQTT-Einstellungen gelöscht.");
    loadMqtt();
  } catch (e) {
    toast(e.message, true);
  }
});

// ---------------------------------------------------------------------------
// Gerät
// ---------------------------------------------------------------------------
$("pwForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  if ($("pwNew").value !== $("pwNew2").value) {
    toast("Die neuen Passwörter stimmen nicht überein.", true);
    return;
  }
  try {
    await api("/api/password", { current: $("pwCurrent").value, new: $("pwNew").value });
    $("pwForm").reset();
    toast("Passwort geändert. Bitte neu anmelden; ggf. auch neu mit dem Access Point verbinden.");
  } catch (e) {
    toast(e.message, true);
  }
});

// ---------------------------------------------------------------------------
// Update (W2): Upload mit Fortschrittsanzeige, danach ggf. auf Neustart warten
// ---------------------------------------------------------------------------
function uploadFile(kind, file) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const form = new FormData();
    form.append("file", file, file.name);
    xhr.open("POST", "/api/update/" + kind);
    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable) {
        const pct = Math.round((ev.loaded / ev.total) * 100);
        $("uploadProgress").value = pct;
        $("uploadText").textContent = "Hochladen … " + pct + " %";
      }
    };
    xhr.onload = () => {
      let json = {};
      try { json = JSON.parse(xhr.responseText); } catch (e) { /* leer */ }
      if (xhr.status === 200 && json.ok) resolve(json);
      else reject(new Error(json.message || ("Fehler " + xhr.status)));
    };
    xhr.onerror = () => reject(new Error("Verbindung unterbrochen"));
    xhr.send(form);
  });
}

async function waitForRestart(oldFirmware) {
  $("uploadText").textContent = "Gerät startet neu …";
  await new Promise((res) => setTimeout(res, 8000));
  for (let i = 0; i < 30; i++) {
    try {
      const s = await api("/api/status");
      toast(s.firmware !== oldFirmware
        ? "Neu gestartet. Firmware jetzt " + s.firmware
        : "Neu gestartet. Firmware unverändert " + s.firmware);
      $("uploadText").textContent = "Fertig.";
      setTimeout(() => location.reload(), 1500);
      return;
    } catch (e) {
      await new Promise((res) => setTimeout(res, 2000));
    }
  }
  toast("Gerät nach 70 s nicht erreichbar. Bitte Seite manuell neu laden.", true);
}

document.querySelectorAll("button.upload").forEach((btn) => {
  btn.addEventListener("click", async () => {
    const file = $(btn.dataset.input).files[0];
    if (!file) { toast("Bitte zuerst eine Datei auswählen.", true); return; }
    if (!confirm("„" + file.name + "“ jetzt aufspielen?")) return;

    const oldFirmware = lastStatus ? lastStatus.firmware : "";
    document.querySelectorAll("button.upload").forEach((b) => (b.disabled = true));
    $("uploadBox").classList.remove("hidden");
    $("uploadProgress").value = 0;
    try {
      const r = await uploadFile(btn.dataset.kind, file);
      toast(r.message);
      if (r.restart) {
        await waitForRestart(oldFirmware);
      } else {
        $("uploadText").textContent = r.message;
        setTimeout(() => location.reload(), 2000);
      }
    } catch (e) {
      $("uploadText").textContent = "Fehler: " + e.message;
      toast(e.message, true);
    }
    document.querySelectorAll("button.upload").forEach((b) => (b.disabled = false));
  });
});

$("restartBtn").addEventListener("click", async () => {
  if (!confirm("Gerät neu starten?")) return;
  try {
    await api("/api/restart", {});
    toast("Neustart … Seite in ca. 15 s neu laden.");
  } catch (e) {
    toast(e.message, true);
  }
});

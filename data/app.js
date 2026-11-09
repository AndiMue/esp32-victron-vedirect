// VE.Direct WebUI – Stufe W1
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
// Reiter
// ---------------------------------------------------------------------------
document.querySelectorAll(".tab").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.add("hidden"));
    btn.classList.add("active");
    $("tab-" + btn.dataset.tab).classList.remove("hidden");
    if (btn.dataset.tab === "wifi") loadWifi();
    if (btn.dataset.tab === "mqtt") loadMqtt();
  });
});

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
    $("pwWarning").classList.toggle("hidden", !s.defaultPassword);
  } catch (e) {
    $("stState").textContent = "Keine Verbindung zum Gerät";
  }
}
refreshStatus();
setInterval(refreshStatus, 3000);

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

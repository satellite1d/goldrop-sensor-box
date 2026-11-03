"use strict";
const $ = id => document.getElementById(id);
const state = {devices: [], naming: false, device: null, before: null, previous: [], next: null, range: null, controller: null, timer: null, generation: 0};
const formatNumber = value => Number.isFinite(value) ? value.toLocaleString("pl-PL", {minimumFractionDigits: 2, maximumFractionDigits: 2}) : "—";
const formatTime = value => value == null ? "Brak czasu UTC" : new Date(value).toLocaleString("pl-PL");
const errors = {none: "OK", disconnected: "niepodłączony", timeout: "brak odpowiedzi", crc: "błąd CRC", invalid_value: "nieprawidłowy odczyt", not_initialized: "niegotowy", communication: "błąd komunikacji"};
function age(time, now) {
  if (time == null) return "brak pomiaru";
  const seconds = Math.floor((now - time) / 1000);
  if (seconds < -60) return "czas pomiaru z przyszłości";
  if (seconds < 60) return "przed chwilą";
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min temu`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} godz. temu`;
  return `${Math.floor(seconds / 86400)} dni temu`;
}
function resetPage() { state.before = null; state.previous = []; state.next = null; }
function setRange(now) {
  const period = $("period").value;
  const end = period === "custom" ? new Date($("to").value).getTime() : now;
  const start = period === "custom" ? new Date($("from").value).getTime() : end - Number(period) * 3600000;
  if (!Number.isFinite(start) || !Number.isFinite(end) || start < 0 || end <= start || end - start > 7 * 86400000) throw new Error("Wybierz poprawny zakres obejmujący maksymalnie 7 dni.");
  state.range = {from_ms: start, to_ms: end};
}
function query() {
  const params = new URLSearchParams({...state.range, device_id: state.device, limit: 200});
  if (state.before !== null) params.set("before_id", state.before);
  return params.toString();
}
async function getJson(path, signal) {
  const response = await fetch(path, {cache: "no-store", signal});
  if (!response.ok) throw new Error(response.status === 400 ? "Nieprawidłowy zakres lub filtr." : "Serwer chwilowo nie odpowiada. Ponawiam odczyt.");
  return response.json();
}
function showDevices(devices, now) {
  $("device-count").textContent = devices.length;
  $("no-devices").hidden = devices.length > 0;
  const focus = document.activeElement?.dataset.device;
  const list = $("device-list");
  list.replaceChildren();
  for (const device of devices) {
    const button = document.createElement("button");
    button.type = "button"; button.className = "device"; button.dataset.device = device.device_id;
    button.setAttribute("aria-pressed", String(device.device_id === state.device));
    const name = document.createElement("span"); name.className = "device-name"; name.textContent = device.name || device.device_id;
    const identity = document.createElement("small"); identity.textContent = device.name ? device.device_id : "Goldrop · ESP32-C3";
    const status = document.createElement("span"); status.className = "device-state";
    const dot = document.createElement("i"); dot.className = "state-dot" + (now - device.last_seen_ms < 45000 ? " online" : "");
    status.append(dot, document.createTextNode("Kontakt " + age(device.last_seen_ms, now)));
    button.append(name, identity, status);
    button.addEventListener("click", () => { state.device = device.device_id; $("name-form").hidden = true; resetPage(); refresh(); });
    list.append(button);
    if (focus === device.device_id) button.focus({preventScroll: true});
  }
}
function showMetrics(device, now) {
  $("rename").disabled = !device || state.naming;
  $("device-name").textContent = device ? device.name || "Urządzenie " + device.device_id.slice(-5) : "Twoje pomiary";
  const status = device?.status || {};
  $("device-detail").textContent = device ? `${device.device_id}${status.firmware_version ? " · firmware " + status.firmware_version : ""}${status.buffered != null ? " · w buforze: " + status.buffered : ""}` : "Oczekiwanie na pierwsze urządzenie";
  $("sht").textContent = formatNumber(device?.temperature_sht_c);
  $("ds").textContent = formatNumber(device?.temperature_ds18_c);
  $("rh").textContent = formatNumber(device?.humidity_rh);
  $("sht-status").textContent = device?.sht45_error ? "Czujnik: " + (errors[device.sht45_error] || device.sht45_error) : "Oczekiwanie na pomiar";
  $("ds-status").textContent = device?.ds18b20_error ? "Czujnik: " + (errors[device.ds18b20_error] || device.ds18b20_error) : "Oczekiwanie na pomiar";
  $("last-measurement").textContent = "Wspólny czas pomiaru: " + (device?.timestamp_ms == null && device?.received_ms != null ? "bez czasu UTC" : age(device?.timestamp_ms, now));

  const measured = device?.timestamp_ms, received = device?.received_ms;
  const delayed = measured != null && received != null && Math.abs(received - measured) > 120000;
  $("data-time").textContent = received == null ? "Oczekiwanie na pierwszy pomiar." :
    `Czas pomiaru wszystkich czujników: ${formatTime(measured)}. Odebrano: ${formatTime(received)} (${age(received, now)}).` +
    (delayed ? " Daty pomiaru i odbioru różnią się o ponad 2 minuty: możliwe zaległości w buforze lub niepoprawny zegar płytki/serwera. Wykres filtruje według czasu pomiaru." : "");
}
function svgElement(name, attrs = {}, text) {
  const node = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  return node;
}
function plot(id, points, series, unit, height, widthMs) {
  const svg = $(id); svg.replaceChildren();
  const mobile = window.matchMedia("(max-width:720px)").matches;
  const width = mobile ? 400 : 1000, ticks = mobile ? 2 : 4;
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  const left = 58, right = width - 18, top = 12, bottom = height - 35;
  const values = points.flatMap(p => series.flatMap(s => [p[s.key], p[s.min], p[s.max]])).filter(Number.isFinite);
  if (!values.length) { svg.append(svgElement("text", {x: width / 2, y: height / 2, "text-anchor": "middle"}, "Brak odczytów z czasem UTC w tym zakresie")); return; }
  let low = Math.min(...values), high = Math.max(...values);
  const margin = Math.max((high - low) * .12, unit === "°C" ? .4 : 2);
  low -= margin; high += margin;
  if (unit === "%") { low = Math.max(0, low); high = Math.min(100, high); }
  const x = time => left + (time - state.range.from_ms) / (state.range.to_ms - state.range.from_ms) * (right - left);
  const y = value => bottom - (value - low) / (high - low) * (bottom - top);
  for (let i = 0; i <= 4; i++) {
    const yy = top + (bottom - top) * i / 4;
    svg.append(svgElement("line", {x1: left, x2: right, y1: yy, y2: yy, class: "gridline"}));
    svg.append(svgElement("text", {x: left - 10, y: yy + 4, "text-anchor": "end"}, ((high - (high - low) * i / 4).toFixed(1)) + " " + unit));
  }
  for (let i = 0; i <= ticks; i++) {
    const time = state.range.from_ms + (state.range.to_ms - state.range.from_ms) * i / ticks;
    const label = new Date(time).toLocaleString("pl-PL", {day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit"});
    svg.append(svgElement("text", {x: x(time), y: height - 8, "text-anchor": i === 0 ? "start" : i === ticks ? "end" : "middle"}, label));
  }
  for (const item of series) {
    let path = "", previous = null;
    for (const point of points) {
      const value = point[item.key];
      if (!Number.isFinite(value)) { previous = null; continue; }
      const gap = previous === null || point.timestamp_ms - previous > Math.max(60000, widthMs * 3);
      path += `${gap ? "M" : "L"}${x(point.timestamp_ms).toFixed(2)},${y(value).toFixed(2)} `;
      previous = point.timestamp_ms;
      const mark = svgElement("circle", {cx: x(point.timestamp_ms), cy: y(value), r: 2, fill: item.color});
      mark.append(svgElement("title", {}, `${formatTime(point.timestamp_ms)} · ${item.name}: ${formatNumber(value)} ${unit}\nMin: ${formatNumber(point[item.min])}, max: ${formatNumber(point[item.max])} · ${point.samples} pomiarów`));
      svg.append(mark);
    }
    svg.append(svgElement("path", {d: path, stroke: item.color, class: "series"}));
  }
}
function showHistory(page) {
  const table = $("history"); table.replaceChildren();
  for (const row of page.items) {
    const tr = document.createElement("tr");
    const time = document.createElement("td"); time.textContent = formatTime(row.timestamp_ms);
    const sequence = document.createElement("small"); sequence.textContent = `Pomiar #${row.sequence}`;
    if (row.timestamp_ms == null) sequence.textContent += ` · ${(row.uptime_ms / 1000).toFixed(0)} s od startu`;
    time.append(sequence); tr.append(time);
    for (const key of ["temperature_sht_c", "temperature_ds18_c", "humidity_rh"]) { const td = document.createElement("td"); td.textContent = formatNumber(row[key]); tr.append(td); }
    const sensors = document.createElement("td");
    const issues = [];
    if (row.sht45_error !== "none") issues.push("Czujnik wewnętrzny: " + (errors[row.sht45_error] || row.sht45_error));
    if (row.ds18b20_error !== "none") issues.push("Czujnik zewnętrzny: " + (errors[row.ds18b20_error] || row.ds18b20_error));
    sensors.textContent = issues.join(" · ") || "OK"; sensors.className = issues.length ? "sensor-error" : "ok";
    const received = document.createElement("td"); received.textContent = formatTime(row.received_ms);
    tr.append(sensors, received); table.append(tr);
  }
  state.next = page.next_before;
  $("next").disabled = state.next === null; $("previous").disabled = !state.previous.length;
  $("no-history").hidden = page.items.length > 0;
  $("page-info").textContent = `${page.items.length} pomiarów · strona ${state.previous.length + 1}`;
  const fullRange = new URLSearchParams({...state.range, device_id: state.device});
  $("export-range").href = "/api/export-range.csv?" + fullRange;
  $("export-range").setAttribute("aria-disabled", "false");
  $("export").href = "/api/export.csv?" + query(); $("export").setAttribute("aria-disabled", "false");
  $("history-note").textContent = state.before === null ? "Widok odświeża się co 10 sekund." : "Przeglądasz historię — automatyczne odświeżanie tabeli jest wstrzymane.";
}
async function refresh(background = false) {
  if (background && state.controller) return;
  clearTimeout(state.timer); state.controller?.abort();
  const controller = new AbortController(), generation = ++state.generation;
  state.controller = controller;
  try {
    const response = await getJson("/api/devices", controller.signal);
    if (generation !== state.generation) return;
    if (!state.device && response.devices.length) state.device = response.devices[0].device_id;
    state.devices = response.devices;
    showDevices(response.devices, response.server_time_ms);
    showMetrics(response.devices.find(d => d.device_id === state.device), response.server_time_ms);
    if (state.device && !$("measurement-panel").hidden && !(background && state.before !== null)) {
      if (state.before === null) setRange(response.server_time_ms);
      const params = query();
      const [page, chart] = await Promise.all([getJson("/api/measurements?" + params, controller.signal), getJson("/api/chart?" + params, controller.signal)]);
      if (generation !== state.generation) return;
      showHistory(page);
      plot("temperature-chart", chart.items, [{key: "temperature_sht_c", min: "min_sht", max: "max_sht", color: "#c78d28", name: "Czujnik wewnętrzny"}, {key: "temperature_ds18_c", min: "min_ds", max: "max_ds", color: "#2b7057", name: "Czujnik zewnętrzny"}], "°C", 240, chart.bucket_ms);
      plot("humidity-chart", chart.items, [{key: "humidity_rh", min: "min_rh", max: "max_rh", color: "#4881ac", name: "Czujnik wewnętrzny"}], "%", 210, chart.bucket_ms);
      $("chart-note").textContent = `Średnie w przedziałach ${(chart.bucket_ms / 1000).toLocaleString("pl-PL")} s; minimum i maksimum są dostępne po wskazaniu punktu. Pomiary bez UTC pozostają w tabeli.`;
      const selected = response.devices.find(d => d.device_id === state.device);
      if (selected?.timestamp_ms != null && (selected.timestamp_ms < state.range.from_ms || selected.timestamp_ms > state.range.to_ms)) {
        $("chart-note").textContent += " Ostatnio odebrany pomiar jest poza wybranym zakresem. Zmień zakres dat, aby go zobaczyć.";
      }
    }
    $("error").hidden = true;
    $("refresh-status").textContent = "Odświeżono " + new Date().toLocaleTimeString("pl-PL");
  } catch (error) {
    if (error.name !== "AbortError" && generation === state.generation) { $("error").textContent = error.message; $("error").hidden = false; $("refresh-status").textContent = "Oczekiwanie na połączenie"; }
  } finally {
    if (generation === state.generation) { state.controller = null; state.timer = setTimeout(() => refresh(true), 10000); }
  }
}
$("rename").addEventListener("click", () => {
  const device = state.devices.find(d => d.device_id === state.device);
  if (!device || state.naming) return;
  $("name-form").dataset.targetDevice = device.device_id;
  $("name-input").value = device.name || "";
  $("name-result").textContent = "";
  $("name-form").hidden = false;
  $("name-input").focus();
});
$("name-cancel").addEventListener("click", () => { $("name-form").hidden = true; });
$("name-form").addEventListener("submit", async event => {
  event.preventDefault();
  if (state.naming) return;
  const target = $("name-form").dataset.targetDevice, name = $("name-input").value.trim();
  state.naming = true; $("name-save").disabled = true; $("rename").disabled = true;
  $("name-result").textContent = "Zapisywanie…";
  const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), 10000);
  try {
    const response = await fetch("/api/device/name", {method: "POST", signal: controller.signal,
      headers: {"Content-Type": "application/json", "X-Goldrop-Request": "1"}, body: JSON.stringify({device_id: target, name})});
    if (!response.ok) throw new Error("Nie zapisano nazwy. Sprawdź połączenie i limit 80 znaków.");
    if (state.device === target) $("name-form").hidden = true;
    await refresh();
  } catch (error) {
    $("name-result").textContent = error.name === "AbortError" ? "Upłynął czas zapisu. Odśwież panel, aby sprawdzić nazwę." : error.message;
  } finally {
    clearTimeout(timeout); state.naming = false; $("name-save").disabled = false; $("rename").disabled = !state.device;
  }
});
$("timezone").textContent = Intl.DateTimeFormat().resolvedOptions().timeZone;
window.matchMedia("(max-width:720px)").addEventListener("change", () => refresh());
$("refresh").addEventListener("click", () => refresh());
$("period").addEventListener("change", () => { $("custom-range").hidden = $("period").value !== "custom"; resetPage(); if ($("period").value !== "custom") refresh(); });
$("apply-range").addEventListener("click", () => { resetPage(); refresh(); });
$("next").addEventListener("click", () => { if (state.next !== null) { state.previous.push(state.before); state.before = state.next; refresh(); } });
$("previous").addEventListener("click", () => { if (state.previous.length) { state.before = state.previous.pop(); refresh(); } });
function localInput(time) { const d = new Date(time); return new Date(time - d.getTimezoneOffset() * 60000).toISOString().slice(0, 16); }
$("from").value = localInput(Date.now() - 86400000); $("to").value = localInput(Date.now());
refresh();

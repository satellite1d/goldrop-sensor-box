"use strict";
(() => {
  const el = id => document.getElementById(id);
  const fleet = {devices: [], releases: [], selected: new Set(), enabled: false, busy: false, active: false, loading: false, timer: null};
  const phases = {idle: "Gotowe", staged: "Przygotowane", armed: "Uzbrojone", testing: "Próba nowych ustawień"};
  const results = {none: "", committed: "Potwierdzono zapis", rolled_back: "Cofnięto zmianę", rejected: "Odrzucono zmianę", cancelled: "Anulowano", local_change: "Zmiana przez USB"};
  const actions = {apply: "zmiana Wi-Fi", stage: "przygotowanie Wi-Fi", arm: "uzbrojenie", activate: "aktywacja", cancel: "anulowanie", sampling: "interwał pomiarów", ota_check: "sprawdzenie OTA"};
  function showError(message) { el("fleet-error").textContent = message; el("fleet-error").hidden = !message; }
  function selectionChanged() {
    el("fleet-count").textContent = `Wybrano ${fleet.selected.size} z ${fleet.devices.length} płytek. Polecenie dotyczy dokładnie tego wyboru.`;
    el("fleet-actions").disabled = !fleet.enabled || fleet.busy || !fleet.selected.size;
  }
  function updateReleaseInfo() {
    const release = fleet.releases.find(r => r.id === el("fleet-release").value);
    el("fleet-release-info").textContent = release ? `${release.hardware_revision} · kanał ${release.channel} · ${(release.size_bytes / 1024).toFixed(0)} KiB` : "Brak przygotowanych wydań. Dodaj podpisany manifest wersji i obraz do katalogu OTA według instrukcji zarządzania.";
  }
  function render() {
    const focus = document.activeElement?.dataset.mac;
    const tbody = el("fleet-devices"); tbody.replaceChildren();
    for (const device of fleet.devices) {
      const row = document.createElement("tr"), checkCell = document.createElement("td"), check = document.createElement("input");
      check.type = "checkbox"; check.checked = fleet.selected.has(device.device_id); check.dataset.mac = device.device_id;
      check.setAttribute("aria-label", "Wybierz " + (device.name || device.device_id));
      check.addEventListener("change", () => { if (check.checked) fleet.selected.add(device.device_id); else fleet.selected.delete(device.device_id); selectionChanged(); });
      checkCell.append(check); row.append(checkCell);
      const name = document.createElement("td"); name.textContent = device.name || device.device_id;
      if (device.name) { const mac = document.createElement("small"); mac.textContent = device.device_id; name.append(mac); }
      const version = document.createElement("td"); version.textContent = device.firmware_version || "Oczekiwanie na wersję";
      const interval = document.createElement("small"); interval.textContent = device.interval_ms ? `Pomiar co ${device.interval_ms / 1000} s` : "Interwał: brak raportu"; version.append(interval);
      if (!device.supports_runtime) { const note = document.createElement("small"); note.textContent = "Interwał i zdalne OTA check: od 0.6.0"; version.append(note); }
      const status = document.createElement("td"), lines = [];
      if (!device.enrolled) lines.push("Brak rejestracji zarządzania");
      else if (!device.report) lines.push("Oczekiwanie na pierwszy raport zarządzania");
      else {
        lines.push(phases[device.report.phase] || device.report.phase);
        if (results[device.report.result]) lines.push(results[device.report.result]);
        if (device.report.error !== "none") lines.push("Błąd: " + device.report.error);
      }
      if (device.management_last_seen) lines.push("Raport: " + new Date(device.management_last_seen * 1000).toLocaleString("pl-PL"));
      if (device.queued_action) lines.push("W kolejce: " + (actions[device.queued_action] || device.queued_action));
      if (device.ota_manifest) {
        const wanted = device.ota_manifest.replace(/^manifest-/, "").replace(/\.json$/, "");
        lines.push(device.firmware_version === wanted ? "Zainstalowano wybrane wydanie " + wanted : "Oczekiwanie na wydanie " + wanted);
      }
      if (device.ota_state) lines.push(`OTA: ${device.ota_state} · ${device.ota_progress || 0}%${device.ota_error && device.ota_error !== "none" ? " · " + device.ota_error : ""}`);
      for (const text of lines) { const line = document.createElement("small"); line.textContent = text; status.append(line); }
      row.append(name, version, status); tbody.append(row);
    }
    if (focus) tbody.querySelector(`input[data-mac="${focus}"]`)?.focus();
    const releaseKey = JSON.stringify(fleet.releases);
    if (el("fleet-release").dataset.releases !== releaseKey) {
    const previous = el("fleet-release").value;
    el("fleet-release").replaceChildren();
    for (const release of fleet.releases) {
      const option = document.createElement("option"); option.value = release.id;
      option.textContent = `${release.version} — ${release.hardware_revision} (${release.channel})`; el("fleet-release").append(option);
    }
    if (fleet.releases.some(r => r.id === previous)) el("fleet-release").value = previous;
    el("fleet-release").dataset.releases = releaseKey;
    el("fleet-ota-confirm").checked = false;
    }
    updateReleaseInfo(); selectionChanged();
  }
  async function refreshFleet() {
    if (fleet.loading) return;
    clearTimeout(fleet.timer); fleet.loading = true;
    const controller = new AbortController(), deadline = setTimeout(() => controller.abort(), 10000);
    try {
      const response = await fetch("/api/management", {cache: "no-store", signal: controller.signal});
      if (!response.ok) throw new Error("Nie udało się pobrać statusu zarządzania.");
      const data = await response.json(); fleet.enabled = data.enabled;
      fleet.devices = data.devices; fleet.releases = data.releases;
      const known = new Set(fleet.devices.map(d => d.device_id));
      for (const mac of fleet.selected) if (!known.has(mac)) fleet.selected.delete(mac);
      el("fleet-disabled").hidden = fleet.enabled; render();
    } catch (error) { showError("Nie udało się odświeżyć statusu. Sprawdź połączenie z serwerem."); }
    finally { clearTimeout(deadline); fleet.loading = false; if (fleet.active) fleet.timer = setTimeout(refreshFleet, 5000); }
  }
  async function command(action, fields = {}) {
    if (fleet.busy) return;
    showError("");
    const devices = [...fleet.selected], selected = fleet.devices.filter(d => fleet.selected.has(d.device_id));
    if (!devices.length) { showError("Najpierw zaznacz płytki."); return; }
    if (action !== "ota_check" && selected.some(d => !d.enrolled || !d.report)) { showError("Wybrane płytki muszą mieć rejestrację i raport zarządzania."); return; }
    if (action === "sampling" && selected.some(d => !d.supports_runtime)) { showError("Najpierw zaktualizuj wybrane płytki do 0.6.0 lub nowszej wersji."); return; }
    if (["stage", "apply", "sampling", "ota_check"].includes(action) && selected.some(d => d.report && (d.report.phase !== "idle" || d.queued_action))) { showError("Najpierw zakończ lub anuluj oczekujące operacje wybranych płytek."); return; }
    fleet.busy = true; selectionChanged();
    const controller = new AbortController(), deadline = setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch("/api/management/command", {method: "POST", signal: controller.signal,
        headers: {"Content-Type": "application/json", "X-Goldrop-Request": "1"},
        body: JSON.stringify({action, devices, ...fields})});
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || "Nie udało się zlecić polecenia.");
      const automatic = result.results.filter(r => r.mode === "automatic_poll").length;
      const current = result.results.filter(r => r.mode === "already_current").length;
      el("fleet-result").textContent = `Zlecono: ${actions[action]}. Liczba urządzeń: ${devices.length}.` +
        (automatic ? ` ${automatic} starszych płytek oczekuje na automatyczne sprawdzenie OTA.` : "") +
        (current ? ` ${current} płytek ma już wybraną wersję.` : "") + " Wynik sprawdzaj w tabeli — samo zlecenie nie jest potwierdzeniem wykonania.";
      el("fleet-ota-confirm").checked = false;
      await refreshFleet();
    } catch (error) { showError(error.name === "AbortError" ? "Upłynął czas odpowiedzi. Sprawdź tabelę przed ponownym wysłaniem polecenia." : error.message); }
    finally { clearTimeout(deadline); fleet.busy = false; selectionChanged(); el("fleet-password").value = ""; }
  }
  function tab(management) {
    fleet.active = management;
    el("measurement-panel").hidden = management; el("management-panel").hidden = !management;
    el("tab-measurements").setAttribute("aria-selected", String(!management));
    el("tab-management").setAttribute("aria-selected", String(management));
    if (management) refreshFleet(); else { clearTimeout(fleet.timer); el("fleet-password").value = ""; refresh(); }
  }
  el("tab-management").addEventListener("click", () => tab(true));
  el("tab-measurements").addEventListener("click", () => tab(false));
  el("device-list").addEventListener("click", event => { if (event.target.closest("button.device") && fleet.active) tab(false); });
  el("fleet-refresh").addEventListener("click", refreshFleet);
  el("fleet-all").addEventListener("click", () => { fleet.selected = new Set(fleet.devices.map(d => d.device_id)); render(); });
  el("fleet-none").addEventListener("click", () => { fleet.selected.clear(); render(); });
  el("fleet-release").addEventListener("change", () => { el("fleet-ota-confirm").checked = false; updateReleaseInfo(); });
  el("fleet-ota").addEventListener("submit", event => { event.preventDefault(); command("ota_check", {release: el("fleet-release").value, confirm_buffer_loss: el("fleet-ota-confirm").checked}); });
  el("fleet-wifi").addEventListener("submit", event => { event.preventDefault(); const fields = {ssid: el("fleet-ssid").value, password: el("fleet-password").value}; command(el("fleet-wifi-mode").value, fields); });
  el("fleet-sampling").addEventListener("submit", event => { event.preventDefault(); command("sampling", {interval_seconds: Number(el("fleet-interval").value)}); });
  for (const action of ["activate", "arm", "cancel"]) el("fleet-" + action).addEventListener("click", () => command(action));
})();

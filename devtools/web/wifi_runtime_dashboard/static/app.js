"use strict";

const csrfToken = document.querySelector('meta[name="meiso-csrf"]').content;
const fieldNames = new Map([
  [1, "link"], [2, "signal"], [4, "rx bytes"], [8, "tx bytes"],
  [16, "rx packets"], [32, "tx packets"], [64, "rx dropped"],
  [128, "tx dropped"], [256, "rx errors"], [512, "tx errors"],
]);
const counterNodes = {
  rx_bytes: ["rx-bytes", "rx-bytes-delta"],
  tx_bytes: ["tx-bytes", "tx-bytes-delta"],
  rx_packets: ["rx-packets", "rx-packets-delta"],
  tx_packets: ["tx-packets", "tx-packets-delta"],
  rx_dropped: ["rx-dropped", "rx-dropped-delta"],
  tx_dropped: ["tx-dropped", "tx-dropped-delta"],
  rx_errors: ["rx-errors", "rx-errors-delta"],
  tx_errors: ["tx-errors", "tx-errors-delta"],
};
const fieldInputs = Array.from(document.querySelectorAll("[data-field-bit]"));

let stream = null;
let eventCount = 0;
let hostConnected = false;
let controlReady = false;
let closing = false;
let commandPending = false;
let catalogCapability = null;
let lastDesired = null;
let lastActual = null;
let activeActual = null;
let sampleTotal = 0;
let lastArrivalMs = null;
let lastArrivalDeltaMs = null;
let previousCounters = {};
let signalHistory = [];
let sampleHistory = [];

function node(id) { return document.getElementById(id); }
function text(id, value) {
  node(id).textContent = value === null || value === undefined ? "—" : String(value);
}
function bool(value) { return value === true ? "yes" : value === false ? "no" : "—"; }
function period(value) { return value === null || value === undefined ? "—" : `${value} ms`; }
function number(value) { return value === null || value === undefined ? "— unavailable" : Number(value).toLocaleString(); }
function mask(value) { return value === null || value === undefined ? "—" : `0x${Number(value).toString(16).padStart(3, "0")}`; }

function fields(value) {
  if (value === null || value === undefined) return "—";
  const selected = [];
  for (const [bit, name] of fieldNames) if ((value & bit) !== 0) selected.push(name);
  return `${selected.join(", ") || "none"} · ${mask(value)}`;
}

function badge(id, value) {
  const element = node(id);
  if (id === "live-status") {
    const dot = document.createElement("i");
    element.replaceChildren(dot, document.createTextNode(` ${String(value || "UNKNOWN")}`));
  } else {
    element.textContent = String(value || "UNKNOWN");
  }
  let tone = "neutral";
  if (["CONNECTED", "READY", "ACCEPTED", "RUNNING", "APPLIED", "MATCHED", "OPEN", "COMPLETE"].includes(value)) tone = "pass";
  if (["CONNECTING", "PUBLISHED", "STARTING", "PENDING", "DEGRADED", "STOPPING", "CLOSING"].includes(value)) tone = "warning";
  if (["FAILED", "REJECTED", "MISMATCH"].includes(value)) tone = "fail";
  element.className = `badge ${tone}`;
}

function appendLog(event) {
  const row = document.createElement("div");
  const details = [event.type, event.command, event.status];
  for (const key of ["version", "desired_version", "state_version", "ordinal", "sequence", "stage", "reason"]) {
    if (event[key] !== undefined) details.push(`${key}=${event[key]}`);
  }
  row.textContent = details.filter(Boolean).join(" · ");
  row.className = `event event-${event.type}`;
  node("event-log").append(row);
  while (node("event-log").childElementCount > 160) node("event-log").firstElementChild.remove();
  node("event-log").scrollTop = node("event-log").scrollHeight;
  eventCount += 1;
  text("event-count", eventCount);
}

function updateControls() {
  node("connect").disabled = hostConnected || closing;
  node("apply").disabled = !controlReady || closing || commandPending;
  node("stop").disabled = !controlReady || closing || commandPending || !lastDesired || !lastDesired.enabled;
  node("close").disabled = !hostConnected || closing;
  node("field-set").disabled = !controlReady || closing;
  node("period-range").disabled = !controlReady || closing;
  node("period-number").disabled = !controlReady || closing;
}

function configureCapability(capability) {
  catalogCapability = {...capability};
  fieldInputs.forEach((input) => {
    const supported = (capability.supported_fields & Number(input.dataset.fieldBit)) !== 0;
    input.disabled = !supported;
    input.closest("label").classList.toggle("unsupported", !supported);
    if (!supported) input.checked = false;
  });
  for (const id of ["period-range", "period-number"]) {
    const input = node(id);
    input.min = String(capability.min_period_ms);
    input.max = String(capability.max_period_ms);
    input.step = String(capability.period_step_ms);
  }
  const current = Number(node("period-number").value);
  const snapped = snapPeriod(current);
  node("period-range").value = String(snapped);
  node("period-number").value = String(snapped);
  text("period-envelope", `${capability.min_period_ms}–${capability.max_period_ms} ms · step ${capability.period_step_ms}`);
}

function configureDesiredInputs(desired) {
  fieldInputs.forEach((input) => {
    if (!input.disabled) {
      input.checked = (desired.selected_fields & Number(input.dataset.fieldBit)) !== 0;
    }
  });
  const desiredPeriod = snapPeriod(desired.sample_period_ms);
  node("period-range").value = String(desiredPeriod);
  node("period-number").value = String(desiredPeriod);
}

function snapPeriod(value) {
  if (!catalogCapability) return value;
  const {min_period_ms: min, max_period_ms: max, period_step_ms: step} = catalogCapability;
  const bounded = Math.min(max, Math.max(min, Number.isFinite(value) ? value : min));
  return Math.min(max, min + Math.round((bounded - min) / step) * step);
}

function selectedFields() {
  return fieldInputs.reduce((value, input) => {
    return value | (input.checked && !input.disabled ? Number(input.dataset.fieldBit) : 0);
  }, 0);
}

function updateIntentMatch() {
  if (!lastDesired || !lastActual) {
    badge("intent-match", "NO ACTUAL");
    return;
  }
  const matches = lastDesired.enabled === lastActual.effective_enabled
    && lastDesired.version === lastActual.observed_desired_version
    && lastActual.apply_result === "APPLIED"
    && (!lastDesired.enabled || (
      lastDesired.selected_fields === lastActual.selected_fields
      && lastDesired.sample_period_ms === lastActual.sample_period_ms
      && lastActual.data_active
    ));
  badge("intent-match", matches ? "MATCHED" : "PENDING");
}

function desiredStillPending() {
  return lastDesired !== null && (
    lastActual === null
    || lastDesired.version !== lastActual.observed_desired_version
  );
}

function resetTelemetry() {
  sampleTotal = 0;
  lastArrivalMs = null;
  lastArrivalDeltaMs = null;
  previousCounters = {};
  signalHistory = [];
  sampleHistory = [];
  text("sample-total", 0);
  for (const id of ["sample-sequence", "sample-cadence", "source-time", "last-arrival", "sample-gap", "sample-link", "sample-signal", "sample-selected", "sample-present"]) text(id, "—");
  for (const [valueId, deltaId] of Object.values(counterNodes)) {
    text(valueId, "—");
    text(deltaId, "—");
  }
  node("sample-history").innerHTML = '<tr class="empty-row"><td colspan="5">等待数据</td></tr>';
  text("history-count", 0);
  text("chart-range", "等待 signal 样本");
  text("signal-now", "— dBm");
  badge("live-status", "STREAM IDLE");
  drawSignal();
}

function resetConnectionView(hostRunning = true, closePending = false) {
  hostConnected = hostRunning;
  controlReady = false;
  closing = closePending;
  commandPending = false;
  catalogCapability = null;
  lastDesired = null;
  lastActual = null;
  activeActual = null;
  eventCount = 0;
  node("event-log").replaceChildren();
  text("event-count", 0);
  for (const id of ["session-status", "catalog-status", "desired-status", "actual-status", "gate-status"]) badge(id, "WAITING");
  for (const id of ["bootstrap-status", "identity-match", "session-limits", "session-close", "catalog-generation", "catalog-shape", "capability-fields", "capability-period", "desired-version", "desired-enabled", "desired-fields", "desired-period", "desired-delivery", "actual-version", "actual-apply", "actual-enabled", "actual-fields", "actual-period"]) text(id, "—");
  text("gate-copy", "等待 Edge actual State。");
  text("sample-context", "等待 active actual effective 参数。");
  badge("host-status", hostRunning ? "CONNECTING" : "DISCONNECTED");
  badge("intent-match", "NO ACTUAL");
  resetTelemetry();
  updateControls();
}

function formatSourceTime(sourceTimeNs) {
  if (sourceTimeNs === null || sourceTimeNs === undefined) return "— unavailable";
  return `${(Number(sourceTimeNs) / 1e9).toFixed(3)} s`;
}

function addHistory(event, arrivalDelta) {
  sampleHistory.unshift({
    sequence: event.sequence,
    source: formatSourceTime(event.source_time_ns),
    arrival: arrivalDelta === null ? "first" : `${Math.round(arrivalDelta)} ms`,
    signal: event.signal_mbm === null ? "—" : `${(event.signal_mbm / 100).toFixed(1)} dBm`,
    link: event.link_state === null ? "—" : (event.link_state ? "up" : "down"),
  });
  sampleHistory = sampleHistory.slice(0, 12);
  const body = node("sample-history");
  body.replaceChildren();
  for (const item of sampleHistory) {
    const row = document.createElement("tr");
    for (const value of [item.sequence, item.source, item.arrival, item.signal, item.link]) {
      const cell = document.createElement("td");
      cell.textContent = String(value);
      row.append(cell);
    }
    body.append(row);
  }
  text("history-count", sampleHistory.length);
}

function drawSignal() {
  const canvas = node("signal-chart");
  const bounds = canvas.getBoundingClientRect();
  const width = Math.max(320, Math.round(bounds.width || 900));
  const height = Math.max(180, Math.round(bounds.height || 250));
  const ratio = Math.min(2, window.devicePixelRatio || 1);
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "rgba(147,161,178,.16)";
  context.lineWidth = 1;
  for (let line = 1; line < 4; line += 1) {
    const y = (height / 4) * line;
    context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
  }
  if (signalHistory.length === 0) return;
  const values = signalHistory.map((item) => item.value);
  const low = Math.min(...values) - 3;
  const high = Math.max(...values) + 3;
  const span = Math.max(10, high - low);
  const x = (index) => signalHistory.length === 1 ? width : index * width / (signalHistory.length - 1);
  const y = (value) => 14 + (high - value) / span * (height - 28);
  const gradient = context.createLinearGradient(0, 0, width, 0);
  gradient.addColorStop(0, "#75a9ff");
  gradient.addColorStop(1, "#57d9d1");
  context.strokeStyle = gradient;
  context.lineWidth = 2.5;
  context.beginPath();
  signalHistory.forEach((item, index) => index === 0 ? context.moveTo(x(index), y(item.value)) : context.lineTo(x(index), y(item.value)));
  context.stroke();
  const latest = signalHistory[signalHistory.length - 1];
  context.fillStyle = "#72df9c";
  context.beginPath(); context.arc(x(signalHistory.length - 1), y(latest.value), 4, 0, Math.PI * 2); context.fill();
  text("chart-range", `${Math.min(...values).toFixed(1)}…${Math.max(...values).toFixed(1)} dBm · ${values.length} samples`);
}

function handleSample(event) {
  const now = Date.now();
  const arrivalDelta = lastArrivalMs === null ? null : now - lastArrivalMs;
  lastArrivalMs = now;
  lastArrivalDeltaMs = arrivalDelta;
  sampleTotal += 1;
  text("sample-total", sampleTotal);
  text("sample-sequence", `${event.sequence} · ordinal ${event.ordinal}`);
  text("sample-cadence", arrivalDelta === null ? "first sample" : `${Math.round(arrivalDelta)} ms`);
  text("source-time", formatSourceTime(event.source_time_ns));
  text("last-arrival", "now");
  text("sample-gap", event.gap ? `yes · missing ${event.missing_before}` : "no");
  text("sample-link", event.link_state === null ? "— unavailable" : (event.link_state ? "up" : "down"));
  text("sample-signal", event.signal_mbm === null ? "— unavailable" : `${(event.signal_mbm / 100).toFixed(2)} dBm`);
  text("sample-selected", mask(event.selected_fields));
  text("sample-present", mask(event.present_fields));

  for (const [key, [valueId, deltaId]] of Object.entries(counterNodes)) {
    const current = event[key];
    text(valueId, number(current));
    if (current === null || current === undefined) {
      text(deltaId, "not present");
      delete previousCounters[key];
      continue;
    }
    const previous = previousCounters[key];
    text(deltaId, previous === undefined ? "baseline" : (current >= previous ? `+${Number(current - previous).toLocaleString()}` : "counter reset"));
    previousCounters[key] = current;
  }

  if (event.signal_mbm !== null) {
    const dbm = event.signal_mbm / 100;
    signalHistory.push({value: dbm, sequence: event.sequence});
    signalHistory = signalHistory.slice(-80);
    text("signal-now", `${dbm.toFixed(1)} dBm`);
    drawSignal();
  }
  addHistory(event, arrivalDelta);
  if (activeActual) {
    const matches = activeActual.selected_fields === event.selected_fields;
    text("sample-context", `actual ${mask(activeActual.selected_fields)} · ${period(activeActual.sample_period_ms)} · sample ${matches ? "matches" : "MISMATCH"}`);
  } else {
    text("sample-context", "收到样本，但尚未观察到 active actual。");
  }
  badge("live-status", "STREAM LIVE");
}

function handleDashboardReset(event) {
  resetConnectionView(
    event.host_running === true,
    event.close_pending === true,
  );
  appendLog(event);
  text("command-status", event.host_running
    ? "Host state baseline reset; waiting for sanitized lifecycle state."
    : "No native Host is connected.");
  updateControls();
}

function handleDashboardSnapshot(event) {
  resetConnectionView(
    event.host_running === true,
    event.close_pending === true,
  );
  appendLog(event);
  if (Array.isArray(event.events)) {
    for (const sticky of event.events) handle(sticky);
  }
  if (activeActual && activeActual.data_active) {
    badge("live-status", "STREAM WAITING");
    text("sample-context", `restored active actual ${mask(activeActual.selected_fields)} · ${period(activeActual.sample_period_ms)} · next new sample establishes LIVE`);
  }
  commandPending = desiredStillPending();
  if (controlReady && lastDesired && lastActual) {
    const applied = lastDesired.version === lastActual.observed_desired_version
      && lastActual.apply_result === "APPLIED";
    text("command-status", applied
      ? `Restored desired v${lastDesired.version} and its observed actual.`
      : `Restored desired v${lastDesired.version}; waiting for matching Edge actual.`);
  } else if (event.host_running === true && controlReady) {
    text("command-status", "Interactive Host restored; choose fields and start.");
  }
  updateControls();
}

function handle(event) {
  if (event.type === "dashboard_reset") {
    handleDashboardReset(event);
    return;
  }
  if (event.type === "dashboard_snapshot") {
    handleDashboardSnapshot(event);
    return;
  }
  appendLog(event);
  if (event.type === "bootstrap") {
    hostConnected = true;
    text("bootstrap-status", "connected · locator redacted");
    badge("host-status", "CONNECTED");
  } else if (event.type === "session") {
    badge("session-status", event.status);
    if (event.status === "READY") {
      hostConnected = true;
      text("identity-match", event.identity_match ? "matched · value redacted" : "not matched");
      text("session-limits", `${event.limits.max_frame_bytes} / ${event.limits.max_runtime_payload_bytes}`);
    } else {
      text("session-close", event.reason);
      badge("host-status", "CLOSING");
    }
  } else if (event.type === "catalog") {
    badge("catalog-status", event.status);
    text("catalog-generation", event.generation);
    text("catalog-shape", `${event.endpoint_count} / ${event.route_count}`);
    text("capability-fields", fields(event.capability.supported_fields));
    text("capability-period", `${event.capability.min_period_ms}–${event.capability.max_period_ms} ms · step ${event.capability.period_step_ms}`);
    configureCapability(event.capability);
  } else if (event.type === "desired") {
    lastDesired = {...event};
    configureDesiredInputs(event);
    badge("desired-status", event.status);
    text("desired-version", event.version);
    text("desired-enabled", bool(event.enabled));
    text("desired-fields", fields(event.selected_fields));
    text("desired-period", period(event.sample_period_ms));
    text("desired-delivery", "pending");
    text("command-status", `desired v${event.version} published; waiting for Edge actual.`);
    updateIntentMatch();
  } else if (event.type === "desired_delivery") {
    text("desired-delivery", `accepted v${event.version}`);
  } else if (event.type === "actual") {
    const productionChanged = event.data_active && (
      activeActual === null
      || activeActual.state_version !== event.state_version
      || activeActual.selected_fields !== event.selected_fields
      || activeActual.sample_period_ms !== event.sample_period_ms
    );
    lastActual = {...event};
    activeActual = event.data_active ? {...event} : null;
    if (productionChanged) {
      lastArrivalMs = null;
      lastArrivalDeltaMs = null;
      previousCounters = {};
    }
    badge("actual-status", event.status);
    text("actual-version", event.state_version);
    text("actual-apply", event.apply_result);
    text("actual-enabled", bool(event.effective_enabled));
    text("actual-fields", fields(event.selected_fields));
    text("actual-period", period(event.sample_period_ms));
    badge("gate-status", event.data_active ? "OPEN" : "CLOSED");
    text("gate-copy", event.data_active
      ? `Stream accepts produced samples (${event.status}).`
      : `Data production is stopped; Session remains available (${event.status}).`);
    if (!event.data_active) {
      badge("live-status", "STREAM IDLE");
      text("sample-context", "Production stopped; latest sample is retained for inspection.");
    } else {
      text("sample-context", `active actual ${mask(event.selected_fields)} · ${period(event.sample_period_ms)} · waiting for this production run's first sample`);
    }
    updateIntentMatch();
  } else if (event.type === "sample") {
    handleSample(event);
  } else if (event.type === "control") {
    if (event.status === "READY") {
      controlReady = true;
      text("command-status", "Host interactive control ready. Choose fields and start.");
    } else if (event.status === "APPLIED") {
      commandPending = false;
      text("command-status", `${event.enabled ? "Start / reconfigure" : "Stop"} applied as desired v${event.desired_version}.`);
    } else if (event.status === "REJECTED") {
      commandPending = false;
      text("request-error", `Host rejected control: ${event.reason}`);
      badge("intent-match", "REJECTED");
    } else if (event.command === "CLOSE" && event.status === "COMPLETE") {
      hostConnected = false;
      controlReady = false;
      closing = false;
      commandPending = false;
      lastDesired = null;
      badge("host-status", "COMPLETE");
      badge("live-status", "STREAM IDLE");
      text("command-status", "Session closed normally; reconnect requires an Edge listener.");
    }
  } else if (event.type === "run") {
    text("command-status", `Compatibility run complete: ${event.received_samples}/${event.requested_samples}.`);
  } else if (event.type === "failure") {
    hostConnected = false;
    controlReady = false;
    closing = false;
    commandPending = false;
    badge("host-status", "FAILED");
    badge("live-status", "STREAM IDLE");
    text("request-error", `Host failure: ${event.stage}`);
  }
  updateControls();
}

function connectEvents() {
  if (stream) stream.close();
  stream = new EventSource("/api/v1/events");
  stream.addEventListener("host", (message) => {
    try { handle(JSON.parse(message.data)); }
    catch (_error) { text("request-error", "invalid sanitized event"); }
  });
  stream.onerror = () => text("request-error", "event stream reconnecting…");
  stream.onopen = () => {
    if (node("request-error").textContent === "event stream reconnecting…") text("request-error", "");
  };
}

async function command(path, body = null) {
  const headers = {"X-Meiso-CSRF": csrfToken};
  const options = {method: "POST", headers, credentials: "same-origin", cache: "no-store"};
  if (body !== null) {
    headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  if (!response.ok) {
    let detail = `HTTP ${response.status}`;
    try { detail = (await response.json()).error || detail; } catch (_error) { /* fixed generic */ }
    throw new Error(detail);
  }
}

node("connect").addEventListener("click", async () => {
  node("connect").disabled = true;
  text("request-error", "");
  resetConnectionView();
  text("command-status", "Starting native Host; waiting for Session and catalog.");
  try {
    await command("/api/v1/connect");
  } catch (error) {
    hostConnected = false;
    text("request-error", `connect failed: ${error.message}`);
    connectEvents();
    updateControls();
  }
});

node("apply").addEventListener("click", async () => {
  const selected = selectedFields();
  const requestedPeriod = snapPeriod(Number(node("period-number").value));
  node("period-number").value = String(requestedPeriod);
  node("period-range").value = String(requestedPeriod);
  if (selected === 0) {
    text("request-error", "至少选择一个 sensor field。");
    return;
  }
  text("request-error", "");
  commandPending = true;
  node("apply").disabled = true;
  try {
    await command("/api/v1/apply", {selected_fields: selected, sample_period_ms: requestedPeriod});
    text("command-status", `Control accepted: ${mask(selected)}, ${requestedPeriod} ms; waiting for desired / actual.`);
    badge("intent-match", "PENDING");
  } catch (error) {
    commandPending = false;
    text("request-error", `apply failed: ${error.message}`);
  } finally {
    updateControls();
  }
});

node("stop").addEventListener("click", async () => {
  text("request-error", "");
  commandPending = true;
  node("stop").disabled = true;
  try {
    await command("/api/v1/stop");
    text("command-status", "Stop accepted; waiting for desired=false and actual STOPPED.");
    badge("intent-match", "PENDING");
  } catch (error) {
    commandPending = false;
    text("request-error", `stop failed: ${error.message}`);
  } finally {
    updateControls();
  }
});

node("close").addEventListener("click", async () => {
  text("request-error", "");
  closing = true;
  badge("host-status", "CLOSING");
  updateControls();
  try {
    await command("/api/v1/close");
    text("command-status", "Normal close accepted; Host will stop production first if needed.");
  } catch (error) {
    closing = false;
    text("request-error", `close failed: ${error.message}`);
    updateControls();
  }
});

node("period-range").addEventListener("input", () => {
  node("period-number").value = node("period-range").value;
});
node("period-number").addEventListener("change", () => {
  const value = snapPeriod(Number(node("period-number").value));
  node("period-number").value = String(value);
  node("period-range").value = String(value);
});

setInterval(() => {
  if (lastArrivalMs === null) return;
  const age = Date.now() - lastArrivalMs;
  text("last-arrival", age < 1000 ? `${age} ms ago` : `${(age / 1000).toFixed(1)} s ago`);
  const expected = activeActual ? activeActual.sample_period_ms : null;
  const stale = expected !== null && age > Math.max(expected * 2.5, 1500);
  if (stale) badge("live-status", "STREAM STALE");
}, 250);

if ("ResizeObserver" in window) new ResizeObserver(drawSignal).observe(node("signal-chart"));
resetTelemetry();
updateControls();
connectEvents();

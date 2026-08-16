"use strict";

const csrfToken = document.querySelector('meta[name="meiso-csrf"]').content;
const counterIds = {
  rx_bytes: "rx-bytes",
  tx_bytes: "tx-bytes",
  rx_packets: "rx-packets",
  tx_packets: "tx-packets",
  rx_dropped: "rx-dropped",
  tx_dropped: "tx-dropped",
  rx_errors: "rx-errors",
  tx_errors: "tx-errors",
};
let stream = null;
let eventCount = 0;
let activeActual = null;
let terminalFailed = false;

function node(id) { return document.getElementById(id); }
function text(id, value) { node(id).textContent = value === null || value === undefined ? "—" : String(value); }
function bool(value) { return value === true ? "yes" : value === false ? "no" : "—"; }
function mask(value) { return value === null || value === undefined ? "—" : `0x${Number(value).toString(16).padStart(3, "0")}`; }
function period(value) { return value === null || value === undefined ? "—" : `${value} ms`; }
function available(value) { return value === null || value === undefined ? "— unavailable" : Number(value).toLocaleString(); }

function badge(id, value) {
  const element = node(id);
  element.textContent = String(value || "UNKNOWN");
  let tone = "neutral";
  if (["CONNECTED", "READY", "ACCEPTED", "RUNNING", "COMPLETE", "OPEN"].includes(value)) tone = "pass";
  if (["STARTING", "DEGRADED", "STOPPING"].includes(value)) tone = "warning";
  if (["FAILED", "REJECTED"].includes(value)) tone = "fail";
  element.className = `badge ${tone}`;
}

function appendLog(event) {
  const row = document.createElement("div");
  const details = [event.type, event.status];
  for (const key of ["version", "state_version", "ordinal", "sequence", "stage"]) {
    if (event[key] !== undefined) details.push(`${key}=${event[key]}`);
  }
  row.textContent = details.filter(Boolean).join(" · ");
  row.className = `event event-${event.type}`;
  node("event-log").append(row);
  while (node("event-log").childElementCount > 128) node("event-log").firstElementChild.remove();
  node("event-log").scrollTop = node("event-log").scrollHeight;
  eventCount += 1;
  text("event-count", eventCount);
}

function handle(event) {
  appendLog(event);
  if (event.type === "bootstrap") {
    text("bootstrap-status", "connected · locator redacted");
    badge("run-status", "RUNNING");
  } else if (event.type === "session") {
    badge("session-status", event.status);
    if (event.status === "READY") {
      text("identity-match", event.identity_match ? "matched · value redacted" : "not matched");
      text("session-limits", `${event.limits.max_frame_bytes} / ${event.limits.max_runtime_payload_bytes}`);
    } else {
      text("session-close", event.reason);
    }
  } else if (event.type === "catalog") {
    badge("catalog-status", event.status);
    text("catalog-generation", event.generation);
    text("catalog-shape", `${event.endpoint_count} / ${event.route_count}`);
    text("capability-fields", mask(event.capability.supported_fields));
    text("capability-period", `${event.capability.min_period_ms}–${event.capability.max_period_ms} ms · step ${event.capability.period_step_ms}`);
  } else if (event.type === "desired") {
    badge("desired-status", event.status);
    text("desired-version", event.version);
    text("desired-enabled", bool(event.enabled));
    text("desired-fields", mask(event.selected_fields));
    text("desired-period", period(event.sample_period_ms));
    text("desired-delivery", "pending");
  } else if (event.type === "desired_delivery") {
    text("desired-delivery", `accepted v${event.version}`);
  } else if (event.type === "actual") {
    badge("actual-status", event.status);
    text("actual-version", event.state_version);
    text("actual-apply", event.apply_result);
    text("actual-enabled", bool(event.effective_enabled));
    text("actual-fields", mask(event.selected_fields));
    text("actual-period", period(event.sample_period_ms));
    if (event.data_active) activeActual = {...event};
    badge("gate-status", event.data_active ? "OPEN" : "CLOSED");
    text("gate-copy", event.data_active
      ? `Stream accepts samples under production epoch (${event.status}).`
      : `Stream gate is closed (${event.status}).`);
  } else if (event.type === "sample") {
    text("sample-sequence", `${event.sequence} · ordinal ${event.ordinal}`);
    text("sample-gap", event.gap ? `yes · missing ${event.missing_before}` : "no");
    text("sample-link", event.link_state === null ? "— unavailable" : (event.link_state ? "up" : "down"));
    text("sample-signal", event.signal_mbm === null ? "— unavailable" : `${(event.signal_mbm / 100).toFixed(2)} dBm`);
    Object.entries(counterIds).forEach(([key, id]) => text(id, available(event[key])));
    if (activeActual) {
      const matches = activeActual.selected_fields === event.selected_fields;
      text("sample-context", `Captured under active actual: fields ${mask(activeActual.selected_fields)}, period ${period(activeActual.sample_period_ms)} · ${matches ? "match" : "MISMATCH"}`);
    } else {
      text("sample-context", "No active actual observed before this sample.");
    }
  } else if (event.type === "run") {
    if (terminalFailed) return;
    badge("run-status", "COMPLETE");
    badge("result-status", "COMPLETE");
    text("run-requested", event.requested_samples);
    text("run-received", event.received_samples);
  } else if (event.type === "failure") {
    terminalFailed = true;
    badge("run-status", "FAILED");
    badge("result-status", "FAILED");
    text("failure-stage", event.stage);
  }
}

function connect() {
  if (stream) stream.close();
  stream = new EventSource("/api/v1/events");
  stream.addEventListener("host", (message) => {
    try { handle(JSON.parse(message.data)); }
    catch (_error) { text("request-error", "invalid sanitized event"); }
  });
  stream.onerror = () => text("request-error", "event stream reconnecting…");
  stream.onopen = () => text("request-error", "");
}

function reset() {
  activeActual = null;
  terminalFailed = false;
  eventCount = 0;
  node("event-log").replaceChildren();
  text("event-count", 0);
  badge("run-status", "STARTING");
  badge("result-status", "WAITING");
  text("failure-stage", "—");
}

node("start").addEventListener("click", async () => {
  node("start").disabled = true;
  if (stream) stream.close();
  try {
    const response = await fetch("/api/v1/start", {
      method: "POST",
      headers: {"X-Meiso-CSRF": csrfToken},
      credentials: "same-origin",
      cache: "no-store",
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    reset();
    connect();
  } catch (error) {
    text("request-error", `start failed: ${error.message}`);
    connect();
  } finally {
    node("start").disabled = false;
  }
});

connect();

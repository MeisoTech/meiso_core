"use strict";

const csrfMeta = document.querySelector('meta[name="meiso-csrf"]');
const csrfToken = csrfMeta ? csrfMeta.content : "";
let eventCursor = 0;
let renderedEvents = 0;
let requestPending = false;
let refreshPending = false;

function byId(id) {
  return document.getElementById(id);
}

function text(id, value) {
  const node = byId(id);
  if (node) {
    node.textContent = value === null || value === undefined ? "—" : String(value);
  }
}

function yesNo(value) {
  return value ? "yes" : "no";
}

function bytes(value) {
  if (value === null || value === undefined) {
    return "—";
  }
  return `${Number(value).toLocaleString()} B`;
}

function temperature(millic) {
  if (millic === null || millic === undefined) {
    return "—";
  }
  return `${(Number(millic) / 1000).toFixed(3)} °C corrected`;
}

function uncertainty(millic) {
  if (millic === null || millic === undefined) {
    return "—";
  }
  return `±${(Number(millic) / 1000).toFixed(3)} °C untrimmed`;
}

function badge(id, value) {
  const node = byId(id);
  if (!node) {
    return;
  }
  const display = String(value || "UNKNOWN").replaceAll("_", " ");
  node.textContent = display;
  let tone = "neutral";
  if (value === "PASS" || value === "READY" || value === "ACCEPTED" || value === "PUBLISHED") {
    tone = "pass";
  } else if (value === "FAIL" || value === "FAILED" || value === "REJECTED") {
    tone = "fail";
  } else if (value === "RUNNING" || value === "CLOSING" || value === "INCOMPLETE") {
    tone = "warning";
  }
  node.className = `badge ${tone}`;
}

function setControls(snapshot) {
  const disabled = requestPending || snapshot.probe.running;
  document.querySelectorAll("button[data-test]").forEach((button) => {
    button.disabled = disabled;
  });
}

function renderSnapshot(snapshot) {
  badge("overall-status", snapshot.overall);
  badge("session-state", snapshot.session.state);
  text("session-failure", snapshot.session.failure);
  text("identity-known", snapshot.session.remote_identity_known ? "known (redacted)" : "unknown");
  text("identity-match", snapshot.session.identity_match ? "matched" : "not matched");
  text("session-id-known", snapshot.session.session_id_known ? "known (redacted)" : "unknown");
  text("protocol-profile", snapshot.session.protocol_profile);

  text("limit-frame", bytes(snapshot.session.limits.max_frame_bytes));
  text("limit-runtime", bytes(snapshot.session.limits.max_runtime_payload_bytes));
  text("limit-registry", snapshot.session.limits.max_registry_entries);

  badge("registry-state", snapshot.registry.state);
  text("registry-entries", snapshot.registry.entries);
  text("registry-capacity", snapshot.registry.capacity);
  text("registry-source", snapshot.registry.source);
  text("registry-notice", snapshot.registry.notice);

  const state = snapshot.semantics.state;
  badge("state-status", state.status);
  text("state-version", state.version);
  text("state-window", state.window_id);
  text("state-interface", yesNo(state.interface_present));
  text("state-carrier", yesNo(state.carrier_present));
  text("state-temperature", temperature(state.temperature_millic));
  text("state-temperature-uncertainty", uncertainty(state.temperature_uncertainty_millic));

  const stream = snapshot.semantics.stream;
  badge("stream-status", stream.status);
  text(
    "stream-key",
    stream.epoch === null || stream.sequence === null ? "—" : `${stream.epoch} / ${stream.sequence}`,
  );
  text("stream-time", stream.source_time_ns === null ? "—" : `${stream.source_time_ns} ns`);
  text("stream-window", stream.window_id);
  text("stream-elapsed", stream.elapsed_ns === null ? "—" : `${stream.elapsed_ns} ns`);
  text("stream-rx-bytes", bytes(stream.rx_bytes));
  text("stream-tx-bytes", bytes(stream.tx_bytes));
  text("stream-rx-packets", stream.rx_packets);
  text("stream-tx-packets", stream.tx_packets);
  text("stream-rx-dropped", stream.rx_dropped);
  text("stream-tx-dropped", stream.tx_dropped);
  text("stream-signal", stream.signal_bucket);
  text("stream-temperature", temperature(stream.temperature_millic));
  text("stream-temperature-uncertainty", uncertainty(stream.temperature_uncertainty_millic));

  const action = snapshot.semantics.action;
  badge("action-status", action.status);
  text("action-operation", action.operation);
  text("action-delivery", yesNo(action.delivery_accepted));
  text("action-accepted", yesNo(action.edge_accepted));
  text("action-once", yesNo(action.executed_once));
  text(
    "action-result",
    action.result_code === null
      ? "—"
      : action.result_value === null
        ? action.result_code
        : `${action.result_code} · ${action.result_value}`,
  );

  const blob = snapshot.semantics.blob;
  badge("blob-status", blob.status);
  text("blob-label", blob.label);
  text("blob-size", bytes(blob.content_size));
  text("blob-integrity", blob.integrity_verified ? "verified" : "not verified");
  text("blob-published", yesNo(blob.published));
  text("blob-duplicate", yesNo(blob.duplicate));

  const rows = byId("sensor-rows");
  rows.replaceChildren();
  snapshot.sensors.forEach((sensor) => {
    const row = document.createElement("tr");
    const name = document.createElement("td");
    const kind = document.createElement("td");
    const status = document.createElement("td");
    name.textContent = sensor.name;
    kind.textContent = sensor.kind;
    status.textContent = sensor.status;
    row.append(name, kind, status);
    rows.append(row);
  });

  setControls(snapshot);
  text("last-refresh", `snapshot ${new Date().toLocaleTimeString()}`);
}

function eventSummary(event) {
  const pieces = [`#${event.seq}`, event.type];
  for (const key of ["layer", "direction", "phase", "status", "test", "state", "result"]) {
    if (event[key] !== undefined) {
      pieces.push(`${key}=${event[key]}`);
    }
  }
  for (const key of [
    "version",
    "window_id",
    "epoch",
    "sequence",
    "payload_size",
    "content_size",
  ]) {
    if (event[key] !== undefined) {
      pieces.push(`${key}=${event[key]}`);
    }
  }
  return pieces.join("  ");
}

function renderEvents(response) {
  const log = byId("event-log");
  if (response.dropped_before !== null && eventCursor < response.dropped_before - 1) {
    const gap = document.createElement("div");
    gap.className = "event gap";
    gap.textContent = `older events evicted before #${response.dropped_before}`;
    log.append(gap);
  }
  response.events.forEach((event) => {
    const row = document.createElement("div");
    row.className = `event event-${event.type}`;
    row.textContent = eventSummary(event);
    log.append(row);
    eventCursor = Math.max(eventCursor, event.seq);
    renderedEvents += 1;
  });
  while (log.childElementCount > 256) {
    log.firstElementChild.remove();
  }
  if (response.events.length > 0) {
    log.scrollTop = log.scrollHeight;
  }
  text("event-count", `${renderedEvents} events rendered`);
}

async function fetchJson(path, options = {}) {
  const response = await fetch(path, {
    cache: "no-store",
    credentials: "same-origin",
    ...options,
  });
  const value = await response.json();
  if (!response.ok) {
    throw new Error(value.error || `HTTP ${response.status}`);
  }
  return value;
}

async function refresh() {
  if (refreshPending) {
    return;
  }
  refreshPending = true;
  try {
    const [snapshot, events] = await Promise.all([
      fetchJson("/api/v1/snapshot"),
      fetchJson(`/api/v1/events?after=${eventCursor}&limit=128`),
    ]);
    renderSnapshot(snapshot);
    renderEvents(events);
    text("request-error", "");
  } catch (error) {
    text("request-error", `refresh failed: ${error.message}`);
  } finally {
    refreshPending = false;
  }
}

async function post(path, body) {
  requestPending = true;
  document.querySelectorAll("button").forEach((button) => {
    button.disabled = true;
  });
  try {
    await fetchJson(path, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Meiso-CSRF": csrfToken,
      },
      body: JSON.stringify(body),
    });
    text("request-error", "");
  } catch (error) {
    text("request-error", `request failed: ${error.message}`);
  } finally {
    requestPending = false;
    await refresh();
  }
}

document.querySelectorAll("button[data-test]").forEach((button) => {
  button.addEventListener("click", () => {
    post("/api/v1/tests", {kind: button.dataset.test});
  });
});

refresh();
window.setInterval(refresh, 500);

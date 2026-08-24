export const clamp = (value, minimum, maximum) =>
  Math.min(maximum, Math.max(minimum, Number(value)));

export function selectedObject(snapshot) {
  const selected = snapshot?.scene?.selected_draft_object;
  return snapshot?.scene?.objects?.find((item) => item.draft_object === selected) ?? null;
}

export function vectorFromInputs(inputs) {
  return inputs.map((input) => Number(input.value));
}

export function objectTransformCommand(object, fields) {
  return {
    command: "scene.object.transform",
    draft_object: object.draft_object,
    position: vectorFromInputs(fields.position),
    rotation_deg: vectorFromInputs(fields.rotation),
    scale: vectorFromInputs(fields.scale),
  };
}

export function cameraCommand(fields) {
  return {
    command: "scene.camera.set",
    position: vectorFromInputs(fields.position),
    rotation_deg: vectorFromInputs(fields.rotation),
    vertical_fov_deg: Number(fields.fov.value),
    near: Number(fields.near.value),
    far: Number(fields.far.value),
  };
}

export function renderIntentCommand(form) {
  return {
    command: "intent.render.set",
    enabled: form.enabled.checked,
    max_hz: Number(form.rate.value),
  };
}

export function cameraIntentCommand(form) {
  return {
    command: "intent.camera.set",
    enabled: form.enabled.checked,
    fps: Number(form.rate.value),
  };
}

export function telemetryIntentCommand(form) {
  return {
    command: "intent.telemetry.set",
    enabled: form.enabled.checked,
    sample_period_ms: Number(form.period.value),
    selected_fields: Number(form.fields.value),
  };
}

export function deviceTelemetryIntentCommand(form) {
  return {
    command: "intent.device.set",
    enabled: form.enabled.checked,
    sample_period_ms: Number(form.period.value),
    selected_fields: Number(form.fields.value),
  };
}

function appendPresent(rows, prefix, value, fields) {
  if (!value) return;
  for (const field of fields) {
    if (Object.hasOwn(value, field)) {
      rows.push({name: `${prefix}.${field}`, value: String(value[field]), bound: true});
    }
  }
}

export function evidenceRows(snapshot) {
  const rows = Object.entries(snapshot.protocol_evidence).map(([name, value]) => ({
    name,
    value: value === null ? "not allocated" : String(value),
    bound: value !== null,
  }));
  const truth = snapshot.native_truth;
  if (!truth) return rows;
  for (const [channel, actual] of Object.entries(truth.actual ?? {})) {
    appendPresent(rows, `actual.${channel}`, actual, [
      "state_version", "apply_result", "availability", "enabled",
      "production_status", "reason", "production_epoch",
    ]);
  }
  appendPresent(rows, "wifi.effective", truth.wifi_effective, [
    "selected_fields", "sample_period_ms",
  ]);
  appendPresent(rows, "device.effective", truth.device_effective, [
    "selected_fields", "sample_period_ms",
  ]);
  appendPresent(rows, "wifi.sample", truth.wifi_sample, [
    "present_fields", "link_state", "signal_mbm", "rx_bytes", "tx_bytes",
    "rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "rx_errors",
    "tx_errors",
  ]);
  appendPresent(rows, "device.sample", truth.device_sample, [
    "present_fields", "temperature_millic", "temperature_uncertainty_millic",
    "cpu_load_ppm", "memory_total_bytes", "memory_available_bytes",
  ]);
  appendPresent(rows, "scene", truth.scene_status, [
    "reason", "active_plan_generation", "world_generation", "object_count",
    "unique_mesh_count", "resident_mesh_bytes", "render_jobs_in_flight",
    "result_queue_depth",
  ]);
  appendPresent(rows, "presentation", truth.presentation_status, [
    "candidate_state", "candidate_plan_generation", "active_state",
    "active_plan_generation", "reason", "target_width", "target_height",
    "color_format", "depth_format",
  ]);
  appendPresent(rows, "render.result", truth.render_result, [
    "result", "active_plan_generation", "world_generation", "object_count",
    "unique_mesh_count", "target_width", "target_height", "color_format",
    "depth_format", "offscreen_complete", "thumbnail_included",
    "fence_complete", "display_commit_accepted", "vblank_observed",
    "physical_verified", "full_target_sha256",
  ]);
  for (const [source, actual] of Object.entries(truth.media_actual ?? {})) {
    appendPresent(rows, `media.${source}`, actual, [
      "pixel_format", "codec", "width", "height", "fps_millihz",
      "bitrate_bps", "gop_frames", "request_key", "media_stream_epoch",
      "producer_dropped_frames", "queue_depth", "recovery_generation",
    ]);
  }
  for (const [source, frame] of Object.entries(snapshot.frames ?? {})) {
    if (!frame.available) continue;
    appendPresent(rows, `frame.${source}`, frame, [
      "pixel_format", "width", "height", "payload_bytes", "frame_sequence",
      "source_clock_epoch", "source_monotonic_ns", "active_plan_generation",
      "world_generation", "applied_scene_state_version",
    ]);
  }
  return rows;
}

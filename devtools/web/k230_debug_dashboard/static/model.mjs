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

export function evidenceRows(snapshot) {
  return Object.entries(snapshot.protocol_evidence).map(([name, value]) => ({
    name,
    value: value === null ? "not allocated" : String(value),
    bound: value !== null,
  }));
}

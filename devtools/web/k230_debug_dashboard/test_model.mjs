import assert from "node:assert/strict";
import test from "node:test";
import {
  cameraCommand,
  cameraIntentCommand,
  evidenceRows,
  objectTransformCommand,
  renderIntentCommand,
  selectedObject,
  shouldSyncForms,
  telemetryIntentCommand,
} from "./static/model.mjs";

const input = (value, checked = false) => ({value: String(value), checked});

test("selected object uses only the server-owned draft handle", () => {
  const snapshot = {
    scene: {
      selected_draft_object: 7,
      objects: [{draft_object: 3}, {draft_object: 7}],
    },
  };
  assert.deepEqual(selectedObject(snapshot), {draft_object: 7});
  assert.equal(selectedObject({scene: {selected_draft_object: null, objects: []}}), null);
});

test("native truth updates do not overwrite an in-progress editor form", () => {
  assert.equal(shouldSyncForms(null, {revision: 0}), true);
  assert.equal(shouldSyncForms({revision: 4}, {revision: 4}), false);
  assert.equal(shouldSyncForms({revision: 4}, {revision: 5}), true);
});

test("object and camera commands contain no protocol identity", () => {
  const object = {draft_object: 8};
  const objectCommand = objectTransformCommand(object, {
    position: [input(1), input(2), input(3)],
    rotation: [input(10), input(20), input(30)],
    scale: [input(.5), input(1), input(2)],
  });
  assert.deepEqual(objectCommand, {
    command: "scene.object.transform",
    draft_object: 8,
    position: [1, 2, 3],
    rotation_deg: [10, 20, 30],
    scale: [.5, 1, 2],
  });
  const camera = cameraCommand({
    position: [input(4), input(5), input(6)],
    rotation: [input(-10), input(25), input(0)],
    fov: input(57), near: input(.1), far: input(200),
  });
  assert.deepEqual(camera, {
    command: "scene.camera.set",
    position: [4, 5, 6], rotation_deg: [-10, 25, 0],
    vertical_fov_deg: 57, near: .1, far: 200,
  });
  for (const command of [objectCommand, camera]) {
    for (const forbidden of ["state_version", "content_id", "object_id", "session_id", "epoch", "sequence"])
      assert.equal(Object.hasOwn(command, forbidden), false);
  }
});

test("intent commands are fixed local desired shapes", () => {
  assert.deepEqual(renderIntentCommand({enabled: input(0, true), rate: input(24)}), {
    command: "intent.render.set", enabled: true, max_hz: 24,
  });
  assert.deepEqual(cameraIntentCommand({enabled: input(0, false), rate: input(30)}), {
    command: "intent.camera.set", enabled: false, fps: 30,
  });
  assert.deepEqual(telemetryIntentCommand({
    enabled: input(0, true), period: input(500), fields: input(63),
  }), {
    command: "intent.telemetry.set", enabled: true,
    sample_period_ms: 500, selected_fields: 63,
  });
});

test("unallocated protocol evidence stays visibly unallocated", () => {
  assert.deepEqual(evidenceRows({protocol_evidence: {
    state_version: null, content_id: null, world_generation: null,
  }}), [
    {name: "state_version", value: "not allocated", bound: false},
    {name: "content_id", value: "not allocated", bound: false},
    {name: "world_generation", value: "not allocated", bound: false},
  ]);
});

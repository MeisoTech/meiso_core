import {
  cameraCommand,
  cameraIntentCommand,
  deviceTelemetryIntentCommand,
  evidenceRows,
  objectTransformCommand,
  renderIntentCommand,
  selectedObject,
  shouldSyncForms,
  telemetryIntentCommand,
} from "./model.mjs";

const csrf = document.querySelector('meta[name="meiso-csrf"]').content;
const node = (id) => document.getElementById(id);
let snapshot = null;
let lastEventId = 0;
let eventSource = null;
let toastTimer = null;
const eventRows = [];

const transformFields = {
  position: [...document.querySelectorAll('[data-vector="position"] input')],
  rotation: [...document.querySelectorAll('[data-vector="rotation"] input')],
  scale: [...document.querySelectorAll('[data-vector="scale"] input')],
};
const cameraFields = {
  position: [...document.querySelectorAll('[data-camera-vector="position"] input')],
  rotation: [...document.querySelectorAll('[data-camera-vector="rotation"] input')],
  fov: node("camera-fov"),
  near: node("camera-near"),
  far: node("camera-far"),
};

function showToast(message, failure = false) {
  const toast = node("toast");
  toast.textContent = message;
  toast.style.borderColor = failure ? "#8e4248" : "#3a4d66";
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 2200);
}

async function send(command) {
  const response = await fetch("/api/v1/command", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Origin": window.location.origin,
      "X-Meiso-CSRF": csrf,
    },
    body: JSON.stringify(command),
  });
  const body = await response.json();
  if (!response.ok) {
    showToast(body.error || `HTTP ${response.status}`, true);
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  showToast(
    snapshot?.backend?.mode?.startsWith("NATIVE_") ?
      `Native command queued · revision ${body.revision}` :
      `Local draft accepted · revision ${body.revision}`
  );
  return body;
}

function setVector(inputs, values) {
  inputs.forEach((input, index) => { input.value = String(values[index]); });
}

function syncForms() {
  const object = selectedObject(snapshot);
  node("transform-fields").disabled = object === null;
  node("delete-object").disabled = object === null;
  node("viewport-label").textContent = object?.name ?? "No selection";
  if (object) {
    setVector(transformFields.position, object.position);
    setVector(transformFields.rotation, object.rotation_deg);
    setVector(transformFields.scale, object.scale);
  }
  const camera = snapshot.scene.camera;
  setVector(cameraFields.position, camera.position);
  setVector(cameraFields.rotation, camera.rotation_deg);
  cameraFields.fov.value = String(camera.vertical_fov_deg);
  cameraFields.near.value = String(camera.near);
  cameraFields.far.value = String(camera.far);

  const render = snapshot.intents.render;
  const renderForm = node("render-intent").elements;
  renderForm.namedItem("enabled").checked = render.enabled;
  renderForm.namedItem("rate").value = String(render.max_hz);
  const cameraIntent = snapshot.intents.camera;
  const cameraForm = node("camera-intent").elements;
  cameraForm.namedItem("enabled").checked = cameraIntent.enabled;
  cameraForm.namedItem("rate").value = String(cameraIntent.fps);
  const telemetry = snapshot.intents.telemetry;
  const telemetryForm = node("telemetry-intent").elements;
  telemetryForm.namedItem("enabled").checked = telemetry.enabled;
  telemetryForm.namedItem("period").value = String(telemetry.sample_period_ms);
  telemetryForm.namedItem("fields").value = String(telemetry.selected_fields);
  const device = snapshot.intents.device;
  const deviceForm = node("device-intent").elements;
  deviceForm.namedItem("enabled").checked = device.enabled;
  deviceForm.namedItem("period").value = String(device.sample_period_ms);
  deviceForm.namedItem("fields").value = String(device.selected_fields);
}

function renderObjectList() {
  const list = node("object-list");
  list.replaceChildren();
  for (const object of snapshot.scene.objects) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "object-button";
    if (object.draft_object === snapshot.scene.selected_draft_object) button.classList.add("selected");
    button.append(document.createTextNode(`◫  ${object.name}`));
    const handle = document.createElement("code");
    handle.textContent = `draft:${object.draft_object}`;
    button.append(handle);
    button.addEventListener("click", () => send({
      command: "scene.object.select",
      draft_object: object.draft_object,
    }).catch(() => {}));
    list.append(button);
  }
  if (snapshot.scene.objects.length === 0) {
    const empty = document.createElement("small");
    empty.textContent = "Empty local scene";
    list.append(empty);
  }
  node("object-count").textContent = `${snapshot.scene.objects.length} / 32`;
}

function renderEvidence() {
  const list = node("evidence-list");
  list.replaceChildren();
  for (const row of evidenceRows(snapshot)) {
    const container = document.createElement("div");
    container.className = "evidence-row";
    const name = document.createElement("span");
    name.textContent = row.name;
    const value = document.createElement("strong");
    value.textContent = row.value;
    container.append(name, value);
    list.append(container);
  }
}

function renderSnapshot(next) {
  const syncEditableForms = shouldSyncForms(snapshot, next);
  snapshot = next;
  node("revision-badge").textContent = `REV ${snapshot.revision}`;
  node("backend-badge").textContent = snapshot.backend.mode.replace("_", " / ");
  node("device-badge").textContent = snapshot.backend.device_connected ? "DEVICE CONNECTED" : "DEVICE NOT CONNECTED";
  node("media-badge").textContent = snapshot.bindings.render_result_stream === "SIMULATED" ?
    "MEDIA SIMULATED" : snapshot.bindings.render_result_stream === "BOUND" ?
      "MEDIA BOUND" : "MEDIA UNBOUND";
  node("render-binding").textContent = snapshot.bindings.render_result_stream;
  node("camera-binding").textContent = snapshot.bindings.camera_media_stream;
  node("wifi-binding").textContent = snapshot.bindings.wifi_telemetry;
  node("device-binding").textContent = snapshot.native_truth?.actual?.device ?
    "ACTUAL RECEIVED" : "DEVICE UNBOUND";
  const native = snapshot.backend.mode.startsWith("NATIVE_");
  const simulated = snapshot.backend.mode === "NATIVE_SIMULATOR";
  node("truth-title").textContent = native ?
    simulated ? "Native synthetic simulator bridge is active." :
      "Native Host typed bridge is active." :
    "这是本地场景草稿，不是 K230 回传。";
  node("truth-detail").textContent = simulated ?
    "编辑命令由本地 caller-owned simulator 执行；没有 Session、设备连接、TCP media 或 physical frame。" : native ?
    "浏览器只提交编辑器意图；下方 generation、epoch、sequence 与 telemetry 是 native service 的只读投影。" :
    "浏览器不创建 State version、ContentId、Session、epoch 或 sequence；显式启动 native Host 前这些字段保持未分配。";
  node("native-frame-links").hidden = !native;
  node("evidence-title").textContent = native ?
    "Sanitized native truth" : "Not allocated";
  node("evidence-badge").textContent = native ? "READ ONLY" : "NO SESSION";
  renderObjectList();
  renderEvidence();
  if (syncEditableForms) syncForms();
  renderer.draw(snapshot);
}

function appendEvent(eventId, event) {
  eventRows.push({id: eventId, command: event.command, status: event.status});
  while (eventRows.length > 256) eventRows.shift();
  const log = node("event-log");
  log.replaceChildren();
  for (const item of eventRows.slice(-80).reverse()) {
    const row = document.createElement("div");
    row.className = "event";
    for (const [className, value] of [
      ["id", `#${item.id}`], ["command", item.command], ["status", item.status],
    ]) {
      const field = document.createElement("span");
      field.className = className;
      field.textContent = value;
      row.append(field);
    }
    log.append(row);
  }
  node("event-count").textContent = `${eventRows.length} / 256`;
}

function connectEvents() {
  eventSource?.close();
  eventSource = new EventSource(`/api/v1/events?after=${lastEventId}`);
  eventSource.addEventListener("dashboard", (message) => {
    const event = JSON.parse(message.data);
    const eventId = Number(message.lastEventId);
    if (!Number.isSafeInteger(eventId) || eventId <= lastEventId) return;
    lastEventId = eventId;
    appendEvent(eventId, event);
    renderSnapshot(event.snapshot);
  });
  eventSource.onerror = () => {
    eventSource.close();
    setTimeout(connectEvents, 900);
  };
}

node("add-object").addEventListener("click", () => send({
  command: "scene.object.add", asset: "builtin:cube",
}).catch(() => {}));
node("delete-object").addEventListener("click", () => {
  const object = selectedObject(snapshot);
  if (object) send({command: "scene.object.delete", draft_object: object.draft_object}).catch(() => {});
});
node("transform-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const object = selectedObject(snapshot);
  if (object) send(objectTransformCommand(object, transformFields)).catch(() => {});
});
node("camera-form").addEventListener("submit", (event) => {
  event.preventDefault();
  send(cameraCommand(cameraFields)).catch(() => {});
});
node("render-intent").addEventListener("submit", (event) => {
  event.preventDefault();
  send(renderIntentCommand(event.currentTarget.elements)).catch(() => {});
});
node("camera-intent").addEventListener("submit", (event) => {
  event.preventDefault();
  send(cameraIntentCommand(event.currentTarget.elements)).catch(() => {});
});
node("telemetry-intent").addEventListener("submit", (event) => {
  event.preventDefault();
  send(telemetryIntentCommand(event.currentTarget.elements)).catch(() => {});
});
node("device-intent").addEventListener("submit", (event) => {
  event.preventDefault();
  send(deviceTelemetryIntentCommand(event.currentTarget.elements)).catch(() => {});
});

function multiply(a, b) {
  const out = new Float32Array(16);
  for (let column = 0; column < 4; column += 1) {
    for (let row = 0; row < 4; row += 1) {
      let value = 0;
      for (let inner = 0; inner < 4; inner += 1) value += a[inner * 4 + row] * b[column * 4 + inner];
      out[column * 4 + row] = value;
    }
  }
  return out;
}
const identity = () => new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
function translation(x, y, z) { const m = identity(); m[12] = x; m[13] = y; m[14] = z; return m; }
function scale(x, y, z) { const m = identity(); m[0] = x; m[5] = y; m[10] = z; return m; }
function rotateX(a) { const c=Math.cos(a),s=Math.sin(a); return new Float32Array([1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1]); }
function rotateY(a) { const c=Math.cos(a),s=Math.sin(a); return new Float32Array([c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1]); }
function rotateZ(a) { const c=Math.cos(a),s=Math.sin(a); return new Float32Array([c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1]); }
function perspective(fov, aspect, near, far) {
  const f = 1 / Math.tan(fov / 2), nf = 1 / (near - far);
  return new Float32Array([f/aspect,0,0,0, 0,f,0,0, 0,0,(far+near)*nf,-1, 0,0,2*far*near*nf,0]);
}
const radians = (degrees) => degrees * Math.PI / 180;
function transform(position, rotation, objectScale) {
  let matrix = translation(...position);
  matrix = multiply(matrix, rotateZ(radians(rotation[2])));
  matrix = multiply(matrix, rotateY(radians(rotation[1])));
  matrix = multiply(matrix, rotateX(radians(rotation[0])));
  return multiply(matrix, scale(...objectScale));
}
function viewMatrix(camera) {
  const rotation = camera.rotation_deg;
  let view = rotateX(-radians(rotation[0]));
  view = multiply(view, rotateY(-radians(rotation[1])));
  view = multiply(view, rotateZ(-radians(rotation[2])));
  return multiply(view, translation(...camera.position.map((value) => -value)));
}

function compile(gl, kind, source) {
  const shader = gl.createShader(kind); gl.shaderSource(shader, source); gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
  return shader;
}

function createRenderer() {
  const canvas = node("viewport");
  const gl = canvas.getContext("webgl2", {antialias: true, alpha: false});
  if (!gl) { node("webgl-warning").hidden = false; return {draw() {}}; }
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, `#version 300 es
    in vec3 position; in vec3 color; uniform mat4 matrix; uniform float selected;
    out vec3 vertexColor;
    void main(){ gl_Position=matrix*vec4(position,1.0); vertexColor=mix(color,color*0.6+vec3(.28,.28,.12),selected); }
  `));
  gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, `#version 300 es
    precision highp float; in vec3 vertexColor; out vec4 outColor;
    void main(){ outColor=vec4(vertexColor,1.0); }
  `));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  const cube = [];
  const faces = [
    [[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1],[.25,.55,1]],
    [[1,-1,-1],[-1,-1,-1],[-1,1,-1],[1,1,-1],[.16,.31,.58]],
    [[-1,-1,-1],[-1,-1,1],[-1,1,1],[-1,1,-1],[.18,.7,.72]],
    [[1,-1,1],[1,-1,-1],[1,1,-1],[1,1,1],[.17,.46,.82]],
    [[-1,1,1],[1,1,1],[1,1,-1],[-1,1,-1],[.42,.72,1]],
    [[-1,-1,-1],[1,-1,-1],[1,-1,1],[-1,-1,1],[.1,.25,.45]],
  ];
  for (const [a,b,c,d,color] of faces) for (const vertex of [a,b,c,a,c,d]) cube.push(...vertex.map(v=>v*.5), ...color);
  const grid = [];
  for (let index=-10; index<=10; index+=1) {
    const strong = index === 0 ? .32 : .13;
    grid.push(-10,0,index,strong,strong,strong, 10,0,index,strong,strong,strong);
    grid.push(index,0,-10,strong,strong,strong, index,0,10,strong,strong,strong);
  }
  function buffer(values) {
    const result=gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER,result); gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(values),gl.STATIC_DRAW); return result;
  }
  const cubeBuffer=buffer(cube), gridBuffer=buffer(grid);
  const position=gl.getAttribLocation(program,"position"), color=gl.getAttribLocation(program,"color");
  const matrix=gl.getUniformLocation(program,"matrix"), selected=gl.getUniformLocation(program,"selected");
  function bind(target) {
    gl.bindBuffer(gl.ARRAY_BUFFER,target); gl.enableVertexAttribArray(position); gl.vertexAttribPointer(position,3,gl.FLOAT,false,24,0);
    gl.enableVertexAttribArray(color); gl.vertexAttribPointer(color,3,gl.FLOAT,false,24,12);
  }
  function draw(state) {
    const rect=canvas.getBoundingClientRect(), ratio=Math.min(2,window.devicePixelRatio||1);
    const width=Math.max(1,Math.round(rect.width*ratio)), height=Math.max(1,Math.round(rect.height*ratio));
    if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}
    gl.viewport(0,0,width,height); gl.enable(gl.DEPTH_TEST); gl.enable(gl.CULL_FACE); gl.clearColor(.025,.04,.065,1); gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    gl.useProgram(program);
    const camera=state.scene.camera;
    const projection=perspective(radians(camera.vertical_fov_deg),width/height,camera.near,camera.far);
    const vp=multiply(projection,viewMatrix(camera));
    bind(gridBuffer); gl.uniformMatrix4fv(matrix,false,vp); gl.uniform1f(selected,0); gl.drawArrays(gl.LINES,0,grid.length/6);
    bind(cubeBuffer);
    for(const object of state.scene.objects){
      const mvp=multiply(vp,transform(object.position,object.rotation_deg,object.scale));
      gl.uniformMatrix4fv(matrix,false,mvp); gl.uniform1f(selected,object.draft_object===state.scene.selected_draft_object?1:0); gl.drawArrays(gl.TRIANGLES,0,cube.length/6);
    }
  }
  new ResizeObserver(()=>{if(snapshot)draw(snapshot);}).observe(canvas);
  return {draw};
}

const renderer = createRenderer();

async function start() {
  const response = await fetch("/api/v1/snapshot", {cache: "no-store"});
  if (!response.ok) throw new Error(`snapshot HTTP ${response.status}`);
  renderSnapshot(await response.json());
  connectEvents();
}
start().catch((error) => showToast(`Dashboard failed: ${error.message}`, true));

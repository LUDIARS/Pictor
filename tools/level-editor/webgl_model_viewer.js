const TAU = Math.PI * 2;

function mat4Identity() {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

function mat4Multiply(a, b) {
  const out = new Array(16).fill(0);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      out[c * 4 + r] =
        a[0 * 4 + r] * b[c * 4 + 0] +
        a[1 * 4 + r] * b[c * 4 + 1] +
        a[2 * 4 + r] * b[c * 4 + 2] +
        a[3 * 4 + r] * b[c * 4 + 3];
    }
  }
  return out;
}

function mat4Perspective(fov, aspect, near, far) {
  const f = 1 / Math.tan(fov / 2);
  const nf = 1 / (near - far);
  return [
    f / aspect, 0, 0, 0,
    0, f, 0, 0,
    0, 0, (far + near) * nf, -1,
    0, 0, 2 * far * near * nf, 0,
  ];
}

function vec3Normalize(v) {
  const l = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / l, v[1] / l, v[2] / l];
}

function vec3Cross(a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function mat4LookAt(eye, target, up) {
  const z = vec3Normalize([eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]]);
  const x = vec3Normalize(vec3Cross(up, z));
  const y = vec3Cross(z, x);
  return [
    x[0], y[0], z[0], 0,
    x[1], y[1], z[1], 0,
    x[2], y[2], z[2], 0,
    -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]),
    -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
    -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]),
    1,
  ];
}

function composeTransform(t) {
  const [px, py, pz] = t.position;
  const [rx, ry, rz] = t.rotation.map((v) => v * Math.PI / 180);
  const [sx, sy, sz] = t.scale;
  const cx = Math.cos(rx), sxn = Math.sin(rx);
  const cy = Math.cos(ry), syn = Math.sin(ry);
  const cz = Math.cos(rz), szn = Math.sin(rz);

  const r00 = cy * cz;
  const r01 = sxn * syn * cz + cx * szn;
  const r02 = -cx * syn * cz + sxn * szn;
  const r10 = -cy * szn;
  const r11 = -sxn * syn * szn + cx * cz;
  const r12 = cx * syn * szn + sxn * cz;
  const r20 = syn;
  const r21 = -sxn * cy;
  const r22 = cx * cy;

  return [
    r00 * sx, r01 * sx, r02 * sx, 0,
    r10 * sy, r11 * sy, r12 * sy, 0,
    r20 * sz, r21 * sz, r22 * sz, 0,
    px, py, pz, 1,
  ];
}

function compile(gl, type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    throw new Error(gl.getShaderInfoLog(shader) || "shader compile failed");
  }
  return shader;
}

function program(gl, vertex, fragment) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, vertex));
  gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fragment));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(p) || "program link failed");
  }
  return p;
}

function createCube() {
  const p = [
    [-1, -1,  1], [ 1, -1,  1], [ 1,  1,  1], [-1,  1,  1],
    [ 1, -1, -1], [-1, -1, -1], [-1,  1, -1], [ 1,  1, -1],
    [-1,  1,  1], [ 1,  1,  1], [ 1,  1, -1], [-1,  1, -1],
    [-1, -1, -1], [ 1, -1, -1], [ 1, -1,  1], [-1, -1,  1],
    [ 1, -1,  1], [ 1, -1, -1], [ 1,  1, -1], [ 1,  1,  1],
    [-1, -1, -1], [-1, -1,  1], [-1,  1,  1], [-1,  1, -1],
  ];
  const n = [
    [0, 0, 1], [0, 0, -1], [0, 1, 0], [0, -1, 0], [1, 0, 0], [-1, 0, 0],
  ];
  const vertices = [];
  for (let f = 0; f < 6; f++) {
    for (let i = 0; i < 4; i++) vertices.push(...p[f * 4 + i], ...n[f]);
  }
  const indices = [];
  for (let f = 0; f < 6; f++) {
    const b = f * 4;
    indices.push(b, b + 1, b + 2, b, b + 2, b + 3);
  }
  return { vertices, indices };
}

function createPlane() {
  return {
    vertices: [
      -1, 0, -1, 0, 1, 0,
       1, 0, -1, 0, 1, 0,
       1, 0,  1, 0, 1, 0,
      -1, 0,  1, 0, 1, 0,
    ],
    indices: [0, 1, 2, 0, 2, 3],
  };
}

function createSphere(segments = 24, rings = 12) {
  const vertices = [];
  const indices = [];
  for (let y = 0; y <= rings; y++) {
    const v = y / rings;
    const phi = v * Math.PI;
    for (let x = 0; x <= segments; x++) {
      const u = x / segments;
      const theta = u * TAU;
      const nx = Math.sin(phi) * Math.cos(theta);
      const ny = Math.cos(phi);
      const nz = Math.sin(phi) * Math.sin(theta);
      vertices.push(nx, ny, nz, nx, ny, nz);
    }
  }
  for (let y = 0; y < rings; y++) {
    for (let x = 0; x < segments; x++) {
      const a = y * (segments + 1) + x;
      const b = a + segments + 1;
      indices.push(a, b, a + 1, b, b + 1, a + 1);
    }
  }
  return { vertices, indices };
}

function createCylinder(segments = 24) {
  const vertices = [];
  const indices = [];
  for (let i = 0; i <= segments; i++) {
    const a = i / segments * TAU;
    const x = Math.cos(a), z = Math.sin(a);
    vertices.push(x, -1, z, x, 0, z, x, 1, z, x, 0, z);
  }
  for (let i = 0; i < segments; i++) {
    const b = i * 2;
    indices.push(b, b + 1, b + 2, b + 1, b + 3, b + 2);
  }
  return { vertices, indices };
}

const vertexSource = `#version 300 es
precision highp float;
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_normal;
uniform mat4 u_viewProjection;
uniform mat4 u_model;
uniform vec4 u_color;
out vec3 v_normal;
out vec3 v_world;
out vec4 v_color;
void main() {
  vec4 world = u_model * vec4(a_position, 1.0);
  gl_Position = u_viewProjection * world;
  v_normal = mat3(u_model) * a_normal;
  v_world = world.xyz;
  v_color = u_color;
}`;

const fragmentSource = `#version 300 es
precision highp float;
in vec3 v_normal;
in vec3 v_world;
in vec4 v_color;
out vec4 outColor;
void main() {
  vec3 n = normalize(v_normal);
  vec3 light = normalize(vec3(0.45, 0.8, 0.25));
  float ndl = max(dot(n, light), 0.0);
  float hemi = n.y * 0.25 + 0.35;
  vec3 base = v_color.rgb * (0.22 + ndl * 0.68 + hemi * 0.22);
  float gridFog = smoothstep(90.0, 8.0, length(v_world.xz));
  outColor = vec4(base * gridFog, v_color.a);
}`;

export class WebGLModelViewer {
  constructor(canvas) {
    this.canvas = canvas;
    this.gl = canvas.getContext("webgl2", { antialias: true, alpha: false });
    if (!this.gl) throw new Error("WebGL2 is not available");
    this.program = program(this.gl, vertexSource, fragmentSource);
    this.meshes = new Map();
    this.camera = { yaw: -0.72, pitch: -0.55, distance: 18, target: [0, 0, 0] };
    this.draws = 0;
    this._drag = null;
    this._installCameraControls();
    this.registerMesh("cube", createCube());
    this.registerMesh("plane", createPlane());
    this.registerMesh("sphere", createSphere());
    this.registerMesh("cylinder", createCylinder());
  }

  registerMesh(id, mesh) {
    const gl = this.gl;
    const vao = gl.createVertexArray();
    const vbo = gl.createBuffer();
    const ibo = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(mesh.vertices), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 24, 12);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(mesh.indices), gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    this.meshes.set(id, { vao, count: mesh.indices.length });
  }

  resize() {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.floor(this.canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }
  }

  render(level, selectedId) {
    this.resize();
    const gl = this.gl;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.CULL_FACE);
    gl.clearColor(0.045, 0.052, 0.062, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    const aspect = this.canvas.width / this.canvas.height;
    const projection = mat4Perspective(52 * Math.PI / 180, aspect, 0.05, 200);
    const eye = this._eye();
    const view = mat4LookAt(eye, this.camera.target, [0, 1, 0]);
    const viewProjection = mat4Multiply(projection, view);

    gl.useProgram(this.program);
    gl.uniformMatrix4fv(gl.getUniformLocation(this.program, "u_viewProjection"), false, viewProjection);
    this.draws = 0;
    this._drawGrid(viewProjection);

    for (const node of level.nodes) {
      if (node.kind !== "model") continue;
      const asset = level.assets.find((a) => a.id === node.assetId);
      if (!asset) continue;
      const mesh = this.meshes.get(asset.previewMesh || "cube") || this.meshes.get("cube");
      gl.bindVertexArray(mesh.vao);
      const model = composeTransform(node.transform);
      const color = node.id === selectedId ? [1.0, 0.86, 0.35, 1] : asset.color;
      gl.uniformMatrix4fv(gl.getUniformLocation(this.program, "u_model"), false, model);
      gl.uniform4fv(gl.getUniformLocation(this.program, "u_color"), color);
      gl.drawElements(gl.TRIANGLES, mesh.count, gl.UNSIGNED_INT, 0);
      this.draws++;
    }

    gl.bindVertexArray(null);
  }

  _drawGrid() {
    const gl = this.gl;
    const mesh = this.meshes.get("plane");
    const model = composeTransform({
      position: [0, -0.02, 0],
      rotation: [0, 0, 0],
      scale: [24, 1, 24],
    });
    gl.bindVertexArray(mesh.vao);
    gl.uniformMatrix4fv(gl.getUniformLocation(this.program, "u_model"), false, model);
    gl.uniform4fv(gl.getUniformLocation(this.program, "u_color"), [0.12, 0.14, 0.16, 1]);
    gl.drawElements(gl.TRIANGLES, mesh.count, gl.UNSIGNED_INT, 0);
    this.draws++;
  }

  _eye() {
    const { yaw, pitch, distance, target } = this.camera;
    const cp = Math.cos(pitch);
    return [
      target[0] + Math.sin(yaw) * cp * distance,
      target[1] + Math.sin(-pitch) * distance,
      target[2] + Math.cos(yaw) * cp * distance,
    ];
  }

  _installCameraControls() {
    this.canvas.addEventListener("pointerdown", (event) => {
      this.canvas.setPointerCapture(event.pointerId);
      this._drag = { x: event.clientX, y: event.clientY };
    });
    this.canvas.addEventListener("pointermove", (event) => {
      if (!this._drag) return;
      const dx = event.clientX - this._drag.x;
      const dy = event.clientY - this._drag.y;
      this._drag = { x: event.clientX, y: event.clientY };
      this.camera.yaw -= dx * 0.006;
      this.camera.pitch = Math.max(-1.25, Math.min(0.1, this.camera.pitch + dy * 0.006));
    });
    this.canvas.addEventListener("pointerup", () => { this._drag = null; });
    this.canvas.addEventListener("wheel", (event) => {
      event.preventDefault();
      const scale = Math.exp(event.deltaY * 0.001);
      this.camera.distance = Math.max(4, Math.min(80, this.camera.distance * scale));
    }, { passive: false });
  }
}

export function defaultAssetCatalog() {
  return [
    { id: "stage_floor", name: "Stage Floor", model: "models/stage_floor.glb", previewMesh: "plane", color: [0.24, 0.28, 0.32, 1] },
    { id: "spawn_marker", name: "Spawn Marker", model: "models/spawn_marker.glb", previewMesh: "cylinder", color: [0.2, 0.72, 1.0, 1] },
    { id: "enemy_anchor", name: "Enemy Anchor", model: "models/enemy_anchor.glb", previewMesh: "sphere", color: [1.0, 0.36, 0.32, 1] },
    { id: "cover_block", name: "Cover Block", model: "models/cover_block.glb", previewMesh: "cube", color: [0.62, 0.72, 0.46, 1] },
  ];
}

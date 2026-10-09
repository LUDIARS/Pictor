// In-process stand-in for the Emscripten module: a byte heap, a bump allocator that tracks
// frees, and the C ABI functions recording what the wrapper passed in.

export function createFakeModule({ failCreate = false, failMesh = false } = {}) {
  const memory = new ArrayBuffer(1 << 20);
  const module = {
    HEAPU8: new Uint8Array(memory),
    HEAPF32: new Float32Array(memory),
    HEAPU32: new Uint32Array(memory),
    calls: [],
    live: new Set(),
    next: 16,
    lastError: "",
    meshCounter: 0,
  };

  const writeError = (text) => {
    module.lastError = text;
    return 0;
  };
  const floatsAt = (pointer, count) => Array.from(module.HEAPF32.subarray(pointer >>> 2, (pointer >>> 2) + count));
  const uintsAt = (pointer, count) => Array.from(module.HEAPU32.subarray(pointer >>> 2, (pointer >>> 2) + count));
  const stringAt = (pointer) => {
    let end = pointer;
    while (module.HEAPU8[end] !== 0) end += 1;
    return new TextDecoder().decode(module.HEAPU8.subarray(pointer, end));
  };

  Object.assign(module, {
    _malloc(bytes) {
      const pointer = module.next;
      module.next += Math.ceil(bytes / 8) * 8 + 8;
      module.live.add(pointer);
      return pointer;
    },
    _free(pointer) {
      if (!module.live.delete(pointer)) throw new Error(`double free ${pointer}`);
    },
    _pictor_web_last_error() {
      const pointer = 1 << 19;
      module.HEAPU8.set(new TextEncoder().encode(`${module.lastError}\0`), pointer);
      return pointer;
    },
    _pictor_web_renderer_create(selectorPointer, antialias) {
      module.calls.push(["create", stringAt(selectorPointer), antialias]);
      return failCreate ? writeError("WebGL2 context could not be created") : 1;
    },
    _pictor_web_renderer_destroy(id) {
      module.calls.push(["destroy", id]);
      return 1;
    },
    _pictor_web_mesh_create(id, positions, normals, colors, vertexCount, indices, indexCount, srgb) {
      module.calls.push(["mesh", {
        id, vertexCount, indexCount, srgb,
        positions: floatsAt(positions, vertexCount * 3),
        colors: floatsAt(colors, vertexCount * 3),
        indices: uintsAt(indices, indexCount),
      }]);
      if (failMesh) return writeError("invalid mesh: index_out_of_range");
      module.meshCounter += 1;
      return module.meshCounter;
    },
    _pictor_web_mesh_release(id, mesh) {
      module.calls.push(["release", mesh]);
      return 1;
    },
    _pictor_web_set_lighting(id, values) {
      module.calls.push(["lighting", floatsAt(values, 22)]);
      return 1;
    },
    _pictor_web_set_surface(id, roughness, metalness) {
      module.calls.push(["surface", roughness, metalness]);
      return 1;
    },
    _pictor_web_begin_frame(id, width, height, viewProjection, camera, r, g, b) {
      module.calls.push(["begin", width, height, floatsAt(viewProjection, 16), floatsAt(camera, 3), [r, g, b]]);
      return 1;
    },
    _pictor_web_submit(id, mesh, model) {
      module.calls.push(["submit", mesh, floatsAt(model, 16)]);
      return 1;
    },
    _pictor_web_end_frame() {
      module.calls.push(["end"]);
      return 0;
    },
  });
  // end_frame is made to fail so the error path is exercised.
  module.lastError = "end_frame without begin_frame";
  return module;
}

/** Minimal canvas + CSS globals for the selector logic. */
export function fakeCanvas(id = "") {
  globalThis.CSS ??= { escape: (value) => value.replace(/[^a-zA-Z0-9_-]/g, (ch) => `\\${ch}`) };
  return { id, isConnected: true };
}

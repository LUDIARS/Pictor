/** Minimal WebGL2 stand-in that records calls and tracks live GPU objects. */
export function createFakeGL({ failCompile = false, failLink = false, missingUniform = false } = {}) {
  let next = 1;
  const live = new Set();
  const calls = [];
  const alloc = (kind) => { const obj = { kind, id: next++ }; live.add(obj); return obj; };
  const free = (obj) => { if (obj) live.delete(obj); };
  const gl = {
    VERTEX_SHADER: 1, FRAGMENT_SHADER: 2, COMPILE_STATUS: 3, LINK_STATUS: 4, ARRAY_BUFFER: 5, STATIC_DRAW: 6,
    NO_ERROR: 0, COLOR_BUFFER_BIT: 16, DEPTH_BUFFER_BIT: 32, DEPTH_TEST: 64, TRIANGLES: 7, FLOAT: 8, MAX_VIEWPORT_DIMS: 9,
    live, calls, lost: false, loseCalls: 0,
    getParameter: (p) => (p === 9 ? new Int32Array([4096, 4096]) : null),
    isContextLost: () => gl.lost,
    getError: () => 0,
    createProgram: () => alloc('program'),
    deleteProgram: free,
    createShader: () => alloc('shader'),
    deleteShader: free,
    shaderSource() {}, compileShader() {}, attachShader() {},
    getShaderParameter: () => !failCompile,
    getShaderInfoLog: () => 'compile error',
    linkProgram() {},
    getProgramParameter: () => !failLink,
    getProgramInfoLog: () => 'link error',
    getUniformLocation: (_, name) => (missingUniform && name === 'model' ? null : { name }),
    createBuffer: () => alloc('buffer'),
    deleteBuffer: free,
    bindBuffer() {}, bufferData() {},
    viewport: (...args) => calls.push(['viewport', ...args]),
    clearColor() {}, clear: () => calls.push(['clear']), enable() {},
    useProgram() {}, uniformMatrix4fv: (location) => calls.push(['uniform', location.name]),
    enableVertexAttribArray() {}, vertexAttribPointer() {},
    drawArrays: (_, first, count) => calls.push(['draw', first, count]),
    flush: () => calls.push(['flush']),
    getExtension: (name) => (name === 'WEBGL_lose_context' ? { loseContext: () => { gl.loseCalls++; gl.lost = true; } } : null),
  };
  return gl;
}

export function fakeCanvas(gl) {
  return { width: 0, height: 0, getContext: (type) => (type === 'webgl2' ? gl : null) };
}

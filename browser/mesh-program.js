const VERTEX = `#version 300 es
precision highp float;
layout(location=0) in vec3 position;
layout(location=1) in vec3 color;
uniform mat4 model;
uniform mat4 viewProjection;
out vec3 world;
out vec3 tint;
void main() {
  vec4 p = model * vec4(position, 1.);
  world = p.xyz;
  gl_Position = viewProjection * p;
  tint = color;
}`;
const FRAGMENT = `#version 300 es
precision highp float;
in vec3 world;
in vec3 tint;
out vec4 pixel;
void main() {
  vec3 normal = normalize(cross(dFdx(world), dFdy(world)));
  float light = .50 + .50 * abs(dot(normal, normalize(vec3(-.4, .8, .5))));
  pixel = vec4(tint * light, 1.);
}`;

/** Owns temporary shaders; the caller owns the successfully linked program. */
export function createMeshProgram(gl) {
  const program = gl.createProgram();
  if (!program) throw new Error('Pictor: program allocation failed');
  const shaders = [];
  try {
    for (const [type, source] of [[gl.VERTEX_SHADER, VERTEX], [gl.FRAGMENT_SHADER, FRAGMENT]]) {
      const shader = gl.createShader(type);
      if (!shader) throw new Error('Pictor: shader allocation failed');
      shaders.push(shader);
      gl.shaderSource(shader, source); gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader) || 'Pictor: shader compilation failed');
      gl.attachShader(program, shader);
    }
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program) || 'Pictor: program link failed');
    return program;
  } catch (error) { gl.deleteProgram(program); throw error; }
  finally { for (const shader of shaders) gl.deleteShader(shader); }
}

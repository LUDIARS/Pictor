// Copies typed arrays into the wasm heap for one call and frees them afterwards.
// The heap views are read after every _malloc because memory growth replaces the buffer.

/**
 * @param {object} module Emscripten module (needs _malloc, _free, HEAPF32 / HEAPU32)
 * @param {(scope: { floats: (values: ArrayLike<number>) => number, uints: (values: ArrayLike<number>) => number, string: (text: string) => number }) => T} body
 * @returns {T}
 * @template T
 */
export function withHeap(module, body) {
  /** @type {number[]} */
  const allocations = [];
  const allocate = (bytes) => {
    const pointer = module._malloc(Math.max(bytes, 4));
    if (!pointer) throw new Error("pictor web mesh: wasm heap allocation failed");
    allocations.push(pointer);
    return pointer;
  };
  const scope = {
    floats(values) {
      const pointer = allocate(values.length * 4);
      module.HEAPF32.set(values, pointer >>> 2);
      return pointer;
    },
    uints(values) {
      const pointer = allocate(values.length * 4);
      module.HEAPU32.set(values, pointer >>> 2);
      return pointer;
    },
    string(text) {
      const bytes = new TextEncoder().encode(`${text}\0`);
      const pointer = allocate(bytes.length);
      module.HEAPU8.set(bytes, pointer);
      return pointer;
    },
  };
  try {
    return body(scope);
  } finally {
    for (const pointer of allocations) module._free(pointer);
  }
}

/** Reads the NUL-terminated UTF-8 string at `pointer`. */
export function readCString(module, pointer) {
  if (!pointer) return "";
  let end = pointer;
  while (module.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(module.HEAPU8.subarray(pointer, end));
}

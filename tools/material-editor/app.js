/**
 * Pictor Material Editor — a static, server-less view over the
 * canonical material JSON emitted by `to_material_json()`. Opens
 * via `file://` or any static server. State lives entirely in
 * the URL hash (so links round-trip between editors).
 */

import { CATALOG, MATERIAL_FEATURE, defaultMaterial, serialize, deserialize } from "./schema.js";

const statusEl  = document.getElementById("status");
const jsonOutEl = document.getElementById("json-out");

const state = {
  name: "",
  desc: defaultMaterial(),
};

// ───────────────────────────────────────────────────────────────
// Small helpers
// ───────────────────────────────────────────────────────────────

function setStatus(text, kind = "") {
  statusEl.textContent = text;
  statusEl.className = "status" + (kind ? " " + kind : "");
}
function el(tag, attrs = {}, ...children) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class")     n.className = v;
    else if (k === "text") n.textContent = v;
    else if (k.startsWith("on") && typeof v === "function") {
      n.addEventListener(k.slice(2).toLowerCase(), v);
    } else {
      n.setAttribute(k, v);
    }
  }
  for (const c of children) {
    if (c == null) continue;
    n.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
  }
  return n;
}

function clamp01(v) { return Math.max(0, Math.min(1, v)); }

function rgbToHex(r, g, b) {
  const to = (v) => Math.round(clamp01(v) * 255).toString(16).padStart(2, "0");
  return "#" + to(r) + to(g) + to(b);
}
function hexToRgb(hex) {
  const m = /^#([0-9a-f]{6})$/i.exec(hex);
  if (!m) return [1, 1, 1];
  const n = parseInt(m[1], 16);
  return [((n >> 16) & 0xff) / 255, ((n >> 8) & 0xff) / 255, (n & 0xff) / 255];
}

// ───────────────────────────────────────────────────────────────
// Feature flag auto-computation (mirrors BaseMaterialBuilder)
// ───────────────────────────────────────────────────────────────

function computeFeatures(d) {
  let f = 0;
  if (d.textures.albedo    !== "none") f |= MATERIAL_FEATURE.ALBEDO_MAP;
  if (d.textures.normal    !== "none") f |= MATERIAL_FEATURE.NORMAL_MAP;
  if (d.textures.metallic  !== "none") f |= MATERIAL_FEATURE.METALLIC_MAP;
  if (d.textures.roughness !== "none") f |= MATERIAL_FEATURE.ROUGHNESS_MAP;
  if (d.textures.ao        !== "none") f |= MATERIAL_FEATURE.AO_MAP;
  if (d.textures.emissive  !== "none") f |= MATERIAL_FEATURE.EMISSIVE_MAP;
  if (d.params.alpha_cutoff > 0)       f |= MATERIAL_FEATURE.ALPHA_TEST;
  if (d.flags.cast_shadow)             f |= MATERIAL_FEATURE.CAST_SHADOW;
  if (d.flags.receive_shadow)          f |= MATERIAL_FEATURE.RECEIVE_SHADOW;
  return f;
}

// ───────────────────────────────────────────────────────────────
// Rendering
// ───────────────────────────────────────────────────────────────

function rebuild() {
  renderTextures();
  renderBaseColor();
  renderEmissive();
  renderScalars();
  renderFlags();
  renderJson();
}

function renderTextures() {
  const grid = document.getElementById("tex-grid");
  grid.innerHTML = "";
  for (const slot of CATALOG.textures) {
    const row = el("div", { class: "tex-slot" },
      el("label", { text: slot.label }),
      el("input", {
        type: "text",
        value: state.desc.textures[slot.key] ?? "none",
        placeholder: "none",
        onInput: (e) => {
          const v = e.target.value.trim() || "none";
          state.desc.textures[slot.key] = v;
          state.desc.flags.features = computeFeatures(state.desc);
          renderFlags();
          renderJson();
        },
      }),
    );
    grid.appendChild(row);
  }
}

function renderBaseColor() {
  const [r, g, b, a] = state.desc.params.base_color;
  document.getElementById("bc-picker").value = rgbToHex(r, g, b);
  document.getElementById("bc-r").value = r.toFixed(3);
  document.getElementById("bc-g").value = g.toFixed(3);
  document.getElementById("bc-b").value = b.toFixed(3);
  document.getElementById("bc-a").value = a.toFixed(3);
}
function renderEmissive() {
  const [r, g, b] = state.desc.params.emissive;
  // Emissive can exceed 1; picker still shows the clamped value.
  document.getElementById("em-picker").value = rgbToHex(
    clamp01(r), clamp01(g), clamp01(b));
  document.getElementById("em-r").value = r.toFixed(3);
  document.getElementById("em-g").value = g.toFixed(3);
  document.getElementById("em-b").value = b.toFixed(3);
}

function renderScalars() {
  const grid = document.getElementById("scalar-grid");
  grid.innerHTML = "";
  for (const sp of CATALOG.scalars) {
    const row = el("div", { class: "scalar-row" });
    row.appendChild(el("label", { text: sp.key }));
    const rng = el("input", {
      type:  "range",
      min:   sp.min.toString(),
      max:   sp.max.toString(),
      step:  sp.step.toString(),
      value: state.desc.params[sp.key].toString(),
    });
    const num = el("input", {
      type:  "number",
      min:   sp.min.toString(),
      max:   sp.max.toString(),
      step:  sp.step.toString(),
      value: state.desc.params[sp.key].toString(),
    });
    const bind = (src) => src.addEventListener("input", () => {
      const v = parseFloat(src.value);
      state.desc.params[sp.key] = Number.isFinite(v) ? v : 0;
      state.desc.flags.features = computeFeatures(state.desc);
      rng.value = state.desc.params[sp.key];
      num.value = state.desc.params[sp.key];
      renderFlags();
      renderJson();
    });
    bind(rng); bind(num);
    row.appendChild(rng);
    row.appendChild(num);
    grid.appendChild(row);
  }
}

function renderFlags() {
  document.getElementById("f-cast-shadow").checked    = state.desc.flags.cast_shadow;
  document.getElementById("f-receive-shadow").checked = state.desc.flags.receive_shadow;
  state.desc.flags.features = computeFeatures(state.desc);

  const chipsEl = document.getElementById("features-chips");
  chipsEl.innerHTML = "";
  for (const [name, bit] of Object.entries(MATERIAL_FEATURE)) {
    const on = (state.desc.flags.features & bit) !== 0;
    chipsEl.appendChild(el("span", { class: "chip" + (on ? " on" : ""), text: name.toLowerCase() }));
  }
}

function renderJson() {
  state.desc.flags.features = computeFeatures(state.desc);
  jsonOutEl.textContent = serialize(state.desc, state.name);
  saveStateToHash();
}

// ───────────────────────────────────────────────────────────────
// Base-color / emissive picker wiring (from-picker and from-num)
// ───────────────────────────────────────────────────────────────

function wireRgba(prefix, target, count /* 3 or 4 */) {
  const picker = document.getElementById(prefix + "-picker");
  picker.addEventListener("input", (e) => {
    const [r, g, b] = hexToRgb(e.target.value);
    target[0] = r; target[1] = g; target[2] = b;
    if (count === 4) target[3] = target[3] ?? 1;
    rebuild();
  });
  const keys = count === 4 ? ["r","g","b","a"] : ["r","g","b"];
  keys.forEach((k, i) => {
    const inp = document.getElementById(`${prefix}-${k}`);
    inp.addEventListener("input", () => {
      const v = parseFloat(inp.value);
      target[i] = Number.isFinite(v) ? v : 0;
      renderBaseColor(); renderEmissive(); renderJson();
    });
  });
}

// ───────────────────────────────────────────────────────────────
// Import / export / copy / new
// ───────────────────────────────────────────────────────────────

document.getElementById("btn-new").addEventListener("click", () => {
  state.name = "";
  state.desc = defaultMaterial();
  document.getElementById("f-name").value = "";
  rebuild();
  setStatus("reset", "ok");
});

document.getElementById("btn-export").addEventListener("click", () => {
  const blob = new Blob([serialize(state.desc, state.name)], { type: "application/json" });
  const a = el("a", {
    href:     URL.createObjectURL(blob),
    download: (state.name || "material") + ".json",
  });
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(a.href);
  setStatus("exported", "ok");
});

document.getElementById("btn-copy").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(jsonOutEl.textContent);
    setStatus("copied to clipboard", "ok");
  } catch (e) {
    setStatus("copy failed: " + e.message, "err");
  }
});

document.getElementById("input-import").addEventListener("change", async (e) => {
  const file = e.target.files?.[0];
  if (!file) return;
  try {
    const text = await file.text();
    const { name, desc } = deserialize(text);
    state.name = name ?? "";
    state.desc = desc;
    document.getElementById("f-name").value = state.name;
    rebuild();
    setStatus("imported " + file.name, "ok");
  } catch (err) {
    setStatus("import failed: " + err.message, "err");
  }
});

document.getElementById("f-name").addEventListener("input", (e) => {
  state.name = e.target.value;
  renderJson();
});
document.getElementById("f-cast-shadow").addEventListener("input", (e) => {
  state.desc.flags.cast_shadow = e.target.checked;
  renderJson();
});
document.getElementById("f-receive-shadow").addEventListener("input", (e) => {
  state.desc.flags.receive_shadow = e.target.checked;
  renderJson();
});

// ───────────────────────────────────────────────────────────────
// URL-hash persistence (base64-encoded JSON so links round-trip)
// ───────────────────────────────────────────────────────────────

function saveStateToHash() {
  try {
    const raw = serialize(state.desc, state.name);
    const b64 = btoa(unescape(encodeURIComponent(raw)));
    history.replaceState(null, "", "#" + b64);
  } catch (_) { /* ignore — hash is best-effort */ }
}
function loadStateFromHash() {
  const h = location.hash.replace(/^#/, "");
  if (!h) return false;
  try {
    const raw = decodeURIComponent(escape(atob(h)));
    const { name, desc } = deserialize(raw);
    state.name = name ?? "";
    state.desc = desc;
    document.getElementById("f-name").value = state.name;
    return true;
  } catch (_) {
    return false;
  }
}

// ───────────────────────────────────────────────────────────────
// Boot
// ───────────────────────────────────────────────────────────────

wireRgba("bc", state.desc.params.base_color, 4);
wireRgba("em", state.desc.params.emissive,   3);
if (loadStateFromHash()) {
  setStatus("restored from URL", "ok");
} else {
  setStatus("ready", "ok");
}
rebuild();

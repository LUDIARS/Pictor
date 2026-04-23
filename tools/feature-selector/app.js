/// Pictor Feature Selector UI.
///
/// Single-page static tool. Opens via `file://` or any static server. The
/// build-system pieces (CMake command, size estimate) are computed
/// client-side from `features.js`. No backend.
///
/// State model:
///   - On page load we derive the initial state from (URL hash | defaults).
///   - Every toggle updates `state`, re-renders the right pane, and
///     pushes a new URL hash so the page can be bookmarked / shared.
///   - Presets just write a known settings map into state and trigger a
///     full re-render.

import { FEATURES, PRESETS } from "./features.js";

const featuresRoot = document.getElementById("features-root");
const presetRow    = document.getElementById("preset-row");
const presetDesc   = document.getElementById("preset-desc");
const sizeFill     = document.getElementById("size-bar-fill");
const sizeText     = document.getElementById("size-bar-text");
const breakdown    = document.getElementById("breakdown-body");
const cmakeCmd     = document.getElementById("cmake-cmd");
const shareUrl     = document.getElementById("share-url");
const copyCmdBtn   = document.getElementById("copy-cmd");
const copyEnvBtn   = document.getElementById("copy-env");
const copyUrlBtn   = document.getElementById("copy-url");
const resetBtn     = document.getElementById("reset-btn");

// ─── state ───────────────────────────────────────────────────

/** id -> "ON" | "OFF" */
let state = {};

function allFeatureEntries() {
    return FEATURES.categories.flatMap((c) => c.items);
}

function defaultState() {
    const out = {};
    for (const it of allFeatureEntries()) out[it.id] = it.default;
    return out;
}

function stateFromHash() {
    if (!location.hash || location.hash.length < 2) return null;
    try {
        const params = new URLSearchParams(location.hash.slice(1));
        const out = {};
        for (const it of allFeatureEntries()) {
            const v = params.get(it.id);
            if (v === "1" || v === "ON")  out[it.id] = "ON";
            else if (v === "0" || v === "OFF") out[it.id] = "OFF";
            else out[it.id] = it.default;
        }
        return out;
    } catch { return null; }
}

function writeHash() {
    const params = new URLSearchParams();
    for (const it of allFeatureEntries()) {
        params.set(it.id, state[it.id] === "ON" ? "1" : "0");
    }
    // Replace so bookmarking works without spamming history.
    history.replaceState(null, "", "#" + params.toString());
    shareUrl.value = location.href;
}

// ─── render: left pane (features) ────────────────────────────

function renderFeatures() {
    featuresRoot.innerHTML = "";
    const tplCat = document.getElementById("tpl-category");
    const tplIt  = document.getElementById("tpl-feature");

    for (const cat of FEATURES.categories) {
        const catEl = tplCat.content.firstElementChild.cloneNode(true);
        catEl.querySelector(".group-title").textContent = cat.label;
        catEl.querySelector(".group-desc").textContent  = cat.desc;
        const listEl = catEl.querySelector(".feature-list");

        for (const it of cat.items) {
            const liEl = tplIt.content.firstElementChild.cloneNode(true);
            const chk  = liEl.querySelector(".feature-toggle");
            chk.checked = (state[it.id] === "ON");
            chk.addEventListener("change", () => {
                state[it.id] = chk.checked ? "ON" : "OFF";
                liEl.classList.toggle("on", chk.checked);
                updateAllDerived();
            });
            liEl.classList.toggle("on", chk.checked);
            if (it.affects_exe_only) liEl.classList.add("affects-exe-only");

            liEl.querySelector(".feature-name").textContent = it.label;
            liEl.querySelector(".feature-id").textContent   = it.id;

            const delta = it.size_delta_kb ?? 0;
            const sizeEl = liEl.querySelector(".feature-size");
            sizeEl.textContent = delta === 0
                ? "0 KB (exe only)"
                : "+" + formatSize(delta);

            liEl.querySelector(".feature-desc").textContent = it.desc;

            const meta = liEl.querySelector(".feature-meta");
            if (it.sources && it.sources.length) {
                for (const s of it.sources) {
                    const chip = document.createElement("span");
                    chip.className = "chip";
                    chip.textContent = "src: " + s;
                    meta.appendChild(chip);
                }
            }
            if (it.deps && it.deps.length) {
                for (const d of it.deps) {
                    const chip = document.createElement("span");
                    chip.className = "chip";
                    chip.textContent = "dep: " + d;
                    meta.appendChild(chip);
                }
            }
            if (it.affects_exe_only) {
                const note = document.createElement("span");
                note.className = "exe-only-note";
                note.textContent = "libpictor サイズに影響しません";
                meta.appendChild(note);
            }
            listEl.appendChild(liEl);
        }
        featuresRoot.appendChild(catEl);
    }
}

// ─── render: right pane (presets / size / cmake) ─────────────

function renderPresets() {
    presetRow.innerHTML = "";
    for (const p of PRESETS) {
        const btn = document.createElement("button");
        btn.className = "preset";
        btn.textContent = p.label;
        btn.title = p.desc;
        btn.addEventListener("click", () => applyPreset(p));
        presetRow.appendChild(btn);
    }
}

function applyPreset(preset) {
    for (const [k, v] of Object.entries(preset.settings)) state[k] = v;
    presetDesc.textContent = preset.desc + "  [preset: " + preset.label + "]";
    renderFeatures();
    updateAllDerived();
    highlightActivePreset();
}

function currentActivePresetId() {
    for (const p of PRESETS) {
        let ok = true;
        for (const [k, v] of Object.entries(p.settings)) {
            if (state[k] !== v) { ok = false; break; }
        }
        if (ok) return p.id;
    }
    return null;
}

function highlightActivePreset() {
    const active = currentActivePresetId();
    for (const btn of presetRow.querySelectorAll("button.preset")) {
        const label = btn.textContent.trim();
        const preset = PRESETS.find((p) => p.label === label);
        btn.classList.toggle("active", preset && preset.id === active);
    }
    if (!active) presetDesc.textContent = "カスタム構成中 (preset 未適用)。";
}

function computeTotalSizeKb() {
    let total = FEATURES.base_size_kb;
    for (const it of allFeatureEntries()) {
        if (it.affects_exe_only) continue;
        if (state[it.id] === "ON") total += (it.size_delta_kb ?? 0);
    }
    return total;
}

function formatSize(kb) {
    if (kb >= 1024) return (kb / 1024).toFixed(1) + " MB";
    return kb + " KB";
}

function renderSize() {
    const total = computeTotalSizeKb();
    // Normalise against "kitchen sink" maximum for the bar width. We
    // recompute the max from the manifest so new features update the bar.
    let maxKb = FEATURES.base_size_kb;
    for (const it of allFeatureEntries()) {
        if (!it.affects_exe_only) maxKb += (it.size_delta_kb ?? 0);
    }
    const pct = Math.max(5, Math.min(100, Math.round((total / maxKb) * 100)));
    sizeFill.style.width = pct + "%";
    sizeText.textContent = formatSize(total) + "  (" + pct + "% of max " + formatSize(maxKb) + ")";

    breakdown.innerHTML = "";
    const addRow = (label, kb, cls = "") => {
        const tr = document.createElement("tr");
        if (cls) tr.className = cls;
        const td1 = document.createElement("td"); td1.textContent = label;
        const td2 = document.createElement("td"); td2.textContent = (kb >= 0 ? "+" : "") + formatSize(Math.abs(kb));
        tr.appendChild(td1); tr.appendChild(td2);
        breakdown.appendChild(tr);
    };
    addRow("base (core)", FEATURES.base_size_kb);
    for (const it of allFeatureEntries()) {
        if (it.affects_exe_only) continue;
        if (state[it.id] === "ON" && (it.size_delta_kb ?? 0) !== 0) {
            addRow(it.label, it.size_delta_kb);
        }
    }
    const totalTr = document.createElement("tr");
    totalTr.style.fontWeight = "600";
    const td1 = document.createElement("td"); td1.textContent = "Total (libpictor)";
    const td2 = document.createElement("td"); td2.textContent = formatSize(total);
    totalTr.appendChild(td1); totalTr.appendChild(td2);
    breakdown.appendChild(totalTr);
}

function renderCmakeCmd() {
    // Emit every option explicitly so the invocation is reproducible even
    // if the defaults change later.
    const parts = ["cmake -S . -B build"];
    for (const it of allFeatureEntries()) {
        parts.push("  -D" + it.id + "=" + state[it.id]);
    }
    cmakeCmd.value = parts.join(" \\\n");
}

function cmakeEnvOnly() {
    // Alternate representation: `ENV=... cmake ...` — occasionally useful
    // in CI scripts that expect env-style config.
    const envs = allFeatureEntries().map((it) => it.id + "=" + state[it.id]).join(" ");
    return "env " + envs + " cmake -S . -B build";
}

// ─── coordination ─────────────────────────────────────────────

function updateAllDerived() {
    renderSize();
    renderCmakeCmd();
    writeHash();
    highlightActivePreset();
}

// ─── copy / reset handlers ────────────────────────────────────

async function copyText(text, btn) {
    try {
        await navigator.clipboard.writeText(text);
        const prev = btn.textContent;
        btn.textContent = "✓ Copied";
        setTimeout(() => (btn.textContent = prev), 1200);
    } catch {
        // Fallback — select the relevant input and prompt.
        prompt("Copy failed. Copy manually:", text);
    }
}

copyCmdBtn.addEventListener("click", () => copyText(cmakeCmd.value, copyCmdBtn));
copyEnvBtn.addEventListener("click", () => copyText(cmakeEnvOnly(), copyEnvBtn));
copyUrlBtn.addEventListener("click", () => copyText(shareUrl.value, copyUrlBtn));

resetBtn.addEventListener("click", () => {
    state = defaultState();
    renderFeatures();
    updateAllDerived();
});

// ─── boot ─────────────────────────────────────────────────────

state = stateFromHash() ?? defaultState();
renderPresets();
renderFeatures();
updateAllDerived();

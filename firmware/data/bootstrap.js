const originalSetInterval = window.setInterval.bind(window);
const originalFetch = window.fetch.bind(window);

const normalizeApiUrl = (inputUrl) => {
  if (typeof inputUrl !== "string") return inputUrl;
  try {
    const u = new URL(inputUrl, window.location.href);
    if (u.origin !== window.location.origin) return inputUrl;
    if (u.pathname.startsWith("/api/api/")) {
      u.pathname = u.pathname.replace("/api/api/", "/api/");
      return u.toString();
    }
    if (u.pathname.startsWith("/settings")) {
      return inputUrl;
    }
    return inputUrl;
  } catch {
    if (inputUrl.startsWith("/api/api/")) return inputUrl.replace("/api/api/", "/api/");
    return inputUrl;
  }
};

const ensureToast = () => {
  let el = document.getElementById("save-toast");
  if (el) return el;
  el = document.createElement("div");
  el.id = "save-toast";
  el.setAttribute("role", "status");
  el.style.position = "fixed";
  el.style.left = "50%";
  el.style.bottom = "calc(18px + env(safe-area-inset-bottom, 0px))";
  el.style.maxWidth = "calc(100vw - 24px)";
  el.style.textAlign = "center";
  el.style.transform = "translateX(-50%)";
  el.style.zIndex = "99999";
  el.style.padding = "10px 14px";
  el.style.borderRadius = "10px";
  el.style.font = "600 14px/1.2 system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif";
  el.style.boxShadow = "0 8px 28px rgba(0,0,0,0.35)";
  el.style.opacity = "0";
  el.style.pointerEvents = "none";
  el.style.transition = "opacity 160ms ease, transform 160ms ease";
  el.style.whiteSpace = "nowrap";
  document.body.appendChild(el);
  return el;
};

let toastTimer = 0;
const showToast = (message, ok) => {
  const el = ensureToast();
  el.textContent = message;
  el.style.background = ok ? "rgba(24, 138, 68, 0.92)" : "rgba(178, 36, 36, 0.92)";
  el.style.color = "#fff";
  el.style.opacity = "1";
  el.style.transform = "translateX(-50%) translateY(0)";
  if (toastTimer) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    el.style.opacity = "0";
    el.style.transform = "translateX(-50%) translateY(6px)";
  }, 1800);
};

window.setInterval = (handler, timeout, ...args) => {
  if (typeof handler === "function") {
    try {
      const src = handler.toString();
      const onSettingsPage = window.location.pathname.startsWith("/settings");
      if (!onSettingsPage && src.includes("/status") && Number(timeout) >= 1000) {
        timeout = 100;
      }
    } catch {
    }
  }

  return originalSetInterval(handler, timeout, ...args);
};

window.fetch = async (input, init) => {
  const req = input instanceof Request ? input : null;
  const url = typeof input === "string" ? input : (req ? req.url : "");
  const method = (init && init.method) || (req && req.method) || "GET";

  if (typeof input === "string") {
    input = normalizeApiUrl(input);
  } else if (req) {
    const nu = normalizeApiUrl(req.url);
    if (nu && nu !== req.url) input = new Request(nu, req);
  }

  const safeInit = sanitizeSettingsBody(url, method, init);
  const res = await originalFetch(input, safeInit);

  if (method.toUpperCase() === "DELETE" && url.includes("/api/schedules")) {
    if (res.ok) {
      setTimeout(() => {
        window.location.reload();
      }, 50);
    }
  }

  if (["POST", "PUT", "PATCH"].includes(method.toUpperCase())) {
    const isSettings = url.includes("/api/settings") || url.endsWith("/settings");
    const isOtherSettings = url.includes("/api/fan") || url.endsWith("/fan") || url.includes("/api/pid") || url.endsWith("/pid") || url.includes("/api/display") || url.endsWith("/display") || url.includes("/api/remote") || url.endsWith("/remote") || url.includes("/api/touch/") || url.includes("/touch/");
    if (isSettings || isOtherSettings) {
      if (res.ok) {
        showToast("\u0417\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043e", true);
      } else {
        showToast("\u041f\u043e\u043c\u0438\u043b\u043a\u0430 \u0437\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043d\u044f", false);
      }
    }
  }

  return res;
};

(() => {
  const origRemoveChild = Node.prototype.removeChild;
  Node.prototype.removeChild = function (child) {
    try {
      return origRemoveChild.call(this, child);
    } catch (e) {
      if (e && (e.name === "NotFoundError" || String(e).includes("NotFoundError"))) return child;
      throw e;
    }
  };
})();

(() => {
  const XHR = window.XMLHttpRequest;
  if (!XHR || !XHR.prototype) return;
  const origOpen = XHR.prototype.open;
  const origSend = XHR.prototype.send;

  XHR.prototype.open = function (method, url, ...rest) {
    try {
      this.__kiln_method = String(method || "GET").toUpperCase();
      const su = String(url || "");
      const nu = normalizeApiUrl(su);
      this.__kiln_url = nu;
      url = nu;
    } catch {
    }
    return origOpen.call(this, method, url, ...rest);
  };

  XHR.prototype.send = function (...args) {
    try {
      this.addEventListener("loadend", () => {
        try {
          const method = this.__kiln_method || "GET";
          const url = this.__kiln_url || "";
          if (!["POST", "PUT", "PATCH"].includes(method)) return;
          const isSettings = url.includes("/api/settings") || url.endsWith("/settings");
          const isOtherSettings = url.includes("/api/fan") || url.endsWith("/fan") || url.includes("/api/pid") || url.endsWith("/pid") || url.includes("/api/display") || url.endsWith("/display") || url.includes("/api/remote") || url.endsWith("/remote") || url.includes("/api/touch/") || url.includes("/touch/");
          if (!isSettings && !isOtherSettings) return;
          const ok = this.status >= 200 && this.status < 300;
          showToast(ok ? "\u0417\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043e" : "\u041f\u043e\u043c\u0438\u043b\u043a\u0430 \u0437\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043d\u044f", ok);
        } catch {
        }
      });
    } catch {
    }
    return origSend.apply(this, args);
  };
})();

const html = await (await originalFetch("/index.html", { cache: "no-store" })).text();
const m = html.match(/src="(\/assets\/index-[^"]+\.js)"/);
const appModulePath = m ? m[1] : "/assets/index-f98ef54f.js";
await import(`${appModulePath}?v=${Date.now()}`);

const parseLocaleNumber = (raw) => {
  const s = String(raw ?? "").trim().replace(",", ".");
  const n = parseFloat(s);
  return Number.isFinite(n) ? n : null;
};

const isThermocoupleTitle = (text) => {
  const title = String(text || "").trim().toLowerCase();
  return title.includes("\u0442\u0435\u0440\u043c\u043e\u043f\u0430\u0440") || title.includes("thermocouple");
};

const findThermocoupleCardByInput = (inputEl) => {
  let card = inputEl ? inputEl.parentElement : null;
  while (card && card !== document.body) {
    const titleEl = card.querySelector ? card.querySelector("h3") : null;
    if (!titleEl || !isThermocoupleTitle(titleEl.textContent || "")) {
      card = card.parentElement;
      continue;
    }
    const inputs = card.querySelectorAll ? card.querySelectorAll('input[type="number"]') : [];
    const hasExactInput = Array.from(inputs).includes(inputEl);
    if (hasExactInput && inputs.length <= 2) {
      return { card, titleEl, inputEl };
    }
    card = card.parentElement;
  }
  return null;
};

const findThermocoupleCardByButton = (buttonEl) => {
  let card = buttonEl ? buttonEl.parentElement : null;
  while (card && card !== document.body) {
    const titleEl = card.querySelector ? card.querySelector("h3") : null;
    if (!titleEl || !isThermocoupleTitle(titleEl.textContent || "")) {
      card = card.parentElement;
      continue;
    }
    const inputs = card.querySelectorAll ? card.querySelectorAll('input[type="number"]') : [];
    if (inputs.length < 1 || inputs.length > 2) {
      card = card.parentElement;
      continue;
    }
    const buttons = card.querySelectorAll ? card.querySelectorAll("button") : [];
    const hasSave = Array.from(buttons).some((b) => {
      const t = (b.textContent || "").trim().toLowerCase();
      return t === "\u0437\u0431\u0435\u0440\u0435\u0433\u0442\u0438" || t === "save";
    });
    if (hasSave) return { card, titleEl, inputEl: inputs[0] };
    card = card.parentElement;
  }
  return null;
};

const postSettings = async (payload, showResultToast = true) => {
  try {
    const res = await originalFetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    if (showResultToast && res.ok) {
      showToast("\u0417\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043e", true);
    } else if (showResultToast) {
      showToast("\u041f\u043e\u043c\u0438\u043b\u043a\u0430 \u0437\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043d\u044f", false);
    }
    return res.ok;
  } catch {
    if (showResultToast) showToast("\u041f\u043e\u043c\u0438\u043b\u043a\u0430 \u0437\u0431\u0435\u0440\u0435\u0436\u0435\u043d\u043d\u044f", false);
    return false;
  }
};

const sanitizeSettingsBody = (url, method, init) => {
  const upperMethod = String(method || "GET").toUpperCase();
  if (!["POST", "PUT", "PATCH"].includes(upperMethod)) return init;
  const isSettings = String(url || "").includes("/api/settings") || String(url || "").endsWith("/settings");
  if (!isSettings || !init || typeof init.body !== "string") return init;
  try {
    const parsed = JSON.parse(init.body);
    if (!parsed || typeof parsed !== "object") return init;
    const hasMax = Number.isFinite(Number(parsed.maxC));
    if (hasMax && Number.isFinite(Number(parsed.offset)) && Math.abs(Number(parsed.offset)) > 100) {
      delete parsed.offset;
    }
    if (hasMax && Number.isFinite(Number(parsed.temp_offset_c)) && Math.abs(Number(parsed.temp_offset_c)) > 100) {
      delete parsed.temp_offset_c;
    }
    return { ...init, body: JSON.stringify(parsed) };
  } catch {
    return init;
  }
};

const fixDegreeSymbols = () => {
  const nodes = document.querySelectorAll("h3, label, p, span");
  for (const n of nodes) {
    const t = n.textContent || "";
    if (!t) continue;
    const needsFix = t.includes("Зміщення термопари") || t.includes("Thermocouple Offset") || t.includes("піч нагріється до") || t.includes("kiln will heat to") || t.includes("УВАГА:") || t.includes("WARNING:");
    if (!needsFix) continue;
    const fixed = t.replace("(�C)", "(°C)").replace("(Â°C)", "(°C)").replace("�C", "°C").replace("Â°C", "°C");
    if (fixed !== t) n.textContent = fixed;
  }
};

const AUTOTUNE_SAVE_BTN_ID = "autotune-target-save-btn";
let autotuneTargetFromController = null;
let autotuneSettingsFetchInFlight = false;

const parseNumberInput = (raw) => {
  const v = String(raw ?? "").trim().replace(",", ".");
  const n = parseFloat(v);
  return Number.isFinite(n) ? n : null;
};

const clampAutotuneTarget = (v) => Math.max(1, Math.min(1300, v));

const detectUiLang = () => {
  const htmlLang = (document.documentElement.lang || "").toLowerCase();
  if (htmlLang.startsWith("uk")) return "uk";
  const text = document.body ? (document.body.innerText || "") : "";
  if (text.includes("Зберегти") || text.includes("Увага") || text.includes("Налаштування") || /[іїєґІЇЄҐ]/.test(text)) return "uk";
  return "en";
};

const readAutotuneTargetFromSettingsJson = (json) => {
  if (!json || typeof json !== "object") return null;
  const candidates = [json.autotune_target_c, json.autotuneTargetC];
  for (const c of candidates) {
    const n = parseNumberInput(c);
    if (n !== null) return clampAutotuneTarget(n);
  }
  return null;
};

const fetchAutotuneTargetFromController = async () => {
  if (autotuneSettingsFetchInFlight) return autotuneTargetFromController;
  autotuneSettingsFetchInFlight = true;
  try {
    const res = await originalFetch("/api/settings", { cache: "no-store" });
    if (!res.ok) return autotuneTargetFromController;
    const json = await res.json();
    const parsed = readAutotuneTargetFromSettingsJson(json);
    if (parsed !== null) autotuneTargetFromController = parsed;
    return autotuneTargetFromController;
  } catch {
    return autotuneTargetFromController;
  } finally {
    autotuneSettingsFetchInFlight = false;
  }
};

const saveAutotuneTargetToController = async (target, lang) => {
  const ok = await postSettings({ autotune_target_c: target }, false);
  if (ok) {
    autotuneTargetFromController = target;
    showToast(lang === "uk" ? "Збережено" : "Saved", true);
  } else {
    showToast(lang === "uk" ? "Помилка збереження" : "Save error", false);
  }
  return ok;
};

const findAutotuneInput = () => document.querySelector('input[type="number"][placeholder*="600"]');

const applyAutotuneInputValue = (input, value) => {
  const str = Number(value).toString();
  const proto = Object.getPrototypeOf(input);
  const desc = Object.getOwnPropertyDescriptor(proto, "value");
  if (desc && typeof desc.set === "function") {
    desc.set.call(input, str);
  } else {
    input.value = str;
  }
  input.dispatchEvent(new Event("input", { bubbles: true }));
};

const updateAutotuneWarningText = () => {
  const input = findAutotuneInput();
  if (!input) return;
  const val = parseNumberInput(input.value);
  const temp = val === null ? 600 : Math.round(val);
  const paragraphs = document.querySelectorAll("p");
  for (const p of paragraphs) {
    const t = (p.textContent || "").trim();
    if (!t) continue;
    if (t.includes("піч нагріється до")) {
      p.textContent = `Увага: піч нагріється до ${temp}°C. Переконайтеся, що піч порожня.`;
    } else if (t.toLowerCase().includes("kiln will heat to")) {
      p.textContent = `Warning: kiln will heat to ${temp}°C. Make sure the kiln is empty.`;
    }
  }
};

const syncAutotuneInputFromSaved = () => {
  const input = findAutotuneInput();
  if (!input) return;
  if (document.activeElement === input) return;
  const saved = autotuneTargetFromController;
  if (saved === null) return;
  const current = parseNumberInput(input.value);
  if (current === null || Math.abs(current - saved) > 0.0001) {
    applyAutotuneInputValue(input, saved);
  }
};

const ensureAutotuneSaveButton = () => {
  const input = findAutotuneInput();
  if (!input) return;
  const lang = detectUiLang();
  const existingBtn = document.getElementById(AUTOTUNE_SAVE_BTN_ID);
  if (existingBtn) {
    existingBtn.textContent = lang === "uk" ? "Зберегти" : "Save";
  }
  if (input.dataset.autotuneSaveBound === "1") {
    updateAutotuneWarningText();
    return;
  }
  const parent = input.parentElement;
  if (!parent || !parent.parentElement) return;
  const wrapper = parent.parentElement;
  const saveBtn = document.createElement("button");
  saveBtn.id = AUTOTUNE_SAVE_BTN_ID;
  saveBtn.type = "button";
  saveBtn.textContent = lang === "uk" ? "Зберегти" : "Save";
  saveBtn.className = "w-full sm:w-auto px-4 py-3 rounded-lg bg-zinc-800 border border-zinc-700 text-white hover:bg-zinc-700 transition-colors";
  saveBtn.addEventListener("click", async () => {
    const n = parseNumberInput(input.value);
    if (n === null) {
      showToast(detectUiLang() === "uk" ? "Невірне число" : "Invalid number", false);
      return;
    }
    const langNow = detectUiLang();
    const clamped = clampAutotuneTarget(n);
    applyAutotuneInputValue(input, clamped);
    await saveAutotuneTargetToController(clamped, langNow);
    updateAutotuneWarningText();
  });
  wrapper.appendChild(saveBtn);
  input.dataset.autotuneSaveBound = "1";

  fetchAutotuneTargetFromController().then(() => {
    syncAutotuneInputFromSaved();
    updateAutotuneWarningText();
  });
  input.addEventListener("input", () => {
    updateAutotuneWarningText();
  }, { passive: true });
  updateAutotuneWarningText();
};

const hideRemoteAccessSection = () => {
  const headings = document.querySelectorAll("h2");
  for (const h of headings) {
    const t = (h.textContent || "").trim().toLowerCase();
    if (!t) continue;
    if (!t.includes("remote access") && !t.includes("віддалений доступ")) continue;
    let card = h.parentElement;
    while (card && card !== document.body) {
      const cls = (card.className || "").toString();
      if (cls.includes("bg-kiln-card")) {
        card.style.display = "none";
        break;
      }
      card = card.parentElement;
    }
  }
};

const downloadJsonFile = (name, payload) => {
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
};

const fetchJsonNoStore = async (url) => {
  const r = await originalFetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
};

const ensureSupportToolsPanel = () => {
  let panel = document.getElementById("support-tools-panel");
  const lang = detectUiLang();
  const findSettingsCard = () => {
    const heads = document.querySelectorAll("h2");
    for (const h of heads) {
      const t = (h.textContent || "").trim().toLowerCase();
      if (!t) continue;
      if (!t.includes("settings") && !t.includes("налаштування")) continue;
      let card = h.parentElement;
      while (card && card !== document.body) {
        const cls = (card.className || "").toString();
        if (cls.includes("bg-kiln-card")) return card;
        card = card.parentElement;
      }
    }
    return null;
  };
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "support-tools-panel";
    panel.style.marginTop = "12px";
    panel.style.display = "flex";
    panel.style.gap = "8px";
    panel.style.flexWrap = "wrap";
    panel.style.justifyContent = "flex-start";
    panel.style.maxWidth = "100%";

    const mkBtn = (id, bg) => {
      const b = document.createElement("button");
      b.id = id;
      b.type = "button";
      b.style.border = "1px solid #334155";
      b.style.borderRadius = "10px";
      b.style.padding = "8px 10px";
      b.style.font = "600 12px/1.2 system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif";
      b.style.color = "#fff";
      b.style.background = bg;
      b.style.cursor = "pointer";
      b.style.whiteSpace = "nowrap";
      return b;
    };

    const exportBtn = mkBtn("btn-backup-export", "#1d4ed8");
    const importBtn = mkBtn("btn-backup-import", "#15803d");
    const diagBtn = mkBtn("btn-diag-export", "#7c3aed");
    const fileInput = document.createElement("input");
    fileInput.id = "backup-import-file";
    fileInput.type = "file";
    fileInput.accept = ".json,application/json";
    fileInput.style.display = "none";

    exportBtn.addEventListener("click", async () => {
      try {
        const json = await fetchJsonNoStore("/api/backup");
        const ts = Date.now();
        downloadJsonFile(`trae-backup-${ts}.json`, json);
        showToast(lang === "uk" ? "Backup експортовано" : "Backup exported", true);
      } catch {
        showToast(lang === "uk" ? "Помилка експорту backup" : "Backup export failed", false);
      }
    });

    importBtn.addEventListener("click", () => {
      fileInput.click();
    });

    fileInput.addEventListener("change", async () => {
      const f = fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
      fileInput.value = "";
      if (!f) return;
      try {
        const text = await f.text();
        const res = await originalFetch("/api/backup/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: text,
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const j = await res.json();
        const ok = !!(j && j.ok);
        showToast(ok ? (lang === "uk" ? "Backup імпортовано" : "Backup imported")
                     : (lang === "uk" ? "Backup імпорт частковий/порожній" : "Backup import partial/empty"), ok);
      } catch {
        showToast(lang === "uk" ? "Помилка імпорту backup" : "Backup import failed", false);
      }
    });

    diagBtn.addEventListener("click", async () => {
      try {
        const json = await fetchJsonNoStore("/api/diagnostics/bundle");
        const ts = Date.now();
        downloadJsonFile(`trae-diagnostic-bundle-${ts}.json`, json);
        showToast(lang === "uk" ? "Diagnostic bundle експортовано" : "Diagnostic bundle exported", true);
      } catch {
        showToast(lang === "uk" ? "Помилка експорту diagnostic bundle" : "Diagnostic export failed", false);
      }
    });

    panel.appendChild(exportBtn);
    panel.appendChild(importBtn);
    panel.appendChild(diagBtn);
    panel.appendChild(fileInput);
  }
  panel.style.position = "static";
  panel.style.right = "";
  panel.style.bottom = "";
  panel.style.left = "";
  panel.style.top = "";
  panel.style.zIndex = "";
  panel.style.pointerEvents = "auto";

  const hostCard = findSettingsCard();
  if (hostCard) {
    if (panel.parentElement !== hostCard) {
      hostCard.appendChild(panel);
    }
    panel.style.display = "flex";
  } else if (!panel.parentElement) {
    document.body.appendChild(panel);
    panel.style.display = "none";
  } else if (panel.parentElement === document.body) {
    panel.style.display = "none";
  } else {
    panel.style.display = "none";
  }

  const exportBtn = document.getElementById("btn-backup-export");
  const importBtn = document.getElementById("btn-backup-import");
  const diagBtn = document.getElementById("btn-diag-export");
  if (exportBtn) exportBtn.textContent = lang === "uk" ? "Експорт backup" : "Export backup";
  if (importBtn) importBtn.textContent = lang === "uk" ? "Імпорт backup" : "Import backup";
  if (diagBtn) diagBtn.textContent = lang === "uk" ? "Експорт diagnostics" : "Export diagnostics";
};

const ensureKpiSummaryPanel = () => {
  const findSettingsCard = () => {
    const heads = document.querySelectorAll("h2");
    for (const h of heads) {
      const t = (h.textContent || "").trim().toLowerCase();
      if (!t) continue;
      if (!t.includes("settings") && !t.includes("налаштування")) continue;
      let card = h.parentElement;
      while (card && card !== document.body) {
        const cls = (card.className || "").toString();
        if (cls.includes("bg-kiln-card")) return card;
        card = card.parentElement;
      }
    }
    return null;
  };

  const host = findSettingsCard();
  let panel = document.getElementById("firing-kpi-panel");
  if (!host) {
    if (panel) panel.style.display = "none";
    return;
  }
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "firing-kpi-panel";
    panel.style.marginTop = "10px";
    panel.style.padding = "10px";
    panel.style.border = "1px solid #334155";
    panel.style.borderRadius = "10px";
    panel.style.background = "#0b1220";
    panel.style.color = "#e2e8f0";
    panel.style.font = "600 12px/1.35 system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif";
    host.appendChild(panel);
  } else if (panel.parentElement !== host) {
    host.appendChild(panel);
  }
  panel.style.display = "block";

  const lang = detectUiLang();
  const title = lang === "uk" ? "Останній firing KPI" : "Last firing KPI";
  panel.textContent = `${title}: ...`;

  fetchJsonNoStore("/api/history").then((list) => {
    const first = Array.isArray(list) && list.length > 0 ? list[0] : null;
    const kpi = first && first.kpi && typeof first.kpi === "object" ? first.kpi : null;
    if (!kpi) {
      panel.textContent = lang === "uk" ? `${title}: немає даних` : `${title}: no data`;
      return;
    }
    const q = Number(kpi.quality_score ?? 0).toFixed(0);
    const rms = Number(kpi.tracking_error_rms ?? 0).toFixed(1);
    const over = Number(kpi.overshoot_pct_max ?? 0).toFixed(1);
    const textUk = `${title}: Quality ${q}% | RMS ${rms}°C | Overshoot ${over}%`;
    const textEn = `${title}: Quality ${q}% | RMS ${rms}°C | Overshoot ${over}%`;
    panel.textContent = lang === "uk" ? textUk : textEn;
  }).catch(() => {
    panel.textContent = lang === "uk" ? `${title}: помилка читання` : `${title}: read error`;
  });
};

const postJson = async (url, bodyObj) => {
  const res = await originalFetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(bodyObj || {}),
  });
  let json = null;
  try {
    json = await res.json();
  } catch {
  }
  return { ok: res.ok, json };
};

const getStatusSafe = async () => {
  try {
    return await fetchJsonNoStore("/api/status");
  } catch {
    return null;
  }
};

let schedulesCacheAt = 0;
let schedulesCacheData = [];
const getSchedulesSafe = async () => {
  const now = Date.now();
  if (now - schedulesCacheAt < 3000 && Array.isArray(schedulesCacheData)) return schedulesCacheData;
  try {
    const json = await fetchJsonNoStore("/api/schedules");
    const arr = Array.isArray(json) ? json : (Array.isArray(json && json.schedules) ? json.schedules : []);
    schedulesCacheData = arr;
    schedulesCacheAt = now;
    return arr;
  } catch {
    return schedulesCacheData;
  }
};

const chooseLangText = (uk, en) => (detectUiLang() === "uk" ? uk : en);

const openDashboardPage = () => {
  const nav = Array.from(document.querySelectorAll("a")).find((a) => {
    const tt = (a.textContent || "").trim().toLowerCase();
    return tt.includes("панель") || tt.includes("dashboard");
  });
  if (nav) {
    nav.click();
    return true;
  }
  if (window.location.pathname !== "/") {
    window.history.pushState({}, "", "/");
    window.dispatchEvent(new PopStateEvent("popstate"));
    return true;
  }
  return false;
};

const findChartCard = () => {
  const heads = document.querySelectorAll("h2");
  for (const h of heads) {
    const t = (h.textContent || "").trim().toLowerCase();
    if (!t) continue;
    if (!t.includes("графік випалу") && !t.includes("firing graph")) continue;
    let card = h.parentElement;
    let best = null;
    while (card && card !== document.body) {
      const cls = (card.className || "").toString();
      if (cls.includes("bg-kiln-card")) {
        best = card;
        if (card.querySelector("canvas")) return card;
      }
      card = card.parentElement;
    }
    if (best) return best;
  }
  return null;
};

const findCardByHeading = (matcher) => {
  const all = Array.from(document.querySelectorAll("div"));
  return all.find((el) => {
    const cls = (el.className || "").toString();
    if (!cls.includes("bg-kiln-card")) return false;
    return matcher((el.textContent || "").trim().toLowerCase(), el);
  }) || null;
};

const ensureMovePlaceholder = (el) => {
  if (!el || el.dataset.kilnMoved === "1") return;
  const ph = document.createElement("div");
  ph.style.display = "none";
  const id = `kiln-ph-${Math.random().toString(36).slice(2, 10)}`;
  ph.dataset.kilnPlaceholder = id;
  el.dataset.kilnPlaceholder = id;
  el.dataset.kilnMoved = "1";
  el.parentElement && el.parentElement.insertBefore(ph, el);
};

const moveInto = (el, parent) => {
  if (!el || !parent) return;
  ensureMovePlaceholder(el);
  parent.appendChild(el);
};

const restoreMovedCards = () => {
  Array.from(document.querySelectorAll("[data-kiln-moved='1']")).forEach((el) => {
    const id = el.dataset.kilnPlaceholder;
    const ph = id ? document.querySelector(`[data-kiln-placeholder="${id}"]`) : null;
    if (ph && ph.parentElement) ph.parentElement.insertBefore(el, ph);
    if (ph) ph.remove();
    delete el.dataset.kilnPlaceholder;
    delete el.dataset.kilnMoved;
  });
  const wrap = document.getElementById("firing-dashboard-layout");
  if (wrap) wrap.remove();
  const lib = document.getElementById("firing-library-card");
  if (lib) {
    Array.from(lib.querySelectorAll("[data-kiln-lib-hidden='1']")).forEach((el) => {
      el.style.display = "";
      delete el.dataset.kilnLibHidden;
    });
    const title = lib.querySelector("h2");
    if (title && title.dataset.kilnOrigTitle) {
      title.innerHTML = title.dataset.kilnOrigTitle;
      delete title.dataset.kilnOrigTitle;
    }
  }
};

const arrangeFiringDashboard = (status, selectedProgramName, schedules) => {
  const useProgramLayout = !!selectedProgramName || !!(status && status.firing);
  if (!useProgramLayout) {
    restoreMovedCards();
    return;
  }
  const compactLayout = window.innerWidth < 960;
  const chart = findChartCard();
  if (!chart) return;
  let wrapper = document.getElementById("firing-dashboard-layout");
  let chartParent = chart.parentElement;
  if (wrapper && wrapper.contains(chart) && wrapper.parentElement) {
    chartParent = wrapper.parentElement;
  }
  if (!chartParent) return;

  if (!wrapper) {
    wrapper = document.createElement("div");
    wrapper.id = "firing-dashboard-layout";
    chartParent.insertBefore(wrapper, chart);
  } else if (wrapper.parentElement !== chartParent) {
    chartParent.insertBefore(wrapper, chartParent.firstChild);
  }
  wrapper.style.display = "grid";
  wrapper.style.gap = compactLayout ? "12px" : "24px";
  wrapper.style.alignItems = "start";
  wrapper.style.marginBottom = "12px";
  wrapper.style.width = "100%";
  wrapper.style.gridTemplateColumns = compactLayout
    ? "minmax(0, 1fr)"
    : "minmax(360px, 420px) minmax(0, 1fr)";

  let left = document.getElementById("firing-dashboard-left");
  let right = document.getElementById("firing-dashboard-right");
  if (!left) {
    left = document.createElement("div");
    left.id = "firing-dashboard-left";
    left.style.display = "grid";
    left.style.gap = "12px";
    left.style.alignItems = "stretch";
    wrapper.appendChild(left);
  }
  left.style.width = "100%";
  left.style.maxWidth = "100%";
  left.style.minWidth = "0";
  left.style.gridTemplateColumns = compactLayout
    ? "minmax(0, 1fr)"
    : "repeat(2, minmax(0, 1fr))";
  left.style.justifyItems = "stretch";
  if (!right) {
    right = document.createElement("div");
    right.id = "firing-dashboard-right";
    right.style.display = "flex";
    right.style.flexDirection = "column";
    right.style.gap = "12px";
    right.style.minWidth = "0";
    wrapper.appendChild(right);
  }
  right.style.width = "100%";

  const library = document.getElementById("firing-library-card") || findCardByHeading((text, el) => {
    const h2 = el.querySelector("h2");
    const t = (h2 && h2.textContent || "").trim().toLowerCase();
    return t.includes("бібліотека") || t.includes("library") || t.includes("обрана програма") || t.includes("selected program");
  });
  if (library) {
    library.id = "firing-library-card";
    library.className = "bg-kiln-card border border-kiln-border rounded-xl p-3 shadow-lg overflow-hidden";
    library.style.width = "100%";
    library.style.boxSizing = "border-box";
    library.style.display = "flex";
    library.style.flexDirection = "column";
    library.style.maxWidth = "100%";
    library.style.minWidth = "0";
    library.style.alignSelf = "stretch";
    library.style.justifySelf = "stretch";
    library.style.gridColumn = "1 / -1";
    library.style.marginLeft = "0";
    library.style.marginRight = "0";
    const title = library.querySelector("h2");
    if (title && !title.dataset.kilnOrigTitle) {
      title.dataset.kilnOrigTitle = title.innerHTML;
      title.innerHTML = `${chooseLangText("Обрана програма", "Selected program")}`;
      title.className = "text-sm font-bold text-white mb-2 px-2";
    }
    const list = library.querySelector(".flex-1.overflow-y-auto, .flex-1");
    if (list) {
      list.innerHTML = "";
      list.className = "rounded-xl cursor-pointer border transition-all relative group bg-zinc-900/50 border-zinc-800 hover:border-zinc-600";
      list.style.width = "100%";
      list.style.boxSizing = "border-box";
      list.style.padding = "16px";
      list.style.margin = "0";
      list.style.maxWidth = "none";
      list.style.minWidth = "0";
      list.style.display = "flex";
      list.style.flexDirection = "column";
      list.style.alignItems = "stretch";
      list.style.gap = "4px";
      list.style.overflow = "hidden";
      const items = Array.isArray(schedules) ? schedules : [];
      const openProgramsPage = () => {
        const nav = Array.from(document.querySelectorAll("a")).find((a) => {
          const tt = (a.textContent || "").trim().toLowerCase();
          return tt.includes("програми") || tt.includes("program");
        });
        if (nav) nav.click();
      };
      if (!items.length) {
        const nm = document.createElement("div");
        nm.className = "font-bold text-white text-sm mb-1 pr-2";
        nm.textContent = chooseLangText("Програми відсутні", "No schedules found");
        const meta = document.createElement("div");
        meta.className = "text-xs text-zinc-500";
        meta.textContent = chooseLangText("Натисни щоб відкрити програми", "Click to open programs");
        list.appendChild(nm);
        list.appendChild(meta);
        list.addEventListener("click", openProgramsPage);
      } else {
        const selectedSchedule = items.find((schedule) => String(schedule && schedule.name || "") === String(selectedProgramName || ""));
        const fallbackSchedule = selectedSchedule || items[0];
        if (fallbackSchedule && !selectedProgramName) {
          window.localStorage.setItem("kiln_selected_program_name", String(fallbackSchedule && fallbackSchedule.name || ""));
        }
        const nm = document.createElement("div");
        nm.className = "font-bold text-white text-sm mb-1 pr-2 truncate";
        nm.textContent = String(fallbackSchedule && fallbackSchedule.name || chooseLangText("Виберіть програму", "Choose program"));
        const meta = document.createElement("div");
        meta.className = "text-xs text-zinc-500";
        if (fallbackSchedule) {
          const stepCount = Array.isArray(fallbackSchedule && fallbackSchedule.steps)
            ? fallbackSchedule.steps.length
            : Number(fallbackSchedule && fallbackSchedule.steps_count || 0);
          const type = String(fallbackSchedule && (fallbackSchedule.schedule_type || fallbackSchedule.type) || "Custom");
          meta.textContent = `${stepCount} ${chooseLangText("кроків", "steps")} | ${type}`;
        } else {
          meta.textContent = chooseLangText("Натисни щоб відкрити програми", "Click to open programs");
        }
        list.appendChild(nm);
        list.appendChild(meta);
        list.addEventListener("click", () => {
          if (fallbackSchedule) {
            window.localStorage.setItem("kiln_selected_program_name", String(fallbackSchedule && fallbackSchedule.name || ""));
          }
          openProgramsPage();
        });
      }
    }
  }

  const currentTemp = findCardByHeading((text) => text.includes("поточна т-ра") || text.includes("current"));
  const remaining = findCardByHeading((text) => text.includes("залишилось часу") || text.includes("remaining"));
  const segment = findCardByHeading((text) => text.includes("сегмент") || text.includes("segment"));
  const fan = Array.from(document.querySelectorAll("div")).find((el) => {
    const cls = (el.className || "").toString();
    if (!cls.includes("bg-kiln-card")) return false;
    return !!el.querySelector("button[title*='fan'],button[title*='Fan']");
  });
  [library, currentTemp, segment, remaining, fan].forEach((el) => moveInto(el, left));
  moveInto(chart, right);
  if (currentTemp) {
    currentTemp.style.width = "100%";
    currentTemp.style.minWidth = "0";
    currentTemp.style.gridColumn = "1 / -1";
    currentTemp.style.minHeight = "";
    currentTemp.style.maxHeight = "";
    currentTemp.style.height = "";
    currentTemp.style.padding = "";
    currentTemp.style.display = "";
    currentTemp.style.alignItems = "";
    currentTemp.style.justifyContent = "";
    currentTemp.style.gap = "";
  }
  if (fan) {
    fan.style.width = "100%";
    fan.style.minWidth = "0";
    fan.style.gridColumn = "1 / -1";
  }
  if (segment) {
    segment.style.width = "100%";
    segment.style.minWidth = "0";
    segment.style.gridColumn = "1 / -1";
  }
  if (remaining) {
    remaining.style.width = "100%";
    remaining.style.minWidth = "0";
    remaining.style.gridColumn = "1 / -1";
    const remainingTitle = remaining.querySelector(".text-\\[10px\\], .text-xs");
    const remainingValue = remaining.querySelector(".text-xl, .text-2xl, .text-lg");
    if (remainingTitle) {
      remainingTitle.textContent = chooseLangText("Орієнтовне завершення", "Estimated finish");
    }
    if (remainingValue) {
      remainingValue.innerHTML = `${formatEstimatedFinishTime(status)} <span class="text-sm text-zinc-600 font-normal ml-0.5 align-baseline"></span>`;
    }
  }
  chart.style.display = "";
  chart.style.width = "100%";
  chart.style.minWidth = "0";
  chart.style.minHeight = "220px";
  chart.style.maxHeight = "260px";
  chart.style.height = "260px";
  const chartTitle = Array.from(chart.querySelectorAll("h2")).find((h) => {
    const t = (h.textContent || "").trim().toLowerCase();
    return t.includes("графік") || t.includes("chart");
  });
  const chartHeader = chartTitle ? chartTitle.closest("div") : null;
  const chartHeaderBars = Array.from(chart.querySelectorAll("div.border-b")).filter((el) => {
    const cls = (el.className || "").toString();
    return cls.includes("bg-kiln-card/50");
  });
  for (const el of chartHeaderBars) el.style.display = "none";
  if (chartHeader) chartHeader.style.display = "none";
  const chartPickButtons = Array.from(chart.querySelectorAll("button")).filter((b) => {
    const t = (b.textContent || "").trim().toLowerCase();
    return t.includes("виберіть програму") || t.includes("choose program");
  });
  for (const b of chartPickButtons) b.remove();
  const chartCanvasWrap = chart.querySelector(".flex-1.p-6.relative");
  if (chartCanvasWrap) {
    chartCanvasWrap.style.padding = "12px";
  }
};

const findProgramButtonText = () => {
  const buttons = Array.from(document.querySelectorAll("button"));
  const b = buttons.find((btn) => {
    const t = (btn.textContent || "").trim().toLowerCase();
    if (!t) return false;
    if (t === "stop" || t === "fan on" || t === "fan off" || t === "+" || t === "-" || t === "ua" || t === "en") return false;
    return t.includes("програм") || t.includes("program");
  });
  return b ? (b.textContent || "").trim() : "";
};

const getSelectedProgramName = () => {
  const key = "kiln_selected_program_name";
  const raw = findProgramButtonText();
  const low = raw.toLowerCase();
  const looksSelected = low && !low.includes("виберіть програму") && !low.includes("choose program") && low !== "редагувати" && low !== "edit";
  if (looksSelected) {
    window.localStorage.setItem(key, raw);
    return raw;
  }
  return window.localStorage.getItem(key) || "";
};

const formatEstimatedFinishTime = (status) => {
  const remainingMin = Number(status && status.timeRemaining);
  if (!Number.isFinite(remainingMin) || remainingMin < 0) return "--:--";
  const finish = new Date(Date.now() + Math.round(remainingMin) * 60000);
  const hh = String(finish.getHours()).padStart(2, "0");
  const mm = String(finish.getMinutes()).padStart(2, "0");
  return `${hh}:${mm}`;
};

const getCachedSelectedSchedule = () => {
  try {
    const raw = window.localStorage.getItem("kiln_selected_schedule_json");
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return null;
    if (!Array.isArray(parsed.steps)) return null;
    return parsed;
  } catch {
    return null;
  }
};

const startSelectedSchedule = async (scheduleName) => {
  const pickedName = String(scheduleName || "").trim();
  if (!pickedName) {
    showToast(chooseLangText("Спочатку вибери програму", "Pick a program first"), false);
    return;
  }
  const schedules = await getSchedulesSafe();
  let schedule = Array.isArray(schedules)
    ? schedules.find((s) => String(s && s.name || "").trim() === pickedName)
    : null;
  if (!schedule) {
    const cached = getCachedSelectedSchedule();
    if (cached && String(cached.name || "").trim() === pickedName) schedule = cached;
  }
  if (!schedule) {
    showToast(chooseLangText("Програму не знайдено", "Program not found"), false);
    return;
  }
  const res = await postJson("/api/start", { schedule });
  if (!res.ok) {
    showToast(chooseLangText("Не вдалося запустити випал", "Failed to start firing"), false);
    return;
  }
  showToast(chooseLangText("Випал запущено", "Firing started"), true);
  scheduleBootstrapRefresh();
  scheduleBootstrapRefresh(180);
};

const findScheduleForRunning = (schedules, status, selectedName) => {
  if (!Array.isArray(schedules) || !schedules.length) return null;
  if (selectedName) {
    const byName = schedules.find((s) => String(s && s.name || "").trim() === selectedName);
    if (byName) return byName;
  }
  const total = Number(status && status.totalSteps || 0);
  if (total > 0) {
    const sameCount = schedules.filter((s) => {
      const steps = Array.isArray(s && s.steps) ? s.steps : [];
      return steps.length === total;
    });
    if (sameCount.length === 1) return sameCount[0];
  }
  return null;
};

const ensureSchedulesLibraryPlayButtons = () => {
  if (!window.location.pathname.startsWith("/schedules")) return;
  const library = findCardByHeading((text, el) => {
    const h2 = el.querySelector("h2");
    const t = (h2 && h2.textContent || text || "").trim().toLowerCase();
    return t.includes("бібліотека") || t.includes("library");
  });
  if (!library) return;
  const cards = Array.from(library.querySelectorAll("div.cursor-pointer.border"));
  for (const card of cards) {
    const nameEl = card.querySelector(".font-bold.text-sm.text-white");
    if (!nameEl) continue;
    const programName = String(nameEl.textContent || "").trim();
    if (!programName) continue;
    const selectedCard = (card.className || "").toString().includes("border-kiln-accent");

    let body = card.querySelector("[data-kiln-lib-card-body='1']");
    let play = card.querySelector("[data-kiln-lib-play='1']");

    if (!body) {
      body = document.createElement("div");
      body.dataset.kilnLibCardBody = "1";
      body.style.flex = "1 1 auto";
      body.style.minWidth = "0";
      while (card.firstChild) {
        body.appendChild(card.firstChild);
      }
      card.appendChild(body);
    }

    card.style.display = "flex";
    card.style.alignItems = "center";
    card.style.justifyContent = "space-between";
    card.style.gap = "12px";

    if (!play) {
      play = document.createElement("button");
      play.type = "button";
      play.dataset.kilnLibPlay = "1";
      play.style.width = "52px";
      play.style.height = "52px";
      play.style.flex = "0 0 52px";
      play.style.display = "inline-flex";
      play.style.alignItems = "center";
      play.style.justifyContent = "center";
      play.style.borderRadius = "26px";
      play.style.border = "none";
      play.style.padding = "0";
      play.style.boxShadow = "none";
      play.style.cursor = "pointer";
      play.title = chooseLangText("Вибрати програму і перейти на панель", "Select program and open dashboard");
      play.setAttribute("aria-label", play.title);
      play.innerHTML = `
        <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
          <path d="M8 5.14v14l11-7z"></path>
        </svg>
      `;
      play.addEventListener("click", (ev) => {
        ev.preventDefault();
        ev.stopPropagation();
        window.localStorage.setItem("kiln_selected_program_name", programName);
        getSchedulesSafe().then((all) => {
          const found = Array.isArray(all)
            ? all.find((s) => String(s && s.name || "").trim() === programName)
            : null;
          if (found) {
            window.localStorage.setItem("kiln_selected_schedule_json", JSON.stringify(found));
          }
        });
        openDashboardPage();
      }, true);
      card.appendChild(play);
    }
    play.style.background = selectedCard ? "#10B981" : "#27272A";
    play.style.color = selectedCard ? "#000000" : "#FFFFFF";
  }
};

const restoreOrHideChartAdjustButtons = (host, firing) => {
  if (!host) return;
  const buttons = host.querySelectorAll("button");
  for (const b of buttons) {
    const t = (b.textContent || "").trim().toLowerCase();
    const hideMe = t === "додати час" || t === "add time" || t === "додати темп" || t === "add temp" || t === "+ час" || t === "+ time" || t === "+ температура" || t === "+ temperature";
    if (!hideMe) continue;
    if (firing) {
      if (b.dataset.hiddenByBootstrap !== "1") {
        b.dataset.hiddenByBootstrap = "1";
        b.dataset.prevDisplay = b.style.display || "";
      }
      b.style.display = "none";
    } else if (b.dataset.hiddenByBootstrap === "1") {
      b.style.display = b.dataset.prevDisplay || "";
      delete b.dataset.prevDisplay;
      delete b.dataset.hiddenByBootstrap;
    }
  }
};

let legacyPanelGuardInit = false;
const removeLegacyRunningPanels = () => {
  const ids = ["firing-running-panel", "firing-action-panel"];
  for (const id of ids) {
    const el = document.getElementById(id);
    if (el) el.remove();
  }
  if (!legacyPanelGuardInit) {
    legacyPanelGuardInit = true;
    const st = document.createElement("style");
    st.id = "kiln-legacy-panel-guard";
    st.textContent = "#firing-running-panel,#firing-action-panel{display:none !important; visibility:hidden !important;}";
    document.head.appendChild(st);
    const obs = new MutationObserver(() => {
      for (const id of ids) {
        const el = document.getElementById(id);
        if (el) el.remove();
      }
    });
    obs.observe(document.documentElement, { childList: true, subtree: true });
  }
};

const normalizeStep = (step) => {
  const isHoldLegacy = Number(step && step.type) === 1;
  const typeRaw = String(step && (step.step_type || step.type) || "").toLowerCase();
  const isHold = typeRaw === "hold" || isHoldLegacy;
  const targetRaw = step && Number.isFinite(Number(step.target)) ? Number(step.target) : Number(step && step.value);
  const rateRaw = step && Number.isFinite(Number(step.rate)) ? Number(step.rate) : 0;
  const holdRaw = step && Number.isFinite(Number(step.hold_time)) ? Number(step.hold_time) : (Number.isFinite(Number(step && step.duration)) ? Number(step.duration) / 60 : 0);
  return {
    type: isHold ? "hold" : "ramp",
    target: Number.isFinite(targetRaw) ? targetRaw : 0,
    rate: Number.isFinite(rateRaw) && rateRaw > 0 ? rateRaw : 1,
    hold: Number.isFinite(holdRaw) && holdRaw >= 0 ? holdRaw : 0,
  };
};

const denormalizeStep = (base, row) => {
  const next = { ...(base || {}) };
  const hasStringType = typeof next.step_type === "string";
  if (hasStringType) {
    next.step_type = row.type;
    next.target = row.target;
    next.rate = row.type === "ramp" ? row.rate : (Number.isFinite(Number(next.rate)) ? Number(next.rate) : row.rate);
    next.hold_time = row.type === "hold" ? row.hold : 0;
    return next;
  }
  next.type = row.type === "hold" ? 1 : 0;
  next.value = row.target;
  if (row.type === "ramp") next.rate = row.rate;
  next.duration = row.type === "hold" ? Math.max(0, Math.round(row.hold * 60)) : 0;
  return next;
};

const effectiveTargetAt = (steps, stepIndex, fallback) => {
  if (!Array.isArray(steps) || !steps.length) return fallback;
  const idx = Math.max(0, Math.min(stepIndex, steps.length - 1));
  for (let i = idx; i >= 0; i -= 1) {
    const n = normalizeStep(steps[i]);
    if (Number.isFinite(n.target)) return n.target;
  }
  return fallback;
};

const parseEditorRows = (panel) => {
  const rows = Array.from(panel.querySelectorAll("[data-step-row]"));
  return rows.map((r) => {
    const type = String(r.querySelector("[data-k='type']").value || "ramp");
    const target = parseLocaleNumber(r.querySelector("[data-k='target']").value);
    const rate = parseLocaleNumber(r.querySelector("[data-k='rate']").value);
    const hold = parseLocaleNumber(r.querySelector("[data-k='hold']").value);
    return { type, target, rate, hold };
  });
};

const setPanelDirty = (panel, dirty) => {
  panel.dataset.dirty = dirty ? "1" : "0";
};

const renderRunningEditor = (panel, lang, schedule, status) => {
  const stepIndex = Math.max(0, Number(status.step ?? 0) | 0);
  const steps = Array.isArray(schedule && schedule.steps) ? schedule.steps : [];
  const name = String(schedule && schedule.name || (lang === "uk" ? "Невідома програма" : "Unknown program"));
  panel.innerHTML = "";
  panel.className = "bg-kiln-card rounded-xl border border-kiln-border p-4 text-white";

  const root = document.createElement("div");
  root.className = "flex flex-col h-full";
  panel.appendChild(root);

  const header = document.createElement("div");
  header.className = "mb-4 flex flex-col md:flex-row justify-between items-center gap-2 shrink-0 px-1";
  root.appendChild(header);

  const headerLeft = document.createElement("div");
  headerLeft.className = "w-full flex items-center gap-2";
  header.appendChild(headerLeft);

  const backBtn = document.createElement("button");
  backBtn.type = "button";
  backBtn.className = "md:hidden p-2 -ml-2 text-zinc-400 hover:text-white";
  backBtn.textContent = "Back";
  headerLeft.appendChild(backBtn);

  const nameInput = document.createElement("input");
  nameInput.className = "text-xl md:text-3xl font-bold text-white bg-transparent border-none p-0 w-full focus:ring-0 truncate";
  nameInput.value = name;
  nameInput.dataset.k = "program-name";
  headerLeft.appendChild(nameInput);

  const headerRight = document.createElement("div");
  headerRight.className = "flex gap-2 w-full md:w-auto";
  header.appendChild(headerRight);

  const mkTopBtn = (label, cls) => {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = label;
    b.className = cls;
    return b;
  };

  const bStart = mkTopBtn(lang === "uk" ? "Старт" : "Start", "flex-1 md:flex-none flex items-center justify-center gap-2 bg-emerald-500 hover:bg-emerald-400 text-black px-3 py-2 rounded-lg font-bold text-xs shadow-lg shadow-emerald-900/20 whitespace-nowrap");
  bStart.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M8 5.14v14l11-7z"></path></svg>${lang === "uk" ? " Старт" : " Start"}`;
  const bSave = mkTopBtn(lang === "uk" ? "Зберегти" : "Save", "flex-1 md:flex-none flex items-center justify-center gap-2 bg-kiln-accent hover:bg-emerald-400 text-black px-3 py-2 rounded-lg font-bold text-xs shadow-lg shadow-emerald-900/20 whitespace-nowrap");
  bSave.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-save" aria-hidden="true"><path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"></path><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"></path><path d="M7 3v4a1 1 0 0 0 1 1h7"></path></svg>${lang === "uk" ? " Зберегти" : " Save"}`;
  const bDeleteProgram = mkTopBtn("", "p-2 bg-red-500/10 text-red-500 rounded-lg hover:bg-red-500/20");
  bDeleteProgram.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-trash2 lucide-trash-2" aria-hidden="true"><path d="M10 11v6"></path><path d="M14 11v6"></path><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"></path><path d="M3 6h18"></path><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>`;
  bDeleteProgram.addEventListener("click", async () => {
    if (!status || !status.firing) {
      showToast(lang === "uk" ? "Кнопка доступна під час випалу" : "Button is available during firing", false);
      return;
    }
    const ok = window.confirm(lang === "uk" ? "Пропустити поточний крок?" : "Skip current step?");
    if (!ok) return;
    const r = await postJson("/api/skip", {});
    showToast(r.ok ? (lang === "uk" ? "Крок пропущено" : "Step skipped")
                   : (lang === "uk" ? "Не вдалося пропустити крок" : "Failed to skip step"), r.ok);
  });
  if (!status || !status.firing) headerRight.appendChild(bStart);
  headerRight.appendChild(bSave);
  headerRight.appendChild(bDeleteProgram);

  const heads = document.createElement("div");
  heads.className = "hidden md:flex gap-6 mb-2 px-6 py-3 bg-zinc-800/50 rounded-lg text-xs font-bold text-zinc-400 uppercase tracking-widest border border-zinc-800 shrink-0";
  heads.innerHTML = `<div class="w-10 text-center">#</div><div class="flex-1 grid grid-cols-9 gap-6"><div class="col-span-3 text-center">${lang === "uk" ? "Швидкість" : "Rate"}</div><div class="col-span-3 text-center">${lang === "uk" ? "Температура" : "Temperature"}</div><div class="col-span-3 text-center">${lang === "uk" ? "Час" : "Time"}</div></div><div class="w-10"></div>`;
  root.appendChild(heads);

  const list = document.createElement("div");
  list.className = "flex-1 overflow-y-auto space-y-3 pr-2 pb-20 md:pb-0 min-h-0 no-scrollbar md:scrollbar-default";
  root.appendChild(list);

  const getCurrentRows = () => Array.from(list.querySelectorAll("[data-step-row]"));
  const reindexRows = () => {
    getCurrentRows().forEach((row, i) => {
      row.dataset.stepRow = "1";
      const n = row.querySelector("[data-step-num]");
      if (n) n.textContent = String(i + 1);
      const nMob = row.querySelector("[data-step-mobile]");
      if (nMob) nMob.textContent = `${lang === "uk" ? "Крок" : "Step"} ${i + 1}`;
      row.dataset.active = i === stepIndex ? "1" : "0";
      row.style.borderColor = i === stepIndex ? "#22c55e" : "";
    });
  };

  const makeStepRow = (stepData, index) => {
    const n = normalizeStep(stepData);
    const row = document.createElement("div");
    row.dataset.stepRow = "1";
    row.dataset.active = index === stepIndex ? "1" : "0";
    row.className = "flex flex-col md:flex-row items-center gap-4 md:gap-6 p-4 bg-kiln-card border border-kiln-border rounded-xl hover:border-zinc-700 transition-colors";
    if (index === stepIndex) row.style.borderColor = "#22c55e";

    const mobileTop = document.createElement("div");
    mobileTop.className = "md:hidden flex justify-between items-center w-full border-b border-zinc-800 pb-2 mb-2";
    const mobileLabel = document.createElement("span");
    mobileLabel.className = "font-bold text-zinc-400 text-sm";
    mobileLabel.dataset.stepMobile = "1";
    mobileLabel.textContent = `${lang === "uk" ? "Крок" : "Step"} ${index + 1}`;
    const mobileDel = document.createElement("button");
    mobileDel.type = "button";
    mobileDel.className = "text-red-500 p-1";
    mobileDel.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-trash2 lucide-trash-2" aria-hidden="true"><path d="M10 11v6"></path><path d="M14 11v6"></path><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"></path><path d="M3 6h18"></path><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>`;
    mobileTop.appendChild(mobileLabel);
    mobileTop.appendChild(mobileDel);
    row.appendChild(mobileTop);

    const num = document.createElement("div");
    num.className = "hidden md:flex w-10 justify-center text-zinc-500 font-mono font-bold text-lg";
    num.dataset.stepNum = "1";
    num.textContent = String(index + 1);
    row.appendChild(num);

    const grid = document.createElement("div");
    grid.className = "flex-1 grid grid-cols-2 md:grid-cols-9 gap-3 md:gap-6 items-center w-full";
    row.appendChild(grid);

    const col1 = document.createElement("div");
    col1.className = "col-span-1 md:col-span-3 relative";
    col1.innerHTML = `<label class="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">${lang === "uk" ? "Швидкість" : "Rate"}</label>`;
    const inRate = document.createElement("input");
    inRate.type = "number";
    inRate.min = "0";
    inRate.max = "9999";
    inRate.className = "bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-14 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg";
    inRate.dataset.k = "rate";
    inRate.value = String(Number(n.rate).toFixed(2));
    inRate.placeholder = "FULL";
    const rateUnit = document.createElement("span");
    rateUnit.className = "absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none";
    rateUnit.textContent = lang === "uk" ? "°C/год" : "°C/h";
    col1.appendChild(inRate);
    col1.appendChild(rateUnit);

    const col2 = document.createElement("div");
    col2.className = "col-span-1 md:col-span-3 relative";
    col2.innerHTML = `<label class="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">${lang === "uk" ? "Температура" : "Temperature"}</label>`;
    const inTarget = document.createElement("input");
    inTarget.type = "number";
    inTarget.min = "0";
    inTarget.max = "1320";
    inTarget.className = "bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-8 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg font-bold";
    inTarget.dataset.k = "target";
    inTarget.value = String(Number(n.target).toFixed(1));
    const targetUnit = document.createElement("span");
    targetUnit.className = "absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none";
    targetUnit.textContent = "°C";
    col2.appendChild(inTarget);
    col2.appendChild(targetUnit);

    const col3 = document.createElement("div");
    col3.className = "col-span-2 md:col-span-3 relative";
    col3.innerHTML = `<label class="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">${lang === "uk" ? "Час" : "Time"}</label>`;
    const inHold = document.createElement("input");
    inHold.type = "number";
    inHold.min = "0";
    inHold.max = "9999";
    inHold.className = "bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-8 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg";
    inHold.dataset.k = "hold";
    inHold.value = String(Number(n.hold).toFixed(1));
    const holdUnit = document.createElement("span");
    holdUnit.className = "absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none";
    holdUnit.textContent = lang === "uk" ? "хв" : "min";
    col3.appendChild(inHold);
    col3.appendChild(holdUnit);

    const hiddenType = document.createElement("select");
    hiddenType.dataset.k = "type";
    hiddenType.style.display = "none";
    hiddenType.innerHTML = `<option value="ramp">${lang === "uk" ? "Нагрів" : "Ramp"}</option><option value="hold">${lang === "uk" ? "Витримка" : "Hold"}</option>`;
    hiddenType.value = n.type;
    row.appendChild(hiddenType);

    grid.appendChild(col1);
    grid.appendChild(col2);
    grid.appendChild(col3);

    const desktopDel = document.createElement("button");
    desktopDel.type = "button";
    desktopDel.className = "hidden md:block w-10 p-2 text-zinc-600 hover:text-red-500";
    desktopDel.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-trash2 lucide-trash-2" aria-hidden="true"><path d="M10 11v6"></path><path d="M14 11v6"></path><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"></path><path d="M3 6h18"></path><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>`;
    row.appendChild(desktopDel);

    const syncEnable = () => {
      const isHold = hiddenType.value === "hold";
      inRate.disabled = isHold;
      inRate.style.opacity = isHold ? "0.45" : "1";
      inHold.disabled = !isHold;
      inHold.style.opacity = isHold ? "1" : "0.65";
    };
    syncEnable();

    [inRate, inTarget, inHold].forEach((el) => el.addEventListener("input", () => setPanelDirty(panel, true)));
    [mobileDel, desktopDel].forEach((btn) => btn.addEventListener("click", () => {
      row.remove();
      reindexRows();
      setPanelDirty(panel, true);
    }));
    inHold.addEventListener("focus", () => {
      hiddenType.value = "hold";
      syncEnable();
      setPanelDirty(panel, true);
    });
    inRate.addEventListener("focus", () => {
      hiddenType.value = "ramp";
      syncEnable();
      setPanelDirty(panel, true);
    });

    return row;
  };

  steps.forEach((s, i) => list.appendChild(makeStepRow(s, i)));

  const addBtn = document.createElement("button");
  addBtn.type = "button";
  addBtn.className = "w-full py-4 border-2 border-dashed border-zinc-800 rounded-xl text-zinc-500 hover:text-kiln-accent hover:border-kiln-accent hover:bg-kiln-accent/5 transition-all font-bold uppercase tracking-wider flex items-center justify-center gap-2";
  addBtn.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-plus" aria-hidden="true"><path d="M5 12h14"></path><path d="M12 5v14"></path></svg> ${lang === "uk" ? "Додати Сегмент" : "Add Segment"}`;
  addBtn.addEventListener("click", () => {
    const rows = parseEditorRows(panel);
    const last = rows.length ? rows[rows.length - 1] : { type: "ramp", target: 100, rate: 100, hold: 0 };
    list.appendChild(makeStepRow({ step_type: last.type, target: last.target ?? 100, rate: last.rate ?? 100, hold_time: last.hold ?? 0 }, rows.length));
    reindexRows();
    setPanelDirty(panel, true);
  });
  root.appendChild(addBtn);

  bSave.addEventListener("click", async () => {
    const rows = parseEditorRows(panel);
    if (rows.length < 1) {
      showToast(lang === "uk" ? "Додай хоча б один сегмент" : "Add at least one segment", false);
      return;
    }
    for (const row of rows) {
      if (row.target === null) {
        showToast(lang === "uk" ? "Некоректна цільова температура" : "Invalid target temperature", false);
        return;
      }
      if (row.type === "ramp" && (row.rate === null || row.rate <= 0)) {
        showToast(lang === "uk" ? "Некоректна швидкість нагріву" : "Invalid ramp rate", false);
        return;
      }
      if (row.type === "hold" && (row.hold === null || row.hold < 0)) {
        showToast(lang === "uk" ? "Некоректний час витримки" : "Invalid hold time", false);
        return;
      }
    }
    const ok = window.confirm(lang === "uk" ? "Зберегти зміни активної програми?" : "Save active program changes?");
    if (!ok) return;

    const updatedSteps = rows.map((row, i) => denormalizeStep(steps[i], row));
    const updatedName = (nameInput.value || "").trim() || name;
    const updatedSchedule = { ...schedule, name: updatedName, steps: updatedSteps, stepsCount: updatedSteps.length };
    const saveRes = await postJson("/api/schedules", { schedule: updatedSchedule });
    if (!saveRes.ok) {
      showToast(lang === "uk" ? "Не вдалося зберегти програму" : "Failed to save program", false);
      return;
    }

    let allOk = true;
    if (status && status.firing) {
      const oldEff = effectiveTargetAt(steps, stepIndex, Number(status.target ?? 0));
      const newEff = effectiveTargetAt(updatedSteps, stepIndex, oldEff);
      if (Math.abs(newEff - oldEff) > 0.0001) {
        const rt = await postJson("/api/addTemp", { degrees: newEff - oldEff });
        allOk = allOk && rt.ok;
      }
      if (stepIndex < updatedSteps.length) {
        const oldRow = normalizeStep(steps[stepIndex]);
        const newRow = normalizeStep(updatedSteps[stepIndex]);
        if (newRow.type === "ramp" && Math.abs(newRow.rate - oldRow.rate) > 0.0001) {
          const rr = await postJson("/api/setRate", { rate: newRow.rate });
          allOk = allOk && rr.ok;
        }
        if (newRow.type === "hold" && Math.abs(newRow.hold - oldRow.hold) > 0.0001) {
          const rm = await postJson("/api/addTime", { minutes: newRow.hold - oldRow.hold });
          allOk = allOk && rm.ok;
        }
      }
    }

    schedulesCacheData = Array.isArray(schedulesCacheData)
      ? schedulesCacheData.map((s) => (String(s && s.name || "") === String(updatedSchedule.name || "") ? updatedSchedule : s))
      : [updatedSchedule];
    schedulesCacheAt = Date.now();
    setPanelDirty(panel, false);
    panel.dataset.renderFrom = "";
    showToast(
      allOk
        ? (lang === "uk" ? "Зміни збережено" : "Changes saved")
        : (lang === "uk" ? "Збережено, але частину runtime-оновлень не застосовано" : "Saved, but some runtime updates failed"),
      allOk
    );
  });

  bStart.addEventListener("click", async () => {
    const rows = parseEditorRows(panel);
    if (!rows.length) {
      showToast(lang === "uk" ? "Додай хоча б один сегмент" : "Add at least one segment", false);
      return;
    }
    const updatedSteps = rows.map((row, i) => denormalizeStep(steps[i], row));
    const updatedName = (nameInput.value || "").trim() || name;
    const updatedSchedule = { ...schedule, name: updatedName, steps: updatedSteps, stepsCount: updatedSteps.length };
    window.localStorage.setItem("kiln_selected_program_name", updatedName);
    window.localStorage.setItem("kiln_selected_schedule_json", JSON.stringify(updatedSchedule));
    await startSelectedSchedule(updatedName);
  });
};

const ensureFiringActionPanel = async () => {
  removeLegacyRunningPanels();
  const status = await getStatusSafe();
  const selectedProgramName = getSelectedProgramName();
  const schedules = await getSchedulesSafe();
  const layoutActive = !!selectedProgramName || !!(status && status.firing);
  if (!layoutActive) {
    restoreMovedCards();
    const panelInactive = document.getElementById("firing-program-editor");
    if (panelInactive) panelInactive.style.display = "none";
    return;
  }
  const host = findChartCard();
  let panel = document.getElementById("firing-program-editor");
  if (!host) {
    if (panel) panel.style.display = "none";
    return;
  }
  const hostParent = host.parentElement;
  if (!hostParent) {
    if (panel) panel.style.display = "none";
    return;
  }
  arrangeFiringDashboard(status, selectedProgramName, schedules);
  const rightCol = document.getElementById("firing-dashboard-right");
  const targetParent = rightCol || hostParent;
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "firing-program-editor";
    panel.style.display = "none";
    panel.style.marginBottom = "12px";
    panel.style.boxSizing = "border-box";
    panel.className = "bg-kiln-card rounded-xl border border-zinc-700 p-3 text-zinc-100";
    targetParent.insertBefore(panel, host.nextSibling);
  } else if (panel.parentElement !== targetParent) {
    targetParent.insertBefore(panel, host.nextSibling);
  } else {
    if (panel.previousElementSibling !== host) {
      targetParent.insertBefore(panel, host.nextSibling);
    }
  }

  restoreOrHideChartAdjustButtons(host, !!(status && status.firing));
  panel.style.display = "block";
  if (!(status && status.firing)) {
    panel.style.width = "100%";
    panel.style.maxWidth = "100%";
    panel.style.alignSelf = "stretch";
  } else if (window.innerWidth < 1100) {
    panel.style.width = "100%";
    panel.style.maxWidth = "100%";
    panel.style.alignSelf = "stretch";
  } else {
    panel.style.width = "82%";
    panel.style.maxWidth = "900px";
    panel.style.alignSelf = "flex-end";
  }
  const lang = detectUiLang();
  const cachedSelectedSchedule = getCachedSelectedSchedule();
  const runningSchedule = findScheduleForRunning(schedules, status || {}, selectedProgramName)
    || (selectedProgramName ? schedules.find((s) => String(s && s.name || "").trim() === selectedProgramName) : null)
    || (cachedSelectedSchedule && selectedProgramName && String(cachedSelectedSchedule.name || "").trim() === selectedProgramName ? cachedSelectedSchedule : null);
  if (!runningSchedule) {
    panel.innerHTML = "";
    const m = document.createElement("div");
    m.className = "text-sm text-zinc-300 mb-2";
    m.textContent = lang === "uk"
      ? "Активну програму не ідентифіковано. Відкрий програму на головному екрані й повтори."
      : "Active program is not identified. Open program on dashboard and retry.";
    panel.appendChild(m);
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = lang === "uk" ? "Пропустити крок" : "Skip step";
    b.className = "px-2.5 py-1.5 rounded-lg text-xs font-bold text-rose-100 flex items-center gap-1.5 transition-colors border border-rose-800 whitespace-nowrap hover:bg-rose-800/45";
    b.style.background = "#be123c";
    b.addEventListener("click", async () => {
      const ok = window.confirm(lang === "uk" ? "Пропустити поточний крок?" : "Skip current step?");
      if (!ok) return;
      const r = await postJson("/api/skip", {});
      showToast(r.ok ? (lang === "uk" ? "Крок пропущено" : "Step skipped")
                     : (lang === "uk" ? "Не вдалося пропустити крок" : "Failed to skip step"), r.ok);
    });
    panel.appendChild(b);
    return;
  }

  const renderKey = `${runningSchedule.name}|${Number(status.step ?? 0)}|${Number(status.totalSteps ?? 0)}|${panel.dataset.dirty || "0"}`;
  if (panel.dataset.renderFrom !== renderKey || panel.dataset.dirty !== "1") {
    renderRunningEditor(panel, lang, runningSchedule, status);
    panel.dataset.renderFrom = renderKey;
    if (panel.dataset.dirty !== "1") setPanelDirty(panel, false);
  }
};

const ensureProgramBlockBehavior = async () => {
  const status = await getStatusSafe();
  const buttons = Array.from(document.querySelectorAll("button"));
  const lang = detectUiLang();
  const pickProgramLike = buttons.find((b) => {
    const t = (b.textContent || "").trim().toLowerCase();
    if (!t) return false;
    if (t === "stop" || t === "fan on" || t === "fan off" || t === "+" || t === "-" || t === "ua" || t === "en") return false;
    return t.includes("програм") || t.includes("program");
  });
  if (!pickProgramLike) return;

  const savedProgramName = window.localStorage.getItem("kiln_selected_program_name") || "";
  const currentText = (pickProgramLike.textContent || "").trim();
  const currentLow = currentText.toLowerCase();
  if (
    savedProgramName &&
    !status?.firing &&
    (currentLow.includes("виберіть програму") || currentLow.includes("choose program"))
  ) {
    pickProgramLike.textContent = savedProgramName;
  }

  if (status && status.firing) {
    pickProgramLike.textContent = lang === "uk" ? "Редагувати" : "Edit";
    if (pickProgramLike.dataset.editBound !== "1") {
      pickProgramLike.dataset.editBound = "1";
      pickProgramLike.addEventListener("click", (ev) => {
        ev.preventDefault();
        ev.stopPropagation();
        const panel = document.getElementById("firing-program-editor");
        if (panel && panel.scrollIntoView) panel.scrollIntoView({ behavior: "smooth", block: "start" });
      }, true);
    }
  } else {
    if (savedProgramName) {
      pickProgramLike.textContent = savedProgramName;
      pickProgramLike.disabled = false;
      pickProgramLike.removeAttribute("disabled");
      pickProgramLike.style.opacity = "1";
      pickProgramLike.style.cursor = "pointer";
      if (pickProgramLike.dataset.startSelectedBound !== "1") {
        pickProgramLike.dataset.startSelectedBound = "1";
        pickProgramLike.addEventListener("click", (ev) => {
          ev.preventDefault();
          ev.stopPropagation();
          startSelectedSchedule(window.localStorage.getItem("kiln_selected_program_name") || "");
        }, true);
      }
    } else {
      delete pickProgramLike.dataset.startSelectedBound;
    }
  }
};

let offsetAutoSaveTimer = 0;
let offsetLastSent = null;
let bootstrapRefreshRaf = 0;
let bootstrapRefreshTimer = 0;
let routeTransitionTimer = 0;
let routeTransitionActive = false;

const setRootTransitionState = (active) => {
  const root = document.getElementById("root");
  if (!root) return;
  root.style.transition = "opacity 120ms ease";
  root.style.opacity = active ? "0" : "1";
};

const beginRouteTransition = () => {
  routeTransitionActive = true;
  if (routeTransitionTimer) {
    window.clearTimeout(routeTransitionTimer);
    routeTransitionTimer = 0;
  }
  setRootTransitionState(true);
};

const endRouteTransition = (delay = 0) => {
  if (!routeTransitionActive && !delay) {
    setRootTransitionState(false);
    return;
  }
  if (routeTransitionTimer) {
    window.clearTimeout(routeTransitionTimer);
    routeTransitionTimer = 0;
  }
  routeTransitionTimer = window.setTimeout(() => {
    routeTransitionActive = false;
    setRootTransitionState(false);
  }, delay);
};

const runBootstrapRefresh = () => {
  bootstrapRefreshRaf = 0;
  removeLegacyRunningPanels();
  ensureSupportToolsPanel();
  ensureKpiSummaryPanel();
  ensureSchedulesLibraryPlayButtons();
  ensureFiringActionPanel();
  ensureProgramBlockBehavior();
  endRouteTransition(60);
};

const scheduleBootstrapRefresh = (delay = 0) => {
  if (bootstrapRefreshTimer) {
    window.clearTimeout(bootstrapRefreshTimer);
    bootstrapRefreshTimer = 0;
  }
  const queueFrame = () => {
    if (bootstrapRefreshRaf) return;
    bootstrapRefreshRaf = window.requestAnimationFrame(runBootstrapRefresh);
  };
  if (delay > 0) {
    bootstrapRefreshTimer = window.setTimeout(queueFrame, delay);
    return;
  }
  queueFrame();
};

const installBootstrapNavigationHooks = () => {
  if (window.__kilnBootstrapNavHooksInstalled) return;
  window.__kilnBootstrapNavHooksInstalled = true;

  const triggerRefreshBurst = () => {
    beginRouteTransition();
    scheduleBootstrapRefresh();
    scheduleBootstrapRefresh(80);
    scheduleBootstrapRefresh(220);
    endRouteTransition(420);
  };

  const origPushState = history.pushState.bind(history);
  history.pushState = function (...args) {
    const result = origPushState(...args);
    triggerRefreshBurst();
    return result;
  };

  const origReplaceState = history.replaceState.bind(history);
  history.replaceState = function (...args) {
    const result = origReplaceState(...args);
    triggerRefreshBurst();
    return result;
  };

  window.addEventListener("popstate", triggerRefreshBurst, true);
  window.addEventListener("hashchange", triggerRefreshBurst, true);
  window.addEventListener("resize", () => scheduleBootstrapRefresh(0), true);

  const root = document.getElementById("root");
  if (root && typeof MutationObserver !== "undefined") {
    const obs = new MutationObserver(() => {
      scheduleBootstrapRefresh(0);
      endRouteTransition(60);
    });
    obs.observe(root, { childList: true, subtree: true });
  }
};

const queueOffsetAutosave = (value) => {
  if (!Number.isFinite(value)) return;
  if (offsetAutoSaveTimer) window.clearTimeout(offsetAutoSaveTimer);
  offsetAutoSaveTimer = window.setTimeout(() => {
    if (offsetLastSent !== null && Math.abs(offsetLastSent - value) < 0.0001) return;
    offsetLastSent = value;
    postSettings({ offset: value }, false);
  }, 350);
};

document.addEventListener(
  "input",
  (ev) => {
    const el = ev.target;
    if (!el || el.tagName !== "INPUT") return;
    if (el.type !== "number") return;
    const v = String(el.value || "");
    if (v.includes(",")) {
      el.value = v.replace(",", ".");
    }
    const parts = findThermocoupleCardByInput(el);
    if (!parts) return;
    const n = parseLocaleNumber(el.value);
    if (n === null) return;
    queueOffsetAutosave(n);
  },
  true
);

document.addEventListener(
  "click",
  (ev) => {
    const btn = ev.target && ev.target.closest ? ev.target.closest("button") : null;
    if (!btn) return;
    const label = (btn.textContent || "").trim().toLowerCase();
    if (label !== "\u0437\u0431\u0435\u0440\u0435\u0433\u0442\u0438" && label !== "save") return;

    const parts = findThermocoupleCardByButton(btn);
    if (!parts) return;
    const inputEl = parts.inputEl;

    const n = parseLocaleNumber(inputEl.value);
    if (n === null) {
      showToast("\u041d\u0435\u0432\u0456\u0440\u043d\u0435 \u0447\u0438\u0441\u043b\u043e", false);
      return;
    }
    ev.preventDefault();
    ev.stopPropagation();
    offsetLastSent = n;
    postSettings({ offset: n }, true);
    return;
  },
  true
);

installBootstrapNavigationHooks();
fixDegreeSymbols();
window.setInterval(fixDegreeSymbols, 1500);
ensureAutotuneSaveButton();
window.setInterval(ensureAutotuneSaveButton, 1200);
window.setInterval(updateAutotuneWarningText, 1200);
fetchAutotuneTargetFromController().then(() => {
  syncAutotuneInputFromSaved();
  updateAutotuneWarningText();
});
window.setInterval(() => {
  fetchAutotuneTargetFromController().then(() => {
    syncAutotuneInputFromSaved();
    updateAutotuneWarningText();
  });
}, 2200);
hideRemoteAccessSection();
window.setInterval(hideRemoteAccessSection, 1500);
ensureSupportToolsPanel();
window.setInterval(ensureSupportToolsPanel, 2000);
ensureKpiSummaryPanel();
window.setInterval(ensureKpiSummaryPanel, 4000);
removeLegacyRunningPanels();
window.setInterval(removeLegacyRunningPanels, 600);
ensureFiringActionPanel();
window.setInterval(ensureFiringActionPanel, 1500);
ensureProgramBlockBehavior();
window.setInterval(ensureProgramBlockBehavior, 1800);
scheduleBootstrapRefresh();
scheduleBootstrapRefresh(120);

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
await import(m ? m[1] : "/assets/index-f98ef54f.js");

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
    if (!t.includes("Зміщення термопари") && !t.includes("Thermocouple Offset")) continue;
    const fixed = t.replace("(�C)", "(°C)").replace("(Â°C)", "(°C)").replace("�C", "°C").replace("Â°C", "°C");
    if (fixed !== t) n.textContent = fixed;
  }
};

let offsetAutoSaveTimer = 0;
let offsetLastSent = null;
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

fixDegreeSymbols();
window.setInterval(fixDegreeSymbols, 1500);

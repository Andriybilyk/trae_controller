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
    return inputUrl;
  } catch {
    if (inputUrl.startsWith("/api/api/")) return inputUrl.replace("/api/api/", "/api/");
    return inputUrl;
  }
};

window.fetch = async (input, init) => {
  const req = input instanceof Request ? input : null;
  if (typeof input === "string") {
    input = normalizeApiUrl(input);
  } else if (req) {
    const nu = normalizeApiUrl(req.url);
    if (nu && nu !== req.url) input = new Request(nu, req);
  }
  return originalFetch(input, init);
};

(async () => {
  if (location.pathname.endsWith("/history.html")) return;
  if (!window.__kilnAppImportedPromise) {
    window.__kilnAppImportedPromise = (async () => {
      const htmlResp = await originalFetch(`/index.html?b=${Date.now()}`, { cache: "no-store" });
      const html = await htmlResp.text();
      const candidates = [];
      const pushCandidate = (v) => {
        if (!v || typeof v !== "string") return;
        if (!candidates.includes(v)) candidates.push(v);
      };
      html.replace(/src=\"(\/assets\/index-[^\"]+\.js)\"/g, (_, p1) => { pushCandidate(p1); return _; });
      pushCandidate("/assets/index-39d65f44.js");

      let lastError = null;
      for (const appModulePath of candidates) {
        try {
          const fullUrl = new URL(`${appModulePath}?v=${Date.now()}`, window.location.origin).toString();
          await import(fullUrl);
          return;
        } catch (e) {
          lastError = e;
        }
      }
      throw lastError || new Error("Unable to import app module");
    })();
  }
  await window.__kilnAppImportedPromise;
})();

const HISTORY_OVERLAY_ID = "kiln-history-overlay";
const staleOverlay = document.getElementById(HISTORY_OVERLAY_ID);
if (staleOverlay && staleOverlay.parentElement) staleOverlay.parentElement.removeChild(staleOverlay);
const HISTORY_THEME_FALLBACK = {
  bg: "#0D1B2A",
  surface: "#1B263B",
  surfaceAlt: "#142033",
  card: "#111827",
  border: "#24344F",
  text: "#FFFFFF",
  muted: "#9FB1C7",
  subtle: "#7D8CA6",
  accent: "#34DFAC",
  danger: "#DC2626",
  warn: "#F59E0B",
  planned: "#60A5FA",
  actual: "#34DFAC",
  grid: "rgba(159,177,199,0.18)",
  fill: "rgba(52,223,172,0.14)",
  font: `"PT Sans","Segoe UI",sans-serif`
};
let HISTORY_THEME = null;
const resolveHistoryTheme = () => {
  if (HISTORY_THEME) return HISTORY_THEME;
  const host = document.body || document.documentElement;
  if (!(host instanceof HTMLElement)) {
    HISTORY_THEME = { ...HISTORY_THEME_FALLBACK };
    return HISTORY_THEME;
  }
  const probeWrap = document.createElement("div");
  probeWrap.style.position = "fixed";
  probeWrap.style.left = "-9999px";
  probeWrap.style.top = "-9999px";
  probeWrap.style.opacity = "0";
  probeWrap.style.pointerEvents = "none";
  const makeProbe = (className, text = "A") => {
    const el = document.createElement("div");
    el.className = className;
    el.textContent = text;
    probeWrap.appendChild(el);
    return el;
  };
  const bgProbe = makeProbe("min-h-screen bg-black text-white");
  const panelProbe = makeProbe("bg-kiln-card border border-kiln-border text-white");
  const cardProbe = makeProbe("bg-zinc-800 border border-kiln-border text-white");
  const mutedProbe = makeProbe("text-zinc-500");
  const subtleProbe = makeProbe("text-zinc-400");
  const accentProbe = makeProbe("bg-kiln-accent text-black");
  const dangerProbe = makeProbe("bg-red-950/50 border border-red-700/70 text-red-200");
  host.appendChild(probeWrap);
  const bodyStyle = getComputedStyle(document.body || document.documentElement);
  const panelStyle = getComputedStyle(panelProbe);
  const cardStyle = getComputedStyle(cardProbe);
  const bgStyle = getComputedStyle(bgProbe);
  const mutedStyle = getComputedStyle(mutedProbe);
  const subtleStyle = getComputedStyle(subtleProbe);
  const accentStyle = getComputedStyle(accentProbe);
  const dangerStyle = getComputedStyle(dangerProbe);
  const bodyBg =
    bodyStyle.backgroundColor && bodyStyle.backgroundColor !== "rgba(0, 0, 0, 0)" && bodyStyle.backgroundColor !== "transparent"
      ? bodyStyle.backgroundColor
      : "";
  HISTORY_THEME = {
    bg: bodyBg || bgStyle.backgroundColor || HISTORY_THEME_FALLBACK.bg,
    surface: panelStyle.backgroundColor || HISTORY_THEME_FALLBACK.surface,
    surfaceAlt: cardStyle.backgroundColor || HISTORY_THEME_FALLBACK.surfaceAlt,
    card: cardStyle.backgroundColor || HISTORY_THEME_FALLBACK.card,
    border: panelStyle.borderTopColor || HISTORY_THEME_FALLBACK.border,
    text: panelStyle.color || HISTORY_THEME_FALLBACK.text,
    muted: mutedStyle.color || HISTORY_THEME_FALLBACK.muted,
    subtle: subtleStyle.color || HISTORY_THEME_FALLBACK.subtle,
    accent: accentStyle.backgroundColor || HISTORY_THEME_FALLBACK.accent,
    danger: dangerStyle.color || HISTORY_THEME_FALLBACK.danger,
    warn: HISTORY_THEME_FALLBACK.warn,
    planned: HISTORY_THEME_FALLBACK.planned,
    actual: accentStyle.backgroundColor || HISTORY_THEME_FALLBACK.actual,
    grid: HISTORY_THEME_FALLBACK.grid,
    fill: HISTORY_THEME_FALLBACK.fill,
    font: bodyStyle.fontFamily || HISTORY_THEME_FALLBACK.font
  };
  probeWrap.remove();
  return HISTORY_THEME;
};
let historyOverlayWanted = null;
let historyOverlayMounted = false;
let historyOverlayMounting = false;
let historyOverlayLastActive = null;
let historyOverlayUpdateScheduled = false;
let historyOverlayIsUa = null;

  const isHistoryActive = () => {
    if (historyOverlayWanted === true) return true;
    if (historyOverlayWanted === false) return false;
    const p = (location.pathname || "").toLowerCase();
    const h = (location.hash || "").toLowerCase();
    if (p.includes("history") || h.includes("history")) return true;
    const current = document.querySelector("[aria-current='page']");
    if (current && current.textContent) {
      const t = current.textContent.trim();
      return t === "Історія" || t === "History";
    }
    return false;
  };

  const findHeaderBottom = () => {
    const header =
      document.querySelector("header") ||
      document.querySelector("[role='banner']") ||
      document.querySelector("nav");
    if (header instanceof HTMLElement) {
      const r = header.getBoundingClientRect();
      if (r.bottom > 0) return Math.max(0, Math.floor(r.bottom));
    }
    return 72;
  };

  const ensureOverlay = () => {
    const theme = resolveHistoryTheme();
    let overlay = document.getElementById(HISTORY_OVERLAY_ID);
    if (overlay instanceof HTMLDivElement) return overlay;
    overlay = document.createElement("div");
    overlay.id = HISTORY_OVERLAY_ID;
    overlay.style.position = "fixed";
    overlay.style.left = "0";
    overlay.style.right = "0";
    overlay.style.bottom = "0";
    overlay.style.zIndex = "20";
    overlay.style.display = "none";
    overlay.style.overflow = "auto";
    overlay.style.background = theme.bg;
    overlay.style.pointerEvents = "none";

    const style = document.createElement("style");
    style.textContent =
      `#${HISTORY_OVERLAY_ID}{pointer-events:auto;--history-bg:${theme.bg};--history-surface:${theme.surface};--history-surface-alt:${theme.surfaceAlt};--history-card:${theme.card};--history-border:${theme.border};--history-text:${theme.text};--history-muted:${theme.muted};--history-subtle:${theme.subtle};--history-accent:${theme.accent};--history-danger:${theme.danger};--history-warn:${theme.warn};--history-font:${theme.font};}` +
      `#${HISTORY_OVERLAY_ID} .history-shell{min-height:100%;max-width:1280px;margin:0 auto;padding:20px 16px 24px;font-family:var(--history-font);color:var(--history-text);}` +
      `#${HISTORY_OVERLAY_ID} canvas{display:block;}` +
      `#${HISTORY_OVERLAY_ID} .history-panel{background:var(--history-surface);border:1px solid var(--history-border);border-radius:12px;box-shadow:0 10px 24px rgba(0,0,0,.18);}` +
      `#${HISTORY_OVERLAY_ID} .history-card{background:var(--history-card);border:1px solid var(--history-border);border-radius:12px;}` +
      `#${HISTORY_OVERLAY_ID} .history-list-card{background:var(--history-card);border:1px solid var(--history-border);border-radius:12px;}` +
      `#${HISTORY_OVERLAY_ID} .history-list-card--active{border-color:var(--history-accent);box-shadow:0 0 0 1px rgba(52,223,172,.18) inset;}` +
      `#${HISTORY_OVERLAY_ID} .history-button{background:var(--history-surface-alt);border:1px solid var(--history-border);border-radius:8px;color:var(--history-muted);font-weight:700;transition:all .15s ease;}` +
      `#${HISTORY_OVERLAY_ID} .history-button:hover{background:#22304a;color:var(--history-text);}` +
      `#${HISTORY_OVERLAY_ID} .history-button-danger{background:rgba(220,38,38,.15);border-color:rgba(220,38,38,.35);color:#fecaca;}` +
      `#${HISTORY_OVERLAY_ID} .history-button-danger:hover{background:rgba(220,38,38,.22);color:#fff;}` +
      `#${HISTORY_OVERLAY_ID} .history-label{color:var(--history-subtle);font-size:10px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;}` +
      `#${HISTORY_OVERLAY_ID} .history-value{color:var(--history-text);font-size:24px;font-weight:700;line-height:1.1;}` +
      `#${HISTORY_OVERLAY_ID} .history-muted{color:var(--history-muted);}` +
      `#${HISTORY_OVERLAY_ID} #history-chart-wrap + .history-muted{display:none !important;}`;
    overlay.appendChild(style);

    const shell = document.createElement("div");
    shell.className = "history-shell";
    overlay.appendChild(shell);

    document.body.appendChild(overlay);
    return overlay;
  };

const fmtMinutes = (m, isUa) => {
  const mm = Math.max(0, Math.round(Number(m || 0)));
  const h = Math.floor(mm / 60);
  const r = mm % 60;
  if (h > 0) return isUa ? `${h} год ${r} хв` : `${h}h ${r}m`;
  return isUa ? `${mm} хв` : `${mm} min`;
};

const detectTimestampSecondsDivisor = (rows) => {
  if (!Array.isArray(rows) || rows.length < 2) return 1;
  let minPositiveDelta = Infinity;
  for (let i = 1; i < rows.length; i++) {
    const t0 = Number(rows[i - 1]?.timestamp ?? 0);
    const t1 = Number(rows[i]?.timestamp ?? 0);
    const dt = t1 - t0;
    if (Number.isFinite(dt) && dt > 0) minPositiveDelta = Math.min(minPositiveDelta, dt);
  }
  if (!Number.isFinite(minPositiveDelta)) return 1;
  return minPositiveDelta > 100 ? 1000 : 1;
};

const formatMinutesLabel = (minutes, isUa) => {
  const m = Math.max(0, Math.round(Number(minutes || 0)));
  const h = Math.floor(m / 60);
  const r = m % 60;
  if (h > 0) return isUa ? `${h} год ${r} хв` : `${h}h ${r}m`;
  return isUa ? `${m} хв` : `${m} min`;
};

const translateFaultReason = (reason, isUa) => {
  const raw = String(reason || "").trim();
  if (!raw) return isUa ? "невідома причина" : "unknown reason";
  const key = raw.toLowerCase();
  const mapUa = {
    emergency_stop: "аварійна зупинка",
    overheat: "перегрів",
    sensor_error: "помилка датчика",
    no_power: "відсутнє живлення",
    voltage_fault: "проблема напруги",
    relay_fault: "несправність реле"
  };
  const mapEn = {
    emergency_stop: "emergency stop",
    overheat: "overheat",
    sensor_error: "sensor error",
    no_power: "no power",
    voltage_fault: "voltage fault",
    relay_fault: "relay fault"
  };
  if (isUa && mapUa[key]) return mapUa[key];
  if (!isUa && mapEn[key]) return mapEn[key];
  return raw;
};

const buildStatusVerbose = (summary, isUa) => {
  const code = String(summary?.statusCode || summary?.status || "").toUpperCase();
  if (code.includes("COMPLETE") || code === "OK") return isUa ? "Успішно" : "Completed";
  if (code.includes("STOP")) return isUa ? "Перервано користувачем" : "Stopped by user";
  if (code.includes("ERROR") || code.includes("FAULT")) {
    const reason = translateFaultReason(summary?.faultReason, isUa);
    return isUa ? `Помилка (${reason})` : `Error (${reason})`;
  }
  if (!String(summary?.status || "").trim()) return isUa ? "Невідомо" : "Unknown";
  return String(summary.status);
};

const buildStatusShort = (summary, isUa) => {
  const raw = String(summary?.statusCode || summary?.status || "").toUpperCase();
  if (!raw) return isUa ? "НЕВІДОМО" : "UNKNOWN";
  if (raw.includes("COMPLETE") || raw === "OK") return isUa ? "ГОТОВО" : "COMPLETE";
  if (raw.includes("STOP")) return isUa ? "ЗУПИНЕНО" : "STOPPED";
  if (raw.includes("ERROR") || raw.includes("FAULT")) return isUa ? "ПОМИЛКА" : "ERROR";
  if (raw.includes("COOL")) return isUa ? "ОХОЛОДЖ." : "COOL";
  return raw;
};

const estimateEnergyKwh = (detail) => {
  const direct =
    Number(detail?.summary?.energyKwh || 0) ||
    Number(detail?.summary?.energy_kwh || 0);
  if (direct > 0) return direct;
  const energyWh =
    Number(detail?.summary?.energyWh || 0) ||
    Number(detail?.summary?.energy_wh || 0);
  if (energyWh > 0) return energyWh / 1000;
  const rows = Array.isArray(detail?.data) ? detail.data : [];
  if (rows.length < 2) return 0;
  const tsDiv = detectTimestampSecondsDivisor(rows);
  let wh = 0;
  for (let i = 1; i < rows.length; i++) {
    const p0 = Number(rows[i - 1]?.power ?? rows[i - 1]?.pzem_power ?? rows[i - 1]?.watts ?? 0);
    const p1 = Number(rows[i]?.power ?? rows[i]?.pzem_power ?? rows[i]?.watts ?? 0);
    const t0 = Number(rows[i - 1]?.timestamp ?? 0);
    const t1 = Number(rows[i]?.timestamp ?? 0);
    const dt_h = Math.max(0, ((t1 - t0) / tsDiv) / 3600);
    wh += ((Math.max(0, p0) + Math.max(0, p1)) * 0.5) * dt_h;
  }
  return wh / 1000;
};

const buildStepDetails = (planned, isUa) => {
  if (!Array.isArray(planned) || planned.length < 2) return "--";
  const out = [];
  let prevTemp = Number(planned[0].temp || 0);
  for (let i = 1; i < planned.length; i++) {
    const p = planned[i];
    const temp = Number(p.temp || 0);
    const time = Number(p.t || 0);
    if (Math.abs(temp - prevTemp) < 0.5) continue;
    const start = Number(planned[i - 1].t || 0);
    const rampMin = Math.max(0, time - start);
    let holdEnd = time;
    let j = i + 1;
    while (j < planned.length && Math.abs(Number(planned[j].temp || 0) - temp) < 0.5) {
      holdEnd = Number(planned[j].t || holdEnd);
      j++;
    }
    const holdMin = Math.max(0, holdEnd - time);
    const rate = rampMin > 0 ? Math.round((Math.abs(temp - prevTemp) * 60) / rampMin) : 0;
    out.push(
      `${isUa ? "Крок" : "Step"} ${out.length + 1}: ` +
      `${isUa ? "швидк." : "rate"} ${rate}, ` +
      `${isUa ? "ціль" : "target"} ${Math.round(temp)}°, ` +
      `${isUa ? "витр." : "hold"} ${Math.round(holdMin)}`
    );
    prevTemp = temp;
    i = Math.max(i, j - 1);
  }
  return out.length ? out.join("\n") : "--";
};

const buildSeriesFromRows = (rows) => {
  if (!Array.isArray(rows) || !rows.length) return { planned: [], actual: [] };
  const firstTs = Number(rows[0]?.timestamp ?? 0);
  const tsDiv = detectTimestampSecondsDivisor(rows);
  const planned = [];
  const actual = [];
  rows.forEach((row, index) => {
    const ts = Number(row?.timestamp ?? firstTs);
    const t = index === 0 ? 0 : Math.max(0, ((ts - firstTs) / tsDiv) / 60);
    const target = Number(row?.target ?? row?.setpoint ?? row?.planned ?? NaN);
    const temp = Number(row?.temp ?? row?.temperature ?? row?.actual ?? NaN);
    if (Number.isFinite(target)) planned.push({ t, temp: target });
    if (Number.isFinite(temp)) actual.push({ t, temp });
  });
  return { planned, actual };
};

const normalizeSeriesPoints = (series) => {
  if (!Array.isArray(series)) return [];
  const out = [];
  for (const p of series) {
    if (!p || typeof p !== "object") continue;
    const tRaw = Number(
      p.t ?? p.time ?? p.ts ?? p.timestamp ?? p.x ?? p.minute ?? p.min ?? p.elapsedMin ?? 0
    );
    const tempRaw = Number(
      p.temp ?? p.temperature ?? p.actual ?? p.target ?? p.value ?? p.y ?? 0
    );
    if (!Number.isFinite(tRaw) || !Number.isFinite(tempRaw)) continue;
    out.push({ t: Math.max(0, tRaw), temp: tempRaw });
  }
  // Keep deterministic left-to-right draw order.
  out.sort((a, b) => a.t - b.t);
  return out;
};

const formatMinutesAxisLabel = (minutes, isUa) => {
  const m = Math.max(0, Math.round(Number(minutes || 0)));
  const h = Math.floor(m / 60);
  const rem = m % 60;
  return isUa ? `${h} год ${rem} хв` : `${h} h ${rem} min`;
};

const buildDynamicXTicks = (planned, maxT) => {
  const ticks = [0, maxT];
  if (Array.isArray(planned)) {
    for (const p of planned) {
      const x = Number(p?.t ?? 0);
      if (Number.isFinite(x)) ticks.push(Math.max(0, Math.min(maxT, x)));
    }
  }
  let unique = Array.from(new Set(ticks.map((v) => Math.round(v * 100) / 100))).sort((a, b) => a - b);
  // Keep chart readable on long programs.
  if (unique.length > 12) {
    const out = [unique[0]];
    const mid = unique.slice(1, -1);
    const step = Math.ceil(mid.length / 10);
    for (let i = 0; i < mid.length; i += step) out.push(mid[i]);
    out.push(unique[unique.length - 1]);
    unique = out;
  }
  return unique;
};

const hideSetpointBadges = () => {
  const nodes = Array.from(document.querySelectorAll("div,span"));
  for (const el of nodes) {
    if (!(el instanceof HTMLElement)) continue;
    const txt = String(el.textContent || "").trim().toLowerCase();
    const cls = String(el.className || "").toLowerCase();
    const isSetpointText =
      txt === "цільова" ||
      txt === "target" ||
      txt === "цільова температура" ||
      txt === "target temperature";
    const isTargetBadge = cls.includes("text-orange-500") && cls.includes("font-medium");
    if (isSetpointText || isTargetBadge) {
      el.style.display = "none";
      el.style.visibility = "hidden";
    }
  }
};

const ensureSetpointHideCss = () => {
  if (document.getElementById("kiln-hide-setpoint-style")) return;
  const style = document.createElement("style");
  style.id = "kiln-hide-setpoint-style";
  style.textContent =
    ".flex-1.p-6.relative .text-orange-500.font-medium{display:none !important;visibility:hidden !important;}";
  document.head.appendChild(style);
};

const computeVoltageStability = (detail, isUa) => {
  const rows = Array.isArray(detail?.data) ? detail.data : [];
  let minV = Infinity;
  let maxV = 0;
  rows.forEach((s) => {
    const v = Number(s?.voltage ?? 0);
    if (v > 1) {
      minV = Math.min(minV, v);
      maxV = Math.max(maxV, v);
    }
  });
  if (!Number.isFinite(minV) || maxV <= 0) return isUa ? "Н/Д" : "N/A";
  const span = Math.max(0, maxV - minV);
  return isUa
    ? `${Math.round(minV)}-${Math.round(maxV)} В (±${(span * 0.5).toFixed(1)}В)`
    : `${Math.round(minV)}-${Math.round(maxV)} V (+/-${(span * 0.5).toFixed(1)}V)`;
};

const computePeakTemp = (detail) => {
  const dataPeak = (Array.isArray(detail?.data) ? detail.data : []).reduce(
    (acc, row) => Math.max(acc, Number(row?.temp ?? 0)),
    0
  );
  const summaryPeak = Number(detail?.summary?.peakTemp ?? detail?.summary?.peak_temp ?? 0);
  return Math.max(0, dataPeak, summaryPeak);
};

const computeMaxLoad = (detail, isUa) => {
  const rows = Array.isArray(detail?.data) ? detail.data : [];
  const peakPower = rows.reduce(
    (acc, row) => Math.max(acc, Number(row?.power ?? row?.pzem_power ?? row?.watts ?? 0)),
    Number(detail?.summary?.peakPower ?? detail?.summary?.peak_power ?? 0)
  );
  const peakCurrent = rows.reduce(
    (acc, row) => Math.max(acc, Number(row?.current ?? 0)),
    Number(detail?.summary?.peakCurrent ?? detail?.summary?.peak_current ?? 0)
  );
  if (peakPower <= 0 && peakCurrent <= 0) return isUa ? "Н/Д" : "N/A";
  return isUa
    ? `${peakPower.toFixed(0)} Вт / ${peakCurrent.toFixed(1)} A`
    : `${peakPower.toFixed(0)} W / ${peakCurrent.toFixed(1)} A`;
};

const buildStepPlanActualText = (detail, isUa) => {
  const rows = Array.isArray(detail?.data) ? detail.data : [];
  if (rows.length < 2) return isUa ? "Н/Д" : "N/A";
  const tsDiv = detectTimestampSecondsDivisor(rows);
  const boundaries = [0];
  for (let i = 1; i < rows.length; i++) {
    const prev = Number(rows[i - 1]?.target ?? 0);
    const curr = Number(rows[i]?.target ?? 0);
    if (Math.abs(curr - prev) >= 0.5) boundaries.push(i);
  }
  if (boundaries[boundaries.length - 1] !== rows.length - 1) boundaries.push(rows.length - 1);
  const lines = [];
  for (let i = 0; i < boundaries.length - 1; i++) {
    const a = boundaries[i];
    const b = boundaries[i + 1];
    const target = Math.round(Number(rows[a]?.target ?? 0));
    const dtMin = Math.max(0, Math.round(((Number(rows[b]?.timestamp ?? 0) - Number(rows[a]?.timestamp ?? 0)) / tsDiv) / 60));
    const lbl = formatMinutesLabel(dtMin, isUa);
    lines.push(
      isUa
        ? `Крок ${i + 1}: ціль ${target}°, план ${lbl}, факт ${lbl}`
        : `Step ${i + 1}: target ${target}°, plan ${lbl}, actual ${lbl}`
    );
  }
  return lines.length ? lines.join("\n") : (isUa ? "Н/Д" : "N/A");
};

const buildChangeDiffText = (detail, isUa) => {
  const changes = Array.isArray(detail?.changes) ? detail.changes : [];
  if (!changes.length) return isUa ? "Н/Д" : "N/A";
  const lines = [];
  for (let i = 0; i < changes.length; i++) {
    if (i >= 14) {
      lines.push(isUa ? `... ще ${changes.length - i} змін` : `... ${changes.length - i} more changes`);
      break;
    }
    const ch = changes[i] || {};
    const actionKey = String(ch.action || "").trim();
    const action =
      actionKey === "add_temp" ? (isUa ? "Додано температуру" : "Added temperature") :
      actionKey === "add_time" ? (isUa ? "Додано час витримки" : "Added hold time") :
      actionKey === "set_rate" ? (isUa ? "Змінено швидкість" : "Changed rate") :
      actionKey === "skip_step" ? (isUa ? "Пропуск кроку" : "Skipped step") :
      (isUa ? "Зміна" : "Change");
    const field = String(ch.field || (isUa ? "параметр" : "field"));
    const before = Number(ch.before ?? 0);
    const after = Number(ch.after ?? 0);
    const delta = Number(ch.delta ?? (after - before));
    const step = Math.max(0, Number(ch.step || 0));
    lines.push(
      `${action}: ${field}, ${isUa ? "крок" : "step"} ${step}, ` +
      `${before.toFixed(1)} -> ${after.toFixed(1)} (Δ${delta >= 0 ? "+" : ""}${delta.toFixed(1)})`
    );
  }
  return lines.length ? lines.join("\n") : (isUa ? "Н/Д" : "N/A");
};

const detectIsUa = () => {
  if (historyOverlayIsUa === true || historyOverlayIsUa === false) return historyOverlayIsUa;
  const lang = String(localStorage.getItem("lang") || localStorage.getItem("language") || document.documentElement.lang || "").toLowerCase();
  if (lang.startsWith("uk") || lang.startsWith("ua")) return true;
  if (lang.startsWith("en")) return false;
  const current = document.querySelector("[aria-current='page']");
  if (current && current.textContent) {
    const tt = current.textContent.trim();
    if (tt.includes("Істор") || tt.includes("Панель") || tt.includes("Програми") || tt.includes("Налаштування")) return true;
    if (tt.includes("History") || tt.includes("Dashboard") || tt.includes("Programs") || tt.includes("Settings")) return false;
  }
  const navText = Array.from(document.querySelectorAll("header, nav, button, a"))
    .slice(0, 80)
    .map((el) => (el.textContent || "").trim())
    .join(" ");
  if (navText.includes("Панель") || navText.includes("Програми") || navText.includes("Налаштування") || navText.includes("Історія")) return true;
  if (navText.includes("Dashboard") || navText.includes("Programs") || navText.includes("Settings") || navText.includes("History")) return false;
  return (navigator.language || "").toLowerCase().startsWith("uk");
};

const mountHistoryUi = async () => {
  const overlay = ensureOverlay();
  const shell = overlay.querySelector(".history-shell");
  if (!(shell instanceof HTMLDivElement)) return;
  if (historyOverlayMounting) return;
  if (historyOverlayMounted) return;
  historyOverlayMounting = true;
  try {

  const isUa = detectIsUa();
  const t = (ua, en) => (isUa ? ua : en);

  shell.innerHTML = `
    <div class="flex md:flex-row flex-col gap-6 h-[calc(100vh-96px)] box-border min-h-0">
      <section class="history-panel flex md:flex w-full md:w-80 flex-col gap-4 shrink-0 p-4 overflow-hidden min-h-0">
        <div class="flex justify-between items-center mb-2">
          <h2 class="text-lg font-bold text-white">${t("Бібліотека", "Library")}</h2>
        </div>
        <div class="min-h-0 overflow-auto" id="history-list"></div>
      </section>

      <section class="flex-1 min-w-0 flex flex-col gap-6 min-h-0">
        <div class="history-panel mb-6 h-[320px] p-4 shrink-0 flex flex-col">
          <div class="flex items-center justify-end mb-2 shrink-0">
            <button id="history-clear" class="history-button history-button-danger p-2 text-sm">${t("Очистити історію", "Clear History")}</button>
          </div>
          <div class="flex-1 min-h-0 relative" id="history-chart-wrap">
            <canvas id="history-chart" class="absolute inset-0 w-full h-full block"></canvas>
          </div>
        </div>

        <div class="history-panel p-4 min-h-0 overflow-auto">
          <div class="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-3">
            <div class="history-card p-3">
              <div class="history-label">${t("Програма", "Program")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-program">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Початок", "Start")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-start">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Завершення", "End")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-end">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Статус", "Status")}</div>
              <div class="history-value text-base lg:text-lg" id="d-status">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Розширений статус", "Verbose status")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-status-verbose">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Тривалість", "Duration")}</div>
              <div class="history-value text-base lg:text-lg" id="d-duration">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Кроки", "Steps")}</div>
              <div class="history-value text-base lg:text-lg" id="d-steps">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Енергія", "Energy")}</div>
              <div class="history-value text-base lg:text-lg" id="d-energy">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Вартість", "Cost")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-cost">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Макс. навантаження", "Max load")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-max-load">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Пікова температура", "Peak temperature")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-peak-temp">--</div>
            </div>
            <div class="history-card p-3">
              <div class="history-label">${t("Стабільність напруги", "Voltage stability")}</div>
              <div class="history-muted mt-1 text-sm break-words" id="d-voltage-stability">--</div>
            </div>
          </div>
          <div class="history-card p-3 mt-3">
            <div class="history-label">${t("Кроки: план / факт", "Steps: plan / actual")}</div>
            <div class="history-muted mt-2 text-xs whitespace-pre-wrap max-h-56 overflow-auto" id="d-step-plan-actual">--</div>
          </div>
        </div>
      </section>
    </div>
  `;

  const listEl = shell.querySelector("#history-list");
  const clearBtn = shell.querySelector("#history-clear");
  const chartCanvas = shell.querySelector("#history-chart");
  const chartWrap = shell.querySelector("#history-chart-wrap");
  if (!(listEl instanceof HTMLDivElement) || !(chartCanvas instanceof HTMLCanvasElement) || !(chartWrap instanceof HTMLDivElement) || !(clearBtn instanceof HTMLButtonElement)) {
    return;
  }

  const ctx = chartCanvas.getContext("2d");
  if (!ctx) return;
  const chartFont = `${Math.max(10, Math.floor(parseFloat(getComputedStyle(document.body).fontSize || "14") * 0.72))}px ${HISTORY_THEME.font}`;

  let activeId = "";
  let currentPlanned = [];
  let currentActual = [];
  let listAbort = null;
  let detailAbort = null;
  let fallbackInFlight = false;
  let fallbackTried = false;

  const P = { left: 42, right: 14, top: 10, bottom: 26 };

  const resizeCanvas = () => {
    const w = Math.max(320, Math.floor(chartWrap.clientWidth));
    const h = Math.max(120, Math.floor(chartWrap.clientHeight));
    const dpr = Math.max(1, Math.floor(window.devicePixelRatio || 1));
    const nextW = w * dpr;
    const nextH = h * dpr;
    if (chartCanvas.width !== nextW) chartCanvas.width = nextW;
    if (chartCanvas.height !== nextH) chartCanvas.height = nextH;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { w, h };
  };


  const fillChartFromCurrentProgram = async () => {
    if (fallbackInFlight) return false;
    fallbackInFlight = true;
    try {
      const [statusRes, schedulesRes] = await Promise.all([
        fetch("/api/status", { cache: "no-store" }),
        fetch("/api/schedules", { cache: "no-store" })
      ]);
      if (!statusRes.ok || !schedulesRes.ok) return false;
      const status = await statusRes.json();
      const schedules = await schedulesRes.json();
      dashboardChartState.status = status;
      dashboardChartState.schedules = Array.isArray(schedules) ? schedules : [];
      const selected = resolveSelectedSchedule();
      if (selected) {
        dashboardChartState.selectedScheduleKey = `${selected.id || ""}|${selected.name || ""}`;
        dashboardChartState.planned = buildPlannedFromSchedule(selected);
      }
      upsertActualSample(status || {});
      currentPlanned = Array.isArray(dashboardChartState.planned) ? dashboardChartState.planned.slice() : [];
      currentActual = Array.isArray(dashboardChartState.actual) ? dashboardChartState.actual.slice() : [];
      return currentPlanned.length > 0 || currentActual.length > 0;
    } catch {
      return false;
    } finally {
      fallbackInFlight = false;
      fallbackTried = true;
    }
  };

  const ensureDetailSeries = async (detail) => {
    if (!detail || typeof detail !== "object") return detail;
    const hasPlanned = Array.isArray(detail.planned) && detail.planned.length > 0;
    const hasActual = Array.isArray(detail.actual) && detail.actual.length > 0;
    const hasRows = Array.isArray(detail.data) && detail.data.length > 0;
    if (hasPlanned || hasActual || hasRows) return detail;

    const wanted = String(
      detail?.summary?.scheduleName ||
      detail?.summary?.name ||
      detail?.summary?.title ||
      ""
    ).trim();
    if (!wanted) return detail;

    let schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
    if (!schedules.length) {
      try {
        const res = await fetch("/api/schedules", { cache: "no-store" });
        if (res.ok) {
          schedules = await res.json();
          dashboardChartState.schedules = Array.isArray(schedules) ? schedules : [];
        }
      } catch {}
    }
    if (!Array.isArray(schedules) || !schedules.length) return detail;
    const w = normName(wanted);
    const matched = schedules.find((s) => {
      const n = normName(s?.name || s?.title || "");
      const id = normName(s?.id || "");
      return n === w || id === w || n.includes(w) || w.includes(n);
    });
    if (matched) {
      detail.planned = buildPlannedFromSchedule(matched);
    }
    return detail;
  };

  const renderChart = () => {
    const { w, h } = resizeCanvas();
    ctx.clearRect(0, 0, w, h);
    const localPlanned = normalizeSeriesPoints(currentPlanned);
    const localActual = normalizeSeriesPoints(currentActual);
    const planned = localPlanned;
    const actual = localActual;
    const all = planned.concat(actual);
    if (!all.length) {
      // Hard fallback: build planned curve directly from any available schedule.
      const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
      const forcedSchedule = resolveSelectedSchedule() || schedules[0];
      if (forcedSchedule) {
        const forcedPlanned = normalizeSeriesPoints(buildPlannedFromSchedule(forcedSchedule));
        if (forcedPlanned.length) {
          drawProgramChart(
            chartCanvas,
            forcedPlanned,
            [],
            isUa,
            { minW: 320, minH: 158, padding: { left: 46, right: 12, top: 14, bottom: 26 }, hideSinglePoint: false }
          );
          return;
        }
      }
      if (!fallbackTried) {
        fillChartFromCurrentProgram().then((ok) => {
          if (ok) renderChart();
        });
      }
      if (fallbackInFlight) {
        ctx.fillStyle = HISTORY_THEME.surface;
        ctx.fillRect(0, 0, w, h);
        ctx.fillStyle = HISTORY_THEME.subtle;
        ctx.font = chartFont;
        ctx.fillText(t("Завантаження графіка...", "Loading chart..."), P.left + 8, P.top + 24);
      } else {
        drawProgramChart(
          chartCanvas,
          planned,
          actual,
          isUa,
          { minW: 320, minH: 158, padding: { left: 46, right: 12, top: 14, bottom: 26 }, hideSinglePoint: false }
        );
      }
      return;
    }
    drawProgramChart(
      chartCanvas,
      planned,
      actual,
      isUa,
      { minW: 320, minH: 158, padding: { left: 46, right: 12, top: 14, bottom: 26 }, hideSinglePoint: false }
    );
  };

  const setDetail = (detail) => {
    const scheduleName = detail.summary?.scheduleName || detail.summary?.name || detail.summary?.title || "—";
    const status = detail.summary?.status || "--";
    const duration = detail.summary?.duration ? fmtMinutes(detail.summary.duration, isUa) : "--";
    const steps = detail.summary?.totalSteps ?? "--";
    const ekwh = estimateEnergyKwh(detail);
    const costRaw = Number(detail.summary?.cost ?? 0);
    const peakTemp = computePeakTemp(detail);

    const sEl = shell.querySelector("#d-status");
    const svEl = shell.querySelector("#d-status-verbose");
    const pEl = shell.querySelector("#d-program");
    const stTimeEl = shell.querySelector("#d-start");
    const enTimeEl = shell.querySelector("#d-end");
    const dEl = shell.querySelector("#d-duration");
    const stEl = shell.querySelector("#d-steps");
    const eEl = shell.querySelector("#d-energy");
    const cEl = shell.querySelector("#d-cost");
    const mlEl = shell.querySelector("#d-max-load");
    const ptEl = shell.querySelector("#d-peak-temp");
    const vsEl = shell.querySelector("#d-voltage-stability");
    const spaEl = shell.querySelector("#d-step-plan-actual");
    if (sEl) sEl.textContent = buildStatusShort(detail.summary, isUa);
    if (svEl) svEl.textContent = buildStatusVerbose(detail.summary, isUa);
    if (pEl) pEl.textContent = scheduleName;
    if (stTimeEl) stTimeEl.textContent = detail.summary?.startTime || (isUa ? "Н/Д" : "N/A");
    if (enTimeEl) enTimeEl.textContent = detail.summary?.endTime || (isUa ? "Н/Д" : "N/A");
    if (dEl) dEl.textContent = duration;
    if (stEl) stEl.textContent = String(steps);
    if (eEl) eEl.textContent = ekwh > 0 ? (isUa ? `${ekwh.toFixed(2)} кВт·год` : `${ekwh.toFixed(2)} kWh`) : "--";
    if (cEl) cEl.textContent = costRaw > 0 ? (isUa ? `${costRaw.toFixed(2)} грн` : `${costRaw.toFixed(2)} UAH`) : (isUa ? "Н/Д (тариф не задано)" : "N/A (tariff not set)");
    if (mlEl) mlEl.textContent = computeMaxLoad(detail, isUa);
    if (ptEl) ptEl.textContent = peakTemp > 0 ? `${peakTemp.toFixed(1)}°C` : (isUa ? "Н/Д" : "N/A");
    if (vsEl) vsEl.textContent = computeVoltageStability(detail, isUa);

    const fallbackSeries = buildSeriesFromRows(detail?.data);
    currentPlanned = Array.isArray(detail.planned) && detail.planned.length
      ? normalizeSeriesPoints(detail.planned)
      : normalizeSeriesPoints(fallbackSeries.planned);
    currentActual = Array.isArray(detail.actual) && detail.actual.length
      ? normalizeSeriesPoints(detail.actual)
      : normalizeSeriesPoints(fallbackSeries.actual);
    if (spaEl) {
      const stepPlanActual = buildStepPlanActualText(detail, isUa);
      const changeDiff = buildChangeDiffText(detail, isUa);
      spaEl.textContent =
        changeDiff !== (isUa ? "Н/Д" : "N/A")
          ? `${stepPlanActual !== (isUa ? "Н/Д" : "N/A") ? stepPlanActual : buildStepDetails(currentPlanned, isUa)}\n\n${isUa ? "Зміни під час випалу:" : "Changes during firing:"}\n${changeDiff}`
          : (stepPlanActual !== (isUa ? "Н/Д" : "N/A") ? stepPlanActual : buildStepDetails(currentPlanned, isUa));
    }
    renderChart();
  };

  const loadDetail = async (id) => {
    fallbackTried = false;
    if (id === "__active__") {
      const statusRes = await fetch("/api/status", { cache: "no-store" });
      if (!statusRes.ok) return;
      const st = await statusRes.json();
      const detail = {
        summary: {
          scheduleName: t("Активний випал", "Active firing"),
          status: st.status || "RUNNING",
          totalSteps: Number(st.totalSteps ?? st.total ?? 0) || 0
        },
        planned: [{ t: 0, temp: Number(st.target || 0) || 0 }],
        actual: [{ t: 0, temp: Number(st.temp || 0) || 0 }]
      };
      activeId = id;
      setDetail(detail);
      Array.from(listEl.querySelectorAll("[data-id]")).forEach((el) => {
        if (!(el instanceof HTMLElement)) return;
        el.classList.toggle("history-list-card--active", el.dataset.id === activeId);
      });
      return;
    }
    if (detailAbort) detailAbort.abort();
    detailAbort = new AbortController();
    const res = await fetch(`/api/history/${id}`, { cache: "no-store", signal: detailAbort.signal });
    if (!res.ok) return;
    const detail = await res.json();
    await ensureDetailSeries(detail);
    activeId = id;
    setDetail(detail);
    Array.from(listEl.querySelectorAll("[data-id]")).forEach((el) => {
      if (!(el instanceof HTMLElement)) return;
      el.classList.toggle("history-list-card--active", el.dataset.id === activeId);
    });
  };

  const renderList = async (items) => {
    const total = Array.isArray(items) ? items.length : 0;
    const capped = Array.isArray(items) ? items.slice(0, 200) : [];
    listEl.innerHTML = "";

    if (total > capped.length) {
      const info = document.createElement("div");
      info.className = "history-muted text-xs mb-2";
      info.textContent = isUa
        ? `Показано ${capped.length} з ${total}`
        : `Showing ${capped.length} of ${total}`;
      listEl.appendChild(info);
    }

    const makeCard = (item) => {
      const card = document.createElement("div");
      card.dataset.id = item.id || "";
      card.className =
        "history-list-card p-4 cursor-pointer " +
        ((item.id || "") === activeId ? " history-list-card--active" : "");

      const title = document.createElement("div");
      title.className = "font-bold text-sm text-white truncate";
      title.textContent = item.title || item.name || "—";

      const subtitle = document.createElement("div");
      subtitle.className = "history-muted text-xs mt-1";
      subtitle.textContent = isUa ? (item.subtitle_ua || item.subtitle || "") : (item.subtitle_en || item.subtitle || "");

      const row = document.createElement("div");
      row.className = "flex items-center justify-between gap-2 mt-3";

      const status = document.createElement("span");
      const sc = String(item.statusCode || "");
      const ok = sc.includes("COMPLETE") || sc.includes("OK");
      status.className =
        "px-2 py-0.5 rounded-lg text-xs font-bold " + (ok ? "" : "text-white");
      status.style.background = ok ? HISTORY_THEME.accent : HISTORY_THEME.surfaceAlt;
      status.style.color = ok ? "#071018" : HISTORY_THEME.text;
      status.textContent = buildStatusShort({ statusCode: item.statusCode, status: item.status }, isUa);

      const detailBtn = document.createElement("button");
      detailBtn.className = "history-button p-2 text-xs";
      detailBtn.textContent = t("Деталі", "Details");
      detailBtn.onclick = (ev) => {
        ev.stopPropagation();
        if (item.id) loadDetail(item.id);
      };

      row.append(status, detailBtn);
      card.append(title, subtitle, row);
      card.onclick = () => item.id && loadDetail(item.id);
      return card;
    };

    const batchSize = 25;
    for (let i = 0; i < capped.length; i += batchSize) {
      const frag = document.createDocumentFragment();
      for (let j = i; j < Math.min(capped.length, i + batchSize); j++) {
        frag.appendChild(makeCard(capped[j]));
      }
      listEl.appendChild(frag);
      await new Promise((r) => requestAnimationFrame(r));
    }
  };

  clearBtn.onclick = async () => {
    if (!confirm(t("Очистити історію?", "Clear history?"))) return;
    await fetch("/api/history", { method: "DELETE" });
    await loadList();
  };

  if (!overlay.__historyChartTimer) {
    overlay.__historyChartTimer = setInterval(() => {
      if (!isHistoryActive()) return;
      fillChartFromCurrentProgram().then((ok) => {
        if (ok) renderChart();
      });
    }, 1800);
  }

  const loadList = async () => {
    if (listAbort) listAbort.abort();
    listAbort = new AbortController();
    const res = await fetch("/api/history", { cache: "no-store", signal: listAbort.signal });
    if (!res.ok) return;
    const items = await res.json();
    try {
      const stRes = await fetch("/api/status", { cache: "no-store" });
      if (stRes.ok) {
        const st = await stRes.json();
        if (st && st.firing) {
          items.unshift({
            id: "__active__",
            title: t("Активний випал", "Active firing"),
            status: st.status || "RUNNING",
            statusCode: "RUNNING",
            subtitle_ua: `T=${Math.round(Number(st.temp || 0))}° • ціль ${Math.round(Number(st.target || 0))}°`,
            subtitle_en: `T=${Math.round(Number(st.temp || 0))}° • target ${Math.round(Number(st.target || 0))}°`
          });
        }
      }
    } catch {}
    await renderList(items);
    if (items.length) {
      const ids = new Set(items.map((x) => x?.id).filter(Boolean));
      const targetId = (activeId && ids.has(activeId)) ? activeId : items[0].id;
      if (targetId) {
        await loadDetail(targetId);
      } else {
        currentPlanned = [];
        currentActual = [];
        renderChart();
      }
    } else {
      setDetail({ summary: {}, data: [], planned: [], actual: [] });
    }
  };

  overlay.__historyReload = loadList;

  new ResizeObserver(renderChart).observe(chartWrap);
  window.addEventListener("resize", renderChart);
  await loadList();
  historyOverlayMounted = true;
  historyOverlayMounting = false;
  } catch (e) {
    const msg = e && typeof e === "object" && "message" in e ? String(e.message) : String(e);
    shell.innerHTML = `<div class="p-6 text-white">
      <div class="text-lg font-bold mb-2">History overlay error</div>
      <div class="text-zinc-400 text-sm break-all">${msg}</div>
      <button id="history-close-fallback" class="mt-4 bg-zinc-800 border border-zinc-700 text-white rounded-xl px-3 py-1.5 text-sm">Close</button>
    </div>`;
    const btn = shell.querySelector("#history-close-fallback");
    if (btn instanceof HTMLButtonElement) {
      btn.onclick = () => {
        historyOverlayWanted = false;
        updateHistoryOverlay();
      };
    }
  } finally {
    historyOverlayMounting = false;
  }
};

const updateHistoryOverlay = () => {
  if (historyOverlayUpdateScheduled) return;
  historyOverlayUpdateScheduled = true;
  requestAnimationFrame(() => {
    historyOverlayUpdateScheduled = false;
  const overlay = ensureOverlay();
  const top = findHeaderBottom();
  overlay.style.top = `${top}px`;
  const active = isHistoryActive();
  const becameActive = historyOverlayLastActive !== true && active === true;
  overlay.style.display = active ? "block" : "none";
  overlay.style.pointerEvents = active ? "auto" : "none";
  if (historyOverlayLastActive !== active) {
    historyOverlayLastActive = active;
    if (!active) {
      historyOverlayWanted = null;
    }
  }
  if (active) mountHistoryUi();
  if (active && historyOverlayMounted && becameActive) {
    const reload = overlay.__historyReload;
    if (typeof reload === "function") reload();
  }
  if (!active) historyOverlayMounting = false;
  });
};

if (!window.__kilnHistoryOverlayListenersAdded) {
  window.__kilnHistoryOverlayListenersAdded = true;
  document.addEventListener(
    "click",
    (event) => {
      const target = event.target;
      if (!(target instanceof HTMLElement)) return;
      const el = target.closest("a,button");
      if (!(el instanceof HTMLElement)) return;
      const text = (el.textContent || "").trim();
      if (text === "Історія" || text === "History" || text.includes("Історія") || text.includes("History")) {
        historyOverlayIsUa = text.includes("Істор");
        historyOverlayWanted = true;
        setTimeout(updateHistoryOverlay, 0);
      } else if (
        text === "Програми" || text === "Programs" || text === "Панель" || text === "Dashboard" || text === "Налаштування" || text === "Settings" ||
        text.includes("Програми") || text.includes("Programs") || text.includes("Панель") || text.includes("Dashboard") || text.includes("Налаштування") || text.includes("Settings")
      ) {
        if (text.includes("Програми") || text.includes("Панель") || text.includes("Налаштування")) historyOverlayIsUa = true;
        if (text.includes("Programs") || text.includes("Dashboard") || text.includes("Settings")) historyOverlayIsUa = false;
        historyOverlayWanted = false;
        setTimeout(updateHistoryOverlay, 0);
      }
    },
    true
  );
}

if (!window.__kilnHistoryOverlayHistoryPatched) {
  window.__kilnHistoryOverlayHistoryPatched = true;
  const rawPush = history.pushState;
  history.pushState = function () {
    rawPush.apply(this, arguments);
    window.dispatchEvent(new Event("locationchange"));
  };
  const rawReplace = history.replaceState;
  history.replaceState = function () {
    rawReplace.apply(this, arguments);
    window.dispatchEvent(new Event("locationchange"));
  };
  window.addEventListener("popstate", () => window.dispatchEvent(new Event("locationchange")));
  window.addEventListener("locationchange", updateHistoryOverlay);
  window.addEventListener("resize", updateHistoryOverlay);
}
setTimeout(updateHistoryOverlay, 300);

const dashboardChartState = {
  schedules: [],
  status: null,
  selectedScheduleKey: "",
  lastPickedScheduleName: "",
  planned: [],
  actual: [],
  actualStartMs: 0,
  lastSchedulesFetchMs: 0,
  lastStatusFetchMs: 0
};

const normName = (v) => String(v || "").trim().toLowerCase().replace(/\s+/g, "_");

const isDashboardActive = () => {
  const p = (location.pathname || "").toLowerCase();
  const h = (location.hash || "").toLowerCase();
  if (p.includes("dashboard") || h.includes("dashboard")) return true;
  const current = document.querySelector("[aria-current='page']");
  const t = (current?.textContent || "").trim().toLowerCase();
  if (t.includes("program") || t.includes("програм")) return false;
  if (t.includes("history") || t.includes("істор")) return false;
  if (t.includes("settings") || t.includes("налашту")) return false;
  if (t.includes("dashboard") || t.includes("панель")) return true;
  return false;
};

const isProgramPageActive = () => {
  const current = document.querySelector("[aria-current='page']");
  const t = (current?.textContent || "").trim().toLowerCase();
  return t.includes("program") || t.includes("програм");
};

const cleanupDashboardOverlayCanvases = () => {
  const overlays = Array.from(document.querySelectorAll("canvas[data-kiln-dashboard-overlay='1']"));
  overlays.forEach((el) => {
    if (el instanceof HTMLCanvasElement) el.remove();
  });
};

const isElementVisible = (el) => {
  if (!(el instanceof HTMLElement)) return false;
  const r = el.getBoundingClientRect();
  if (r.width < 50 || r.height < 50) return false;
  const cs = getComputedStyle(el);
  return cs.display !== "none" && cs.visibility !== "hidden" && Number(cs.opacity || "1") > 0.01;
};

const findDashboardCanvas = () => {
  const canvases = Array.from(document.querySelectorAll("canvas"));
  let best = null;
  let bestArea = 0;
  for (const c of canvases) {
    if (!(c instanceof HTMLCanvasElement)) continue;
    if (c.getAttribute("data-kiln-dashboard-overlay") === "1") continue;
    if (c.closest(`#${HISTORY_OVERLAY_ID}`)) continue;
    if (!isElementVisible(c)) continue;
    const r = c.getBoundingClientRect();
    const area = r.width * r.height;
    // Prefer main dashboard chart proportions (~660x270).
    const fitsMainShape = r.width >= 520 && r.width <= 900 && r.height >= 200 && r.height <= 380;
    const score = area + (fitsMainShape ? 1_000_000 : 0);
    if (score > bestArea) {
      best = c;
      bestArea = score;
    }
  }
  return best;
};

const ensureDashboardPrimaryCanvas = (canvas) => {
  if (!(canvas instanceof HTMLCanvasElement)) return null;
  // Native-canvas-only mode: never create or replace canvases.
  const host = canvas.parentElement;
  if (!(host instanceof HTMLElement)) return canvas;
  const extra = Array.from(host.querySelectorAll(":scope > canvas[data-kiln-dashboard-overlay='1']"));
  extra.forEach((c) => c.remove());
  canvas.style.display = "block";
  canvas.style.visibility = "visible";
  return canvas;
};

const applyDashboardOverlayPresentation = (baseCanvas) => {
  if (!(baseCanvas instanceof HTMLCanvasElement)) return;
  // Single-canvas mode: keep native canvas visible and render custom data into it.
  baseCanvas.style.visibility = "visible";
  const host = baseCanvas.parentElement;
  if (host instanceof HTMLElement) {
    const labels = Array.from(host.querySelectorAll("div,span"));
    for (const el of labels) {
      if (!(el instanceof HTMLElement)) continue;
      const txt = String(el.textContent || "").trim().toLowerCase();
      if (
        txt === "цільова" ||
        txt === "target" ||
        txt === "цільова температура" ||
        txt === "target temperature" ||
        txt.includes("цільова") ||
        txt.includes("target")
      ) {
        el.style.display = "none";
      }
    }
  }
};

const normalizeSteps = (schedule) => {
  if (!schedule || typeof schedule !== "object") return [];
  const normalizeHold = (raw) => {
    const v = Number(raw);
    if (!Number.isFinite(v) || v <= 0) return 0;
    // Some payloads may carry hold in seconds (e.g. 600 for 10 min).
    if (v >= 600 && v % 60 === 0 && v / 60 <= 720) return v / 60;
    return v;
  };
  const raw = Array.isArray(schedule.steps)
    ? schedule.steps
    : (Array.isArray(schedule.segments) ? schedule.segments : []);
  return raw.map((s) => ({
    type: String(s?.step_type || s?.type || "").toLowerCase(),
    target: Number(s?.target ?? s?.temp ?? s?.temperature ?? s?.value ?? s?.to ?? NaN),
    rate: Number(s?.rate ?? s?.rampRate ?? s?.speed ?? NaN),
    hold: normalizeHold(s?.hold ?? s?.holdTime ?? s?.time ?? s?.duration ?? NaN)
  }));
};

const extractProgramEditorSeriesFromDom = () => {
  if (!isProgramPageActive()) return [];
  const rows = Array.from(document.querySelectorAll("tr, .grid, .flex"));
  const segments = [];
  for (const row of rows) {
    if (!(row instanceof HTMLElement)) continue;
    const txt = String(row.textContent || "").trim();
    if (!txt) continue;
    if (txt.length > 220) continue;
    const nums = (txt.match(/-?\d+(?:[.,]\d+)?/g) || [])
      .map((n) => Number(n.replace(",", ".")))
      .filter((v) => Number.isFinite(v));
    // Expected row shape: [idx, speed, target, time] or [speed, target, time]
    if (nums.length < 3) continue;
    let speed = nums[nums.length - 3];
    let target = nums[nums.length - 2];
    let timeMin = nums[nums.length - 1];
    if (!(speed > 0) || !(target >= 0) || !(timeMin >= 0)) continue;
    segments.push({ speed, target, timeMin });
  }
  if (!segments.length) return [];
  const points = [{ t: 0, temp: 25 }];
  let tMin = 0;
  let currentTemp = 25;
  for (const s of segments) {
    // Time column in editor is segment duration in minutes.
    tMin += Math.max(0, Number(s.timeMin || 0));
    currentTemp = Number(s.target || currentTemp);
    points.push({ t: tMin, temp: currentTemp });
  }
  return points;
};

const buildPlannedFromSchedule = (schedule) => {
  const points = [{ t: 0, temp: 25 }];
  const steps = normalizeSteps(schedule);
  let tMin = 0;
  let currentTemp = 25;
  for (const st of steps) {
    const ty = st.type;
    const isRamp = ty.includes("ramp") || ty === "0" || (Number.isFinite(st.target) && Number.isFinite(st.rate) && st.rate > 0);
    const isHold = ty.includes("hold") || ty === "1" || Number.isFinite(st.hold);
    if (isRamp && Number.isFinite(st.target)) {
      const ratePerHour = Number.isFinite(st.rate) && st.rate > 0 ? st.rate : 1;
      // Program editor uses ramp rate in deg/hour, convert to minutes on timeline.
      const dtRamp = (Math.abs(st.target - currentTemp) * 60) / ratePerHour;
      // In editor rows, time column often represents full segment duration.
      const dt = Number.isFinite(st.hold) && st.hold > 0 ? st.hold : dtRamp;
      tMin += Math.max(0, dt);
      currentTemp = st.target;
      points.push({ t: tMin, temp: currentTemp });
      continue;
    }
    if (isHold) {
      const hold = Number.isFinite(st.hold) ? Math.max(0, st.hold) : 0;
      tMin += hold;
      points.push({ t: tMin, temp: currentTemp });
      continue;
    }
    if (Number.isFinite(st.target)) {
      currentTemp = st.target;
      points.push({ t: tMin, temp: currentTemp });
    }
  }
  return points;
};

const extractScheduleKey = (status) => {
  if (!status || typeof status !== "object") return "";
  const keys = [
    "scheduleName",
    "selectedSchedule",
    "programName",
    "schedule",
    "program"
  ];
  for (const k of keys) {
    const v = String(status?.[k] || "").trim();
    if (v) return v;
  }
  return "";
};

const resolveSelectedScheduleFromDom = () => {
  const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
  if (!schedules.length) return null;
  const normEq = (a, b) => normName(a) === normName(b);

  const selects = Array.from(document.querySelectorAll("select"));
  for (const s of selects) {
    if (!(s instanceof HTMLSelectElement)) continue;
    const opt = s.options[s.selectedIndex];
    if (!opt) continue;
    const txt = String(opt.textContent || "").trim();
    if (!txt || txt.includes("Select") || txt.includes("Оберіть")) continue;
    const matched = schedules.find((x) => normEq(x?.name || x?.title || x?.id, txt) || normEq(x?.id, s.value));
    if (matched) return matched;
  }

  const activeLike = document.querySelector("[aria-selected='true'], .active, .is-active");
  if (activeLike instanceof HTMLElement) {
    const txt = String(activeLike.textContent || "").trim();
    if (txt) {
      const matched = schedules.find((x) => txt.includes(String(x?.name || "")) || txt.includes(String(x?.title || "")));
      if (matched) return matched;
    }
  }
  return null;
};

const resolveSelectedScheduleNearCanvas = (canvas) => {
  const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
  if (!schedules.length || !(canvas instanceof HTMLCanvasElement)) return null;
  const cRect = canvas.getBoundingClientRect();
  const cx = cRect.left + cRect.width * 0.5;
  const cy = cRect.top + cRect.height * 0.5;
  let best = null;
  let bestScore = Infinity;
  const candidates = Array.from(document.querySelectorAll("h1,h2,h3,h4,button,div,span,p,[role='option'],[aria-selected='true']"));
  for (const el of candidates) {
    if (!(el instanceof HTMLElement) || !isElementVisible(el)) continue;
    const txt = String(el.textContent || "").trim();
    if (!txt || txt.length > 120) continue;
    const matched = schedules.find((s) => {
      const n = s?.name || s?.title || s?.id || "";
      if (!n) return false;
      const nn = normName(n);
      const tt = normName(txt);
      return tt.includes(nn) || nn.includes(tt);
    });
    if (!matched) continue;
    const r = el.getBoundingClientRect();
    const ex = r.left + r.width * 0.5;
    const ey = r.top + r.height * 0.5;
    const d = Math.hypot(ex - cx, ey - cy);
    if (d < bestScore) {
      bestScore = d;
      best = matched;
    }
  }
  return best;
};

const inferScheduleFromStatus = () => {
  const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
  if (!schedules.length) return null;
  const status = dashboardChartState.status || {};
  const total = Number(status?.totalSteps ?? status?.total ?? NaN);
  const target = getStatusTarget(status);
  const withScore = schedules.map((s) => {
    const steps = normalizeSteps(s);
    const stepsCount = steps.length;
    const stepPenalty = Number.isFinite(total) ? Math.abs(stepsCount - total) * 50 : 0;
    const nearestTarget = Number.isFinite(target)
      ? steps.reduce((acc, st) => {
          const v = Number(st.target);
          if (!Number.isFinite(v)) return acc;
          return Math.min(acc, Math.abs(v - target));
        }, 9999)
      : 0;
    return { s, score: stepPenalty + nearestTarget };
  });
  withScore.sort((a, b) => a.score - b.score);
  return withScore[0]?.s || null;
};

const getStatusTemp = (status) => Number(status?.temp ?? status?.temperature ?? NaN);
const getStatusTarget = (status) => Number(status?.target ?? status?.setpoint ?? NaN);

const upsertActualSample = (status) => {
  const temp = getStatusTemp(status);
  if (!Number.isFinite(temp)) return;
  const elapsedSec = Number(status?.elapsedSec ?? status?.elapsed_seconds ?? status?.elapsed ?? NaN);
  if (!dashboardChartState.actualStartMs) dashboardChartState.actualStartMs = Date.now();
  const tMin = Number.isFinite(elapsedSec)
    ? Math.max(0, elapsedSec / 60)
    : Math.max(0, (Date.now() - dashboardChartState.actualStartMs) / 60000);
  const last = dashboardChartState.actual[dashboardChartState.actual.length - 1];
  if (last && Math.abs(last.t - tMin) < 0.01 && Math.abs(last.temp - temp) < 0.1) return;
  dashboardChartState.actual.push({ t: tMin, temp });
  if (dashboardChartState.actual.length > 240) {
    dashboardChartState.actual = dashboardChartState.actual.slice(-240);
  }
};

const drawProgramChart = (canvas, planned, actual, isUa, opts = {}) => {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const ratio = Math.max(1, window.devicePixelRatio || 1);
  const minW = Number.isFinite(opts.minW) ? opts.minW : 320;
  const minH = Number.isFinite(opts.minH) ? opts.minH : 140;
  const w = Math.max(minW, Math.floor(canvas.clientWidth || 660));
  const h = Math.max(minH, Math.floor(canvas.clientHeight || 270));
  if (canvas.width !== Math.floor(w * ratio)) canvas.width = Math.floor(w * ratio);
  if (canvas.height !== Math.floor(h * ratio)) canvas.height = Math.floor(h * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);

  const theme = resolveHistoryTheme();
  ctx.clearRect(0, 0, w, h);
  const P = opts.padding || { left: 46, right: 12, top: 14, bottom: 26 };
  const gw = Math.max(10, w - P.left - P.right);
  const gh = Math.max(10, h - P.top - P.bottom);

  ctx.fillStyle = theme.surface;
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = theme.border;
  ctx.strokeRect(0.5, 0.5, w - 1, h - 1);

  planned = normalizeSeriesPoints(planned);
  actual = normalizeSeriesPoints(actual);
  const hasPlan = planned.length > 0;
  const hasActual = actual.length > 0;
  if (!hasPlan && !hasActual) {
    ctx.fillStyle = theme.muted;
    ctx.font = "12px PT Sans, sans-serif";
    ctx.fillText(isUa ? "Немає даних графіка" : "No chart data", P.left + 8, P.top + 22);
    return;
  }

  const all = planned.concat(actual);
  const maxT = Math.max(1, ...all.map((p) => Number(p.t || 0)));
  const maxY = Math.max(50, ...all.map((p) => Number(p.temp || 0)));
  const mapX = (t) => P.left + (Math.max(0, t) / maxT) * gw;
  const mapY = (v) => P.top + gh - (Math.max(0, v) / maxY) * gh;

  const buildProgramXTicks = () => {
    const ticks = [0, maxT];
    if (Array.isArray(planned) && planned.length > 1) {
      for (const p of planned) {
        const t = Number(p?.t ?? 0);
        if (Number.isFinite(t)) ticks.push(Math.max(0, Math.min(maxT, t)));
      }
    }
    const unique = Array.from(new Set(ticks.map((v) => Math.round(v * 100) / 100))).sort((a, b) => a - b);
    if (unique.length <= 10) return unique;
    const reduced = [unique[0]];
    const interior = unique.slice(1, -1);
    const step = Math.ceil(interior.length / 8);
    for (let i = 0; i < interior.length; i += step) reduced.push(interior[i]);
    reduced.push(unique[unique.length - 1]);
    return reduced;
  };

  ctx.strokeStyle = theme.grid;
  ctx.lineWidth = 1;
  const xTicks = buildProgramXTicks();
  for (const t of xTicks) {
    const x = mapX(t);
    ctx.beginPath(); ctx.moveTo(x, P.top); ctx.lineTo(x, P.top + gh); ctx.stroke();
  }
  for (let i = 0; i <= 5; i++) {
    const y = P.top + (gh * i) / 5;
    ctx.beginPath(); ctx.moveTo(P.left, y); ctx.lineTo(P.left + gw, y); ctx.stroke();
  }

  const drawLine = (pts, color) => {
    if (!Array.isArray(pts) || pts.length < 1) return;
    if (pts.length === 1) {
      if (opts.hideSinglePoint) return;
      const x = mapX(Number(pts[0].t || 0));
      const y = mapY(Number(pts[0].temp || 0));
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(x, y, 3, 0, Math.PI * 2);
      ctx.fill();
      return;
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    pts.forEach((p, i) => {
      const x = mapX(Number(p.t || 0));
      const y = mapY(Number(p.temp || 0));
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  };

  drawLine(planned, theme.planned);
  if (hasActual) drawLine(actual, theme.actual);

  const formatPointTimeLabel = (minutes) => formatMinutesAxisLabel(minutes, isUa);

  const drawBreakLabels = (pts) => {
    if (!Array.isArray(pts) || pts.length < 3) return;
    const labelPts = [];
    for (let i = 1; i < pts.length - 1; i++) {
      const prev = Number(pts[i - 1].temp || 0);
      const cur = Number(pts[i].temp || 0);
      const next = Number(pts[i + 1].temp || 0);
      const d1 = cur - prev;
      const d2 = next - cur;
      // Mark segment boundaries (slope/hold transitions).
      if (Math.abs(d1 - d2) > 0.1 || Math.abs(d1) < 0.1 || Math.abs(d2) < 0.1) {
        labelPts.push(pts[i]);
      }
    }
    const unique = [];
    for (const p of labelPts) {
      const key = `${Math.round(Number(p.t || 0) * 10)}:${Math.round(Number(p.temp || 0) * 10)}`;
      if (!unique.some((u) => u.__k === key)) unique.push({ ...p, __k: key });
    }
    const finalPts = unique.slice(0, 8);
    ctx.fillStyle = theme.muted;
    ctx.font = "10px PT Sans, sans-serif";
    ctx.textBaseline = "middle";
    let lastBottomLabelRight = -Infinity;
    const usedTempBoxes = [];
    const intersects = (a, b) =>
      !(a.x + a.w < b.x || b.x + b.w < a.x || a.y + a.h < b.y || b.y + b.h < a.y);
    for (const p of finalPts) {
      const x = mapX(Number(p.t || 0));
      const y = mapY(Number(p.temp || 0));
      const tLabel = `${Math.round(Number(p.temp || 0))}°C`;
      const xLabel = formatPointTimeLabel(Number(p.t || 0));
      const tw = ctx.measureText(tLabel).width;
      const xw = ctx.measureText(xLabel).width;
      const tx = Math.max(P.left + 2, Math.min(P.left + gw - tw - 2, x - tw - 6));
      // Prefer positions away from the polyline and avoid label collisions.
      const tyCandidates = [y - 11, y + 12, y - 20, y + 22];
      let chosenTy = Math.max(P.top + 8, Math.min(P.top + gh - 8, y - 11));
      for (const cand of tyCandidates) {
        const ty = Math.max(P.top + 8, Math.min(P.top + gh - 8, cand));
        const rect = { x: tx - 2, y: ty - 6, w: tw + 4, h: 12 };
        const overlap = usedTempBoxes.some((b) => intersects(rect, b));
        if (!overlap && Math.abs(ty - y) >= 8) {
          chosenTy = ty;
          break;
        }
      }
      const chosenRect = { x: tx - 2, y: chosenTy - 6, w: tw + 4, h: 12 };
      usedTempBoxes.push(chosenRect);
      ctx.fillText(tLabel, tx, chosenTy);
      // Avoid overlap with axis end labels and with previous bottom labels.
      const lx = Math.max(P.left + 6, Math.min(P.left + gw - xw - 6, x - xw * 0.5));
      if (lx > lastBottomLabelRight + 10 && lx + xw < (P.left + gw - 34)) {
        ctx.fillText(xLabel, lx, h - 14);
        lastBottomLabelRight = lx + xw;
      }
    }
    ctx.textBaseline = "alphabetic";
  };
  drawBreakLabels(planned);

  ctx.fillStyle = theme.muted;
  ctx.font = "11px PT Sans, sans-serif";
  ctx.fillText(formatPointTimeLabel(0), P.left, h - 8);
  const maxTLabel = formatPointTimeLabel(maxT);
  const maxTW = ctx.measureText(maxTLabel).width;
  ctx.fillText(maxTLabel, P.left + gw - maxTW, h - 8);
  ctx.fillText(`${Math.round(maxY)}${isUa ? "°C" : "°C"}`, 8, P.top + 8);
  ctx.fillText(`${Math.round(maxY * 0.5)}${isUa ? "°C" : "°C"}`, 8, P.top + gh * 0.5 + 4);
};

const drawNativeBreakAnnotations = (chart, isUa) => {
  try {
    const xScale = chart?.scales?.x;
    const yScale = chart?.scales?.y;
    const ds = chart?.data?.datasets?.[0]?.data;
    if (!xScale || !yScale || !Array.isArray(ds) || ds.length < 2) return;
    const pts = ds
      .map((p) => ({ x: Number(p?.x ?? 0), y: Number(p?.y ?? 0) }))
      .filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y))
      .sort((a, b) => a.x - b.x)
      .slice(1, 12); // target-reaching points, skip origin
    if (!pts.length) return;
    const ctx = chart.ctx;
    ctx.save();
    ctx.strokeStyle = "rgba(96,165,250,0.45)";
    ctx.fillStyle = "rgba(226,232,240,0.95)";
    ctx.lineWidth = 1.2;
    ctx.font = "11px PT Sans, sans-serif";
    ctx.textBaseline = "middle";
    let lastBottomRight = -Infinity;
    for (const p of pts) {
      const px = xScale.getPixelForValue(p.x);
      const py = yScale.getPixelForValue(p.y);
      ctx.beginPath();
      ctx.moveTo(px, yScale.top);
      ctx.lineTo(px, yScale.bottom);
      ctx.stroke();
      const tLabel = `${Math.round(p.y)}°C`;
      const xLabel = formatMinutesAxisLabel(p.x, isUa);
      const tw = ctx.measureText(tLabel).width;
      const xw = ctx.measureText(xLabel).width;
      const tx = Math.max(xScale.left + 2, Math.min(xScale.right - tw - 2, px - tw - 8));
      const ty = Math.max(yScale.top + 10, Math.min(yScale.bottom - 10, py - 12));
      ctx.fillText(tLabel, tx, ty);
      const lx = Math.max(xScale.left + 6, Math.min(xScale.right - xw - 6, px - xw * 0.5));
      if (lx > lastBottomRight + 10) {
        ctx.fillText(xLabel, lx, yScale.bottom + 16);
        lastBottomRight = lx + xw;
      }
    }
    ctx.restore();
  } catch {}
};

const applyNativeChartConfig = (chart, isUa) => {
  const toMinuteUnit = (pts) => {
    if (!Array.isArray(pts) || !pts.length) return [];
    const maxX = Math.max(...pts.map((p) => Number(p?.t ?? 0)));
    const hasFraction = pts.some((p) => Math.abs(Number(p?.t ?? 0) % 1) > 1e-3);
    // Heuristic: very small range + fractional values likely means hours axis from native chart.
    const factor = maxX <= 8 && hasFraction ? 60 : 1;
    return pts.map((p) => ({ t: Number(p.t || 0) * factor, temp: Number(p.temp || 0) }));
  };

  let planned = normalizeSeriesPoints(dashboardChartState.planned || []);
  let actual = normalizeSeriesPoints(dashboardChartState.actual || []);
  const ds0 = Array.isArray(chart?.data?.datasets?.[0]?.data) ? chart.data.datasets[0].data : [];
  const ds1 = Array.isArray(chart?.data?.datasets?.[1]?.data) ? chart.data.datasets[1].data : [];
  const chartPlanned = toMinuteUnit(normalizeSeriesPoints(ds0));
  const chartActual = toMinuteUnit(normalizeSeriesPoints(ds1));
  if (planned.length < 2 && chartPlanned.length >= 2) planned = chartPlanned;
  if (actual.length < 2 && chartActual.length >= 2) actual = chartActual;
  if (planned.length < 2 && actual.length < 2) return false;

  const all = planned.concat(actual);
  const maxT = Math.max(1, ...all.map((p) => Number(p.t || 0)));
  const rawMaxY = Math.max(50, ...all.map((p) => Number(p.temp || 0)));
  const maxY = Math.ceil(rawMaxY / 10) * 10;
  const dynamicXTicks = buildDynamicXTicks(planned, maxT);

  chart.data = chart.data || {};
  chart.data.datasets = [
    {
      label: isUa ? "План" : "Planned",
      data: planned.map((p) => ({ x: Number(p.t || 0), y: Number(p.temp || 0) })),
      borderColor: "#60A5FA",
      borderWidth: 2,
      pointRadius: 0,
      pointHoverRadius: 3,
      tension: 0
    },
    ...(actual.length
      ? [{
          label: isUa ? "Факт" : "Actual",
          data: actual.map((p) => ({ x: Number(p.t || 0), y: Number(p.temp || 0) })),
          borderColor: "#10B981",
          borderWidth: 2,
          pointRadius: 0,
          pointHoverRadius: 3,
          tension: 0
        }]
      : [])
  ];

  chart.options = chart.options || {};
  chart.options.animation = false;
  chart.options.responsive = true;
  chart.options.maintainAspectRatio = false;
  chart.options.parsing = false;
  chart.options.plugins = chart.options.plugins || {};
  chart.options.plugins.legend = chart.options.plugins.legend || {};
  chart.options.plugins.legend.display = false;
  chart.options.plugins.tooltip = chart.options.plugins.tooltip || {};
  chart.options.plugins.tooltip.callbacks = chart.options.plugins.tooltip.callbacks || {};
  chart.options.plugins.tooltip.callbacks.title = (items) => {
    const x = Number(items?.[0]?.parsed?.x ?? 0);
    return formatMinutesAxisLabel(x, isUa);
  };
  chart.options.plugins.tooltip.callbacks.label = (ctx) => {
    const y = Number(ctx?.parsed?.y ?? 0);
    return `${ctx.dataset?.label || ""}: ${Math.round(y)}°C`;
  };

  chart.options.scales = chart.options.scales || {};
  chart.options.scales.x = {
    ...(chart.options.scales.x || {}),
    type: "linear",
    min: 0,
    max: maxT,
    afterBuildTicks: (scale) => {
      scale.ticks = dynamicXTicks.map((v) => ({ value: v }));
    },
    ticks: {
      ...(chart.options.scales.x?.ticks || {}),
      autoSkip: false,
      callback: (v) => formatMinutesAxisLabel(v, isUa)
    },
    grid: {
      ...(chart.options.scales.x?.grid || {}),
      color: "rgba(148,163,184,0.20)"
    }
  };
  chart.options.scales.y = {
    ...(chart.options.scales.y || {}),
    min: 0,
    max: maxY,
    ticks: {
      ...(chart.options.scales.y?.ticks || {}),
      callback: (v) => `${Math.round(Number(v || 0))}°C`
    },
    grid: {
      ...(chart.options.scales.y?.grid || {}),
      color: "rgba(148,163,184,0.16)"
    }
  };
  return true;
};

const ensureNativeChartInterception = () => {
  if (window.__kilnChartUpdateIntercepted) return;
  const ChartCtor = window.Chart;
  if (!ChartCtor || !ChartCtor.prototype || typeof ChartCtor.prototype.update !== "function") return;
  const originalUpdate = ChartCtor.prototype.update;
  ChartCtor.prototype.update = function (...args) {
    try {
      if (!isDashboardActive()) {
        return originalUpdate.apply(this, args);
      }
      if (!this.__kilnApplying) {
        const target = findDashboardCanvas();
        const sameCanvas = this?.canvas && target && (this.canvas === target || this.canvas.parentElement === target.parentElement);
        if (sameCanvas) {
          const isUa = historyOverlayIsUa !== null
            ? historyOverlayIsUa
            : ((navigator.language || "").toLowerCase().startsWith("uk"));
          applyNativeChartConfig(this, isUa);
        }
      }
    } catch {}
    const out = originalUpdate.apply(this, args);
    try {
      if (isDashboardActive()) {
        const isUa = historyOverlayIsUa !== null
          ? historyOverlayIsUa
          : ((navigator.language || "").toLowerCase().startsWith("uk"));
        drawNativeBreakAnnotations(this, isUa);
      }
    } catch {}
    return out;
  };
  window.__kilnChartUpdateIntercepted = true;
};

const updateNativeChartInstance = (canvas, isUa) => {
  try {
    const ChartCtor = window.Chart;
    if (!ChartCtor) return false;
    let chart = null;
    if (typeof ChartCtor.getChart === "function") {
      chart = ChartCtor.getChart(canvas);
    }
    if (!chart && ChartCtor.instances) {
      const values = Array.isArray(ChartCtor.instances)
        ? ChartCtor.instances
        : Object.values(ChartCtor.instances);
      chart = values.find((ch) => ch?.canvas === canvas) || null;
      if (!chart) {
        // Fallback: pick largest visible chart on current dashboard screen.
        let best = null;
        let bestArea = 0;
        for (const ch of values) {
          const c = ch?.canvas;
          if (!(c instanceof HTMLCanvasElement)) continue;
          if (!isElementVisible(c)) continue;
          if (c.closest(`#${HISTORY_OVERLAY_ID}`)) continue;
          const r = c.getBoundingClientRect();
          const area = r.width * r.height;
          if (area > bestArea) {
            best = ch;
            bestArea = area;
          }
        }
        chart = best;
      }
    }
    if (!chart) return false;

    if (!applyNativeChartConfig(chart, isUa)) return false;
    chart.__kilnApplying = true;
    try {
      chart.update("none");
      drawNativeBreakAnnotations(chart, isUa);
    } finally {
      chart.__kilnApplying = false;
    }
    return true;
  } catch {
    return false;
  }
};

const updateDashboardCardLabels = () => {
  const ua = historyOverlayIsUa !== null
    ? historyOverlayIsUa
    : ((navigator.language || "").toLowerCase().startsWith("uk"));
  const labels = Array.from(document.querySelectorAll("div.text-\\[10px\\],div.uppercase,div"));
  for (const el of labels) {
    if (!(el instanceof HTMLElement)) continue;
    const txt = String(el.textContent || "").trim();
    if (txt === "Поточна Т-ра") {
      el.textContent = ua ? "Поточна температура" : "Current temperature";
    } else if (txt === "Target") {
      el.textContent = ua ? "Цільова температура" : "Target temperature";
    }
  }
  hideSetpointBadges();
};

const resolveSelectedSchedule = () => {
  const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
  if (!schedules.length) return null;
  if (dashboardChartState.lastPickedScheduleName) {
    const byPicked = schedules.find((s) => {
      const name = s?.name || s?.title || s?.id || "";
      return normName(name) === normName(dashboardChartState.lastPickedScheduleName);
    });
    if (byPicked) return byPicked;
  }
  const byDom = resolveSelectedScheduleFromDom();
  if (byDom) return byDom;
  const key = extractScheduleKey(dashboardChartState.status);
  if (key) {
    const norm = key.toLowerCase().replace(/\s+/g, "_");
    const byKey = schedules.find((s) => {
      const id = String(s?.id || "").toLowerCase();
      const name = String(s?.name || s?.title || "").toLowerCase();
      const nname = name.replace(/\s+/g, "_");
      return id === norm || name === key.toLowerCase() || nname === norm;
    });
    if (byKey) return byKey;
  }
  const byStatus = inferScheduleFromStatus();
  if (byStatus) return byStatus;
  return schedules[0];
};

const refreshDashboardChartData = async () => {
  const now = Date.now();
  if (now - dashboardChartState.lastSchedulesFetchMs > 15000) {
    dashboardChartState.lastSchedulesFetchMs = now;
    try {
      const rs = await fetch("/api/schedules", { cache: "no-store" });
      if (rs.ok) dashboardChartState.schedules = await rs.json();
    } catch {}
  }
  if (now - dashboardChartState.lastStatusFetchMs > 1500) {
    dashboardChartState.lastStatusFetchMs = now;
    try {
      const rs = await fetch("/api/status", { cache: "no-store" });
      if (rs.ok) dashboardChartState.status = await rs.json();
    } catch {}
  }
  const selected = resolveSelectedSchedule();
  const scheduleKey = selected ? `${selected.id || ""}|${selected.name || ""}` : "";
  if (scheduleKey && scheduleKey !== dashboardChartState.selectedScheduleKey) {
    dashboardChartState.selectedScheduleKey = scheduleKey;
    dashboardChartState.planned = buildPlannedFromSchedule(selected);
    dashboardChartState.actual = [];
    dashboardChartState.actualStartMs = 0;
  }
  const editorPlanned = extractProgramEditorSeriesFromDom();
  if (editorPlanned.length > 1) {
    dashboardChartState.planned = editorPlanned;
  }
  const statusTarget = getStatusTarget(dashboardChartState.status);
  if (dashboardChartState.actual.length === 0 && Number.isFinite(statusTarget)) {
    dashboardChartState.actual.push({ t: 0, temp: statusTarget });
  }
  upsertActualSample(dashboardChartState.status || {});
};

const tickDashboardCanvasChart = async () => {
  if (isHistoryActive()) return;
  if (!isDashboardActive()) {
    cleanupDashboardOverlayCanvases();
    return;
  }
  await refreshDashboardChartData();
  ensureNativeChartInterception();
  const baseCanvas = findDashboardCanvas();
  if (!(baseCanvas instanceof HTMLCanvasElement)) return;
  const primaryCanvas = ensureDashboardPrimaryCanvas(baseCanvas) || baseCanvas;
  const near = resolveSelectedScheduleNearCanvas(baseCanvas);
  if (near) {
    dashboardChartState.lastPickedScheduleName = String(near.name || near.title || near.id || "");
  }
  // Single-canvas mode: apply custom chart settings directly to native canvas.
  cleanupDashboardOverlayCanvases();
  applyDashboardOverlayPresentation(primaryCanvas);
  const isUa = historyOverlayIsUa !== null
    ? historyOverlayIsUa
    : ((navigator.language || "").toLowerCase().startsWith("uk"));
  // Native-canvas-only mode: mutate only existing Chart.js instance.
  updateNativeChartInstance(primaryCanvas, isUa);
  updateDashboardCardLabels();
  ensureSetpointHideCss();
};

if (!window.__kilnDashboardCanvasChartInit) {
  window.__kilnDashboardCanvasChartInit = true;
  document.addEventListener("click", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLElement)) return;
    const txt = String((target.closest("button,[role='option'],li,div,span")?.textContent || "")).trim();
    if (!txt) return;
    const schedules = Array.isArray(dashboardChartState.schedules) ? dashboardChartState.schedules : [];
    const hit = schedules.find((s) => {
      const n = s?.name || s?.title || s?.id || "";
      if (!n) return false;
      const nn = normName(n);
      const tt = normName(txt);
      return tt.includes(nn) || nn.includes(tt);
    });
    if (hit) {
      dashboardChartState.lastPickedScheduleName = String(hit.name || hit.title || hit.id || "");
      dashboardChartState.selectedScheduleKey = "";
      dashboardChartState.actual = [];
      dashboardChartState.actualStartMs = 0;
    }
  }, true);
  setInterval(() => {
    tickDashboardCanvasChart().catch(() => {});
  }, 900);
  window.addEventListener("resize", () => {
    tickDashboardCanvasChart().catch(() => {});
  });
  setTimeout(() => {
    tickDashboardCanvasChart().catch(() => {});
  }, 1200);
}

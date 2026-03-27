﻿﻿﻿const originalFetch = window.fetch.bind(window);

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
  const html = await (await originalFetch(`/index.html?b=${Date.now()}`, { cache: "no-store" })).text();
  const m = html.match(/src=\"(\/assets\/index-[^\"]+\.js)\"/);
  const appModulePath = m ? m[1] : "/assets/index-39d65f44.js";
  await import(`${appModulePath}?v=${Date.now()}`);
})();

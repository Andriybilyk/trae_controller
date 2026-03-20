const originalSetInterval = window.setInterval.bind(window);
const originalFetch = window.fetch.bind(window);

window.setInterval = (handler, timeout, ...args) => {
  if (typeof handler === "function") {
    try {
      const src = handler.toString();
      if (src.includes("/status") && Number(timeout) >= 1000) {
        timeout = 250;
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

  const res = await originalFetch(input, init);

  if (method.toUpperCase() === "DELETE" && url.includes("/api/schedules")) {
    if (res.ok) {
      setTimeout(() => {
        window.location.reload();
      }, 50);
    }
  }

  return res;
};

const html = await (await originalFetch("/index.html", { cache: "no-store" })).text();
const m = html.match(/src="(\/assets\/index-[^"]+\.js)"/);
await import(m ? m[1] : "/assets/index-f98ef54f.js");

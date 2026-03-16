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

await import("/assets/index-da16b362.js");

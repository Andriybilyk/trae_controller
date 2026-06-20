import json
import os
import sys
import time
from urllib import request


def load_env(path: str) -> dict:
    out = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except FileNotFoundError:
        return out
    return out


def post_event(url: str, payload: dict) -> None:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with request.urlopen(req, timeout=2.0) as resp:
        resp.read()


def main() -> int:
    try:
        import serial  # type: ignore
    except Exception as e:
        print("pyserial_missing", repr(e))
        return 2

    port = os.environ.get("SERIAL_PORT", "COM11")
    baud = int(os.environ.get("SERIAL_BAUD", "115200"))
    session = os.environ.get("DEBUG_SESSION_ID", "program-delete-freeze")
    env_path = os.environ.get("DEBUG_ENV_PATH", os.path.join(".dbg", f"{session}.env"))
    env = load_env(env_path)
    url = os.environ.get("DEBUG_SERVER_URL") or env.get("DEBUG_SERVER_URL")
    if not url:
        print("missing_DEBUG_SERVER_URL")
        return 2

    run_id = os.environ.get("RUN_ID", "pre")
    host = os.environ.get("HOST", "pc")

    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except Exception as e:
        print("serial_open_failed", port, repr(e))
        return 3

    print(f"forwarding {port}@{baud} -> {url}")
    buf = b""
    last_post = time.time()
    try:
        while True:
            chunk = ser.read(512)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        text = line.decode("utf-8", errors="replace").rstrip("\r")
                    except Exception:
                        text = repr(line)
                    payload = {
                        "ts": time.time(),
                        "sessionId": session,
                        "runId": run_id,
                        "hypothesisId": "H1",
                        "source": host,
                        "event": "uart",
                        "message": text,
                    }
                    post_event(url, payload)
                    last_post = time.time()
            else:
                if time.time() - last_post > 20:
                    post_event(
                        url,
                        {
                            "ts": time.time(),
                            "sessionId": session,
                            "runId": run_id,
                            "hypothesisId": "H1",
                            "source": host,
                            "event": "heartbeat",
                            "message": "no_uart_data",
                        },
                    )
                    last_post = time.time()
    except KeyboardInterrupt:
        return 0
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())


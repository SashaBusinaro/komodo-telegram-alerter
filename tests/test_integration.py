#!/usr/bin/env python3
import concurrent.futures
import http.server
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request


class MockTelegramState:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.next_status = 200
        self.next_body = {"ok": True, "result": {"message_id": 1}}
        self.delay_sec = 0.0
        self.raw_response = None


def make_handler(state: MockTelegramState):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            body = json.loads(raw.decode("utf-8"))

            with state.lock:
                delay = state.delay_sec
                status = state.next_status
                next_body = state.next_body
                raw_resp = state.raw_response

            if delay > 0:
                time.sleep(delay)

            with state.lock:
                state.requests.append({"path": self.path, "body": body})

            resp_bytes = (
                raw_resp
                if raw_resp is not None
                else json.dumps(next_body).encode("utf-8")
            )
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)

        def log_message(self, format, *args):
            pass

    return Handler


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_req(method: str, url: str, body=None, headers=None):
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else body.encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def main():
    if len(sys.argv) < 2:
        print("Usage: test_integration.py <binary_path>", file=sys.stderr)
        sys.exit(2)
    binary = sys.argv[1]

    state = MockTelegramState()
    tg_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
    tg_port = tg_server.server_address[1]
    tg_thread = threading.Thread(target=tg_server.serve_forever, daemon=True)
    tg_thread.start()

    app_port = find_free_port()
    env = os.environ.copy()
    env["PORT"] = str(app_port)
    env["BIND_ADDR"] = "127.0.0.1"
    env["TELEGRAM_API_URL"] = f"http://127.0.0.1:{tg_port}"
    env["MAX_BODY_BYTES"] = "4096"
    env["LOG_LEVEL"] = "debug"
    env.pop("HTTPS_PROXY", None)
    env.pop("https_proxy", None)
    env.pop("HTTP_PROXY", None)
    env.pop("http_proxy", None)

    stdout_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    stderr_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    proc = subprocess.Popen(
        [binary],
        env=env,
        stdout=stdout_file,
        stderr=stderr_file,
        text=True,
    )

    base_url = f"http://127.0.0.1:{app_port}"
    try:
        # Wait for server readiness
        ready = False
        for _ in range(50):
            if proc.poll() is not None:
                break
            try:
                status, data = http_req("GET", f"{base_url}/health")
                if status == 200 and data == {"status": "ok"}:
                    ready = True
                    break
            except Exception:
                time.sleep(0.05)

        if not ready:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=2)
            stdout_file.seek(0)
            stderr_file.seek(0)
            out = stdout_file.read()
            err = stderr_file.read()
            raise RuntimeError(f"Server failed to start:\nstdout={out}\nstderr={err}")

        # 1. GET /health
        status, data = http_req("GET", f"{base_url}/health")
        assert status == 200 and data == {"status": "ok"}, (status, data)

        # 2. Missing params -> 400
        status, data = http_req(
            "POST", f"{base_url}/alert", body="{}", headers={"Content-Type": "application/json"}
        )
        assert status == 400 and data == {"error": "Missing token or chat_id"}, (status, data)

        # 3. Invalid token (SSRF attempt) -> 400, no Telegram request made
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=123:abc/../../evil&chat_id=999",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert status == 400 and "error" in data, (status, data)
        assert len(state.requests) == 0

        # 4. Invalid JSON -> 400
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=12345:ABC_def&chat_id=-10042",
            body="{not-json",
            headers={
                "Content-Type": "application/json",
                "X-Forwarded-For": "203.0.113.10, 10.0.0.1",
                "X-Real-IP": "203.0.113.10",
                "User-Agent": 'komodo/"test"',
            },
        )
        assert status == 400 and data == {"error": "Invalid JSON payload"}, (status, data)

        # 5. Valid Komodo alert -> 200 and exact Telegram payload
        komodo_payload = {
            "level": "OK",
            "resolved": True,
            "target": {"type": "Stack"},
            "data": {
                "type": "StackAutoUpdated",
                "data": {
                    "id": "685db922720baf6840daffca",
                    "name": "komodo",
                    "images": ["mongo:latest"],
                },
            },
        }
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
            body=json.dumps(komodo_payload),
            headers={
                "Content-Type": "application/json",
                "X-Forwarded-For": "198.51.100.7",
                "X-Real-IP": "198.51.100.7",
                "User-Agent": "komodo-core/1.0",
            },
        )
        assert status == 200 and data == {"success": True}, (status, data)
        assert len(state.requests) == 1
        sent = state.requests[0]
        assert sent["path"] == "/bot12345:ABC_def-1/sendMessage", sent["path"]
        assert sent["body"]["chat_id"] == "-10042"
        assert sent["body"]["parse_mode"] == "Markdown"
        expected_text = (
            "✅ OK - StackAutoUpdated\n"
            "*Name*: komodo (Stack)\n"
            "*Resolved*: ✅\n"
            "*Data*: {\n"
            '  "id": "685db922720baf6840daffca",\n'
            '  "name": "komodo",\n'
            '  "images": [\n'
            '    "mongo:latest"\n'
            "  ]\n"
            "}"
        )
        assert sent["body"]["text"] == expected_text, repr(sent["body"]["text"])

        # 6. Telegram API error -> 500 with description
        state.next_status = 400
        state.next_body = {"ok": False, "description": "Bad Request: chat not found"}
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
            body=json.dumps({"level": "ERROR"}),
            headers={"Content-Type": "application/json"},
        )
        assert status == 500 and data == {"error": "Bad Request: chat not found"}, (status, data)

        # 7. Oversized body (> MAX_BODY_BYTES=4096) -> 413
        big_body = json.dumps({"level": "INFO", "pad": "x" * 5000})
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
            body=big_body,
            headers={"Content-Type": "application/json"},
        )
        assert status == 413, (status, data)

        # 8. Unknown path -> 404, wrong method -> 405
        status, _ = http_req("GET", f"{base_url}/nope")
        assert status == 404, status
        status, _ = http_req("GET", f"{base_url}/alert")
        assert status == 405, status

        # 9. Oversized URL -> 414, oversized header -> 431
        status, _ = http_req("GET", f"{base_url}/health?x=" + ("a" * 2500))
        assert status == 414, status
        status, _ = http_req(
            "GET", f"{base_url}/health", headers={"X-Pad": "b" * 9000}
        )
        assert status == 431, status
        status, data = http_req("GET", f"{base_url}/health")
        assert status == 200 and data == {"status": "ok"}, (status, data)

        # 10. Telegram response overflow (>64 KiB) -> 500 without crashing
        with state.lock:
            state.next_status = 200
            state.raw_response = b'{"ok":true,"pad":"' + (b"x" * 70000) + b'"}'
        status, data = http_req(
            "POST",
            f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
            body=json.dumps({"level": "INFO"}),
            headers={"Content-Type": "application/json"},
        )
        assert status == 500 and data == {"error": "Failed to send message to Telegram"}, (
            status,
            data,
        )
        with state.lock:
            state.raw_response = None
            state.next_body = {"ok": True, "result": {"message_id": 1}}
            state.requests.clear()

        # 11. Concurrent /alert + /health burst
        def send_burst_req(idx: int):
            if idx % 4 == 0:
                return ("health", http_req("GET", f"{base_url}/health"))
            return (
                "alert",
                http_req(
                    "POST",
                    f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
                    body=json.dumps({"level": "OK", "data": {"type": f"Burst{idx}"}}),
                    headers={"Content-Type": "application/json"},
                ),
            )

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(send_burst_req, range(64)))
        for kind, (st, resp_data) in results:
            assert st == 200, (kind, st, resp_data)
        with state.lock:
            assert len(state.requests) == 48, len(state.requests)

        # 12. Worker saturation (429 Too Many Requests) and recovery
        with state.lock:
            state.delay_sec = 0.25

        def send_slow_alert(idx: int):
            req = urllib.request.Request(
                f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
                data=json.dumps({"level": "INFO", "data": {"type": f"Sat{idx}"}}).encode("utf-8"),
                method="POST",
            )
            req.add_header("Content-Type", "application/json")
            try:
                with urllib.request.urlopen(req, timeout=10) as resp:
                    return (
                        resp.status,
                        json.loads(resp.read().decode("utf-8")),
                        resp.headers.get("Retry-After"),
                    )
            except urllib.error.HTTPError as e:
                return (
                    e.code,
                    json.loads(e.read().decode("utf-8")),
                    e.headers.get("Retry-After"),
                )
            except (urllib.error.URLError, ConnectionError, OSError) as exc:
                return (None, {"error": str(exc)}, None)

        with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
            sat_results = list(pool.map(send_slow_alert, range(20)))
        statuses = [st for st, _, _ in sat_results]
        assert all(st in (200, 429, None) for st in statuses), sat_results
        assert 200 in statuses, sat_results
        assert 429 in statuses, sat_results
        for st, body_data, retry_after in sat_results:
            if st == 429:
                assert retry_after == "1", (st, body_data, retry_after)
                assert body_data == {"error": "Too many requests, try again later"}, (
                    st,
                    body_data,
                    retry_after,
                )

        with state.lock:
            state.delay_sec = 0.0
        status, data = http_req("GET", f"{base_url}/health")
        assert status == 200 and data == {"status": "ok"}, (status, data)

        # 13. Graceful SIGTERM under active concurrent load
        stop_load = threading.Event()

        def bg_load_worker():
            while not stop_load.is_set():
                try:
                    http_req("GET", f"{base_url}/health")
                    http_req(
                        "POST",
                        f"{base_url}/alert?token=12345:ABC_def-1&chat_id=-10042",
                        body='{"level":"INFO"}',
                        headers={"Content-Type": "application/json"},
                    )
                except Exception:
                    if stop_load.is_set():
                        break

        bg_threads = [threading.Thread(target=bg_load_worker, daemon=True) for _ in range(8)]
        for t in bg_threads:
            t.start()

        time.sleep(0.05)
        proc.send_signal(signal.SIGTERM)
        rc = proc.wait(timeout=10)
        stop_load.set()
        for t in bg_threads:
            t.join(timeout=5)
        assert rc == 0, f"Expected exit 0 on SIGTERM under load, got {rc}"

        # Ensure bot token was never leaked, verbose diagnostics emitted, and no sanitizer errors
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read()
        stderr = stderr_file.read()
        assert "ABC_def-1" not in stdout and "ABC_def-1" not in stderr
        assert "Invalid JSON payload:" in stderr, stderr
        assert "at byte offset" in stderr, stderr
        assert "preview=" in stderr, stderr
        assert "[DEBUG]" in stdout, stdout
        assert "from=127.0.0.1:" in stdout, stdout
        assert "from=127.0.0.1:" in stderr, stderr
        assert 'xff="203.0.113.10, 10.0.0.1"' in stderr, stderr
        assert 'real_ip="203.0.113.10"' in stderr, stderr
        assert 'ua="komodo/\\"test\\""' in stderr, stderr
        assert "Message sent successfully (from=127.0.0.1:" in stdout, stdout
        assert 'xff="198.51.100.7", ua="komodo-core/1.0"' in stdout, stdout
        assert "Telegram API error (http=400, from=127.0.0.1:" in stderr, stderr
        assert "Route not found (404): GET /nope (from=127.0.0.1:" in stderr, stderr
        assert "rejecting client with 429 (from=127.0.0.1:" in stderr, stderr
        for marker in (
            "AddressSanitizer",
            "UndefinedBehaviorSanitizer",
            "ThreadSanitizer",
        ):
            assert marker not in stderr, stderr

        print("All integration tests passed!")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stdout_file.close()
        stderr_file.close()
        tg_server.shutdown()
        tg_server.server_close()


if __name__ == "__main__":
    main()

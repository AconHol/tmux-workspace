"""Local-only OpenAI-compatible fixture. Uses a fake credential and synthetic text."""
import argparse
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    errors = []
    requests = 0

    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
            assert self.path.endswith("/chat/completions"), "wrong resource path"
            assert self.headers.get("Authorization") == "Bearer local-test-token", "missing configured credential"
            assert body["model"] == "fixture-model" and body["stream"] is False, "wrong model/stream"
            assert len(body["messages"]) == 2 and body["messages"][0]["role"] == "system", "wrong message contract"
            assert body["messages"][1]["role"] == "user" and "检查" in body["messages"][1]["content"], "wrong source text"
            assert "英语" in body["messages"][0]["content"], "target language missing"
            Handler.requests += 1
            case = self.path.split("/")[1]
            status = {"unauthorized": 401, "rate-limit": 429, "redirect": 307}.get(case, 200)
            text = {"empty": "", "escape": "bad\x1b[31m", "lines": "first\nsecond\r\nthird\t", "slow": "CANCEL_SHOULD_NOT_INSERT"}.get(case, "Translation CODEX_TRANSLATION_MARKER 中文")
            data = b"invalid JSON" if case == "malformed" else json.dumps({"choices": [{"message": {"content": text}}]}, ensure_ascii=False).encode()
            if case == "slow":
                time.sleep(1)
            self.send_response(status)
            if case == "redirect":
                self.send_header("Location", "/v1/chat/completions")
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            Handler.errors.append(str(error))
            self.send_error(400)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"http://127.0.0.1:{server.server_port}"
        result = subprocess.run([arg.replace("{base_url}", url) for arg in command], check=False)
        assert not Handler.errors, Handler.errors
        assert Handler.requests, "The actual application never called the local API"
        return result.returncode
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())

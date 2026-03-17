#!/usr/bin/env python3
import json
import socketserver
import sys
from datetime import datetime


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("utf-8").strip()
        if not line:
            return
        request = json.loads(line)
        action = request.get("action")
        payload = request.get("payload", {})
        if action == "get_status":
            response = {
                "ok": True,
                "data": {
                    "service": "server_echo",
                    "timestamp": datetime.utcnow().isoformat() + "Z",
                },
            }
        elif action == "echo":
            response = {"ok": True, "data": payload}
        else:
            response = {"ok": False, "error": f"unsupported action: {action}"}
        self.wfile.write((json.dumps(response, ensure_ascii=False) + "\n").encode("utf-8"))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9201
    with socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler) as server:
        server.daemon_threads = True
        server.serve_forever()

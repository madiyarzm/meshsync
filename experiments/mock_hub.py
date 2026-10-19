"""Serve the hub's web page on a laptop, without a board.

Run:  python experiments/mock_hub.py   then open http://localhost:8000
Uses the exact page from src/web_page.h and answers /teach, /guess, and /status with the
Python prototype, so the page can be tried and checked without hardware. There is no mesh:
this mock is a single room (A) and forgets everything when stopped.
"""

import json
import re
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs

from construct_proto import NUM_CLASSES, VAR_FLOOR, GaussianNB, Node, combine, nearest_centroid_predict

PAGE_FILE = Path(__file__).resolve().parent.parent / "src" / "web_page.h"
PAGE = re.search(r'R"HTML\((.*)\)HTML"', PAGE_FILE.read_text(), re.S).group(1).encode()
NUM_SLOTS = 8  # matches MAX_NODES in the firmware
node = Node(0, NUM_SLOTS)


def parse_pixels(text):
    try:
        pixels = [int(v) for v in text.split(",")]
    except ValueError:
        return None
    if len(pixels) != 64 or any(v < 0 or v > 16 for v in pixels):
        return None
    return pixels


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, content_type="application/json"):
        if isinstance(body, dict):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            knowledge = [[slot.count[label] for slot in node.slots] for label in range(NUM_CLASSES)]
            self.reply(200, {
                "board": "A", "uptime_s": 0, "versions": node.version_vector(), "peers": [],
                "knowledge": knowledge,
                "sync": {"sent": 0, "received": 0, "applied": 0, "crc_failures": 0, "syncing": False},
            })
        else:
            self.reply(200, PAGE, "text/html")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        fields = parse_qs(self.rfile.read(length).decode())
        pixels = parse_pixels(fields.get("pixels", [""])[0])
        if pixels is None:
            self.reply(400, {"ok": False, "error": "pixels must be 64 values 0-16"})
            return
        if self.path == "/teach":
            label = fields.get("label", [""])[0]
            if len(label) != 1 or not label.isdigit():
                self.reply(400, {"ok": False, "error": "label must be one digit 0-9"})
                return
            node.teach(pixels, int(label))
            self.reply(200, {"ok": True, "version": node.slots[0].version, "saved": False})
        elif self.path == "/guess":
            totals = combine(node.slots)
            label, confidence = GaussianNB(totals, VAR_FLOOR).predict(pixels)
            centroid = nearest_centroid_predict(totals, pixels)
            self.reply(200, {"ok": True, "label": label, "confidence": confidence, "centroid": centroid})
        else:
            self.reply(404, {"ok": False, "error": "not found"})

    def log_message(self, format, *args):
        print(f"{self.command} {self.path} -> {args[1] if len(args) > 1 else ''}")


if __name__ == "__main__":
    print("mock hub on http://localhost:8000 (Ctrl+C to stop)")
    HTTPServer(("127.0.0.1", 8000), Handler).serve_forever()

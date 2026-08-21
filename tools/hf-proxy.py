#!/usr/bin/env python3
"""Bridge the desktop to a Hugging Face Space.

The desktop has no TLS stack and no JSON parser. Adding both to reach one Space
would mean mbedTLS, a CA bundle and its expiry, plus a parser in a zapp that has
no libc -- a lot of firmware for a paperclip. This runs on the development host
instead and does three things the device should not have to:

  1. Terminates TLS to huggingface.co.
  2. Performs Gradio's two-step call. A prediction is a POST that returns an
     event id, then a GET that streams server-sent events until one of them is
     `complete`. Two round trips and an SSE parser.
  3. Answers in plain text, so the zapp reads bytes and shows them.

The device therefore speaks the simplest possible HTTP: POST a question as the
body, get an answer back as the body.

    python tools/hf-proxy.py
    python tools/hf-proxy.py --space eoinedge/ros2 --port 8080

From QEMU user-mode networking the host is 10.0.2.2, which is what
zapps/clippy/clippy.c points at.

This is a development tool. It binds loopback by default, has no authentication
and no rate limiting, and should not be exposed to a network you do not control.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_SPACE = "eoinedge/zephyrproject-rag"
DEFAULT_API = "do_answer"
DEFAULT_K = 4

# The Space runs its model on CPU. Generous on purpose; the device is not
# blocked while it waits.
READ_TIMEOUT_S = 180


class SpaceError(RuntimeError):
    pass


def space_base(space: str) -> str:
    """eoinedge/zephyrproject-rag -> https://eoinedge-zephyrproject-rag.hf.space"""
    return f"https://{space.replace('/', '-').replace('_', '-')}.hf.space"


def ask_space(space: str, api: str, question: str, k: int) -> str:
    base = space_base(space)
    endpoint = f"{base}/gradio_api/call/{api}"

    payload = json.dumps({"data": [question, k]}).encode("utf-8")
    request = urllib.request.Request(
        endpoint, payload, {"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            event_id = json.load(response).get("event_id")
    except urllib.error.HTTPError as error:
        raise SpaceError(f"POST returned HTTP {error.code}") from error
    except OSError as error:
        raise SpaceError(f"could not reach {base}: {error}") from error

    if not event_id:
        raise SpaceError("the Space did not return an event id")

    try:
        with urllib.request.urlopen(f"{endpoint}/{event_id}", timeout=READ_TIMEOUT_S) as stream:
            raw = stream.read().decode("utf-8", "replace")
    except OSError as error:
        raise SpaceError(f"reading the result stream failed: {error}") from error

    # The stream ends with `event: complete` (or `error`) and one JSON data line.
    # Heartbeats in between are skipped.
    for event in raw.split("\n\n"):
        if event.startswith("event: error"):
            raise SpaceError("the Space reported an error")
        if not event.startswith("event: complete"):
            continue
        _, _, data = event.partition("data: ")
        parsed = json.loads(data)
        # do_answer returns [answer, sources]; the assistant Spaces put the
        # grounded passages in the second element, which is the part worth
        # showing on a small screen.
        parts = [str(p).strip() for p in parsed if isinstance(p, str) and p.strip()]
        if not parts:
            raise SpaceError("the Space answered with nothing")
        return "\n\n".join(parts)

    raise SpaceError("the stream closed without an answer")


def strip_markdown(text: str) -> str:
    """Flatten to something a fixed-width text widget can show.

    The desktop's text widget renders plain text. Markdown left in place shows
    as literal asterisks and backticks, which on a 320-pixel window is most of
    the line.
    """
    out = []
    for line in text.splitlines():
        line = line.replace("**", "").replace("`", "")
        if line.startswith("### "):
            line = line[4:].upper()
        elif line.startswith("## "):
            line = line[3:].upper()
        elif line.startswith("# "):
            line = line[2:].upper()
        out.append(line.rstrip())
    # Collapse runs of blank lines; vertical space is expensive here.
    flattened: list[str] = []
    for line in out:
        if not line and flattened and not flattened[-1]:
            continue
        flattened.append(line)
    return "\n".join(flattened).strip()


class Handler(BaseHTTPRequestHandler):
    space = DEFAULT_SPACE
    api = DEFAULT_API
    k = DEFAULT_K
    raw = False

    def _reply(self, status: int, body: str) -> None:
        payload = body.encode("utf-8", "replace")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler's name
        if self.path.startswith("/health"):
            self._reply(200, f"ok\nspace: {self.space}\napi: {self.api}\n")
            return
        self._reply(404, "POST a question to /ask\n")

    def do_POST(self) -> None:  # noqa: N802
        if not self.path.startswith("/ask"):
            self._reply(404, "POST a question to /ask\n")
            return

        length = int(self.headers.get("Content-Length") or 0)
        question = self.rfile.read(length).decode("utf-8", "replace").strip()
        if not question:
            self._reply(400, "Empty question.\n")
            return

        print(f"  -> {question}", flush=True)
        try:
            answer = ask_space(self.space, self.api, question, self.k)
        except SpaceError as error:
            # A 502 tells the zapp the proxy was reached but the Space was not,
            # which is a different problem from the proxy being absent.
            print(f"  !! {error}", flush=True)
            self._reply(502, f"The Space did not answer.\n\n{error}\n")
            return

        if not self.raw:
            answer = strip_markdown(answer)
        print(f"  <- {len(answer)} bytes", flush=True)
        self._reply(200, answer + "\n")

    def log_message(self, *args) -> None:
        """Quiet: the handlers print what matters."""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--space", default=DEFAULT_SPACE)
    parser.add_argument("--api", default=DEFAULT_API)
    parser.add_argument("--k", type=int, default=DEFAULT_K)
    parser.add_argument("--host", default="0.0.0.0",
                        help="0.0.0.0 so QEMU's 10.0.2.2 alias can reach it")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--raw", action="store_true",
                        help="do not flatten markdown")
    args = parser.parse_args()

    Handler.space = args.space
    Handler.api = args.api
    Handler.k = args.k
    Handler.raw = args.raw

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"hf-proxy -> {space_base(args.space)}/gradio_api/call/{args.api}")
    print(f"listening on http://{args.host}:{args.port}/ask")
    print("from QEMU the desktop reaches this at http://10.0.2.2:%d/ask" % args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())

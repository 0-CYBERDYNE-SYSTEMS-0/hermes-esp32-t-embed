"""Live dashboard for a T-Embed session, served from the host toolkit.

Reads a session directory — either a host capture (frames.jsonl) or a session
pulled from the device card (frames.ndjson) — and serves a self-contained
instrument view: waterfall, top signals, HUNT events, provenance and download
links for the raw artifacts.

Deliberately dependency-free (stdlib http.server, no CDN assets) so it works
on a locked-down laptop with no network, which is the situation the tool is
used in.

    python3 dashboard.py <session-dir> --port 8080
"""

from __future__ import annotations

import json
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

MAX_WATERFALL_FRAMES = 220
WATERFALL_COLUMNS = 160
MAX_FILE_BYTES = 64 * 1024 * 1024


def frame_path(session_dir: Path) -> Path | None:
    for name in ("frames.ndjson", "frames.jsonl"):
        path = session_dir / name
        if path.exists():
            return path
    return None


def load_manifest(session_dir: Path) -> dict:
    path = session_dir / "session.json"
    if not path.exists():
        path = session_dir / "meta.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return {}


def _codes_digest(codes: list[int], columns: int) -> list[int]:
    """Downsample bins to `columns` values by taking each bucket's maximum,
    so a narrow carrier stays visible instead of averaging away."""
    if not codes:
        return []
    step = max(1, len(codes) // columns)
    out = []
    for start in range(0, len(codes), step):
        bucket = codes[start:start + step]
        if bucket:
            out.append(max(bucket))
        if len(out) >= columns:
            break
    return out


class SessionState:
    """Cached view of a session that grows while the device is still writing."""

    def __init__(self, session_dir: Path) -> None:
        self.dir = session_dir
        self._lock = threading.Lock()
        self._stamp: tuple[int, int] | None = None
        self._frames: list[dict] = []
        self._total = 0

    def _path(self) -> Path | None:
        return frame_path(self.dir)

    def _refresh(self) -> None:
        path = self._path()
        if path is None:
            return
        stat = path.stat()
        stamp = (stat.st_size, stat.st_mtime_ns)
        if stamp == self._stamp:
            return
        with self._lock:
            if stamp == self._stamp:
                return
            keep: list[dict] = []
            total = 0
            # Read the tail: a long session would otherwise load megabytes into
            # the dashboard for no visible benefit.
            with path.open("rb") as handle:
                handle.seek(max(0, stat.st_size - 4 * 1024 * 1024))
                for line in handle:
                    if not line.strip():
                        continue
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue  # a half-written last line while the device appends
                    total += 1
                    keep.append(record)
            self._frames = keep[-MAX_WATERFALL_FRAMES:]
            self._total = total
            self._stamp = stamp

    def snapshot(self) -> dict:
        self._refresh()
        frames = self._frames
        if not frames:
            return {"frames": 0, "waterfall": [], "peaks": [], "lo_mhz": None,
                    "nfft": None, "duration_s": 0, "min_code": 0, "max_code": 0}
        latest = frames[-1]
        nfft = latest.get("nfft") or len(latest.get("codes", []))
        floor = latest.get("floor_code", 0)
        peaks = []
        codes = latest.get("codes", [])
        threshold = floor + 10
        for index, code in enumerate(codes):
            if code < threshold:
                continue
            left = codes[index - 1] if index else -1
            right = codes[index + 1] if index + 1 < len(codes) else -1
            if code >= left and code >= right:
                rate = latest.get("rate_hz") or 0
                lo = latest.get("lo_mhz") or 0
                offset = (index - nfft // 2) * (rate / nfft) if nfft else 0
                peaks.append({
                    "bin": index,
                    "freq_mhz": round(lo + offset / 1e6, 3),
                    "code": code,
                    "over_floor": code - floor,
                })
        peaks.sort(key=lambda p: -p["code"])
        return {
            "frames": self._total,
            "window": len(frames),
            "waterfall": [_codes_digest(f.get("codes", []), WATERFALL_COLUMNS) for f in frames],
            "peaks": peaks[:8],
            "lo_mhz": latest.get("lo_mhz"),
            "nfft": nfft,
            "rate_hz": latest.get("rate_hz"),
            "floor_code": floor,
            "duration_s": frames[-1].get("t_s", 0),
            "min_code": min((min(f.get("codes", [0])) for f in frames), default=0),
            "max_code": max((max(f.get("codes", [0])) for f in frames), default=0),
        }

    def inventory(self) -> dict:
        recordings = []
        for meta in sorted((self.dir / "iq").glob("*.sigmf-meta")) if (self.dir / "iq").exists() else []:
            recordings.append(meta.name[: -len(".sigmf-meta")])
        captures = [p.name for p in sorted((self.dir / "net").glob("*.pcapng"))] \
            if (self.dir / "net").exists() else []
        events = []
        events_path = self.dir / "events.ndjson"
        if events_path.exists():
            for line in events_path.read_text().splitlines()[-40:]:
                if line.strip():
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        return {"manifest": load_manifest(self.dir), "recordings": recordings,
                "captures": captures, "events": events, "session": self.dir.name}


PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__ — T-Embed instrument</title>
<style>
 :root { --bg:#0b0e12; --panel:#141a21; --line:#1f2731; --ink:#e9e6df;
         --dim:#8695a5; --accent:#48c9ff; --hot:#ff5f57; --warm:#ffb340; }
 * { box-sizing:border-box; }
 body { margin:0; background:var(--bg); color:var(--ink);
        font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }
 header { display:flex; align-items:baseline; gap:16px; flex-wrap:wrap;
          padding:16px 20px; border-bottom:1px solid var(--line); }
 h1 { font-size:15px; letter-spacing:.12em; text-transform:uppercase; margin:0; }
 .pill { font-size:11px; letter-spacing:.08em; color:var(--dim);
         border:1px solid var(--line); border-radius:999px; padding:2px 9px; }
 .pill b { color:var(--ink); font-weight:600; }
 main { display:grid; grid-template-columns:minmax(0,1fr) 320px; gap:18px;
        padding:18px 20px 40px; align-items:start; }
 @media (max-width:880px) { main { grid-template-columns:1fr; } }
 section { background:var(--panel); border:1px solid var(--line);
           border-radius:10px; padding:14px 16px; }
 h2 { font-size:11px; letter-spacing:.16em; text-transform:uppercase;
      color:var(--dim); margin:0 0 12px; font-weight:600; }
 canvas { width:100%; height:240px; display:block; border-radius:6px;
          background:#080b0f; }
 table { width:100%; border-collapse:collapse; }
 th,td { text-align:right; padding:5px 8px; border-bottom:1px solid var(--line); }
 th { color:var(--dim); font-size:10px; letter-spacing:.1em; text-transform:uppercase; }
 td:first-child, th:first-child { text-align:left; }
 .hot { color:var(--hot); } .warm { color:var(--warm); }
 ul { margin:0; padding-left:18px; } li { margin:2px 0; color:var(--dim); }
 li b { color:var(--ink); font-weight:600; }
 a { color:var(--accent); text-decoration:none; } a:hover { text-decoration:underline; }
 .empty { color:var(--dim); padding:24px 0; text-align:center; }
 .grid2 { display:grid; grid-template-columns:1fr 1fr; gap:14px; }
</style></head>
<body>
<header>
  <h1>__TITLE__</h1>
  <span class="pill">lo <b id="lo">—</b> MHz</span>
  <span class="pill">frames <b id="frames">0</b></span>
  <span class="pill">span <b id="dur">0</b> s</span>
  <span class="pill" id="prov">no manifest</span>
</header>
<main>
  <div>
    <section>
      <h2>Waterfall (newest at the bottom)</h2>
      <canvas id="wf" width="900" height="240"></canvas>
      <div class="empty" id="wf-empty">waiting for frames…</div>
    </section>
    <section style="margin-top:18px">
      <h2>Strongest bins now</h2>
      <table><thead><tr><th>MHz</th><th>code</th><th>over floor</th></tr></thead>
      <tbody id="peaks"><tr><td colspan="3" class="empty">no peaks above floor</td></tr></tbody></table>
    </section>
  </div>
  <div>
    <section>
      <h2>HUNT events</h2>
      <ul id="events"><li>none logged</li></ul>
    </section>
    <section style="margin-top:18px">
      <h2>Artifacts</h2>
      <div id="artifacts" class="empty">none yet</div>
    </section>
    <section style="margin-top:18px">
      <h2>Provenance</h2>
      <div id="meta" class="empty">no session.json</div>
    </section>
  </div>
</main>
<script>
const wf = document.getElementById('wf');
const ctx = wf.getContext('2d');

// Everything below comes from files on a card, so treat it as untrusted text
// rather than markup: escape before it is ever placed in innerHTML.
const esc = (value) => String(value ?? '').replace(/[&<>"']/g,
  ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));

function color(level, floor, span) {
  const t = Math.max(0, Math.min(1, (level - floor) / Math.max(span, 1)));
  // cold -> warm -> hot, no external palette
  const r = Math.round(20 + 235 * Math.pow(t, 1.4));
  const g = Math.round(60 + 150 * Math.pow(t, 2.4));
  const b = Math.round(120 + 60 * t - 90 * Math.pow(t, 2));
  return `rgb(${r},${g},${b})`;
}

function draw(state) {
  const rows = state.waterfall;
  ctx.clearRect(0, 0, wf.width, wf.height);
  document.getElementById('wf-empty').style.display = rows.length ? 'none' : 'block';
  if (!rows.length) return;
  const h = Math.max(1, Math.floor(wf.height / Math.max(rows.length, 1)));
  const cols = rows[0].length;
  const cw = wf.width / cols;
  for (let y = 0; y < rows.length; y++) {
    for (let x = 0; x < cols; x++) {
      ctx.fillStyle = color(rows[y][x], state.floor_code, 120);
      ctx.fillRect(Math.floor(x * cw), y * h, Math.ceil(cw), h);
    }
  }
  document.getElementById('lo').textContent = state.lo_mhz ?? '—';
  document.getElementById('frames').textContent = state.frames;
  document.getElementById('dur').textContent = (state.duration_s ?? 0).toFixed(0);
  const peaks = document.getElementById('peaks');
  if (state.peaks.length) {
    peaks.innerHTML = state.peaks.map(p =>
      `<tr><td>${p.freq_mhz.toFixed(3)}</td><td class="${p.over_floor > 30 ? 'hot' : 'warm'}">` +
      `${p.code}</td><td>+${p.over_floor}</td></tr>`).join('');
  } else {
    peaks.innerHTML = '<tr><td colspan="3" class="empty">no peaks above floor</td></tr>';
  }
}

async function poll() {
  try {
    const state = await (await fetch('/api/frames')).json();
    draw(state);
  } catch (e) { /* keep the last frame on screen */ }
}

async function inventory() {
  const inv = await (await fetch('/api/session')).json();
  const events = document.getElementById('events');
  events.innerHTML = inv.events.length
    ? inv.events.slice().reverse().map(e =>
        `<li><b>${esc(e.khz)} kHz</b> ${esc(e.level)} — ${esc(new Date(e.ts || Date.now()).toLocaleTimeString())} </li>`).join('')
    : '<li>none logged</li>';
  const arts = [];
  for (const r of inv.recordings) {
    arts.push(`<div><a href="/files/iq/${encodeURIComponent(r)}.sigmf-data">${esc(r)}.sigmf-data</a>
      <a href="/files/iq/${encodeURIComponent(r)}.sigmf-meta">(meta)</a></div>`);
  }
  for (const c of inv.captures) {
    arts.push(`<div><a href="/files/net/${encodeURIComponent(c)}">${esc(c)}</a></div>`);
  }
  if (inv.manifest && inv.manifest.session) {
    arts.push('<div><a href="/files/frames.ndjson">frames.ndjson</a></div>');
    arts.push('<div><a href="/files/session.json">session.json</a></div>');
  }
  document.getElementById('artifacts').innerHTML = arts.join('') || 'none yet';
  const m = inv.manifest || {};
  const bits = [];
  if (m.build) bits.push(`build <b>${m.build}</b>`);
  if (m.label) bits.push(`label <b>${m.label}</b>`);
  if (m.time_source) bits.push(`clock <b>${m.time_source}</b>`);
  if (m.gain_index !== undefined) bits.push(`gain <b>${m.gain_index}</b>`);
  if (m.nfft) bits.push(`nfft <b>${m.nfft}</b>`);
  document.getElementById('meta').innerHTML = bits.join('<br>') || 'no session.json';
  document.getElementById('prov').innerHTML =
    m.time_source ? `clock <b>${m.time_source}</b>` : 'no manifest';
}

poll(); inventory();
setInterval(poll, 1000);
setInterval(inventory, 4000);
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    state: SessionState
    session_dir: Path

    def log_message(self, format, *args):  # noqa: A002 - stdlib signature
        pass

    def _send(self, status, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - stdlib API
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            page = PAGE.replace("__TITLE__", self.session_dir.name)
            self._send(HTTPStatus.OK, page.encode(), "text/html; charset=utf-8")
            return
        if path == "/api/frames":
            self._send(HTTPStatus.OK, json.dumps(self.state.snapshot()).encode(),
                       "application/json")
            return
        if path == "/api/session":
            self._send(HTTPStatus.OK, json.dumps(self.state.inventory()).encode(),
                       "application/json")
            return
        if path.startswith("/files/"):
            self._serve_file(unquote(path[len("/files/"):]))
            return
        self._send(HTTPStatus.NOT_FOUND, b"not found\n", "text/plain")

    def _serve_file(self, relative: str) -> None:
        target = (self.session_dir / relative).resolve()
        try:
            target.relative_to(self.session_dir.resolve())
        except ValueError:
            self._send(HTTPStatus.FORBIDDEN, b"forbidden\n", "text/plain")
            return
        if not target.is_file():
            self._send(HTTPStatus.NOT_FOUND, b"not found\n", "text/plain")
            return
        size = target.stat().st_size
        if size > MAX_FILE_BYTES:
            self._send(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, b"too large\n", "text/plain")
            return
        self._send(HTTPStatus.OK, target.read_bytes(), "application/octet-stream")


def make_server(session_dir: Path, host: str = "127.0.0.1", port: int = 0) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {})
    handler.state = SessionState(session_dir)
    handler.session_dir = session_dir
    return ThreadingHTTPServer((host, port), handler)


def serve(session_dir: Path, host: str = "127.0.0.1", port: int = 8080) -> int:
    server = make_server(session_dir, host, port)
    address, actual_port = server.server_address[:2]
    print(f"dashboard: http://{address}:{actual_port}/  ({session_dir})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0

#!/usr/bin/env python3
"""
make texpanel: a page in the browser to pick each image's Dreamcast size and
format: the original next to what pvrtex makes of it, remade as you change
them, saved to texsizes.txt (make assets uses it from there).

The preview is made the way rtcwconv's tex.cpp makes the .dt (same caps,
pvrtex flags and mipmaps); only the resize differs a little (PIL bicubic, not
stb_image_resize).

With --disc <the disc's main folder> it also serves the model and map
viewer (tools/viewer) at /view/, on the disc's files: click a surface to
pick its image here, its new size and format shown on the mesh.

usage: texpanel.py <pvrtex> <src dir> <texsizes.txt> [port] [--disc <dir>]
"""
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading

import numpy as np
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

from PIL import Image

MAX_SIZE, MAX_SIZE_2D = 128, 256        # rtcwconv -t and the 2D cap
PREFIXES_2D = ("gfx/", "ui/", "ui_mp/", "menu/", "levelshots/", "fonts/")
FORMATS = ("vq", "vq565", "vq1555", "vq4444", "raw", "565", "1555", "4444", "yuv", "pal8")  # vq: no word in texsizes.txt
PVR_FORMATS = {"565": "RGB565", "1555": "ARGB1555", "4444": "ARGB4444", "yuv": "YUV422", "pal8": "PAL8BPP"}
PIXEL_FORMATS = ("1555", "565", "4444", "YUV422", "bump", "PAL4", "PAL8", "?")

pvrtex = srcdir = sizesfile = discdir = None
VIEWER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "viewer")
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8"}
images = {}         # name (lower case, no extension) -> {path, w, h}
cache = {}          # (name, w, h, fmt) -> (png bytes, info)
cache_lock = threading.Lock()
file_lock = threading.Lock()


def is_2d(name):
    return name.startswith(PREFIXES_2D)


def default(name):
    """what tex.cpp gives an image texsizes.txt doesn't name"""
    im = images[name]
    w, h = im["w"], im["h"]
    cap = MAX_SIZE_2D if is_2d(name) else MAX_SIZE
    while w > cap or h > cap:
        w, h = max(w // 2, 1), max(h // 2, 1)
    return {"w": near_pow2(w), "h": near_pow2(h), "fmt": "vq"}


def near_pow2(v):
    """pvrtex -r NEAR: the nearer power of two, down on a tie (8 at least)"""
    down = 1 << (v.bit_length() - 1)
    up = down if down == v else down * 2
    return max(8, up if up - v < v - down else down)


def read_sizes():
    """texsizes.txt: name -> {w, h, fmt}"""
    sizes = {}
    with open(sizesfile) as f:
        for line in f:
            words = line.split("#")[0].split()
            if len(words) >= 3:
                sizes[words[0].lower()] = {"w": int(words[1]), "h": int(words[2]),
                                           "fmt": words[3] if len(words) > 3 else "vq"}
    return sizes


def write_size(name, setting):
    """set (or with None, drop) name's line in texsizes.txt, keeping its comment"""
    with file_lock:
        with open(sizesfile) as f:
            lines = f.read().splitlines()
        new = None
        if setting:
            new = "%-35s %d %d" % (name, setting["w"], setting["h"])
            if setting["fmt"] != "vq":
                new += " " + setting["fmt"]
        for i, line in enumerate(lines):
            words = line.split("#")[0].split()
            if words and words[0].lower() == name:
                if new is None:
                    del lines[i]
                else:
                    comment = line.find("#")
                    lines[i] = new + ("     " + line[comment:] if comment >= 0 else "")
                break
        else:
            if new is not None:
                if "# texpanel" not in lines:
                    lines += ["", "# texpanel"]
                lines.append(new)
        with open(sizesfile + ".tmp", "w") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(sizesfile + ".tmp", sizesfile)


def pixel_format(im):
    """tex.cpp's PixelFormat: AUTO when opaque, else whichever of 4444 / 1555
    is nearer the image blended over black and white, in lightness"""
    p = np.asarray(im).astype(np.int64)
    a = p[..., 3:4]
    if (a == 255).all():
        return "AUTO"
    a4, a1 = (a * 15 + 127) // 255 * 17, np.where(a >= 128, 255, 0)
    c = p[..., :3]
    q5 = (c * 31 + 127) // 255
    v4, v1 = (c * 15 + 127) // 255 * 17, (q5 << 3) | (q5 >> 2)
    err4, err1 = 0.0, 0.0
    for bg in (0, 255):
        o = np.cbrt(c * a + bg * (255 - a))
        err4 += float(((np.cbrt(v4 * a4 + bg * (255 - a4)) - o) ** 2).sum())
        err1 += float(((np.cbrt(v1 * a1 + bg * (255 - a1)) - o) ** 2).sum())
    return "ARGB1555" if err1 <= err4 else "ARGB4444"


def convert(name, w, h, fmt):
    """the .dt pvrtex makes: (png of its top level, info)"""
    key = (name, w, h, fmt)
    with cache_lock:
        if key in cache:
            return cache[key]
    vq = fmt.startswith("vq")
    pixels = fmt[2:] if vq else fmt
    mips = [] if is_2d(name) else ["-m", "-R", "OPT"]
    with tempfile.TemporaryDirectory() as tmp:
        src, dt, png = (os.path.join(tmp, n) for n in ("in.tga", "out.dt", "out.png"))
        im = Image.open(images[name]["path"]).convert("RGBA")
        if im.size != (w, h):
            im = im.resize((w, h), Image.BICUBIC)
        args = ["-r", "NEAR", "-f", PVR_FORMATS.get(pixels) or pixel_format(im)] + (["-c", "small"] if vq else []) + mips
        im.save(src)
        r = subprocess.run([pvrtex, "-i", src, "-o", dt, "-p", png] + args, capture_output=True)
        if r.returncode or not os.path.exists(png):
            raise RuntimeError("pvrtex: " + r.stderr.decode(errors="replace")[-300:])
        data = open(dt, "rb").read()
        nbytes = len(data) - 32
        if os.path.exists(dt + ".pal"):
            nbytes += os.path.getsize(dt + ".pal") - 8
        ptype = struct.unpack_from("<I", data, 16)[0]
        pw, ph = 8 << (ptype >> 3 & 7), 8 << (ptype & 7)
        out = io.BytesIO()
        Image.open(png).crop((0, 0, pw, ph)).save(out, "PNG")
    info = {"bytes": nbytes, "w": pw, "h": ph, "mips": bool(ptype >> 31),
            "fmt": "VQ " if ptype & 1 << 30 else ""}
    info["fmt"] += PIXEL_FORMATS[ptype >> 27 & 7]
    result = (out.getvalue(), info)
    with cache_lock:
        cache[key] = result
    return result


def totals():
    """texsizes.txt's lines against the defaults, in VRAM bytes"""
    sizes = {n: s for n, s in read_sizes().items() if n in images}
    jobs = [(n, s) for n, s in sizes.items()] + [(n, default(n)) for n in sizes]
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        got = list(pool.map(lambda j: convert(j[0], j[1]["w"], j[1]["h"], j[1]["fmt"])[1]["bytes"], jobs))
    half = len(sizes)
    return {"count": half, "bytes": sum(got[:half]), "defaultBytes": sum(got[half:]),
            "pal8": sum(1 for s in sizes.values() if s["fmt"] == "pal8"),
            "missing": sorted(set(read_sizes()) - set(images))}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, ctype="application/json", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, root, rel):
        """a file under root, not outside it"""
        path = os.path.realpath(os.path.join(root, rel))
        if not path.startswith(os.path.realpath(root) + os.sep) or not os.path.isfile(path):
            self.send(404, {"error": "not found"})
            return
        with open(path, "rb") as f:
            body = f.read()
        self.send(200, body, TYPES.get(os.path.splitext(path)[1].lower(), "application/octet-stream"))

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        try:
            if url.path in ("/view", "/view/"):
                self.send_file(VIEWER, "index.html")
            elif url.path.startswith("/view/"):
                self.send_file(VIEWER, unquote(url.path[6:]))
            elif url.path == "/api/files":
                if not discdir:
                    raise ValueError("no --disc folder")
                files = []
                for root, _, names in os.walk(discdir):
                    for n in names:
                        p = os.path.join(root, n)
                        files.append([os.path.relpath(p, discdir).replace(os.sep, "/"), os.path.getsize(p)])
                self.send(200, files)
            elif url.path.startswith("/disc/") and discdir:
                self.send_file(discdir, unquote(url.path[6:]))
            elif url.path == "/":
                self.send(200, PAGE.encode(), "text/html; charset=utf-8")
            elif url.path == "/api/list":
                sizes = read_sizes()
                self.send(200, [{"name": n, "w": im["w"], "h": im["h"], "def": default(n), "set": sizes.get(n)}
                                for n, im in sorted(images.items())])
            elif url.path == "/api/orig":
                out = io.BytesIO()
                Image.open(images[q["name"]]["path"]).convert("RGBA").save(out, "PNG")
                self.send(200, out.getvalue(), "image/png")
            elif url.path == "/api/dc":
                w, h, fmt = int(q["w"]), int(q["h"]), q["fmt"]
                if fmt not in FORMATS or not 8 <= w <= 1024 or not 8 <= h <= 1024:
                    raise ValueError("bad size or format")
                png, info = convert(q["name"], w, h, fmt)
                self.send(200, png, "image/png", [("X-Info", json.dumps(info))])
            elif url.path == "/api/totals":
                self.send(200, totals())
            else:
                self.send(404, {"error": "not found"})
        except Exception as e:
            self.send(500, {"error": str(e)})

    def do_POST(self):
        try:
            if urlparse(self.path).path != "/api/save":
                raise ValueError("not found")
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            name, setting = body["name"], body.get("set")
            if name not in images:
                raise ValueError("no such image")
            if setting:
                w, h = int(setting["w"]), int(setting["h"])
                if setting["fmt"] not in FORMATS or any(v < 8 or v > 1024 or v & (v - 1) for v in (w, h)):
                    raise ValueError("sizes are powers of two, 8 to 1024")
                setting = {"w": w, "h": h, "fmt": setting["fmt"]}
            write_size(name, setting)
            self.send(200, {"set": read_sizes().get(name)})
        except Exception as e:
            self.send(500, {"error": str(e)})


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>Texture Panel</title>
<style>
:root { --bg:#16181c; --panel:#1f2228; --line:#30343c; --text:#d8dce3; --dim:#8a919c; --accent:#f0b429; --set:#5ec27a; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:13px/1.4 system-ui,sans-serif; display:flex; height:100vh; }
#side { width:340px; display:flex; flex-direction:column; border-right:1px solid var(--line); background:var(--panel); }
#side input { margin:8px; padding:6px 8px; background:var(--bg); color:var(--text); border:1px solid var(--line); border-radius:4px; }
#side label { margin:0 8px 6px; color:var(--dim); }
#list { overflow-y:auto; flex:1; }
.row { padding:3px 8px; cursor:pointer; display:flex; justify-content:space-between; gap:8px; white-space:nowrap; }
.row:hover { background:#2a2e36; } .row.sel { background:#3a3220; }
.row .n { overflow:hidden; text-overflow:ellipsis; } .row .s { color:var(--dim); } .row.set .s { color:var(--set); }
#main { flex:1; display:flex; flex-direction:column; min-width:0; }
#bar { padding:8px 12px; border-bottom:1px solid var(--line); display:flex; flex-wrap:wrap; gap:10px; align-items:center; background:var(--panel); }
#bar h2 { font-size:14px; margin:0 12px 0 0; color:var(--accent); }
select, button { background:var(--bg); color:var(--text); border:1px solid var(--line); border-radius:4px; padding:4px 8px; font:inherit; }
button { cursor:pointer; } button.go { border-color:var(--accent); color:var(--accent); }
#stat { color:var(--dim); } #stat b { color:var(--text); }
#totals { padding:6px 12px; color:var(--dim); border-bottom:1px solid var(--line); }
#view { flex:1; overflow:auto; display:flex; gap:12px; padding:12px; align-items:flex-start; }
.pane { display:flex; flex-direction:column; gap:4px; }
.pane div { color:var(--dim); }
.pane img { image-rendering:pixelated; background:repeating-conic-gradient(#555 0 25%,#777 0 50%) 0 0/16px 16px; display:block; }
.err { color:#ff6b6b; }
</style></head><body>
<div id="side">
  <input id="filter" placeholder="filter (e.g. ui/assets, bjui)" autofocus>
  <label><input type="checkbox" id="onlySet"> only ones in texsizes.txt</label>
  <div id="list"></div>
</div>
<div id="main">
  <div id="totals">…</div>
  <div id="bar">
    <h2 id="name">pick an image</h2>
    W <select id="w"></select> H <select id="h"></select>
    <select id="fmt" title="vq: compressed, raw: not; with no pixel format the converter picks (565 if opaque, else 4444 or 1555)">
      <optgroup label="VQ compressed"><option>vq</option><option>vq565</option><option>vq1555</option><option>vq4444</option></optgroup>
      <optgroup label="uncompressed"><option>raw</option><option>565</option><option>1555</option><option>4444</option><option>yuv</option><option>pal8</option></optgroup>
    </select>
    view <select id="zoom"><option value="0">original size</option><option value="1">1x</option><option value="2" selected>2x</option><option value="4">4x</option></select>
    <button class="go" id="save">save to texsizes.txt</button>
    <button id="reset">back to default</button>
    <span id="stat"></span>
    <a href="/view/" style="margin-left:auto;color:var(--accent)">3D view →</a>
  </div>
  <div id="view">
    <div class="pane"><div id="ocap">original</div><img id="orig"></div>
    <div class="pane"><div id="dcap">Dreamcast</div><img id="dc"></div>
  </div>
</div>
<script>
const $ = id => document.getElementById(id);
let all = [], cur = null, req = 0;
for (let v = 8; v <= 1024; v *= 2) for (const s of [$('w'), $('h')]) s.add(new Option(v, v));
const kb = b => (b / 1024).toFixed(1) + ' KB';
const desc = s => s.w + 'x' + s.h + (s.fmt === 'vq' ? '' : ' ' + s.fmt);
const now = it => it.set || it.def;

async function load() {
  all = await (await fetch('/api/list')).json();
  draw();
  totals();
}
async function totals() {
  $('totals').textContent = 'working out texsizes.txt totals…';
  const t = await (await fetch('/api/totals')).json();
  let s = `texsizes.txt: ${t.count} images, ${kb(t.bytes)} of VRAM (${kb(t.defaultBytes)} by default, ` +
          `${t.bytes >= t.defaultBytes ? '+' : ''}${kb(t.bytes - t.defaultBytes)})  ·  pal8: ${t.pal8} (4 can be loaded at once)`;
  if (t.missing.length) s += '  ·  not found: ' + t.missing.join(', ');
  $('totals').textContent = s;
}
function draw() {
  const f = $('filter').value.toLowerCase(), only = $('onlySet').checked;
  const html = [];
  for (const it of all) {
    if (f && !it.name.includes(f)) continue;
    if (only && !it.set) continue;
    html.push(`<div class="row${it.set ? ' set' : ''}${cur === it ? ' sel' : ''}" data-n="${it.name}">` +
              `<span class="n">${it.name}</span><span class="s">${desc(now(it))}</span></div>`);
  }
  $('list').innerHTML = html.join('');
}
$('list').onclick = e => {
  const r = e.target.closest('.row'); if (!r) return;
  pick(all.find(it => it.name === r.dataset.n));
};
function pick(it) {
  cur = it;
  $('name').textContent = it.name;
  const s = now(it);
  $('w').value = s.w; $('h').value = s.h; $('fmt').value = s.fmt;
  $('orig').src = '/api/orig?name=' + encodeURIComponent(it.name);
  $('ocap').textContent = `original ${it.w}x${it.h}`;
  draw();
  preview();
}
function choice() { return { w: +$('w').value, h: +$('h').value, fmt: $('fmt').value }; }
function size() {
  if (!cur) return;
  const z = +$('zoom').value;
  // both shown the size of the original (times the zoom), the Dreamcast one stretched to it
  const w = cur.w * (z || 1), h = cur.h * (z || 1);
  for (const id of ['orig', 'dc']) { $(id).style.width = w + 'px'; $(id).style.height = h + 'px'; }
}
async function preview() {
  if (!cur) return;
  const c = choice(), id = ++req, it = cur;
  $('stat').textContent = 'converting…';
  size();
  const q = `/api/dc?name=${encodeURIComponent(it.name)}&w=${c.w}&h=${c.h}&fmt=${c.fmt}`;
  const r = await fetch(q);
  if (id !== req) return;
  if (!r.ok) { $('stat').innerHTML = `<span class="err">${(await r.json()).error}</span>`; return; }
  const info = JSON.parse(r.headers.get('X-Info'));
  $('dc').src = URL.createObjectURL(await r.blob());
  $('dcap').textContent = `Dreamcast ${info.w}x${info.h} ${info.fmt}${info.mips ? ' + mipmaps' : ''}`;
  let s = `<b>${kb(info.bytes)}</b> VRAM`;
  const d = it.def;
  if (c.w !== d.w || c.h !== d.h || c.fmt !== d.fmt) {
    const dr = await fetch(`/api/dc?name=${encodeURIComponent(it.name)}&w=${d.w}&h=${d.h}&fmt=${d.fmt}`);
    if (id !== req) return;
    const dinfo = JSON.parse(dr.headers.get('X-Info'));
    s += ` · default ${desc(d)} ${kb(dinfo.bytes)}`;
  } else s += ' · the default';
  const saved = it.set ? desc(it.set) : desc(d) + ' (default)';
  const same = it.set ? (c.w === it.set.w && c.h === it.set.h && c.fmt === it.set.fmt)
                      : (c.w === d.w && c.h === d.h && c.fmt === d.fmt);
  s += same ? ' · saved' : ` · <span style="color:var(--accent)">not saved (texsizes.txt: ${saved})</span>`;
  $('stat').innerHTML = s;
}
async function save(set) {
  const r = await fetch('/api/save', { method: 'POST', body: JSON.stringify({ name: cur.name, set }) });
  const j = await r.json();
  if (!r.ok) { $('stat').innerHTML = `<span class="err">${j.error}</span>`; return; }
  cur.set = j.set;
  if (!set) { const d = cur.def; $('w').value = d.w; $('h').value = d.h; $('fmt').value = d.fmt; }
  draw(); preview(); totals();
}
for (const id of ['w', 'h', 'fmt']) $(id).onchange = preview;
$('zoom').onchange = size;
$('save').onclick = () => cur && save(choice());
$('reset').onclick = () => cur && save(null);
$('filter').oninput = draw;
$('onlySet').onchange = draw;
load();
</script></body></html>
"""


def main():
    global pvrtex, srcdir, sizesfile, discdir
    args = sys.argv[1:]
    if "--disc" in args:
        i = args.index("--disc")
        discdir = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    if len(args) not in (3, 4):
        sys.exit(__doc__)
    pvrtex, srcdir, sizesfile = args[:3]
    port = int(args[3]) if len(args) == 4 else 8765
    for root, _, files in os.walk(srcdir):
        for f in files:
            base, ext = os.path.splitext(f)
            if ext.lower() in (".tga", ".jpg"):
                path = os.path.join(root, f)
                name = os.path.relpath(os.path.join(root, base), srcdir).lower()
                if name not in images:
                    try:
                        w, h = Image.open(path).size
                    except Exception:
                        continue
                    images[name] = {"path": path, "w": w, "h": h}
    print("texpanel: %d images, http://localhost:%d/" % (len(images), port), flush=True)
    if discdir:
        print("viewer: %s at http://localhost:%d/view/" % (discdir, port), flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()

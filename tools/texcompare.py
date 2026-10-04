#!/usr/bin/env python3
"""
make texcompare: each image `make assets` made a .dt of, as a .png of the
original (left) next to the .dt as the Dreamcast gets it (right, decoded by
pvrtex and blown up to the original's size), to flick through.

usage: texcompare.py <pvrtex> <src dir> <dc dir> <out dir> [path prefix]
"""
import os
import struct
import subprocess
import sys
import tempfile
from multiprocessing import Pool

from PIL import Image, ImageDraw

MIN_HEIGHT = 256    # small ones blown up to this, to be seen at all
BAR = 20            # the caption bar


def checker(w, h):
    """a checkerboard under the images, so alpha shows"""
    im = Image.new("RGBA", (w, h), (96, 96, 96, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, h, 16):
        for x in range((y // 16) % 2 * 16, w, 32):
            d.rectangle([x, y, x + 15, y + 15], fill=(128, 128, 128, 255))
    return im


def one(job):
    pvrtex, orig, dt, out, rel = job
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "dc.png")
        # a pal8 .dt has its palette on the end: pvrtex wants it in <dt>.pal
        data = open(dt, "rb").read()
        size = struct.unpack_from("<I", data, 4)[0]
        fmt = ("VQ" if data[19] & 0x40 else ("1555", "565", "4444", "YUV422", "", "PAL4", "PAL8")[data[19] >> 3 & 7])
        if len(data) > size:
            split = os.path.join(tmp, "dc.dt")
            open(split, "wb").write(data[:size])
            open(split + ".pal", "wb").write(b"DPAL" + struct.pack("<I", (len(data) - size) // 4) + data[size:])
            dt_in = split
        else:
            dt_in = dt
        r = subprocess.run([pvrtex, "-i", dt_in, "-p", png], capture_output=True)
        if r.returncode or not os.path.exists(png):
            return rel + ": pvrtex can't decode it"
        dc = Image.open(png).convert("RGBA")
    try:
        og = Image.open(orig).convert("RGBA")
    except Exception as e:
        return "%s: %s" % (rel, e)

    w, h = og.size
    scale = max(1, -(-MIN_HEIGHT // h))
    w, h = w * scale, h * scale
    left = og.resize((w, h), Image.NEAREST)
    right = dc.resize((w, h), Image.NEAREST)

    im = Image.new("RGBA", (w * 2 + 8, h + BAR), (0, 0, 0, 255))
    for x, part in ((0, left), (w + 8, right)):
        under = checker(w, h)
        under.alpha_composite(part)
        im.paste(under, (x, BAR))
    d = ImageDraw.Draw(im)
    d.text((4, 4), "%s  original %dx%d" % (rel, og.size[0], og.size[1]), fill=(255, 255, 255, 255))
    d.text((w + 12, 4), "DC %dx%d %s, %d bytes" % (dc.size[0], dc.size[1], fmt, len(data)),
           fill=(255, 255, 0, 255))

    os.makedirs(os.path.dirname(out), exist_ok=True)
    im.convert("RGB").save(out)
    return None


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    pvrtex, src, dc, outdir = sys.argv[1:5]
    prefix = sys.argv[5].lower() if len(sys.argv) == 6 else ""

    # the originals by name without extension, lower case (the pk3s' case varies)
    origs = {}
    for root, _, files in os.walk(src):
        for f in files:
            base, ext = os.path.splitext(f)
            if ext.lower() in (".tga", ".jpg"):
                rel = os.path.relpath(os.path.join(root, base), src)
                origs.setdefault(rel.lower(), os.path.join(root, f))

    jobs = []
    for root, _, files in os.walk(dc):
        for f in files:
            if not f.endswith(".dt"):
                continue
            rel = os.path.relpath(os.path.join(root, f[:-3]), dc)
            orig = origs.get(rel.lower())
            if not orig or not rel.lower().startswith(prefix):
                continue    # a map's lightmaps: no image of their own
            jobs.append((pvrtex, orig, os.path.join(root, f), os.path.join(outdir, rel + ".png"), rel))

    with Pool() as pool:
        for err in pool.imap_unordered(one, jobs, chunksize=8):
            if err:
                print(err, file=sys.stderr)
    print("texcompare: %d images in %s" % (len(jobs), outdir))


if __name__ == "__main__":
    main()

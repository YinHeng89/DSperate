#!/usr/bin/env python3
"""Look at the smooth-edge mode's split map (Renderer3D::extract_splits) for one frame.

  DS_EDGE_MOCK=edges.bin [DS_EDGE_MOCK_FROM=N] dsperate-headless ... --subpixel \\
      --dump-frames frames.bin --dump-from F --dump-count 1 ROM
  tools/edge_mock.py edges.bin frames.bin OUT.png [--from N] [--crop x y w h] [--zoom 16]

DS_EDGE_MOCK turns the mode on and appends, per rendered 3D frame, the 3D output
(u32 x 256 x 192) and its split map (two u32 record slots a pixel: the pixel's own cut,
then a neighbour's spill into it). The 3D frame a display frame shows was rendered a
frame or two earlier, on either screen, so the record and screen are found by matching
colours, directly or at the 15 bits a display capture keeps.

OUT.png is the 3D picture at `zoom` with every record's replaced region tinted: red a
strong (silhouette-by-depth) cut, orange a weak one (the scaler's colour test decides),
green a spill (the polygon's colour reaching into a neighbour's cell), blue an
edge-marked pixel (the scaler lays the outline band from its position). The census is
printed. What the scaler finally admits is not in the map: for that, dump its output
with --dump-scaled N FILE.

Use DS_R3D_THREADS=1 when comparing runs or builds: with several raster bands a few
records on a band's first line vary from run to run.
"""
import argparse
import numpy as np
from PIL import Image

W, H = 256, 192
N = W * H
REC = N * 4 + N * 8


def expand(c):
    v = ((c & 0x3F) << 18) | ((c & 0x3F00) << 2) | ((c & 0x3F0000) >> 14)
    return v | ((v & 0xC0C0C0) >> 6) | 0xFF000000


def captured(c):
    v = (((c >> 1) & 0x1F) << 1) | (((c >> 9) & 0x1F) << 9) | (((c >> 17) & 0x1F) << 17)
    return expand(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('edges'); ap.add_argument('frames'); ap.add_argument('out')
    ap.add_argument('--frame', type=int, default=0, help='index into the frames file')
    ap.add_argument('--crop', type=int, nargs=4, default=[0, 0, W, H], help='x y w h in DS pixels')
    ap.add_argument('--zoom', type=int, default=8)
    a = ap.parse_args()

    d = np.fromfile(a.edges, dtype=np.uint8)
    nrec = len(d) // REC
    with open(a.frames, 'rb') as f:
        f.seek(a.frame * N * 8)
        fr = np.frombuffer(f.read(N * 8), dtype='<u4').reshape(2, H, W)

    def rec(i):
        o = d[i * REC:(i + 1) * REC]
        return o[:N * 4].view('<u4').reshape(H, W), o[N * 4:].view('<u4').reshape(H, W, 2)

    best = max((int((fn(rec(i)[0]) == fr[s]).sum()), i, s, via) for i in range(nrec) for s in range(2)
               for via, fn in (('direct', expand), ('capture', captured)))
    m, i, s, via = best
    top, sm = rec(i)
    side = sm & 7
    print(f'record {i} of {nrec}, screen {s}, shown {via}: {m} of {N} pixels are the 3D layer')
    print(f'  records: own {int((side[..., 0] != 0).sum())}, spills {int((side[..., 1] != 0).sum())}, '
          f'weak {int(((sm >> 31) & 1).sum())}, edge-marked {int(((sm >> 29) & 1).sum())}')

    x0, y0, w, h = a.crop
    Z = a.zoom
    e = expand(top)
    img = np.zeros((h * Z, w * Z, 3), np.float32)
    jj, ii = np.mgrid[0:Z, 0:Z]
    fx = ((2 * ii + 1) * 32) // (2 * Z); fy = ((2 * jj + 1) * 32) // (2 * Z)
    for y in range(h):
        for x in range(w):
            c = int(e[y0 + y, x0 + x])
            cell = np.empty((Z, Z, 3), np.float32); cell[:] = ((c >> 16) & 255, (c >> 8) & 255, c & 255)
            for k in range(2):
                v = int(sm[y0 + y, x0 + x, k])
                if not v:
                    continue
                sd = v & 7; unc = (v >> 3) & 63; sl = (v >> 9) & 0x7F; sl = sl - 128 if sl >= 64 else sl
                marked, spill, weak = (v >> 29) & 1, (v >> 30) & 1, (v >> 31) & 1
                if marked:
                    unc = max(0, min(32, ((v >> 19) & 0x7F) - 32))
                low = sd in (1, 3); horiz = sd >= 3
                along = fy if horiz else fx; across = (fx if horiz else fy) - 16
                bound = (unc if low else 32 - unc) + (sl * across) // 32
                region = (along < bound) if low else (along >= bound)
                tint = (0, 90, 255) if marked else (0, 255, 0) if spill else (255, 160, 0) if weak else (255, 0, 0)
                cell[region] = cell[region] * 0.4 + np.array(tint, np.float32) * 0.6
            img[y * Z:(y + 1) * Z, x * Z:(x + 1) * Z] = cell
    if Z >= 8:
        img[::Z, :, :] = img[::Z, :, :] * 0.5 + 40; img[:, ::Z, :] = img[:, ::Z, :] * 0.5 + 40
    Image.fromarray(img.astype(np.uint8)).save(a.out)
    print(f'  wrote {a.out}')


if __name__ == '__main__':
    main()

import sys, glob, numpy as np
from PIL import Image

def unpack(f, W, H):
    stride = W*10//8
    raw = np.fromfile(f, dtype=np.uint8)[:stride*H].reshape(H, stride)
    bits = np.unpackbits(raw, axis=1, bitorder='little').reshape(H, -1)[:, :W*10]
    return (bits.reshape(H, W, 10) * (1 << np.arange(10))).sum(-1).astype(np.float32)

def demosaic(b, pat):
    """바이리니어 보간 — 전체 해상도 유지"""
    H, W = b.shape
    off = {"RGGB": (0,0), "GRBG": (0,1), "GBRG": (1,0), "BGGR": (1,1)}[pat]
    R = np.zeros_like(b); G = np.zeros_like(b); B = np.zeros_like(b)
    m = np.zeros((H, W), np.uint8)
    m[off[0]::2, off[1]::2] = 1                       # R
    m[1-off[0]::2, 1-off[1]::2] = 3                   # B
    m[m == 0] = 2                                     # G
    for ch, val in ((R,1), (G,2), (B,3)):
        mask = (m == val).astype(np.float32)
        num = b * mask
        # 3x3 박스로 보간
        k = np.ones((3,3), np.float32)
        from numpy.lib.stride_tricks import sliding_window_view
        pad_n = np.pad(num, 1, mode='reflect'); pad_m = np.pad(mask, 1, mode='reflect')
        sn = sliding_window_view(pad_n, (3,3)).sum(axis=(2,3))
        sm = sliding_window_view(pad_m, (3,3)).sum(axis=(2,3))
        ch[:] = np.where(mask > 0, b, sn/np.maximum(sm, 1))
    return np.stack([R, G, B], -1)

if __name__ == "__main__":
    pat_glob, W, H, bp, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    files = sorted(glob.glob(pat_glob))
    acc = None
    for f in files:
        b = unpack(f, W, H); acc = b if acc is None else acc + b
    b = acc/len(files)
    print(f"{len(files)}장, raw mean={b.mean():.1f} max={b.max():.0f} p99={np.percentile(b,99):.0f}")
    b = np.clip(b - 64, 0, None)
    rgb = demosaic(b, bp)
    m = rgb.reshape(-1,3).mean(0)
    rgb = rgb * (m.mean()/m).reshape(1,1,3)          # gray-world
    hi = np.percentile(rgb, 99.5)
    rgb = np.clip(rgb/hi, 0, 1) ** (1/2.2)
    Image.fromarray((rgb*255).astype(np.uint8)).save(out)
    print("saved", out, rgb.shape, "mean=", round(float(rgb.mean()),3))

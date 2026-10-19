# Measures an icon pill in a sim screenshot: ink insets on every side, the
# icon-to-text gap, and where the icon's blurred blob is centered.
# Usage: measure_pill.py <png> <pill top> <pill height>
import sys

import numpy as np
from PIL import Image, ImageFilter

path, top, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(path).convert("L")
L = np.asarray(im).astype(int)
mid = top + h // 2
far = L[top - 6, 180]
x0 = 180
while abs(L[mid, x0 - 1] - far) > 2:
    x0 -= 1
x1 = 180
while abs(L[mid, x1 + 1] - far) > 2:
    x1 += 1
pill = L[top : top + h, x0 : x1 + 1]
pb = np.median(pill)
ink = pill > pb + 30
cols = np.where(ink.any(0))[0]
split = next(cols[i + 1] for i in range(len(cols) - 1) if cols[i + 1] - cols[i] > 2)
icon = ink[:, :split]
r = np.where(icon.any(1))[0]
c = np.where(icon.any(0))[0]
text = ink[:, split:]
r2 = np.where(text.any(1))[0]
c2 = np.where(text.any(0))[0] + split
w = x1 - x0 + 1
print(f"pill {w}x{h} at x={x0}")
print(
    f"icon ink {c[-1] - c[0] + 1}x{r[-1] - r[0] + 1}: left {c[0]}, top {r[0]}, bottom {h - 1 - r[-1]}"
)
print(
    f"text ink: left {c2[0]}, right {w - 1 - c2[-1]}, top {r2[0]}, bottom {h - 1 - r2[-1]}"
)
print(f"gap icon->text {c2[0] - c[-1] - 1}")

# Optical center: blur the icon to a blob and take its weighted center.
crop = Image.fromarray((pill[:, :split] - pb).clip(0, 255).astype(np.uint8))
blob = np.asarray(crop.filter(ImageFilter.GaussianBlur(4))).astype(float)
ys, xs_ = np.mgrid[0 : blob.shape[0], 0 : blob.shape[1]]
cy = (blob * ys).sum() / blob.sum()
cx = (blob * xs_).sum() / blob.sum()
print(
    f"icon blob center x={cx:.1f} y={cy:.1f} (pill mid y={(h - 1) / 2:.1f}); "
    f"blob left inset = {cx - (c[-1] - c[0] + 1) / 2:.1f} equiv"
)

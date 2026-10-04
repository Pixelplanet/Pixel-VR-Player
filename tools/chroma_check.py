import sys

w, h = 1920, 1080
fr = w * h * 3 // 2
luma = w * h
for name in sys.argv[1:]:
    data = open(name, "rb").read()
    n = len(data) // fr
    print(name, "frames", n)
    for i in range(n):
        base = i * fr + luma
        c = data[base:base + w * (h // 2)]
        lo, hi = min(c), max(c)
        mean = sum(c) / len(c)
        # fraction of chroma bytes near neutral (128) vs near 0
        near0 = sum(1 for b in c if b < 8) / len(c)
        print("  f%d chroma min/max/mean=%d/%d/%.1f near0=%.2f" % (i, lo, hi, mean, near0))

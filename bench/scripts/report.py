#!/usr/bin/env python3
"""Reads bench/results/*.csv and writes results/summary.md and results/charts/*.svg (Python 3 standard library only)."""
import csv
import math
import os
import platform
import subprocess
import sys
from collections import defaultdict

RES = sys.argv[1] if len(sys.argv) > 1 else "results"
CH = os.path.join(RES, "charts")
os.makedirs(CH, exist_ok=True)


def load(name):
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        return []
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


# ------------------------------------------------------------------ data
air = load("airtime.csv")
bands = load("bands.csv")
maxp = load("max_payload.csv")
runs = {p: load(p + ".csv") for p in ("snr", "size", "cond", "rate44", "mtfixed")}
idle = load("idle.csv")
inaud = load("inaudible.csv")          # plan column: inaudible-snr / inaudible-cond / inaudible-size
idle18 = load("inaudible-idle.csv")

papr = {}  # (config, rate) -> PAPR dB of the 32-byte payload
for r in air:
    if r["size"] == "32" and r["papr_db"]:
        papr[(r["config"], int(r["rate"]))] = float(r["papr_db"])
offset = {}  # (config, rate, noise) -> in-band SNR offset
for r in bands:
    for nz in ("white", "pink", "babble"):
        offset[(r["config"], int(r["rate"]), nz)] = float(r["offset_%s_db" % nz])
NOISE_OF = {"awgn": "white", "echo": "white", "drift+100": "white", "drift-100": "white", "pink": "pink", "babble": "babble", "combo": "pink"}

CLEAN = 99.0  # key used for the noiseless cell


def cells(rows):
    """(config, rate, size, condition, reps) -> {snr: [ok, n, wrong, failed]} plus raw rows by cell."""
    agg = defaultdict(lambda: defaultdict(lambda: [0, 0, 0, 0]))
    for r in rows:
        if r["success"] == "-1":
            continue
        snr = CLEAN if r["clean"] == "1" else float(r["snr_db"])
        k = (r["config"], int(r["rate"]), r["size"], r["condition"], int(r["reps"]))
        a = agg[k][snr]
        a[0] += int(r["success"])
        a[1] += 1
        a[2] += int(r["n_wrong"])
        a[3] += int(r["n_failed"])
    return agg


def rate_at(curve, snr):
    a = curve.get(snr)
    return None if not a or not a[1] else a[0] / a[1]


def threshold(curve, level=0.9):
    """Lowest grid SNR s such that success >= level at s and at every higher grid point (clean included)."""
    pts = sorted(curve)
    if not pts or curve[pts[-1]][1] == 0 or curve[pts[-1]][0] / curve[pts[-1]][1] < level:
        return None
    best = None
    for s in reversed(pts):
        ok, n = curve[s][0], curve[s][1]
        if n and ok / n >= level:
            best = s
        else:
            break
    return best


def fmt_thr(t, add=0.0):
    if t is None:
        return "—"
    if t == CLEAN:
        return "clean only"
    return "%+.0f" % (t + add)


def pct(curve, s):
    v = rate_at(curve, s)
    return "" if v is None else "%.0f" % (100 * v)


BYS_ORDER = ["bysound top/normal", "bysound high/normal", "bysound upper/normal", "bysound mid/normal", "bysound xhigh/normal", "bysound wide/normal", "bysound audible/normal",
             "bysound top/robust", "bysound high/robust", "bysound upper/robust", "bysound mid/robust"]
GG_ORDER = ["ggwave ultrasound-normal", "ggwave ultrasound-fast", "ggwave ultrasound-fastest", "ggwave audible-normal", "ggwave audible-fast", "ggwave audible-fastest",
            "ggwave dt-normal", "ggwave dt-fast", "ggwave dt-fastest"]
MT_ORDER = ["ggwave mt-normal (fixed-len)", "ggwave mt-fast (fixed-len)", "ggwave mt-fastest (fixed-len)"]
INAUD_ORDER = ["bysound top/normal", "bysound top/robust", "ggwave dt-normal @18k", "ggwave dt-fast @18k", "ggwave dt-fastest @18k",
               "ggwave mt-normal @18k (fixed-len)", "ggwave mt-fast @18k (fixed-len)", "ggwave mt-fastest @18k (fixed-len)"]
ALL_ORDER = BYS_ORDER + GG_ORDER


def name(c):
    return c.replace("bysound ", "bysound ").replace("ggwave ", "ggwave ")


def airof(cfg, size, rate=48000):
    for r in air:
        if r["config"] == cfg and r["size"] == size and int(r["rate"]) == rate and r["encoded"] == "1":
            return r
    return None


def statusof(cfg, size, rate=48000):
    """Why a size could not be sent: 'exceeds 30 s cap' (bysound) or 'cannot send' (ggwave), '' if it was."""
    for r in air:
        if r["config"] == cfg and r["size"] == size and int(r["rate"]) == rate:
            return "" if r["encoded"] == "1" else r.get("status", "cannot send")
    return ""


def median(v):
    v = sorted(v)
    if not v:
        return None
    m = len(v) // 2
    return v[m] if len(v) % 2 else 0.5 * (v[m - 1] + v[m])


# ------------------------------------------------------------------ SVG
PALETTE = ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#0ca30c", "#9085e9", "#e66767"]  # validated on #ffffff and #0d1117
INK = "#888888"
FONT = "font-family=\"-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif\""


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def line_chart(path, title, subtitle, xlabel, ylabel, series, xr, yr, xticks, yticks, xlog=False, yfmt=lambda v: "%g" % v, xfmt=lambda v: "%g" % v):
    """series: list of dicts {name, color, dash, pts:[(x,y)]}; transparent background, neutral gray text."""
    sub = []
    for part in (subtitle or "").split("; "):
        if sub and len(sub[-1]) + len(part) < 105:
            sub[-1] += "; " + part
        elif part:
            sub.append(part)
    W, H = 1000, 470 + 16 * max(0, len(sub) - 1)
    L, R, T, B = 64, 300, 52 + 16 * len(sub), 52
    pw, ph = W - L - R, H - T - B

    def X(x):
        if xlog:
            return L + (math.log(x) - math.log(xr[0])) / (math.log(xr[1]) - math.log(xr[0])) * pw
        return L + (x - xr[0]) / (xr[1] - xr[0]) * pw

    def Y(y):
        return T + ph - (y - yr[0]) / (yr[1] - yr[0]) * ph

    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" %s font-size="12">' % (W, H, W, H, FONT)]
    o.append('<title>%s</title>' % esc(title))
    o.append('<text x="%d" y="24" fill="%s" font-size="15" font-weight="600">%s</text>' % (L, INK, esc(title)))
    for i, line in enumerate(sub):
        o.append('<text x="%d" y="%d" fill="%s" font-size="12">%s</text>' % (L, 44 + 16 * i, INK, esc(line)))
    for v in yticks:
        y = Y(v)
        o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="%s" stroke-opacity="0.22" stroke-width="1"/>' % (L, L + pw, y, y, INK))
        o.append('<text x="%d" y="%.1f" fill="%s" text-anchor="end" dominant-baseline="middle">%s</text>' % (L - 8, y, INK, esc(yfmt(v))))
    for v in xticks:
        x = X(v)
        o.append('<line x1="%.1f" x2="%.1f" y1="%d" y2="%d" stroke="%s" stroke-opacity="0.12" stroke-width="1"/>' % (x, x, T, T + ph, INK))
        o.append('<text x="%.1f" y="%d" fill="%s" text-anchor="middle">%s</text>' % (x, T + ph + 18, INK, esc(xfmt(v))))
    o.append('<line x1="%d" x2="%d" y1="%d" y2="%d" stroke="%s" stroke-opacity="0.6" stroke-width="1"/>' % (L, L + pw, T + ph, T + ph, INK))
    o.append('<text x="%.1f" y="%d" fill="%s" text-anchor="middle">%s</text>' % (L + pw / 2, H - 10, INK, esc(xlabel)))
    o.append('<text transform="translate(16 %.1f) rotate(-90)" fill="%s" text-anchor="middle">%s</text>' % (T + ph / 2, INK, esc(ylabel)))
    for s in series:
        pts = [(X(x), Y(y), x, y) for x, y in s["pts"] if y is not None and xr[0] - 1e-9 <= x <= xr[1] + 1e-9]
        if not pts:
            continue
        d = " ".join(("M" if i == 0 else "L") + "%.1f %.1f" % (p[0], p[1]) for i, p in enumerate(pts))
        dash = ' stroke-dasharray="7 4"' if s.get("dash") else ""
        o.append('<path d="%s" fill="none" stroke="%s" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"%s/>' % (d, s["color"], dash))
        for px, py, x, y in pts:
            shape = ('<rect x="%.1f" y="%.1f" width="7" height="7" rx="1"' % (px - 3.5, py - 3.5)) if s.get("dash") else ('<circle cx="%.1f" cy="%.1f" r="3.5"' % (px, py))
            o.append('%s fill="%s"><title>%s: %s → %s</title>%s' % (shape, s["color"], esc(s["name"]), esc(xfmt(x)), esc(yfmt(y)), "</rect>" if s.get("dash") else "</circle>"))
    # legend (right column); ggwave series are dashed with square markers, bysound solid with round markers
    ly = T + 6
    for s in series:
        dash = ' stroke-dasharray="7 4"' if s.get("dash") else ""
        lx = L + pw + 18
        o.append('<line x1="%d" x2="%d" y1="%d" y2="%d" stroke="%s" stroke-width="2"%s/>' % (lx, lx + 26, ly, ly, s["color"], dash))
        if s.get("dash"):
            o.append('<rect x="%.1f" y="%.1f" width="7" height="7" rx="1" fill="%s"/>' % (lx + 9.5, ly - 3.5, s["color"]))
        else:
            o.append('<circle cx="%d" cy="%d" r="3.5" fill="%s"/>' % (lx + 13, ly, s["color"]))
        o.append('<text x="%d" y="%d" fill="%s" dominant-baseline="middle">%s</text>' % (lx + 34, ly, INK, esc(s["name"])))
        ly += 22
    o.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(o) + "\n")


def bar_chart(path, title, subtitle, rows, xlabel, left=250):
    """Horizontal bars on a log axis: rows = [(label, value, color)]."""
    W = 900
    L, R, T = left, 90, 64
    bh, gap = 24, 10
    H = T + len(rows) * (bh + gap) + 50
    pw = W - L - R
    vmax = 10 ** math.ceil(math.log10(max(v for _, v, _ in rows)))

    def X(v):
        return L + math.log10(v) / math.log10(vmax) * pw

    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" %s font-size="12">' % (W, H, W, H, FONT)]
    o.append('<title>%s</title>' % esc(title))
    o.append('<text x="24" y="24" fill="%s" font-size="15" font-weight="600">%s</text>' % (INK, esc(title)))
    o.append('<text x="24" y="44" fill="%s">%s</text>' % (INK, esc(subtitle)))
    k = 1
    while k <= vmax:
        x = X(k)
        o.append('<line x1="%.1f" x2="%.1f" y1="%d" y2="%d" stroke="%s" stroke-opacity="0.2"/>' % (x, x, T - 6, H - 44, INK))
        o.append('<text x="%.1f" y="%d" fill="%s" text-anchor="middle">%s</text>' % (x, H - 28, INK, "{:,}".format(k)))
        k *= 10
    o.append('<text x="%.1f" y="%d" fill="%s" text-anchor="middle">%s</text>' % (L + pw / 2, H - 8, INK, esc(xlabel)))
    y = T
    for lab, v, col in rows:
        w = max(X(v) - L, 2)
        o.append('<text x="%d" y="%.1f" fill="%s" text-anchor="end" dominant-baseline="middle">%s</text>' % (L - 10, y + bh / 2, INK, esc(lab)))
        o.append('<path d="M%d %d H%.1f a4 4 0 0 1 4 4 V%.1f a4 4 0 0 1 -4 4 H%d Z" fill="%s"><title>%s: %s bytes</title></path>' % (L, y, L + w - 4, y + bh - 4, L, col, esc(lab), "{:,}".format(v)))
        o.append('<text x="%.1f" y="%.1f" fill="%s" dominant-baseline="middle">%s B</text>' % (L + w + 8, y + bh / 2, INK, "{:,}".format(v)))
        y += bh + gap
    o.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(o) + "\n")


# ------------------------------------------------------------------ charts
charts = []
SNR_X = (-36, 4)
SNR_TICKS = list(range(-36, 5, 4))
PCT_TICKS = [0, 25, 50, 75, 100]


def curve_series(agg, specs, rate=48000, size="32", cond="awgn", reps=1, xshift=None):
    out = []
    for i, (cfg, col) in enumerate(specs):
        c = agg.get((cfg, rate, size, cond, reps))
        if not c:
            continue
        sh = xshift(cfg) if xshift else 0.0
        pts = [(s + sh, 100.0 * c[s][0] / c[s][1]) for s in sorted(c) if s != CLEAN and c[s][1]]
        out.append({"name": name(cfg), "color": col, "dash": cfg.startswith("ggwave"), "pts": pts})
    return out


snr_agg = cells(runs["snr"])
ULTRA = [("bysound top/normal", PALETTE[0]), ("ggwave ultrasound-fast", PALETTE[1]), ("bysound high/normal", PALETTE[2]), ("ggwave ultrasound-fastest", PALETTE[3]),
         ("ggwave ultrasound-normal", PALETTE[4]), ("bysound top/robust", PALETTE[5]), ("bysound wide/normal", PALETTE[6])]
AUDI = [("bysound audible/normal", PALETTE[0]), ("ggwave audible-fast", PALETTE[1]), ("ggwave dt-fast", PALETTE[2]), ("ggwave audible-fastest", PALETTE[3]),
        ("ggwave audible-normal", PALETTE[4]), ("ggwave dt-normal", PALETTE[5]), ("ggwave dt-fastest", PALETTE[6])]
SBSN = [("bysound top/normal", PALETTE[0]), ("bysound upper/normal", PALETTE[1]), ("bysound high/normal", PALETTE[2]), ("bysound xhigh/normal", PALETTE[3]),
        ("bysound mid/normal", PALETTE[4]), ("bysound audible/normal", PALETTE[5]), ("bysound wide/normal", PALETTE[6])]
SBSR = [("bysound top/robust", PALETTE[0]), ("bysound upper/robust", PALETTE[1]), ("bysound high/robust", PALETTE[2]), ("bysound mid/robust", PALETTE[3])]


def ntrials(agg, cfg, **kw):
    c = agg.get((cfg, kw.get("rate", 48000), kw.get("size", "32"), kw.get("cond", "awgn"), kw.get("reps", 1)))
    return max((v[1] for v in c.values()), default=0) if c else 0


if snr_agg:
    nt = ntrials(snr_agg, "bysound top/normal")
    for reps in (1, 2):
        tag = "1 transmission" if reps == 1 else "2 transmissions (any decoded)"
        for fn, title, specs in (("snr_ultrasonic", "Near-ultrasonic modes: success vs SNR", ULTRA), ("snr_audible", "Audible modes: success vs SNR", AUDI),
                                 ("snr_bysound_normal", "bysound normal profile, every channel", SBSN), ("snr_bysound_robust", "bysound robust profile", SBSR)):
            if fn.startswith("snr_bysound") and reps == 2:
                continue
            p = os.path.join(CH, "%s_%dx.svg" % (fn, reps))
            line_chart(p, title, "32-byte ASCII payload, AWGN, 48 kHz, %s, %d trials per point" % (tag, nt), "SNR (dB): sound-on signal power / white-noise power in 0–24 kHz",
                       "exact payload decoded (%)", curve_series(snr_agg, specs, reps=reps), SNR_X, (0, 100), SNR_TICKS, PCT_TICKS, yfmt=lambda v: "%g%%" % v)
            charts.append(p)
    for fn, title, specs in (("snr_ultrasonic_peak", "Near-ultrasonic modes at equal peak level", ULTRA), ("snr_audible_peak", "Audible modes at equal peak level", AUDI)):
        p = os.path.join(CH, fn + "_1x.svg")
        line_chart(p, title, "same trials as the SNR chart, x shifted by each mode's peak-to-average ratio (both played at the same peak)",
                   "peak-to-noise ratio (dB): peak² of the waveform / noise power in 0–24 kHz", "exact payload decoded (%)",
                   curve_series(snr_agg, specs, reps=1, xshift=lambda c: papr.get((c, 48000), 0.0)), (-28, 16), (0, 100), list(range(-28, 17, 4)), PCT_TICKS,
                   yfmt=lambda v: "%g%%" % v)
        charts.append(p)

cond_agg = cells(runs["cond"])
COND = [("bysound top/normal", PALETTE[0]), ("ggwave ultrasound-fast", PALETTE[1]), ("bysound high/normal", PALETTE[2]), ("ggwave audible-fast", PALETTE[3]),
        ("bysound audible/normal", PALETTE[4]), ("bysound top/robust", PALETTE[5]), ("ggwave ultrasound-normal", PALETTE[6])]
COND_DESC = {"echo": "echo taps 0.5 at 8 ms + 0.25 at 23 ms, white noise", "pink": "pink noise (more power at low frequencies)",
             "babble": "babble-like noise (6 speech-shaped, syllable-modulated sources)", "drift+100": "receiver clock +100 ppm, white noise",
             "drift-100": "receiver clock −100 ppm, white noise", "combo": "echo + clock +100 ppm + pink noise"}
for cd in ("echo", "pink", "babble", "drift+100", "drift-100", "combo"):
    if not any(k[3] == cd for k in cond_agg):
        continue
    p = os.path.join(CH, "cond_%s_1x.svg" % cd.replace("+", "plus").replace("-", "minus"))
    line_chart(p, "Condition: %s" % cd, "32-byte payload, 1 transmission, %s, %d trials per point" % (COND_DESC[cd], ntrials(cond_agg, "bysound top/normal", cond=cd)),
               "SNR (dB): sound-on signal power / noise power in 0–24 kHz", "exact payload decoded (%)", curve_series(cond_agg, COND, cond=cd), (-32, 4), (0, 100),
               list(range(-32, 5, 4)), PCT_TICKS, yfmt=lambda v: "%g%%" % v)
    charts.append(p)

SIZES = ["8", "16", "32", "48", "64", "96", "128", "140", "256", "512", "1024", "2048"]
TP = [("bysound top/normal", PALETTE[0], "bysound top/normal"), ("ggwave ultrasound-fast", PALETTE[1], "ggwave fast (audible = ultrasound)"),
      ("bysound high/normal", PALETTE[2], "bysound high/normal (= upper, mid)"), ("ggwave ultrasound-fastest", PALETTE[3], "ggwave fastest (audible = ultrasound)"),
      ("ggwave ultrasound-normal", PALETTE[4], "ggwave normal (audible = ultrasound)"), ("bysound top/robust", PALETTE[5], "bysound top/robust"),
      ("bysound wide/normal", PALETTE[6], "bysound wide/normal"), ("bysound audible/normal", PALETTE[7], "bysound audible/normal")]
if air:
    for metric, fn, title, ylab, yr, yt in (("throughput_Bps", "throughput_vs_size", "Throughput vs payload size", "payload bytes / airtime (B/s)", (0, 80), [0, 20, 40, 60, 80]),
                                            ("air_s", "airtime_vs_size", "Airtime vs payload size", "airtime of one transmission (s)", None, None)):
        ser = []
        for cfg, col, lab in TP:
            pts = []
            for s in SIZES:
                r = airof(cfg, s)
                if r:
                    pts.append((int(s), float(r[metric])))
            ser.append({"name": lab, "color": col, "dash": cfg.startswith("ggwave"), "pts": pts})
        if yr is None:
            top = max(y for s in ser for _, y in s["pts"])
            yr = (0.1, 10 ** math.ceil(math.log10(top)))
            # log y for airtime: emulate by transforming values
            for s in ser:
                s["pts"] = [(x, math.log10(y)) for x, y in s["pts"]]
            yr = (-1, math.log10(yr[1]))
            yt = list(range(-1, int(yr[1]) + 1))
            yfmt = lambda v: "%g" % (10 ** v)
        else:
            yfmt = lambda v: "%g" % v
        p = os.path.join(CH, fn + ".svg")
        line_chart(p, title, "computed from each encoder's output at 48 kHz (no channel); airtime includes bysound's 350 ms trailing gap and ggwave's start/end markers; ggwave stops at 140 B and bysound at its 30 s cap",
                   "payload size (bytes, log scale)", ylab, ser, (8, 2048), yr, [8, 16, 32, 64, 128, 256, 512, 1024, 2048], yt, xlog=True, yfmt=yfmt, xfmt=lambda v: "%d" % v)
        charts.append(p)

if maxp:
    mp = {r["config"]: r for r in maxp}

    def row(label, cfgs, col):
        r = mp.get(cfgs[0])
        return (label % float(r["air_s_at_max"]), int(r["max_payload_bytes"]), col) if r else None

    rows = [x for x in (
        row("bysound wide (%.1f s)", ["bysound wide/normal"], PALETTE[0]),
        row("bysound xhigh (%.1f s)", ["bysound xhigh/normal"], PALETTE[0]),
        row("bysound audible (%.1f s)", ["bysound audible/normal"], PALETTE[0]),
        row("bysound high/upper/mid (%.1f s)", ["bysound high/normal"], PALETTE[0]),
        row("bysound top (%.1f s)", ["bysound top/normal"], PALETTE[0]),
        row("bysound high/upper/mid robust (%.1f s)", ["bysound high/robust"], PALETTE[5]),
        row("bysound top robust (%.1f s)", ["bysound top/robust"], PALETTE[5]),
    ) if x]
    gv = [float(r["air_s_at_max"]) for r in maxp if r["system"] == "ggwave" and "fixed" not in r["config"]]
    if gv:
        rows.append(("ggwave variable length (%.1f-%.1f s)" % (min(gv), max(gv)), 140, PALETTE[1]))
    gf = [float(r["air_s_at_max"]) for r in maxp if "fixed" in r["config"]]
    if gf:
        rows.append(("ggwave fixed length (%.1f-%.1f s)" % (min(gf), max(gf)), 64, PALETTE[2]))
    p = os.path.join(CH, "max_payload.svg")
    bar_chart(p, "Max payload in one transmission", "ASCII bytes the encoder accepts; bysound is capped at 30 s of airtime (350 ms gap included), ggwave at 140 B (64 B fixed length); airtime at the maximum in brackets",
              rows, "bytes (log scale)", left=300)
    charts.append(p)

# inaudible group: bysound top vs ggwave DT/MT moved to 18 kHz
INAUD_SPECS = [(c, PALETTE[i]) for i, c in enumerate(INAUD_ORDER)]
in_snr = cells([r for r in inaud if r["plan"] == "inaudible-snr"])
in_cond = cells([r for r in inaud if r["plan"] == "inaudible-cond"])
in_size = cells([r for r in inaud if r["plan"] == "inaudible-size"])
if in_snr:
    p = os.path.join(CH, "snr_inaudible_1x.svg")
    line_chart(p, "Inaudible channels (18–19.5 kHz): success vs SNR",
               "32-byte ASCII payload, AWGN, 48 kHz, 1 transmission, %d trials per point; ggwave DT/MT moved to start at 18 kHz (custom configuration), receiver restricted to them; "
               "MT in fixed-length mode (receiver knows the length)" % ntrials(in_snr, "bysound top/normal"),
               "SNR (dB): sound-on signal power / white-noise power in 0–24 kHz", "exact payload decoded (%)", curve_series(in_snr, INAUD_SPECS), SNR_X, (0, 100), SNR_TICKS,
               PCT_TICKS, yfmt=lambda v: "%g%%" % v)
    charts.append(p)
if in_cond:
    p = os.path.join(CH, "cond_echo_inaudible_1x.svg")
    line_chart(p, "Inaudible channels (18–19.5 kHz): echo", "32-byte payload, 1 transmission, %s, %d trials per point" % (COND_DESC["echo"], ntrials(in_cond, "bysound top/normal", cond="echo")),
               "SNR (dB): sound-on signal power / noise power in 0–24 kHz", "exact payload decoded (%)", curve_series(in_cond, INAUD_SPECS, cond="echo"), (-32, 4), (0, 100),
               list(range(-32, 5, 4)), PCT_TICKS, yfmt=lambda v: "%g%%" % v)
    charts.append(p)
if air and any(r["config"].endswith("@18k") or "@18k " in r["config"] for r in air):
    ser = []
    for cfg, col in INAUD_SPECS:
        pts = [(int(sz), float(airof(cfg, sz)["air_s"])) for sz in ("8", "16", "32", "48", "64", "96", "128", "140") if airof(cfg, sz)]
        ser.append({"name": name(cfg), "color": col, "dash": cfg.startswith("ggwave"), "pts": pts})
    p = os.path.join(CH, "airtime_inaudible.svg")
    line_chart(p, "Inaudible channels (18–19.5 kHz): airtime vs payload size",
               "computed from each encoder's output at 48 kHz (no channel); bysound includes its 350 ms trailing gap, ggwave DT its start/end markers; "
               "ggwave MT (fixed-length) stops at 64 B, DT at 140 B", "payload size (bytes)", "airtime of one transmission (s)", ser, (0, 140), (0, 40),
               [8, 32, 64, 96, 128, 140], [0, 5, 10, 15, 20, 25, 30, 35, 40], xfmt=lambda v: "%d" % v)
    charts.append(p)

# ------------------------------------------------------------------ summary.md
md = []
w = md.append


def gg_commit():
    try:
        return subprocess.check_output(["git", "-C", os.path.join(os.path.dirname(RES), "third_party", "ggwave"), "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return "unknown"


def machine():
    try:
        cpu = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
        n = subprocess.check_output(["sysctl", "-n", "hw.ncpu"], text=True).strip()
        osv = subprocess.check_output(["sw_vers", "-productVersion"], text=True).strip()
        cc = subprocess.check_output(["clang", "--version"], text=True).splitlines()[0]
        return "%s, %s logical CPUs, macOS %s, %s" % (cpu, n, osv, cc)
    except Exception:
        return platform.platform()


w("# Benchmark results: bysound v4 vs ggwave\n")
w("Generated by `scripts/report.py` from the CSV files in this folder. ggwave commit `%s`. Machine: %s.\n" % (gg_commit(), machine()))
w("All numbers are measured in a **simulated** channel (see README for the exact model). SNR is full-band (0–24 kHz): sound-on signal power over noise power. "
  "Success = the exact payload came out of the receiver. With 2 transmissions, success = at least one exact decode. "
  "The SNR90 columns give the lowest grid SNR from which every higher grid point, the noiseless case included, reached ≥ 90 % success (grid step in each table title); "
  "“—” means not even the noiseless case reached 90 %.\n")


def snr_table(agg, reps, cols, title, configs, size="32", cond="awgn", rate=48000, step=2):
    nz = NOISE_OF[cond]
    w("\n### %s\n" % title)
    w("| configuration | " + " | ".join(("%+d dB" % c) if c != CLEAN else "clean" for c in cols) + " | SNR90 full-band (dB, step %d) | SNR90 in-band | SNR90 peak-ref. | false decodes |" % step)
    w("|---|" + "---:|" * (len(cols) + 4))
    for cfg in configs:
        c = agg.get((cfg, rate, size, cond, reps))
        if not c:
            continue
        t = threshold(c)
        wrong = sum(v[2] for v in c.values())
        w("| %s | " % name(cfg) + " | ".join(pct(c, s) for s in cols) + " | %s | %s | %s | %d |" % (
            fmt_thr(t), fmt_thr(t, offset.get((cfg, rate, nz), 0.0)) if t not in (None, CLEAN) else fmt_thr(t),
            fmt_thr(t, papr.get((cfg, rate), 0.0)) if t not in (None, CLEAN) else fmt_thr(t), wrong))


if snr_agg:
    cols = [-28, -24, -20, -16, -12, -8, -4, 0, CLEAN]
    nt = ntrials(snr_agg, "bysound top/normal")
    w("\n## 1. Success rate (%) vs SNR — 32-byte ASCII, AWGN, 48 kHz\n")
    w("In-band SNR = full-band SNR + offset, where the offset uses the band holding 99 % of each mode's signal power (`bands.csv`). "
      "Peak-referenced = full-band SNR + the mode's peak-to-average power ratio (PAPR, `airtime.csv`): the noise level both modes tolerate when played at the same peak amplitude.\n")
    snr_table(snr_agg, 1, cols, "1 transmission (%d trials per cell)" % nt, ALL_ORDER)
    snr_table(snr_agg, 2, cols, "2 transmissions, success if any decodes (%d trials per cell)" % ntrials(snr_agg, "bysound top/normal", reps=2), ALL_ORDER)

# airtime / throughput
if air:
    w("\n## 2. Airtime and throughput (deterministic, from the encoders, 48 kHz)\n")
    w("Airtime = length of one transmission as produced by the library's encoder: bysound includes its 350 ms trailing gap (part of `bys_tx_fill` output), "
      "ggwave includes its start and end sound markers. Throughput = payload bytes / airtime. Sound-on throughput excludes bysound's gap. "
      "“exceeds 30 s cap” = bysound's encoder refuses it (one transmission, gap included, lasts at most 30 s); “—” = ggwave cannot send it (140 B limit).\n")
    szs = ["8", "32", "64", "128", "utf8", "512", "2048"]
    w("| configuration | " + " | ".join("%s B air (s)" % ("48 UTF-8" if s == "utf8" else s) for s in szs) + " | 32 B (B/s) | 128 B (B/s) | 128 B sound-on (B/s) | PAPR (dB) |")
    w("|---|" + "---:|" * (len(szs) + 4))
    for cfg in ALL_ORDER + MT_ORDER:
        cellsx = []
        for s in szs:
            r = airof(cfg, s)
            cellsx.append("%.2f" % float(r["air_s"]) if r else (statusof(cfg, s) or "—"))
        r32, r128 = airof(cfg, "32"), airof(cfg, "128")
        w("| %s | %s | %s | %s | %s | %s |" % (name(cfg), " | ".join(cellsx), "%.1f" % float(r32["throughput_Bps"]) if r32 else "—",
                                              "%.1f" % float(r128["throughput_Bps"]) if r128 else "—", "%.1f" % float(r128["throughput_active_Bps"]) if r128 else "—",
                                              r32["papr_db"] if r32 else "—"))
    if maxp:
        w("\n### Max payload in one transmission (ASCII bytes accepted by the encoder)\n")
        w("| configuration | max bytes | airtime at max (s) |")
        w("|---|---:|---:|")
        for r in maxp:
            w("| %s | %s | %.1f |" % (name(r["config"]), r["max_payload_bytes"], float(r["air_s_at_max"])))
        w("\nbysound's limit is the 30 s cap on one transmission (preamble + header + body + 350 ms gap; `maxFrameSeconds` in `protocol-v4.json`), so it depends on the channel and profile; "
          "the figure is in packed bytes (ASCII letters pack 1:1; digit pairs pack 2:1, so numeric text can be twice as long). "
          "ggwave's variable-length mode truncates anything above 140 bytes, fixed-length mode accepts at most 64; its airtime at the maximum ranges from 5.0 s to 38.9 s.\n")

# latency & CPU from the snr plan (clean cells)
if runs["snr"]:
    w("\n## 3. Decode latency and receiver CPU cost (32 B, AWGN plan, 1 transmission)\n")
    w("Latency = time from the first sample of the transmission to the moment the decoder returned the payload (1024-sample chunk granularity, median over the noiseless trials). "
      "Real-time factor = audio duration / receiver thread CPU time (receiver init included). It is given twice: as the median over all trials of the configuration "
      "(every SNR, so most trials are below the threshold and nothing is decoded) and as the median over the trials that decoded the payload. "
      "bysound listens to all channels of its profile at once (7 normal / 4 robust); ggwave listens to all its protocols (library default).\n")
    w("| configuration | median latency clean (s) | airtime (s) | real-time factor, all trials | CPU per trial, all trials (ms) | real-time factor, decoded trials | CPU per trial, decoded trials (ms) | decoded trials |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|")
    for cfg in ALL_ORDER:
        rr = [r for r in runs["snr"] if r["config"] == cfg and r["reps"] == "1"]
        if not rr:
            continue
        lat = median([float(r["latency_s"]) for r in rr if r["clean"] == "1" and float(r["latency_s"]) >= 0])
        rtf = median([float(r["audio_s"]) / float(r["cpu_s"]) for r in rr if float(r["cpu_s"]) > 0])
        cpu = median([1000 * float(r["cpu_s"]) for r in rr])
        dd = [r for r in rr if r["success"] == "1" and float(r["cpu_s"]) > 0]
        rtf_d = median([float(r["audio_s"]) / float(r["cpu_s"]) for r in dd])
        cpu_d = median([1000 * float(r["cpu_s"]) for r in dd])
        a = airof(cfg, "32")
        w("| %s | %s | %s | %s | %s | %s | %s | %d |" % (name(cfg), "%.2f" % lat if lat is not None else "—", "%.2f" % float(a["air_s"]) if a else "—", "%.0f×" % rtf if rtf else "—",
                                                       "%.1f" % cpu if cpu is not None else "—", "%.0f×" % rtf_d if rtf_d else "—", "%.1f" % cpu_d if cpu_d is not None else "—", len(dd)))
    if idle:
        w("\nIdle listening (no transmission; 60 s segments of noise at −20 dBFS RMS, `idle.csv`), median over the segments of each receiver:\n")
        w("| receiver | segments | median CPU per 60 s of audio (ms) | real-time factor |")
        w("|---|---:|---:|---:|")
        by_rx = defaultdict(list)
        for r in idle:
            by_rx[r["receiver"]].append(float(r["cpu_s"]) / float(r["seconds"]))
        for rx in sorted(by_rx):
            m = median(by_rx[rx])
            w("| %s | %d | %.1f | %.0f× |" % (rx, len(by_rx[rx]), 60000 * m, 1 / m))

# payload size
size_agg = cells(runs["size"])
if size_agg:
    w("\n## 4. Payload size vs robustness — SNR90 (full-band dB, grid step 4; 512/2048 B grid −24…−8 step 4 and 0)\n")
    szs = ["8", "32", "64", "128", "utf8", "512", "2048"]
    w("| configuration | " + " | ".join(("48 B UTF-8" if s == "utf8" else s + " B") for s in szs) + " |")
    w("|---|" + "---:|" * len(szs))
    for cfg in ALL_ORDER:
        row = []
        for s in szs:
            if s == "32":
                c = cells([r for r in runs["snr"] if r["snr_db"] == "" or float(r["snr_db"]) % 4 == 0]).get((cfg, 48000, "32", "awgn", 1))
            else:
                c = size_agg.get((cfg, 48000, s, "awgn", 1))
            row.append(fmt_thr(threshold(c)) if c else (statusof(cfg, s) or "—"))
        w("| %s | %s |" % (name(cfg), " | ".join(row)))
    w("\n(32 B column taken from section 1, restricted to the same 4 dB grid.) Trials per cell: %s.\n" % ", ".join(
        "%s: %d" % ("48 B UTF-8" if s == "utf8" else s + " B", max(ntrials(size_agg, c, size=s) for c in BYS_ORDER)) for s in szs if s != "32"))
    w("\nSuccess at the noiseless point (%):\n")
    w("| configuration | " + " | ".join(("48 B UTF-8" if s == "utf8" else s + " B") for s in szs if s != "32") + " |")
    w("|---|" + "---:|" * (len(szs) - 1))
    for cfg in ALL_ORDER:
        row = []
        for s in szs:
            if s == "32":
                continue
            c = size_agg.get((cfg, 48000, s, "awgn", 1))
            row.append(pct(c, CLEAN) if c else (statusof(cfg, s) or "—"))
        w("| %s | %s |" % (name(cfg), " | ".join(row)))

# conditions
if cond_agg:
    w("\n## 5. Channel impairments — SNR90 (full-band dB, grid step 4), 32 B\n")
    w("Same SNR definition (noise power in 0–24 kHz). For pink and babble noise most of the noise power sits at low frequency, so the in-band SNR of near-ultrasonic modes is much higher than the full-band figure (offsets in `bands.csv`).\n")
    conds = ["echo", "pink", "babble", "drift+100", "drift-100", "combo"]
    for reps in (1, 2):
        w("\n%s:\n" % ("1 transmission" if reps == 1 else "2 transmissions (any decoded)"))
        w("| configuration | AWGN (sec. 1) | " + " | ".join(conds) + " |")
        w("|---|" + "---:|" * (len(conds) + 1))
        base = cells([r for r in runs["snr"] if r["snr_db"] == "" or float(r["snr_db"]) % 4 == 0])
        for cfg in ALL_ORDER:
            row = [fmt_thr(threshold(base.get((cfg, 48000, "32", "awgn", reps), {})))]
            for cd in conds:
                c = cond_agg.get((cfg, 48000, "32", cd, reps))
                row.append(fmt_thr(threshold(c)) if c else "")
            w("| %s | %s |" % (name(cfg), " | ".join(row)))
    w("\nNoiseless success (%) under each impairment, 1 transmission:\n")
    w("| configuration | " + " | ".join(conds) + " |")
    w("|---|" + "---:|" * len(conds))
    for cfg in ALL_ORDER:
        w("| %s | %s |" % (name(cfg), " | ".join(pct(cond_agg.get((cfg, 48000, "32", cd, 1), {}), CLEAN) for cd in conds)))

# 44.1 kHz
r44 = cells(runs["rate44"])
if r44:
    w("\n## 6. bysound at 44.1 kHz vs 48 kHz — SNR90 (full-band dB, grid step 4), 32 B, 1 transmission\n")
    base = cells([r for r in runs["snr"] if r["snr_db"] == "" or float(r["snr_db"]) % 4 == 0])
    w("| configuration | 48 kHz | 44.1 kHz | clean success 44.1 kHz (%) |")
    w("|---|---:|---:|---:|")
    for cfg in BYS_ORDER:
        c = r44.get((cfg, 44100, "32", "awgn", 1))
        w("| %s | %s | %s | %s |" % (name(cfg), fmt_thr(threshold(base.get((cfg, 48000, "32", "awgn", 1), {}))), fmt_thr(threshold(c)) if c else "—", pct(c, CLEAN) if c else "—"))

# MT fixed
mt = cells(runs["mtfixed"])
if mt:
    w("\n## 7. ggwave mono-tone (MT) protocols — fixed-length mode only\n")
    w("ggwave refuses MT protocols in variable-length mode (`Mono-tone protocols with variable length are not supported`). They were run with `payloadLength` = payload size on both ends, "
      "which means the receiver must know the length in advance (not comparable to the other rows). SNR90 full-band, grid step 4:\n")
    w("| configuration | 8 B | 32 B | 64 B | clean success 32 B (%) |")
    w("|---|---:|---:|---:|---:|")
    for cfg in MT_ORDER:
        w("| %s | %s | %s | %s | %s |" % (name(cfg), *[fmt_thr(threshold(mt.get((cfg, 48000, s, "awgn", 1), {}))) for s in ("8", "32", "64")],
                                          pct(mt.get((cfg, 48000, "32", "awgn", 1), {}), CLEAN)))

# false decodes
w("\n## 8. False decodes\n")
tot = defaultdict(lambda: [0, 0, 0])
for p, rows in runs.items():
    for r in rows:
        if r["success"] == "-1":
            continue
        k = r["system"]
        if p == "mtfixed":
            k += ", mono-tone protocols in fixed-length mode (section 7)"
        elif k == "ggwave":
            k += ", default variable-length protocols"
        tot[k][0] += int(r["n_wrong"])
        tot[k][1] += 1
        tot[k][2] += 1 if int(r["n_wrong"]) else 0
w("Wrong payloads accepted during the transmission trials (any decoded payload that differs from the one sent, any channel/protocol; for bysound, acknowledgement, "
  "reference or risk frames decoded out of noise also count):\n")
w("| system | trials | wrong payloads accepted | trials with at least one |")
w("|---|---:|---:|---:|")
for k in sorted(tot):
    w("| %s | %d | %d | %d |" % (k, tot[k][1], tot[k][0], tot[k][2]))
w("\nA trial can deliver more than one wrong payload, so the first count can exceed the second. "
  "Total trials: %d." % sum(v[1] for v in tot.values()))
if idle:
    w("\nNoise only, no transmission (60 s segments at −20 dBFS RMS):\n")
    w("| receiver | noise | minutes | detections | ggwave “could not decode” events |")
    w("|---|---|---:|---:|---:|")
    agg = defaultdict(lambda: [0, 0, 0])
    for r in idle:
        a = agg[(r["receiver"], r["noise"])]
        a[0] += int(r["seconds"])
        a[1] += int(r["detections"])
        a[2] += int(r["failed"])
    for (rx, nz), a in sorted(agg.items()):
        w("| %s | %s | %d | %d | %s |" % (rx, nz, a[0] // 60, a[1], a[2] if rx.startswith("ggwave") else "n/a"))

# band table
if bands:
    w("\n## 9. Occupied band and in-band SNR offsets (48 kHz)\n")
    w("| configuration | band holding 99 % of power (Hz) | offset white (dB) | offset pink (dB) | offset babble (dB) |")
    w("|---|---|---:|---:|---:|")
    for cfg in ALL_ORDER + MT_ORDER:
        r = next((b for b in bands if b["config"] == cfg and b["rate"] == "48000"), None)
        if r:
            w("| %s | %s–%s | %s | %s | %s |" % (name(cfg), r["band_lo_hz"], r["band_hi_hz"], r["offset_white_db"], r["offset_pink_db"], r["offset_babble_db"]))

# noiseless scans
import re
scan = {}
for sz in ("8", "32", "128"):
    pth = os.path.join(RES, "noiseless_scan_%sB.txt" % sz)
    if not os.path.exists(pth):
        continue
    for line in open(pth):
        m = re.match(r"(.+?)\s+(\d+) B: (\d+) of (\d+) payloads not delivered without noise, (\d+) wrong", line)
        if m:
            scan[(m.group(1).strip(), sz)] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
if scan:
    w("\n## 10. Noiseless delivery scan (no channel at all)\n")
    w("Each library's encoder output at full scale, 12345 zero samples before and 1 s after, fed to its own receiver in 1024-sample chunks; "
      "many different random payloads (`bench scan <size> <n>`, output in `noiseless_scan_*B.txt`). Cell = payloads not delivered / payloads tried, "
      "and wrong payloads accepted.\n")
    w("| configuration | 8 B not delivered | 8 B wrong | 32 B not delivered | 32 B wrong | 128 B not delivered | 128 B wrong |")
    w("|---|---:|---:|---:|---:|---:|---:|")
    for cfg in ALL_ORDER:
        row = []
        for sz in ("8", "32", "128"):
            v = scan.get((cfg, sz))
            row += ["%d / %d" % (v[0], v[1]), "%d" % v[2]] if v else ["—", "—"]
        w("| %s | %s |" % (name(cfg), " | ".join(row)))
cpath = os.path.join(RES, "bysound_codec_scan.txt")
if os.path.exists(cpath):
    w("\nbysound symbol-level round trip (encoder symbols straight into `bys_rx_push`/`bys_rx_finish`, no audio; `bench codec <size> <n>`, output in `bysound_codec_scan.txt`). "
      "Payloads whose last block ends in a 0x00 Reed-Solomon parity byte are counted separately: the block without that byte is also a valid codeword, "
      "so the receiver closes the last block at the first length whose assembled payload passes the CRC-32 (`docs/PROTOCOL.md`, section 14.7):\n")
    w("| size | payloads failing / tried (same for all 11 channel/profile combinations) | payloads with last parity byte = 0x00 | of those, failing |")
    w("|---|---:|---:|---:|")
    seen = set()
    for line in open(cpath):
        m = re.match(r"bysound top/normal\s+(\d+) B: (\d+) of (\d+) payloads fail.*last parity byte 0x00 in (\d+); prediction matches (\d+) of (\d+)", line)
        if m and m.group(1) not in seen:
            seen.add(m.group(1))
            fails, tried, zero, agree = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
            both = (fails + zero - (tried - agree)) // 2  # |fail xor zero| = tried - agree
            w("| %s B | %d / %d | %d | %d |" % (m.group(1), fails, tried, zero, both))

# inaudible channels
if inaud:
    w("\n## 11. Inaudible channels (18–19.5 kHz)\n")
    w("ggwave has no built-in protocol above 18 kHz: its audible protocols use ~1.9–6.3 kHz, DT/MT ~1.1–2.6 kHz and “ultrasound” 15.0–19.4 kHz (6 tones × 16 bins = 4.5 kHz). "
      "The 6-tone layout does not fit above 18 kHz (it would reach 22.5 kHz), but the 2-tone DT protocols (2 × 16 bins = 1.5 kHz) and the 1-tone MT protocols (16 bins) do. "
      "For this section they were **moved to start at FFT bin 384 = 18,000 Hz** with ggwave's public C API (`ggwave_txProtocolSetFreqStart`, `ggwave_rxProtocolSetFreqStart`), "
      "a custom configuration, not a ggwave default. Everything else keeps the library defaults. DT's start/end markers are generated from the protocol's own start bin, so they move too. "
      "**The ggwave receiver is restricted to the six moved protocols** (`ggwave_rxToggleProtocol`); DT runs in the default variable-length mode, MT in fixed-length mode "
      "(the receiver knows the payload length, as in section 7). bysound is unchanged: `top` (18.0–19.5 kHz, 2 tones), with the same receivers as in the other sections "
      "(normal: 7 channels, robust: 4). Same channel, chunking, grids and trial counts as sections 1, 4 and 5, **and the same seeds** (one transmission).\n")
    # reproducibility check: bysound rows vs the snr/cond/size plans
    def key(r, plan):
        return (plan, r["config"], r["size"], r["condition"], r["snr_db"], r["clean"], r["reps"], r["trial"])
    ref = {}
    for pl in ("snr", "cond", "size"):
        for r in runs[pl]:
            if r["config"] in ("bysound top/normal", "bysound top/robust") and r["reps"] == "1":
                ref[key(r, pl)] = (r["success"], r["n_correct"], r["n_wrong"], r["latency_s"])
    same = diff = 0
    for r in inaud:
        if r["system"] != "bysound":
            continue
        k = key(r, r["plan"].replace("inaudible-", ""))
        if k in ref:
            if ref[k] == (r["success"], r["n_correct"], r["n_wrong"], r["latency_s"]):
                same += 1
            else:
                diff += 1
    if same + diff:
        w("Seed check: %d bysound trials of this plan have a counterpart in the `snr`, `cond` or `size` plan; %d give the same result (success, decodes, wrong payloads, latency), %d differ.\n"
          % (same + diff, same, diff))

    w("\n### 11.1 Measured band (PSD of the encoded waveform, 32 B)\n")
    w("Welch PSD (4096-point Hann) of each encoder's whole output, markers included (`bands.csv`). “Share in 18.0–19.5 kHz” = power in bysound `top`'s 32 tone slots "
      "(centres 18,000 + 46.875·k Hz, k = 0…31, each ±23.4 Hz, i.e. 17,977–19,477 Hz).\n")
    w("| configuration | band holding 99 % of power (Hz) | share in 18.0–19.5 kHz | PAPR (dB) | offset white (dB) | offset pink (dB) |")
    w("|---|---|---:|---:|---:|---:|")
    for cfg in INAUD_ORDER:
        r = next((b for b in bands if b["config"] == cfg and b["rate"] == "48000"), None)
        if r:
            fr = r.get("frac_18k_19k5")
            w("| %s | %s–%s | %s | %s | %s | %s |" % (name(cfg), r["band_lo_hz"], r["band_hi_hz"], "%.2f %%" % (100 * float(fr)) if fr else "—",
                                                    "%.2f" % papr[(cfg, 48000)] if (cfg, 48000) in papr else "—", r["offset_white_db"], r["offset_pink_db"]))

    if in_snr:
        cols = [-20, -18, -16, -14, -12, -10, -8, -6, -4, 0, CLEAN]
        w("\n### 11.2 Success rate (%%) vs SNR — 32 B, AWGN, 1 transmission (%d trials per cell)\n" % ntrials(in_snr, "bysound top/normal"))
        w("| configuration | " + " | ".join(("%+d dB" % c) if c != CLEAN else "clean" for c in cols) + " | SNR90 full-band (step 2) | SNR90 in-band | SNR90 peak-ref. | false decodes | airtime 32 B (s) |")
        w("|---|" + "---:|" * (len(cols) + 5))
        for cfg in INAUD_ORDER:
            c = in_snr.get((cfg, 48000, "32", "awgn", 1))
            if not c:
                continue
            t = threshold(c)
            a = airof(cfg, "32")
            w("| %s | %s | %s | %s | %s | %d | %s |" % (name(cfg), " | ".join(pct(c, s_) for s_ in cols), fmt_thr(t),
                                                       fmt_thr(t, offset.get((cfg, 48000, "white"), 0.0)) if t not in (None, CLEAN) else fmt_thr(t),
                                                       fmt_thr(t, papr.get((cfg, 48000), 0.0)) if t not in (None, CLEAN) else fmt_thr(t),
                                                       sum(v[2] for v in c.values()), "%.2f" % float(a["air_s"]) if a else "—"))

    dpath = os.path.join(RES, "dt_clean_check.txt")
    if os.path.exists(dpath):
        dt = defaultdict(lambda: [0, 0, 0])
        for line in open(dpath):
            m = re.match(r"(ggwave dt-\S+( @18k)?)\s+plan (\S+)\s*: (\d+) clean trials, (\d+) failed, (\d+) of those", line)
            if m:
                a_ = dt["moved to 18 kHz (plan inaudible)" if m.group(2) else "built-in, 1.1–2.6 kHz (plans snr, size, cond)"]
                a_[0] += int(m.group(4))
                a_[1] += int(m.group(5))
                a_[2] += int(m.group(6))
        if dt:
            w("\n**ggwave DT at the noiseless point** (`dt_clean_check.txt`, from `bench dither`). Some DT trials fail at the clean point (signal at −40 dBFS, int16 quantisation, "
              "no noise) although they decode at 0 dB. ggwave's own log (`BENCH_GGWAVE_LOG=1 build/bench trial …`) shows the cause: the receiver detects its end marker "
              "inside the data, stops recording early and the decode fails. Each failing trial was re-run with the same seed plus noise at +60 dB SNR "
              "(−100 dBFS RMS, about the size of the int16 quantisation noise and far below anything a microphone adds):\n")
            w("| DT protocols | clean-point trials | failed | of those, decoded with noise at +60 dB SNR |")
            w("|---|---:|---:|---:|")
            for k in sorted(dt, reverse=True):
                w("| %s | %d | %d (%.1f %%) | %d |" % (k, dt[k][0], dt[k][1], 100.0 * dt[k][1] / max(1, dt[k][0]), dt[k][2]))
            w("\nSo these clean-point failures need input with no noise at all (the one built-in DT trial that still fails is under echo). The SNR90 definition still counts the clean point; "
              "for the DT protocols moved to 18 kHz it does not change any SNR90 in section 11.2, because their clean point stays at or above 90 %.\n")

    if in_cond:
        conds = ["echo", "pink", "babble", "drift+100", "drift-100", "combo"]
        w("\n### 11.3 Channel impairments — SNR90 (full-band dB, grid step 4), 32 B, 1 transmission (%d trials per cell)\n" % ntrials(in_cond, "bysound top/normal", cond="echo"))
        base = cells([r for r in inaud if r["plan"] == "inaudible-snr" and (r["snr_db"] == "" or float(r["snr_db"]) % 4 == 0)])
        w("| configuration | AWGN (11.2, 4 dB grid) | " + " | ".join(conds) + " |")
        w("|---|" + "---:|" * (len(conds) + 1))
        for cfg in INAUD_ORDER:
            row = [fmt_thr(threshold(base.get((cfg, 48000, "32", "awgn", 1), {})))]
            for cd in conds:
                c = in_cond.get((cfg, 48000, "32", cd, 1))
                row.append(fmt_thr(threshold(c)) if c else "")
            w("| %s | %s |" % (name(cfg), " | ".join(row)))
        w("\nNoiseless success (%) under each impairment, and wrong payloads accepted over all SNRs of the impairment runs:\n")
        w("| configuration | " + " | ".join(conds) + " | wrong payloads |")
        w("|---|" + "---:|" * (len(conds) + 1))
        for cfg in INAUD_ORDER:
            wr = sum(v[2] for cd in conds for v in in_cond.get((cfg, 48000, "32", cd, 1), {}).values())
            w("| %s | %s | %d |" % (name(cfg), " | ".join(pct(in_cond.get((cfg, 48000, "32", cd, 1), {}), CLEAN) for cd in conds), wr))

    if in_size:
        szs = ["8", "32", "64", "128", "140"]
        w("\n### 11.4 Payload size — airtime (deterministic) and SNR90 (full-band dB, grid step 4, %d trials per cell)\n" % ntrials(in_size, "bysound top/normal", size="8"))
        w("| configuration | " + " | ".join("%s B air (s)" % s_ for s_ in szs) + " | " + " | ".join("%s B SNR90" % s_ for s_ in szs) + " | " +
          " | ".join("%s B clean (%%)" % s_ for s_ in szs) + " |")
        w("|---|" + "---:|" * (3 * len(szs)))
        for cfg in INAUD_ORDER:
            airs = []
            for s_ in szs:
                a = airof(cfg, s_)
                airs.append("%.2f" % float(a["air_s"]) if a else (statusof(cfg, s_) or "—"))
            thr = [fmt_thr(threshold(in_size[(cfg, 48000, s_, "awgn", 1)])) if (cfg, 48000, s_, "awgn", 1) in in_size else (statusof(cfg, s_) or "—") for s_ in szs]
            cln = [pct(in_size[(cfg, 48000, s_, "awgn", 1)], CLEAN) if (cfg, 48000, s_, "awgn", 1) in in_size else (statusof(cfg, s_) or "—") for s_ in szs]
            w("| %s | %s | %s | %s |" % (name(cfg), " | ".join(airs), " | ".join(thr), " | ".join(cln)))

    # CPU and latency
    rr_all = [r for r in inaud if r["plan"] == "inaudible-snr"]
    if rr_all:
        w("\n### 11.5 Decode latency and receiver CPU (32 B, section 11.2 trials)\n")
        w("As in section 3. In these trials bysound's receivers listen to all channels of their profile (7 / 4), ggwave's only to the six moved protocols; "
          "the idle table below also gives bysound listening to `top` only.\n")
        w("| configuration | median latency clean (s) | airtime (s) | real-time factor, all trials | real-time factor, decoded trials | CPU per decoded trial (ms) | decoded trials |")
        w("|---|---:|---:|---:|---:|---:|---:|")
        for cfg in INAUD_ORDER:
            rr = [r for r in rr_all if r["config"] == cfg]
            if not rr:
                continue
            lat = median([float(r["latency_s"]) for r in rr if r["clean"] == "1" and float(r["latency_s"]) >= 0])
            rtf = median([float(r["audio_s"]) / float(r["cpu_s"]) for r in rr if float(r["cpu_s"]) > 0])
            dd = [r for r in rr if r["success"] == "1" and float(r["cpu_s"]) > 0]
            rtf_d = median([float(r["audio_s"]) / float(r["cpu_s"]) for r in dd])
            cpu_d = median([1000 * float(r["cpu_s"]) for r in dd])
            a = airof(cfg, "32")
            w("| %s | %s | %s | %s | %s | %s | %d |" % (name(cfg), "%.2f" % lat if lat is not None else "—", "%.2f" % float(a["air_s"]) if a else "—",
                                                       "%.0f×" % rtf if rtf else "—", "%.0f×" % rtf_d if rtf_d else "—", "%.1f" % cpu_d if cpu_d is not None else "—", len(dd)))
    if idle18:
        w("\nIdle listening and false detections (no transmission; 10 × 60 s of each of white, pink and babble-like noise at −20 dBFS RMS, `inaudible-idle.csv`):\n")
        w("| receiver | minutes | median CPU per 60 s of audio (ms) | real-time factor | detections | ggwave “could not decode” events |")
        w("|---|---:|---:|---:|---:|---:|")
        by_rx = defaultdict(list)
        det = defaultdict(lambda: [0, 0, 0])
        for r in idle18:
            by_rx[r["receiver"]].append(float(r["cpu_s"]) / float(r["seconds"]))
            d = det[r["receiver"]]
            d[0] += int(r["seconds"])
            d[1] += int(r["detections"])
            d[2] += int(r["failed"])
        for rx in sorted(by_rx):
            m = median(by_rx[rx])
            w("| %s | %d | %.1f | %.0f× | %d | %s |" % (rx, det[rx][0] // 60, 60000 * m, 1 / m, det[rx][1], det[rx][2] if rx.startswith("ggwave") else "n/a"))

    # false decodes over every inaudible trial
    w("\n### 11.6 False decodes (all trials of this section)\n")
    w("| configuration | trials | wrong payloads accepted | trials with at least one |")
    w("|---|---:|---:|---:|")
    fd = defaultdict(lambda: [0, 0, 0])
    for r in inaud:
        if r["success"] == "-1":
            continue
        a = fd[r["config"]]
        a[0] += 1
        a[1] += int(r["n_wrong"])
        a[2] += 1 if int(r["n_wrong"]) else 0
    for cfg in INAUD_ORDER:
        if cfg in fd:
            w("| %s | %d | %d | %d |" % (name(cfg), *fd[cfg]))
    bysz = defaultdict(lambda: [0, 0])
    for r in inaud:
        if int(r["n_wrong"] or 0) and r["success"] != "-1":
            bysz[r["size"]][0] += int(r["n_wrong"])
            bysz[r["size"]][1] += 1 if r["clean"] == "1" else 0
    if bysz:
        w("\nBy payload size: " + "; ".join("%s B: %d wrong payloads (%d trials at the clean point)" % (k, v[0], v[1]) for k, v in sorted(bysz.items(), key=lambda kv: int(kv[0]))) +
          ". The fixed-length MT receiver decodes at every frame and can return a payload more than once per trial.\n")

    scan18 = {}
    for sz in ("8", "32", "128"):
        pth = os.path.join(RES, "noiseless_scan_inaudible_%sB.txt" % sz)
        if not os.path.exists(pth):
            continue
        for line in open(pth):
            m = re.match(r"(.+?)\s+(\d+) B: (\d+) of (\d+) payloads not delivered without noise, (\d+) wrong", line)
            if m:
                scan18[(m.group(1).strip(), sz)] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
    if scan18:
        w("\n### 11.7 Noiseless delivery scan (no channel; as section 10)\n")
        w("| configuration | 8 B not delivered | 8 B wrong | 32 B not delivered | 32 B wrong | 128 B not delivered | 128 B wrong |")
        w("|---|---:|---:|---:|---:|---:|---:|")
        for cfg in INAUD_ORDER:
            row = []
            for sz in ("8", "32", "128"):
                v = scan18.get((cfg, sz))
                row += ["%d / %d" % (v[0], v[1]), "%d" % v[2]] if v else [statusof(cfg, sz) or "—", "—"]
            w("| %s | %s |" % (name(cfg), " | ".join(row)))

w("\n## Charts\n")
for p in charts:
    w("- `charts/%s`" % os.path.basename(p))
w("")
with open(os.path.join(RES, "summary.md"), "w") as f:
    f.write("\n".join(md))
print("wrote %s and %d charts" % (os.path.join(RES, "summary.md"), len(charts)))

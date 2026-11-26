#!/usr/bin/env python3
"""Regenerates every table and chart of the over-the-air field report from field-v4.csv.

Standard library only. Run from anywhere:

    python3 field/analyze.py

Writes field/results.md (all tables) and field/charts/*.svg, and prints the tables.
The comparison with the simulation reads ../bench/results/summary.md, ../bench/results/bands.csv
and ../protocol-v4.json.
"""
import csv
import json
import math
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(HERE, "field-v4.csv")
CHARTS = os.path.join(HERE, "charts")

# Validity check: in a run where the noise source played, the receiver's in-band noise floor during the
# high-level noise scenarios rises clearly above the same run's quiet baseline. A run where no receiver
# rises by at least this much had an inaudible noise source (and therefore an inaudible rival too).
MIN_NOISE_RISE_DB = 3.0

CORRECT = {"not_delivered", "refused", "refused_multiple", "delivered_warned", "rogue_read"}
UNDUE = {"undue_delivery", "delivered_with_other_sender"}
INCONCLUSIVE = {"not_heard", "delivered_other_maybe_silent", "not_delivered_unattributed"}

CLASS_ORDER = ["medium", "hard", "ultra"]
LAYOUT_ORDER = ["s45-medium", "s45-hard", "side", "s45-very-hard", "between"]
SECURITY_ORDER = ["other-same-channel", "other-other-channel", "other-intermittent", "other-intermittent-bar",
                  "two-overlapping", "risk-signal", "rogue-full-request", "competing-0s", "competing-1.5s", "competing-4s"]


# ---------------------------------------------------------------- helpers

def f(v):
    return float(v) if v not in ("", None) else None


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def pct(k, n):
    return 100.0 * k / n if n else None


def wilson(k, n, z=1.96):
    if n == 0:
        return (None, None)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (100 * (c - h), 100 * (c + h))


def fmt(v, nd=0, unit=""):
    if v is None:
        return "–"
    s = f"{v:.{nd}f}"
    if s.startswith("-"):
        s = "−" + s[1:]
    return s + unit


def rate(k, n):
    if not n:
        return "–"
    lo, hi = wilson(k, n)
    return f"{k}/{n} ({fmt(pct(k, n))} %, 95 % CI {fmt(lo)}–{fmt(hi)})"


def rate_short(k, n):
    return f"{k}/{n} ({fmt(pct(k, n))} %)" if n else "–"


def nearest_rank(xs, p):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    return xs[max(0, math.ceil(p / 100 * len(xs)) - 1)]


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" if i == 0 else "---:" for i in range(len(head))) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def srange(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "–"
    return f"{fmt(med(xs))} ({fmt(min(xs))}–{fmt(max(xs))})"


# ---------------------------------------------------------------- load

def load():
    rows = list(csv.DictReader(open(DATA, encoding="utf-8")))
    for r in rows:
        for k in ("measured_snr_db", "noise_floor_db", "read_ms", "decode_ms", "receiver_warning_ms", "sender_detect_ms",
                  "effective_margin_db", "sender_to_mic_cm", "noise_to_mic_cm", "first_signal_ms"):
            r[k] = f(r[k])
        r["undue"] = r["undue_delivery"] == "true"
    return rows


def validity(rows):
    """Per run and receiver: median noise floor in high-level noise scenarios minus the quiet baseline."""
    checks = []
    excluded = set()
    for run in sorted({r["run_id"] for r in rows}):
        rr = [r for r in rows if r["run_id"] == run]
        rises = []
        for rx in sorted({r["receiver_device"] for r in rr}):
            base = med([r["noise_floor_db"] for r in rr if r["category"] == "baseline" and r["receiver_device"] == rx])
            loud = med([r["noise_floor_db"] for r in rr if r["category"] == "noise" and r["level"] == "high" and r["receiver_device"] == rx])
            rise = None if base is None or loud is None else loud - base
            rises.append(rise)
            checks.append((run, rx, base, loud, rise))
        known = [x for x in rises if x is not None]
        if known and max(known) < MIN_NOISE_RISE_DB:
            excluded.add(run)
    return checks, excluded


# ---------------------------------------------------------------- simulation side

def simulation():
    """In-band SNR90 of the simulated AWGN benchmark and its per-tone equivalent (what the receiver measures)."""
    proto = json.load(open(os.path.join(ROOT, "protocol-v4.json")))
    spacing = proto["spacingHz"]
    n = round(48000 / spacing)  # 1024-sample v4 symbol window at 48 kHz
    taper = 0.10  # Tukey taper of the v4 analysis window
    edge = taper * (n - 1) / 2
    w = [1.0 if min(i, n - 1 - i) >= edge else 0.5 - 0.5 * math.cos(math.pi * min(i, n - 1 - i) / edge) for i in range(n)]
    enbw_bins = n * sum(x * x for x in w) / sum(w) ** 2
    enbw_hz = enbw_bins * spacing
    full_scale_db = 10 * math.log10((sum(w) / 2) ** 2)
    bands = {}
    for b in csv.DictReader(open(os.path.join(ROOT, "bench", "results", "bands.csv"))):
        if b["rate"] == "48000":
            bands[b["config"]] = b
    summary = open(os.path.join(ROOT, "bench", "results", "summary.md"), encoding="utf-8").read()
    one_tx = summary.split("### 1 transmission", 1)[1].split("###", 1)[0]
    out = {}
    for ch in ("top", "mid"):
        for prof in ("normal", "robust"):
            cfg = f"bysound {ch}/{prof}"
            m = re.search(r"^\| " + re.escape(cfg) + r" \|(.*)$", one_tx, re.M)
            cells = [c.strip() for c in m.group(1).split("|")]
            full = float(cells[-5].replace("−", "-").replace("+", ""))
            b = bands[cfg]
            width = float(b["band_hi_hz"]) - float(b["band_lo_hz"])
            groups = proto["bands"][ch]["groups"]
            inband = full + float(b["offset_white_db"])
            conv = 10 * math.log10(width / (groups * enbw_hz))
            out[(ch, prof)] = {"full": full, "offset": float(b["offset_white_db"]), "inband": inband, "width": width,
                               "groups": groups, "conv": conv, "per_tone": inband + conv}
    return out, enbw_bins, enbw_hz, full_scale_db


# ---------------------------------------------------------------- charts (static SVG, transparent, house style)

FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"
STYLE = ("<style>.t{fill:#8b949e;font-size:13px}.s{fill:#8b949e;font-size:11px}"
         ".h{fill:#8b949e;font-size:15px;font-weight:600}.v{fill:#8b949e;font-size:12px;font-weight:700}</style>")
TEAL, AMBER, GRAY = "#14b8a6", "#d97706", "#8b949e"


def svg_open(w, h, title):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" font-family="{FONT}">',
            f"<title>{title}</title>", STYLE]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def chart_noise(stats, path):
    """stats: list of (label, k, n) sorted by rate."""
    w, x0, bw = 720, 250, 330
    row = 30
    h = 70 + row * len(stats) + 46
    o = svg_open(w, h, "Over-the-air read rate per noise scenario")
    o.append('<text class="h" x="20" y="28">Over the air: requests read per noise scenario</text>')
    o.append(f'<rect x="{x0}" y="40" width="10" height="10" rx="2" fill="{TEAL}"/><text class="s" x="{x0 + 16}" y="49">read before the request expired</text>')
    y = 70
    for label, k, n in stats:
        p = k / n if n else 0
        o.append(f'<text class="t" x="20" y="{y + 13}">{esc(label)}</text>')
        o.append(f'<rect x="{x0}" y="{y}" width="{bw}" height="18" rx="4" fill="{GRAY}" fill-opacity=".15"/>')
        if k:
            o.append(f'<rect x="{x0}" y="{y}" width="{max(bw * p, 4):.1f}" height="18" rx="4" fill="{TEAL}"><title>{esc(label)}: {k} of {n} read</title></rect>')
        o.append(f'<text class="v" x="{x0 + bw + 8}" y="{y + 13}">{round(100 * p)} %  <tspan class="s" font-weight="400">{k}/{n}</tspan></text>')
        y += row
    o.append(f'<text class="s" x="20" y="{y + 14}">Linear scale, 0–100 %. All layouts, levels and both directions pooled; one Galaxy A50 and one iPad, near-ultrasonic channels.</text>')
    o.append(f'<text class="s" x="20" y="{y + 30}">Noise played from a laptop placed closer to the listening microphone than the sender in most attempts (worst case).</text>')
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


def chart_snr(points, not_decoded, thresholds, path):
    """points: list of (snr, ok). thresholds: list of (value, label)."""
    w, x0, x1 = 720, 170, 690
    lo, hi = 0, 70
    sx = lambda v: x0 + (min(max(v, lo), hi) - lo) / (hi - lo) * (x1 - x0)
    h = 330
    o = svg_open(w, h, "Measured in-the-air SNR of every decoded request")
    o.append('<text class="h" x="20" y="28">Every decoded request, by the SNR the receiver measured</text>')
    o.append(f'<circle cx="256" cy="45" r="5" fill="{TEAL}"/><text class="s" x="266" y="49">read in time</text>')
    o.append(f'<circle cx="356" cy="45" r="5" fill="{AMBER}"/><text class="s" x="366" y="49">decoded after the request expired (20 s)</text>')
    lanes = {True: 112, False: 192}
    o.append(f'<text class="t" x="20" y="{lanes[True] + 4}">Read in time</text>')
    o.append(f'<text class="t" x="20" y="{lanes[False] + 4}">Decoded too late</text>')
    for v in range(lo, hi + 1, 10):
        x = sx(v)
        o.append(f'<line x1="{x:.1f}" y1="80" x2="{x:.1f}" y2="222" stroke="{GRAY}" stroke-opacity=".18"/>')
        o.append(f'<text class="s" x="{x:.1f}" y="240" text-anchor="middle">{v}</text>')
    o.append(f'<text class="s" x="{(x0 + x1) / 2:.0f}" y="258" text-anchor="middle">measured SNR, dB (preamble tone over that tone\'s noise floor)</text>')
    for i, (val, label) in enumerate(thresholds):
        x = sx(val)
        o.append(f'<line x1="{x:.1f}" y1="68" x2="{x:.1f}" y2="222" stroke="{GRAY}" stroke-width="1.5" stroke-dasharray="4 3"/>')
        o.append(f'<text class="s" x="{x - 4:.1f}" y="{74 + 12 * i}" text-anchor="end">{esc(label)}</text>')
    counts = {}
    for snr, ok in sorted(points):
        b = round(snr)
        k = counts.get((b, ok), 0)
        counts[(b, ok)] = k + 1
        dy = ((k % 9) - 4) * 6
        o.append(f'<circle cx="{sx(snr):.1f}" cy="{lanes[ok] + dy}" r="4" fill="{TEAL if ok else AMBER}" stroke-opacity="1"><title>{fmt(snr, 1)} dB, {"read" if ok else "decoded after expiry"}</title></circle>')
    o.append(f'<text class="s" x="20" y="284">Not shown: {not_decoded} attempts where nothing was decoded (the receiver reports no SNR without a decoded frame).</text>')
    o.append('<text class="s" x="20" y="300">Dashed: the simulated 90 % threshold converted to this per-tone measure (AWGN, see field/README.md). Linear scale.</text>')
    o.append('<text class="s" x="20" y="316">Noise and quiet scenarios, all layouts, both directions; one Galaxy A50 and one iPad.</text>')
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


def chart_security(stats, path):
    """stats: list of (label, correct, undue, inconclusive)."""
    w, x0, bw = 720, 345, 270
    row = 30
    nmax = max(c + u + i for _, c, u, i in stats)
    h = 70 + row * len(stats) + 50
    o = svg_open(w, h, "Outcomes of the security scenarios over the air")
    o.append('<defs><pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
             f'<rect width="6" height="6" fill="{GRAY}" fill-opacity=".25"/><line x1="0" y1="0" x2="0" y2="6" stroke="{GRAY}" stroke-width="2"/></pattern></defs>')
    o.append('<text class="h" x="20" y="28">Another sender in the room: what the receiver did</text>')
    o.append(f'<rect x="{x0}" y="40" width="10" height="10" rx="2" fill="{TEAL}"/><text class="s" x="{x0 + 16}" y="49">as intended</text>')
    o.append(f'<rect x="{x0 + 90}" y="40" width="10" height="10" rx="2" fill="{AMBER}"/><text class="s" x="{x0 + 106}" y="49">undue delivery</text>')
    o.append(f'<rect x="{x0 + 200}" y="40" width="10" height="10" rx="2" fill="url(#hatch)"/><text class="s" x="{x0 + 216}" y="49">inconclusive</text>')
    y = 70
    for label, c, u, i in stats:
        o.append(f'<text class="t" x="20" y="{y + 13}">{esc(label)}</text>')
        x = x0
        for val, fill, name in ((c, TEAL, "as intended"), (u, AMBER, "undue delivery"), (i, "url(#hatch)", "inconclusive")):
            if val:
                wd = bw * val / nmax
                o.append(f'<rect x="{x:.1f}" y="{y}" width="{max(wd - 2, 2):.1f}" height="18" rx="4" fill="{fill}"><title>{esc(label)}: {val} {name}</title></rect>')
                x += wd
        parts = [f"{c} ok"] + ([f"{u} undue"] if u else []) + ([f"{i} inconcl."] if i else [])
        o.append(f'<text class="v" x="{x + 8:.1f}" y="{y + 13}">{" · ".join(parts)}</text>')
        y += row
    o.append(f'<text class="s" x="20" y="{y + 14}">Bar length = number of attempts (linear). "As intended" = not delivered, refused, or delivered with a conflict warning within 3 s.</text>')
    o.append(f'<text class="s" x="20" y="{y + 30}">The rogue complete request is read by design: the receiver cannot tell it from a real one; confirmation and the backend must.</text>')
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


# ---------------------------------------------------------------- report

def main():
    rows = load()
    checks, excluded = validity(rows)
    valid = [r for r in rows if r["run_id"] not in excluded]
    sim, enbw_bins, enbw_hz, full_scale_db = simulation()
    md = []
    P = md.append

    P("# Field results (generated)\n")
    P("Generated by `field/analyze.py` from `field/field-v4.csv`. Do not edit by hand.\n")

    runs = sorted({r["run_id"] for r in rows})
    dates = sorted({r["date_utc"] for r in rows})
    P("## 0. Data set\n")
    P(table(["item", "value"], [
        ["v4 attempts in the file", len(rows)],
        ["runs", f"{len(runs)} ({runs[0]}–{runs[-1]})"],
        ["dates (UTC)", ", ".join(dates)],
        ["library builds", ", ".join(sorted({r['lib_version'] for r in rows}))],
        ["excluded runs (noise source inaudible)", ", ".join(sorted(excluded)) or "none"],
        ["attempts analysed", len(valid)],
        ["  noise / security / quiet baseline",
         " / ".join(str(sum(1 for r in valid if r["category"] == c)) for c in ("noise", "security", "baseline"))],
    ]))
    P("")

    P("### Validity check: did the noise source play?\n")
    P(f"Median receiver noise floor in the high-level noise scenarios minus the same run's quiet baseline, per receiver. "
      f"A run where no receiver rises by at least {MIN_NOISE_RISE_DB:.0f} dB is excluded.\n")
    P(table(["run", "receiver", "quiet floor (dB)", "noise floor, high level (dB)", "rise (dB)", "kept"],
            [[run, rx.split(" (")[0], fmt(b, 1), fmt(l, 1), fmt(rise, 1), "no" if run in excluded else "yes"]
             for run, rx, b, l, rise in checks]))
    P("")

    # ---------------- (a) noise
    noise = [r for r in valid if r["category"] == "noise"]
    base = [r for r in valid if r["category"] == "baseline"]
    ok = lambda rs: sum(1 for r in rs if r["outcome"] == "read")
    P("## 1. Noise scenarios\n")
    P(f"All noise attempts: {rate(ok(noise), len(noise))}. Quiet baseline: {rate(ok(base), len(base))}, "
      f"median SNR {fmt(med([r['measured_snr_db'] for r in base]), 1)} dB.\n")

    order = {}
    for r in noise:
        order.setdefault(r["scenario"], []).append(r)
    per_scen = []
    for name, rs in order.items():
        reads = [r for r in rs if r["outcome"] == "read"]
        per_scen.append((name, ok(rs), len(rs), med([r["measured_snr_db"] for r in reads]), med([r["read_ms"] for r in reads]),
                         sum(1 for r in rs if r["level"] == "high"), ok([r for r in rs if r["level"] == "high"])))
    per_scen.sort(key=lambda t: (-t[1] / t[2], -t[2]))
    P("### 1a. Per scenario\n")
    P(table(["scenario", "read / attempts", "high level only", "median SNR of reads (dB)", "median read time (s)"],
            [[n, rate_short(k, t), rate_short(kh, th), fmt(s, 1), fmt(ms / 1000 if ms else None, 1)]
             for n, k, t, s, ms, th, kh in per_scen]))
    P("")
    chart_noise([(n, k, t) for n, k, t, *_ in per_scen], os.path.join(CHARTS, "noise-by-scenario.svg"))

    P("### 1b. Per geometric difficulty class (noise scenarios)\n")
    P("Class from the effective geometric margin = 20·log10(noise→mic / sender→mic) − occlusion and off-axis penalties: "
      "easy ≥ +6 dB, medium 0…+6, hard −10…0, ultra < −10.\n")
    rows_c = []
    for c in CLASS_ORDER:
        rs = [r for r in noise if r["difficulty_class"] == c]
        if not rs:
            continue
        reads = [r for r in rs if r["outcome"] == "read"]
        hi = [r for r in rs if r["level"] == "high"]
        rows_c.append([c, rate(ok(rs), len(rs)), rate_short(ok(hi), len(hi)), srange([r["effective_margin_db"] for r in rs]),
                       fmt(med([r["measured_snr_db"] for r in reads]), 1), fmt(med([r["read_ms"] for r in reads]) / 1000, 1)])
    P(table(["class", "read / attempts", "high level only", "effective margin, median (range) dB", "median SNR of reads (dB)", "median read time (s)"], rows_c))
    P("")

    P("### 1c. Per layout\n")
    rows_l = []
    for lay in LAYOUT_ORDER:
        rs = [r for r in noise if r["layout"] == lay]
        if not rs:
            continue
        geo = []
        for d in ("A50->iPad", "iPad->A50"):
            g = next((r for r in rs if r["direction"] == d), None)
            if g:
                geo.append(f"{d}: {fmt(g['sender_to_mic_cm'])} / {fmt(g['noise_to_mic_cm'])} cm, {fmt(g['effective_margin_db'], 1)} dB ({g['difficulty_class']}, occlusion {g['occlusion']})")
        hi = [r for r in rs if r["level"] == "high"]
        vol = sorted({r["noise_source_volume_pct"] for r in rs if r["noise_source_volume_pct"]})
        rows_l.append([lay, "<br>".join(geo), rate_short(ok(rs), len(rs)), rate_short(ok(hi), len(hi)),
                       ", ".join(v + " %" for v in vol) + (" / not recorded" if any(not r["noise_source_volume_pct"] for r in rs) else "") if vol else "not recorded",
                       ", ".join(sorted({r['run_id'] for r in rs}))])
    P(table(["layout", "sender→mic / noise→mic, effective margin", "read / attempts", "high level only", "noise-source volume", "runs"], rows_l))
    P("")

    P("### 1d. Per noise level (gain applied to the noise recording)\n")
    P(table(["level", "gain", "read / attempts"],
            [[lv, g, rate_short(ok([r for r in noise if r["level"] == lv]), sum(1 for r in noise if r["level"] == lv))]
             for lv, g in (("low", "−18 dB"), ("medium", "−9 dB"), ("high", "0 dB"))]))
    P("")

    P("### 1e. Failures\n")
    fails = [r for r in noise if r["outcome"] != "read"]
    fo = {}
    for r in fails:
        fo[r["outcome"]] = fo.get(r["outcome"], 0) + 1
    late = [r for r in fails if r["outcome"] == "read_after_expiry"]
    P(table(["outcome", "attempts"], sorted(fo.items(), key=lambda t: -t[1])))
    P("")
    if late:
        P(f"Decoded after the 20 s request lifetime: read time {fmt(min(r['read_ms'] for r in late) / 1000, 1)}–"
          f"{fmt(max(r['read_ms'] for r in late) / 1000, 1)} s, measured SNR {fmt(min(r['measured_snr_db'] for r in late), 1)}–"
          f"{fmt(max(r['measured_snr_db'] for r in late), 1)} dB, channels {', '.join(sorted({r['channel'] for r in late}))}.\n")
    worst = [r for r in fails if r["outcome"] == "not_read"]
    ws = {}
    for r in worst:
        ws[r["scenario"]] = ws.get(r["scenario"], 0) + 1
    P("Nothing decoded, by scenario: " + ", ".join(f"{k} {v}" for k, v in sorted(ws.items(), key=lambda t: -t[1])) + ".\n")

    # ---------------- (b) SNR equivalence
    P("## 2. Success vs measured SNR, and the simulated threshold\n")
    dec = [r for r in noise + base if r["measured_snr_db"] is not None]
    nodec = [r for r in noise + base if r["measured_snr_db"] is None]
    bins = [("≥ 20 dB", 20, 1e9), ("10–20 dB", 10, 20), ("0–10 dB", 0, 10), ("< 0 dB", -1e9, 0)]
    rows_b = []
    for name, a, b in bins:
        rs = [r for r in dec if a <= r["measured_snr_db"] < b]
        rows_b.append([name, len(rs), rate_short(ok(rs), len(rs)) if rs else "–"])
    rows_b.append(["nothing decoded (no SNR)", len(nodec), rate_short(ok(nodec), len(nodec))])
    P("Noise and quiet attempts. The receiver reports SNR only for a decoded frame, so every row above the last one is conditional on a decode.\n")
    P(table(["measured SNR", "attempts", "read in time"], rows_b))
    P("")
    reads = [r for r in dec if r["outcome"] == "read"]
    rows_ch = []
    for ch in ("top", "mid"):
        rs = [r["measured_snr_db"] for r in reads if r["channel"] == ch]
        rows_ch.append([ch, len(rs), fmt(min(rs), 1), fmt(nearest_rank(rs, 10), 1), fmt(med(rs), 1)])
    P("Lowest measured SNR of successful reads (the field threshold is at or below these values):\n")
    P(table(["channel", "reads", "minimum (dB)", "10th percentile (dB)", "median (dB)"], rows_ch))
    P("")
    P(f"Conversion. The v4 analysis window is {round(48000 / 46.875)} samples (Tukey, 10 % taper) with an equivalent noise bandwidth of "
      f"{fmt(enbw_bins, 3)} bins = {fmt(enbw_hz, 1)} Hz. With K tones sounding at once spread over a band B, "
      f"per-tone SNR = in-band SNR + 10·log10(B / (K · ENBW)).\n")
    rows_s = []
    for (ch, prof), s in sim.items():
        field_min = next(x for x in rows_ch if x[0] == ch)[2]
        rows_s.append([f"{ch}/{prof}", fmt(s["full"]), f"+{s['offset']:.2f}", fmt(s["inband"], 1), f"{s['width']:.0f} Hz, K={s['groups']}",
                       f"+{s['conv']:.1f}", fmt(s["per_tone"], 1), field_min if prof == "normal" else "(profile not recorded)"])
    P(table(["simulated config (1 transmission, AWGN)", "SNR90 full-band (dB)", "white-noise offset (dB)", "SNR90 in-band (dB)",
             "band, tones", "per-tone conversion (dB)", "SNR90 as the receiver measures it (dB)", "field minimum of reads (dB)"], rows_s))
    P("")
    for ch in ("top", "mid"):
        rs = [r["measured_snr_db"] for r in reads if r["channel"] == ch]
        s = sim[(ch, "normal")]
        P(f"- {ch}: field minimum {fmt(min(rs), 1)} dB vs simulated {fmt(s['per_tone'], 1)} dB (normal) / "
          f"{fmt(sim[(ch, 'robust')]['per_tone'], 1)} dB (robust): difference {fmt(min(rs) - s['per_tone'], 1)} dB. "
          f"In in-band terms the weakest field read is {fmt(min(rs) - s['conv'], 1)} dB.")
    P("")
    P(f"Scale of `noise_floor_db`: 10·log10 of the mean per-tone noise energy, |X(k)|² on samples in [−1, 1]; "
      f"a full-scale sine exactly on a tone reads {fmt(full_scale_db, 1)} dB on the same scale at 48 kHz.\n")
    thr = [(sim[("top", "normal")]["per_tone"], f"simulated 90 %, top/normal ≈ {fmt(sim[('top', 'normal')]['per_tone'], 1)} dB")]
    chart_snr([(r["measured_snr_db"], r["outcome"] == "read") for r in dec], len(nodec), thr, os.path.join(CHARTS, "success-vs-snr.svg"))

    # ---------------- (c) security
    sec = [r for r in valid if r["category"] == "security"]
    P("## 3. Security scenarios\n")
    P("As intended = not delivered / refused / delivered with a conflict warning within 3 s / (rogue complete request) read as a well-formed request. "
      "Undue = delivered while another sender was active, without a warning within 3 s. Inconclusive = nothing heard, or the intermittent rival may have been silent.\n")
    rows_x, chart_x = [], []
    for sid in SECURITY_ORDER:
        rs = [r for r in sec if r["scenario_id"] == sid]
        if not rs:
            continue
        c = sum(1 for r in rs if r["outcome"] in CORRECT)
        u = sum(1 for r in rs if r["outcome"] in UNDUE)
        i = sum(1 for r in rs if r["outcome"] in INCONCLUSIVE)
        oc = {}
        for r in rs:
            oc[r["outcome"]] = oc.get(r["outcome"], 0) + 1
        rows_x.append([rs[0]["scenario"], len(rs), c, u, i, ", ".join(f"{k} {v}" for k, v in sorted(oc.items(), key=lambda t: -t[1])),
                       srange([r["receiver_warning_ms"] for r in rs if r["outcome"] in ("delivered_warned", "undue_delivery")]),
                       srange([r["sender_detect_ms"] for r in rs])])
        chart_x.append((rs[0]["scenario"], c, u, i))
    P(table(["scenario", "attempts", "as intended", "undue", "inconclusive", "outcomes", "receiver warning after delivery, ms median (range)",
             "sender detected the other, ms median (range)"], rows_x))
    P("")
    tot_u = sum(1 for r in sec if r["outcome"] in UNDUE)
    P(f"Undue deliveries: {tot_u} of {len(sec)} security attempts. Noise and quiet attempts with an undue delivery: "
      f"{sum(1 for r in noise + base if r['undue'])}.\n")
    coll = [r for r in sec if r["scenario_id"].startswith("competing")]
    rows_v = []
    for lib in sorted({r["lib_version"] for r in coll}):
        rs = [r for r in coll if r["lib_version"] == lib]
        rows_v.append([lib, len(rs), sum(1 for r in rs if r["outcome"] == "refused"), sum(1 for r in rs if r["outcome"] == "delivered_warned"),
                       sum(1 for r in rs if r["outcome"] == "undue_delivery"), sum(1 for r in rs if r["outcome"] == "not_delivered_unattributed"),
                       srange([r["receiver_warning_ms"] for r in rs if r["outcome"] in ("delivered_warned", "undue_delivery")])])
    P("Competing registered requests by library build:\n")
    P(table(["build", "attempts", "refused", "delivered + warned ≤ 3 s", "undue", "inconclusive", "warning ms, median (range)"], rows_v))
    P("")
    warned = [r for r in coll if r["outcome"] == "delivered_warned"]
    P(f"Delivered with warning: {sum(1 for r in warned if r['delivered_request'] == 'legitimate')} legitimate, "
      f"{sum(1 for r in warned if r['delivered_request'] == 'competing')} competing request. "
      f"Undue deliveries: {sum(1 for r in coll if r['outcome'] == 'undue_delivery' and r['delivered_request'] == 'competing')} of the competing request, "
      f"{sum(1 for r in coll if r['outcome'] == 'undue_delivery' and r['delivered_request'] == 'legitimate')} of the legitimate one.\n")
    other = [r for r in sec if r["scenario_id"] in ("other-same-channel", "other-other-channel")]
    P(f"Continuous other sender (same or other channel): sender aborted with OTHER_TRANSMISSION in "
      f"{sum(1 for r in other if r['sender_code'] == 'OTHER_TRANSMISSION')} of {len(other)}; the receiver decoded the other request "
      f"(SNR median {fmt(med([r['measured_snr_db'] for r in other]), 1)} dB) and its lookup returned NOT_FOUND in "
      f"{sum(1 for r in other if r['receiver_code'] == 'NOT_FOUND')}.\n")
    chart_security(chart_x, os.path.join(CHARTS, "security-outcomes.svg"))

    # ---------------- (d) channel and direction
    P("## 4. Channel and direction\n")
    rows_d = []
    for ch in ("top", "mid"):
        rs = [r for r in noise + base if r["channel"] == ch and r["outcome"] == "read"]
        rows_d.append([ch, len(rs), fmt(med([r["measured_snr_db"] for r in rs]), 1), fmt(med([r["read_ms"] for r in rs]) / 1000, 1),
                       fmt(med([r["decode_ms"] for r in rs]) / 1000, 2)])
    P("Successful reads (noise + quiet) by the channel they were decoded on:\n")
    P(table(["channel", "reads", "median SNR (dB)", "median read time (s)", "median decode time (s)"], rows_d))
    P("")
    rows_dir = []
    for d in ("A50->iPad", "iPad->A50"):
        rn = [r for r in noise if r["direction"] == d]
        rb = [r for r in base if r["direction"] == d]
        rows_dir.append([d.replace("->", " → "), rate_short(ok(rn), len(rn)),
                         " · ".join(f"{c} {rate_short(ok([r for r in rn if r['difficulty_class'] == c]), sum(1 for r in rn if r['difficulty_class'] == c))}" for c in CLASS_ORDER),
                         fmt(med([r["measured_snr_db"] for r in rn if r["outcome"] == "read"]), 1),
                         f"{fmt(med([r['measured_snr_db'] for r in rb]), 1)} ({ok(rb)}/{len(rb)})"])
    P(table(["direction", "noise: read / attempts", "by class (medium · hard · ultra)", "median SNR of noise reads (dB)", "quiet: median SNR (reads)"], rows_dir))
    P("")
    hard_noise = [r for r in noise if r["scenario_id"] in ("hair-dryer", "vacuum")]
    rows_hn = []
    for d in ("A50->iPad", "iPad->A50"):
        for sid in ("hair-dryer", "vacuum"):
            rs = [r for r in hard_noise if r["direction"] == d and r["scenario_id"] == sid]
            rows_hn.append([d.replace("->", " → "), sid, rate_short(ok(rs), len(rs))])
    P("Broadband noise (hair dryer, vacuum) by direction:\n")
    P(table(["direction", "scenario", "read / attempts"], rows_hn))
    P("")
    times = [r for r in noise + base if r["outcome"] == "read"]
    P(f"Timing of successful reads: read time (listening start → data) median {fmt(med([r['read_ms'] for r in times]) / 1000, 1)} s, "
      f"90th percentile {fmt(nearest_rank([r['read_ms'] for r in times], 90) / 1000, 1)} s; decode time (first preamble → frame) median "
      f"{fmt(med([r['decode_ms'] for r in times]) / 1000, 2)} s; first preamble detected median {fmt(med([r['first_signal_ms'] for r in times]) / 1000, 1)} s.\n")

    out = "\n".join(md) + "\n"
    open(os.path.join(HERE, "results.md"), "w", encoding="utf-8").write(out)
    print(out)


if __name__ == "__main__":
    os.makedirs(CHARTS, exist_ok=True)
    main()

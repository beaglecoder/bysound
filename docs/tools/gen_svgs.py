import json, math, os
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = ROOT + '/docs/assets'
spec = json.load(open(ROOT + '/protocol-v4.json'))
vec = json.load(open(ROOT + '/test-vectors/v4-frames.json'))['vectors']
SP = spec['spacingHz']
FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"
TXT = '#8b949e'
BLUE, TEAL, ORANGE, PURPLE, RED, GREEN, YELLOW = '#4c8dd6', '#26a69a', '#e8913a', '#a371f7', '#e5534b', '#3fb950', '#d29922'

def payload_vec(band, profile='normal', name='ejemplo'):
    return next(v for v in vec if v['band'] == band and v['mode'] == 'payload' and v['profile'] == profile and v['name'] == name)

def head(w, h, title, desc, extra_style=''):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-labelledby="t d">\n'
            f'<title id="t">{title}</title>\n<desc id="d">{desc}</desc>\n'
            f'<style>text{{font-family:{FONT};fill:{TXT}}} .b{{font-weight:600}} {extra_style}'
            '@media (prefers-reduced-motion: reduce){*{animation:none!important}}</style>\n')

def write(name, s):
    s += '</svg>\n'
    open(f'{OUT}/{name}', 'w').write(s)
    print(name, len(s.encode()))

# ---------------------------------------------------------------- spectrogram.svg
def spectrogram():
    v = payload_vec('mid')
    band = spec['bands']['mid']; G = band['groups']; sym_ms = band['symbolSeconds'] * 1000
    N = 36  # symbols shown: 8 preamble + 3 header + 25 body
    syms = v['symbols'][:N]
    hdr = math.ceil(8 / G)
    W, H = 820, 400
    x0, y0, pw = 96, 46, 660
    rows = 16 * G; rh = 5.5; ph = rows * rh
    cw = pw / N
    s = head(W, H, 'bysound v4 spectrogram', 'Waterfall of a real bysound v4 payload frame on the mid band: 8 preamble symbols, 3 header symbols, then data symbols; one tone per group per 32 ms symbol.')
    s += f'<defs><clipPath id="rv"><rect x="{x0}" y="{y0-2}" height="{ph+4}" width="0"><animate attributeName="width" values="0;{pw};{pw}" keyTimes="0;0.82;1" dur="8s" repeatCount="indefinite"/></rect></clipPath></defs>\n'
    # group backgrounds
    for g in range(G):
        y = y0 + (G - 1 - g) * 16 * rh
        s += f'<rect x="{x0}" y="{y:.1f}" width="{pw}" height="{16*rh:.1f}" fill="{TXT}" fill-opacity="{0.05 if g % 2 == 0 else 0.10}"/>\n'
        s += f'<text x="{x0+pw+8}" y="{y+8*rh+4:.1f}" font-size="11">group {g}</text>\n'
    # y labels at group edges
    for g in range(G + 1):
        y = y0 + (G - g) * 16 * rh
        f = (band['bin'] + 16 * g) * SP / 1000
        s += f'<line x1="{x0-4}" x2="{x0}" y1="{y:.1f}" y2="{y:.1f}" stroke="{TXT}"/><text x="{x0-8}" y="{y+4:.1f}" font-size="11" text-anchor="end">{f:.2f}</text>\n'
    s += f'<text x="22" y="{y0+ph/2:.1f}" font-size="11" transform="rotate(-90 22 {y0+ph/2:.1f})" text-anchor="middle">frequency (kHz)</text>\n'
    # x ticks
    for k in range(0, N + 1, 8):
        x = x0 + k * cw
        s += f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{y0+ph}" y2="{y0+ph+4}" stroke="{TXT}"/><text x="{x:.1f}" y="{y0+ph+16}" font-size="11" text-anchor="middle">{int(k*sym_ms)}</text>\n'
    s += f'<text x="{x0+pw/2}" y="{y0+ph+32}" font-size="11" text-anchor="middle">time (ms) · one column = one 32 ms symbol</text>\n'
    # cells
    s += '<g clip-path="url(#rv)">\n'
    for k, nibs in enumerate(syms):
        col = PURPLE if k < 8 else ORANGE if k < 8 + hdr else TEAL
        for g, n in enumerate(nibs):
            t = g * 16 + n
            y = y0 + (rows - 1 - t) * rh
            s += f'<rect x="{x0+k*cw+1:.1f}" y="{y:.1f}" width="{cw-2:.1f}" height="{rh:.1f}" rx="1.2" fill="{col}"/>'
        s += '\n'
    s += '</g>\n'
    # playhead
    s += (f'<line y1="{y0-4}" y2="{y0+ph+4}" stroke="{BLUE}" stroke-width="2" x1="{x0}" x2="{x0}">'
          f'<animate attributeName="x1" values="{x0};{x0+pw};{x0+pw}" keyTimes="0;0.82;1" dur="8s" repeatCount="indefinite"/>'
          f'<animate attributeName="x2" values="{x0};{x0+pw};{x0+pw}" keyTimes="0;0.82;1" dur="8s" repeatCount="indefinite"/></line>\n')
    # brackets
    yb = 24
    def bracket(a, b, label, col):
        xa, xb = x0 + a * cw + 1, x0 + b * cw - 1
        return (f'<path d="M{xa:.1f} {yb+8} V{yb+2} H{xb:.1f} V{yb+8}" fill="none" stroke="{col}" stroke-width="1.5"/>'
                f'<text x="{(xa+xb)/2:.1f}" y="{yb-4}" font-size="11.5" text-anchor="middle" class="b" style="fill:{col}">{label}</text>\n')
    s += bracket(0, 8, 'preamble · 8 symbols', PURPLE)
    s += bracket(8, 8 + hdr, 'header', ORANGE)
    s += bracket(8 + hdr, N, f'body · start of block 0 of {int(v["header"][2:4],16)} ({len(v["symbols"])} symbols in the whole frame)', TEAL)
    s += f'<text x="{x0}" y="{H-14}" font-size="11">Real symbols from test-vectors/v4-frames.json (payload, band mid, normal profile). Tone = (bin + 16·group + nibble) × 46.875 Hz.</text>\n'
    write('spectrogram.svg', s)

# ---------------------------------------------------------------- frame.svg
def frame():
    W, H = 820, 290
    s = head(W, H, 'bysound v4 frame anatomy', 'Frame anatomy: preamble, 4-byte header, Reed-Solomon protected data blocks, and the silent gap before the next repetition.')
    v = payload_vec('mid'); body = v['body']
    y, h = 70, 46
    segs = [('preamble', '8 symbols', PURPLE, 100), ('header', '4 bytes', ORANGE, 70)]
    x = 30
    s += '<defs><linearGradient id="sw" x1="0" x2="1"><stop offset="0" stop-color="#4c8dd6" stop-opacity="0"/><stop offset="0.5" stop-color="#4c8dd6" stop-opacity="0.35"/><stop offset="1" stop-color="#4c8dd6" stop-opacity="0"/></linearGradient>'
    s += f'<pattern id="hz" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><rect width="6" height="6" fill="{TXT}" fill-opacity="0.08"/><line x1="0" y1="0" x2="0" y2="6" stroke="{TXT}" stroke-opacity="0.45" stroke-width="2"/></pattern></defs>\n'
    def box(x, w, col, op=0.22):
        return f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{h}" rx="5" fill="{col}" fill-opacity="{op}" stroke="{col}" stroke-width="1.5"/>\n'
    for name, sub, col, w in segs:
        s += box(x, w, col)
        s += f'<text x="{x+w/2:.1f}" y="{y+20}" font-size="13" text-anchor="middle" class="b" style="fill:{col}">{name}</text><text x="{x+w/2:.1f}" y="{y+36}" font-size="11" text-anchor="middle">{sub}</text>\n'
        x += w + 4
    # blocks: index/total 2, data 48 (last: 20), parity 12
    blocks = [(48, 'block 0'), (48, 'block 1'), (20, 'block 2 (last)')]
    scale = 2.85
    bx0 = x
    for i, (dlen, name) in enumerate(blocks):
        wi, wd, wp = 2 * scale + 4, dlen * scale, 12 * scale
        s += box(x, wi, YELLOW, 0.5)
        crc = 4 if i == len(blocks) - 1 else 0
        s += box(x + wi, wd - crc * scale, TEAL)
        if crc:
            s += box(x + wi + wd - crc * scale, crc * scale, GREEN, 0.55)
        s += box(x + wi + wd, wp, BLUE, 0.45)
        s += f'<text x="{x+(wi+wd+wp)/2:.1f}" y="{y+h+16}" font-size="11.5" text-anchor="middle">{name}</text>\n'
        x += wi + wd + wp + 4
    s += f'<path d="M{bx0:.1f} {y-10} V{y-16} H{x-4:.1f} V{y-10}" fill="none" stroke="{TEAL}" stroke-width="1.5"/><text x="{(bx0+x)/2:.1f}" y="{y-22}" font-size="12.5" text-anchor="middle" class="b" style="fill:{TEAL}">body · {len(body)//2} bytes in this example</text>\n'
    gw = 790 - x
    s += f'<rect x="{x:.1f}" y="{y}" width="{gw:.1f}" height="{h}" rx="5" fill="url(#hz)" stroke="{TXT}" stroke-dasharray="4 3"/>'
    s += f'<text x="{x+gw/2:.1f}" y="{y+20}" font-size="13" text-anchor="middle" class="b">gap</text><text x="{x+gw/2:.1f}" y="{y+36}" font-size="11" text-anchor="middle">350 ms</text>\n'
    s += f'<text x="30" y="{y-22}" font-size="12.5" class="b">one repetition (“lap”)</text>\n'
    # sweep highlight
    s += (f'<rect y="{y-6}" height="{h+12}" width="90" fill="url(#sw)" x="-90">'
          f'<animate attributeName="x" values="-90;800" dur="4.5s" repeatCount="indefinite"/></rect>\n')
    # legend
    ly = 172
    items = [(YELLOW, 0.5, 'block index, block count (2 B)'), (TEAL, 0.22, 'packed text (≤ 48 B per block)'), (GREEN, 0.55, 'CRC-32 (4 B, end of data)'), (BLUE, 0.45, 'Reed–Solomon parity (12 B)')]
    for (col, op, label), lx in zip(items, [30, 236, 444, 622]):
        s += f'<rect x="{lx}" y="{ly}" width="14" height="14" rx="3" fill="{col}" fill-opacity="{op}" stroke="{col}"/><text x="{lx+20}" y="{ly+11.5}" font-size="12">{label}</text>\n'
    # header detail
    hy = 212
    s += f'<text x="30" y="{hy}" font-size="12.5" class="b" style="fill:{ORANGE}">header</text>'
    hx = 90
    for lab in ['magic 0x56', 'blocks (1–255)', 'CRC-8 (0x2F)', 'version · channel · set']:
        w = len(lab) * 6.6 + 18
        s += f'<rect x="{hx}" y="{hy-14}" width="{w:.1f}" height="20" rx="4" fill="{ORANGE}" fill-opacity="0.15" stroke="{ORANGE}"/><text x="{hx+w/2:.1f}" y="{hy}" font-size="11.5" text-anchor="middle">{lab}</text>'
        hx += w + 6
    s += '\n'
    s += f'<text x="30" y="{hy+30}" font-size="12.5" class="b" style="fill:{TEAL}">block</text><text x="90" y="{hy+30}" font-size="12">RS(62,50) over GF(256): 2 + 48 + 12 = 62 bytes; the last block is shorter (2 + n + 12).</text>\n'
    s += f'<text x="30" y="{hy+56}" font-size="11">Not to scale. Example: test vector “ejemplo”, payload, band mid — 8 + 3 + 106 symbols × 32 ms = 3.744 s, then 0.35 s of silence.</text>\n'
    write('frame.svg', s)

# ---------------------------------------------------------------- pipeline.svg
def pipeline():
    W, H = 960, 372
    s = head(W, H, 'bysound v4 pipeline', 'Encoder pipeline from text to tones, sound through the air, and decoder pipeline from microphone to text.',
             '@keyframes flow{to{stroke-dashoffset:-24}} .fl{stroke-dasharray:6 6;animation:flow 0.8s linear infinite} '
             '@keyframes air{to{stroke-dashoffset:40}} .air{stroke-dasharray:10 10;animation:air 1s linear infinite} ')
    s += f'<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0L10 5L0 10z" fill="{TXT}"/></marker></defs>\n'
    def row(items, y, x0, bw, gap, cols, label):
        right = label.startswith('Decoder')
        lx = x0 + len(items) * (bw + gap) - gap if right else x0
        out = f'<text x="{lx}" y="{y-14}" font-size="13" class="b" text-anchor="{'end' if right else 'start'}">{label}</text>\n'
        xs = []
        for i, ((t, sub), col) in enumerate(zip(items, cols)):
            x = x0 + i * (bw + gap); xs.append(x)
            out += (f'<rect x="{x}" y="{y}" width="{bw}" height="62" rx="9" fill="{col}" fill-opacity="0.13" stroke="{col}" stroke-width="1.6">'
                    f'<animate attributeName="fill-opacity" values="0.13;0.13;0.4;0.13;0.13" keyTimes="0;{i/len(items)*0.9:.3f};{(i+0.5)/len(items)*0.9:.3f};{(i+1)/len(items)*0.9:.3f};1" dur="5s" repeatCount="indefinite"/></rect>'
                    f'<text x="{x+bw/2}" y="{y+26}" font-size="13" text-anchor="middle" class="b" style="fill:{col}">{t}</text>'
                    f'<text x="{x+bw/2}" y="{y+45}" font-size="10" text-anchor="middle">{sub}</text>\n')
            if i:
                out += f'<line x1="{x-gap+2}" y1="{y+31}" x2="{x-2}" y2="{y+31}" stroke="{TXT}" stroke-width="1.6" marker-end="url(#ar)"/>\n'
        return out, xs
    enc = [('Text', 'UTF-8 string'), ('Pack', 'digit pairs → 1 byte'), ('Blocks', '48 B + CRC-32'), ('Reed–Solomon', '+12 parity / block'), ('Nibbles', '4 bits per tone'), ('Tones', 'on a 46.875 Hz grid')]
    dec = [('Microphone', 'PCM 44.1 / 48 kHz'), ('Windows', '21.3 ms, hop 4 ms'), ('Preamble', 'match ≥ 0.32'), ('Header', 'best of 259 valid'), ('Soft symbols', 'whitened, de-echoed'), ('RS + erasures', 'least-sure bytes'), ('Combine laps', 'up to 6 repeats'), ('Text', 'CRC-32 verified')]
    ecol = [BLUE, BLUE, TEAL, TEAL, PURPLE, PURPLE]
    dcol = [ORANGE, ORANGE, PURPLE, PURPLE, TEAL, TEAL, BLUE, GREEN]
    a, ex = row(enc, 52, 22, 136, 20, ecol, 'Encoder (transmitter)')
    s += a
    b, dx = row(dec, 262, 14, 106, 12, dcol, 'Decoder (receiver)')
    s += b
    # air path: from Tones box bottom to Microphone box top
    tx = ex[-1] + 68; mx = dx[0] + 53
    path = f'M{tx} 114 C{tx} 180 {tx-40} 182 {tx-90} 182 L{mx+90} 182 C{mx+40} 182 {mx} 184 {mx} 258'
    s += f'<path d="{path}" fill="none" stroke="{TXT}" stroke-opacity="0.35" stroke-width="2"/>\n'
    s += f'<path d="{path}" fill="none" stroke="{BLUE}" stroke-width="2.4" class="air" style="stroke-dashoffset:0"/>\n'
    # moving tone dots along air path
    for i, col in enumerate([PURPLE, TEAL, ORANGE]):
        s += f'<circle r="5" fill="{col}"><animateMotion path="{path}" dur="3s" begin="{i}s" repeatCount="indefinite"/></circle>\n'
    s += f'<text x="{(tx+mx)/2}" y="174" font-size="12" text-anchor="middle">sound through the air · each frame repeats, gap 350 ms</text>\n'
    # flow overlays on arrows
    write('pipeline.svg', s)

# ---------------------------------------------------------------- bands.svg
def bands():
    W, H = 820, 348
    s = head(W, H, 'bysound v4 bands', 'The seven bysound v4 bands on a 0 to 22 kHz axis; six sit between 15 and 19.5 kHz, where human hearing fades, and one is audible.')
    x0, x1, fmax = 40, 790, 22000
    X = lambda f: x0 + (x1 - x0) * f / fmax
    ytop, rh = 66, 30
    order = ['audible', 'mid', 'upper', 'high', 'top', 'xhigh', 'wide']
    plot_h = rh * len(order) + 8
    s += f'<defs><linearGradient id="hear" x1="0" x2="1"><stop offset="0" stop-color="{ORANGE}" stop-opacity="0.30"/><stop offset="{14000/fmax:.4f}" stop-color="{ORANGE}" stop-opacity="0.30"/><stop offset="{20000/fmax:.4f}" stop-color="{ORANGE}" stop-opacity="0"/></linearGradient></defs>\n'
    s += f'<rect x="{x0}" y="34" width="{x1-x0}" height="12" rx="3" fill="url(#hear)"/>'
    s += f'<text x="{x0}" y="26" font-size="12" class="b" style="fill:{ORANGE}">human hearing</text><text x="{X(16000)-6:.1f}" y="26" font-size="11.5" text-anchor="end">fades with frequency and age</text>\n'
    s += f'<line x1="{X(16000):.1f}" x2="{X(16000):.1f}" y1="30" y2="{ytop+plot_h}" stroke="{ORANGE}" stroke-dasharray="3 4" stroke-opacity="0.8"/><text x="{X(16000)+4:.1f}" y="26" font-size="11.5" style="fill:{ORANGE}">~16 kHz</text>\n'
    # axis
    ya = ytop + plot_h + 6
    s += f'<line x1="{x0}" x2="{x1}" y1="{ya}" y2="{ya}" stroke="{TXT}"/>\n'
    for k in range(0, 23, 2):
        s += f'<line x1="{X(k*1000):.1f}" x2="{X(k*1000):.1f}" y1="{ya}" y2="{ya+5}" stroke="{TXT}"/><text x="{X(k*1000):.1f}" y="{ya+18}" font-size="11" text-anchor="middle">{k}</text>\n'
    s += f'<text x="{(x0+x1)/2}" y="{ya+36}" font-size="11.5" text-anchor="middle">frequency (kHz) · grid step 46.875 Hz · moving marks are real symbols, slowed down 8×</text>\n'
    cols = {'audible': ORANGE, 'mid': PURPLE, 'upper': BLUE, 'high': TEAL, 'top': GREEN, 'xhigh': RED, 'wide': YELLOW}
    SLOW = 8
    for i, bid in enumerate(order):
        b = spec['bands'][bid]; G = b['groups']; col = cols[bid]
        y = ytop + i * rh
        fa, fb = b['bin'] * SP, (b['bin'] + 16 * G) * SP
        xa, xb = X(fa), X(fb)
        s += f'<rect x="{xa:.2f}" y="{y}" width="{xb-xa:.2f}" height="18" rx="3" fill="{col}" fill-opacity="0.22" stroke="{col}" stroke-width="1.2"/>\n'
        for g in range(1, G):
            xg = X((b['bin'] + 16 * g) * SP)
            s += f'<line x1="{xg:.2f}" x2="{xg:.2f}" y1="{y+2}" y2="{y+16}" stroke="{col}" stroke-opacity="0.6"/>'
        rate = 4 * G / b['symbolSeconds']
        info = f"{G} tones · {b['symbolSeconds']*1000:g} ms · {rate:g} bit/s"
        if bid == 'audible':
            s += f'<text x="{xb+8:.1f}" y="{y+13}" font-size="12"><tspan class="b" style="fill:{col}">{bid}</tspan>  {fa/1000:.3f}–{fb/1000:.3f} kHz · {info}</text>\n'
        else:
            s += f'<text x="{X(15000)-10:.1f}" y="{y+13}" font-size="12" text-anchor="end"><tspan class="b" style="fill:{col}">{bid}</tspan>  {fa/1000:.2f}–{fb/1000:.2f} kHz · {info}</text>\n'
        # animated tone markers from a real payload vector
        v = payload_vec(bid)
        seq = v['symbols'][:48]
        dur = len(seq) * b['symbolSeconds'] * SLOW
        for g in range(G):
            vals = ';'.join(f"{X((b['bin'] + 16*g + nib[g]) * SP) - 1.5:.1f}" for nib in seq)
            s += f'<rect y="{y-3}" width="3" height="24" rx="1" fill="{col}" x="{X((b["bin"]+16*g+seq[0][g])*SP)-1.5:.1f}"><animate attributeName="x" values="{vals}" dur="{dur:.3f}s" calcMode="discrete" repeatCount="indefinite"/></rect>'
        s += '\n'
    write('bands.svg', s)

# ---------------------------------------------------------------- hero.svg
def hero():
    W, H = 820, 260
    s = head(W, H, 'Transfer by sound', 'A phone sends data as sound waves to another phone and to an ESP32 board.')
    # sender phone
    def phone(x, y, w, h, scr):
        return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{w*0.17:.1f}" fill="{TXT}" fill-opacity="0.08" stroke="{TXT}" stroke-width="2.2"/>'
                f'<rect x="{x+7}" y="{y+18}" width="{w-14}" height="{h-36}" rx="6" fill="{scr}" fill-opacity="0.16"/>'
                f'<rect x="{x+w/2-12:.1f}" y="{y+7}" width="24" height="4" rx="2" fill="{TXT}" fill-opacity="0.6"/>'
                f'<circle cx="{x+w/2:.1f}" cy="{y+h-9}" r="4" fill="none" stroke="{TXT}" stroke-opacity="0.7"/>\n')
    s += phone(70, 40, 100, 180, BLUE)
    # text lines on sender screen
    for i, wl in enumerate([62, 48, 70, 40]):
        s += f'<rect x="86" y="{78+i*16}" width="{wl}" height="7" rx="3.5" fill="{BLUE}" fill-opacity="0.65"/>'
    s += f'<text x="120" y="190" font-size="11" text-anchor="middle" class="b" style="fill:{BLUE}">sending…</text>\n'
    # waves: expanding wavefronts from the speaker, clipped to the right half
    cx, cy = 172, 130
    s += f'<defs><clipPath id="rh"><rect x="{cx}" y="0" width="{W-cx}" height="{H}"/></clipPath></defs><g clip-path="url(#rh)">\n'
    wcols = [TEAL, BLUE, PURPLE, TEAL]
    for i in range(4):
        s += (f'<circle cx="{cx}" cy="{cy}" r="20" fill="none" stroke="{wcols[i]}" stroke-width="4" opacity="0">'
              f'<animate attributeName="r" values="20;470" dur="3.2s" begin="{i*0.8}s" repeatCount="indefinite"/>'
              f'<animate attributeName="opacity" values="0;0.95;0.7;0" keyTimes="0;0.1;0.75;1" dur="3.2s" begin="{i*0.8}s" repeatCount="indefinite"/></circle>\n')
    s += '</g>\n'
    # receiver phone
    s += phone(560, 30, 80, 150, TEAL)
    s += (f'<path d="M583 104 l12 12 l22 -26" fill="none" stroke="{GREEN}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" stroke-dasharray="60" stroke-dashoffset="60">'
          f'<animate attributeName="stroke-dashoffset" values="60;60;0;0;60" keyTimes="0;0.55;0.68;0.95;1" dur="3.2s" repeatCount="indefinite"/></path>\n')
    # ESP32 board
    bx, by = 668, 128
    s += f'<rect x="{bx}" y="{by}" width="118" height="80" rx="6" fill="{TXT}" fill-opacity="0.08" stroke="{TXT}" stroke-width="2.2"/>'
    for i in range(9):
        s += f'<circle cx="{bx+12+i*12}" cy="{by+8}" r="2.2" fill="{TXT}" fill-opacity="0.7"/><circle cx="{bx+12+i*12}" cy="{by+72}" r="2.2" fill="{TXT}" fill-opacity="0.7"/>'
    s += f'<rect x="{bx+18}" y="{by+20}" width="48" height="40" rx="3" fill="{TXT}" fill-opacity="0.25" stroke="{TXT}"/><text x="{bx+42}" y="{by+44}" font-size="10" text-anchor="middle" class="b">ESP32</text>'
    s += f'<path d="M{bx+78} {by+26} h20 v8 h-14 v8 h14 v8 h-14 v8 h14" fill="none" stroke="{TXT}" stroke-width="2"/>'
    s += (f'<circle cx="{bx+104}" cy="{by+22}" r="5" fill="{GREEN}" opacity="0.2">'
          f'<animate attributeName="opacity" values="0.2;0.2;1;1;0.2" keyTimes="0;0.6;0.66;0.95;1" dur="3.2s" repeatCount="indefinite"/></circle>\n')
    s += f'<circle cx="{bx+30}" cy="{by+66}" r="0" fill="none"/>'
    # mic dots
    s += f'<circle cx="600" cy="38" r="2" fill="{TXT}"/><circle cx="{bx+104}" cy="{by+42}" r="4" fill="none" stroke="{TXT}" stroke-width="1.5"/>\n'
    write('hero.svg', s)

spectrogram(); frame(); pipeline(); bands(); hero()

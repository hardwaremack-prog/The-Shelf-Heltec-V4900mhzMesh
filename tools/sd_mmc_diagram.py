# In-line 4-bit SD/MMC card reader on a Heltec V4.3, for the Heltec V4 MMC build (-DSHELF_MMC)
# Reader pins (printed order): 3.3V GND D0 CMD CLK D3 D2 D1 CD. Laid face down on J3, it lines up as below.
J3 = ['GND', '3V3', '3V3', '37', '46', '45', '42', '41', '40', '39', '38', '1', '2', '3', '4', '5', '6', '7']
W, H = 1200, 820
bx, by, bw, bh = 170, 520, 860, 220
px0 = bx + 70; step = (bw - 140) / 17
px = lambda i: px0 + i * step
topy = by + 28
#        label   J3 index  colour     kind
MOD = [('3.3V', 4, '#ff5a5a', 'jump'), ('GND', 5, '#9aa4ae', 'jump'), ('D0', 6, '#ffa53a', 'pin'),
       ('CMD', 7, '#4cd964', 'pin'), ('CLK', 8, '#3aa0ff', 'pin'), ('D3', 9, '#c77dff', 'pin'),
       ('D2', 10, '#ff7ac8', 'pin'), ('D1', 11, '#ffd23a', 'jump'), ('CD', 12, '#6b7d90', 'cut')]
DEST = {'3.3V': 2, 'GND': 0, 'D1': 14}
my = 250
mx0, mx1 = px(4) - 40, px(12) + 40
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Segoe UI,Roboto,Helvetica,Arial,sans-serif">',
     f'<rect width="{W}" height="{H}" fill="#0b0f14"/>',
     '<text x="40" y="44" fill="#fff" font-size="28" font-weight="700">4-bit SD/MMC reader in line on a Heltec V4.3 → The Shelf, Heltec V4 MMC</text>',
     '<text x="40" y="74" fill="#7f93a8" font-size="16">Reader face down. D0 · CMD · CLK · D3 · D2 drop straight into GPIO 42 · 41 · 40 · 39 · 38. Snip 3.3V, GND, D1 and CD; 3 short wires.</text>']
o.append(f'<rect x="{mx0}" y="{my-120}" width="{mx1-mx0}" height="120" rx="12" fill="#1f7a3a" stroke="#3fae62" stroke-width="3"/>')
o.append(f'<text x="{(mx0+mx1)/2}" y="{my-70}" fill="#e8f5ec" font-size="17" font-weight="700" text-anchor="middle">SDIO card reader (face down)</text>')
for lab, i, col, k in MOD:
    o.append(f'<text x="{px(i)}" y="{my-12}" fill="#fff" font-size="14" font-weight="700" text-anchor="middle">{lab}</text>')
o.append(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="18" fill="#141a22" stroke="#3a4a5c" stroke-width="3"/>')
o.append(f'<rect x="{bx-34}" y="{by+bh/2-38}" width="46" height="76" rx="10" fill="#9aa4ae" stroke="#5f6b78" stroke-width="3"/>')
o.append(f'<text x="{bx-11}" y="{by+bh/2+60}" fill="#7f93a8" font-size="14" text-anchor="middle">USB-C</text>')
o.append(f'<text x="{bx+bw/2}" y="{by+150}" fill="#7f93a8" font-size="15" text-anchor="middle">Heltec WiFi LoRa 32 V4.3, right-hand header (J3)</text>')
straight = {i: c for _, i, c, k in MOD if k == 'pin'}
ends = {DEST[l]: c for l, i, c, k in MOD if l in DEST}
for i, n in enumerate(J3):
    x = px(i); col = straight.get(i) or ends.get(i)
    o.append(f'<circle cx="{x}" cy="{topy}" r="{11 if col else 8}" fill="{col or "#0b0f14"}" stroke="{col or "#6b7d90"}" stroke-width="3"/>')
    o.append(f'<text x="{x}" y="{topy+32}" fill="{"#fff" if col else "#6b7d90"}" font-size="{14 if col else 12}" font-weight="{700 if col else 400}" text-anchor="middle">{n}</text>')
for lab, i, col, k in MOD:
    x = px(i)
    if k == 'pin':
        o.append(f'<rect x="{x-4}" y="{my}" width="8" height="{topy-my-12}" fill="#d4af37"/>')
        o.append(f'<line x1="{x}" y1="{my+30}" x2="{x}" y2="{topy-30}" stroke="{col}" stroke-width="4" stroke-dasharray="2 8" stroke-linecap="round"/>')
    else:   # snipped: a short stub, and its hole stays empty
        o.append(f'<rect x="{x-4}" y="{my}" width="8" height="16" fill="#d4af37"/>')
        o.append(f'<circle cx="{x}" cy="{topy}" r="15" fill="none" stroke="#ff6b6b" stroke-width="3"/>')
        o.append(f'<line x1="{x-10}" y1="{topy-10}" x2="{x+10}" y2="{topy+10}" stroke="#ff6b6b" stroke-width="3"/><line x1="{x+10}" y1="{topy-10}" x2="{x-10}" y2="{topy+10}" stroke="#ff6b6b" stroke-width="3"/>')
# the 3 wires
lv = {'GND': topy - 150, '3.3V': topy - 95, 'D1': topy - 120}
for lab, i, col, k in MOD:
    if lab not in DEST: continue
    x, d, y = px(i), px(DEST[lab]), lv[lab]
    o.append(f'<path d="M{x},{my+16} L{x},{y} L{d},{y} L{d},{topy-12}" fill="none" stroke="{col}" stroke-width="6" stroke-linecap="round" stroke-linejoin="round"/>')
o.append(f'<text x="{px(1)}" y="{lv["GND"]-12}" fill="#c3cad2" font-size="14" font-weight="600" text-anchor="middle">GND wire</text>')
o.append(f'<text x="{px(1)}" y="{lv["3.3V"]+24}" fill="#ff8f8f" font-size="14" font-weight="600" text-anchor="middle">3.3V wire</text>')
o.append(f'<text x="{px(14)}" y="{lv["D1"]-12}" fill="#ffe27a" font-size="14" font-weight="600" text-anchor="middle">D1 wire → GPIO 4</text>')
o.append(f'<text x="{px(12)+30}" y="{my+40}" fill="#8fa1b3" font-size="13">CD: snip, not used</text>')
notes = ['Leave holes 46, 45, 1 and 2 empty: boot straps (46, 45), battery sense (1) and the LoRa amplifier (2).',
         'Bend the five pins 90° so the reader lies flat, beside the board or under it (tape between them).',
         'GPIO 38-42 are the GNSS connector pins, so leave the GPS kit unplugged. Go by the pin names printed on the board.']
for n, t in enumerate(notes):
    o.append(f'<text x="40" y="{H-66+n*22}" fill="#7f93a8" font-size="15">{t}</text>')
o.append('</svg>')
import sys
open((sys.argv[1] if len(sys.argv) > 1 else '.') + '/sd-mmc-wiring-shelf-v4.svg', 'w').write('\n'.join(o))

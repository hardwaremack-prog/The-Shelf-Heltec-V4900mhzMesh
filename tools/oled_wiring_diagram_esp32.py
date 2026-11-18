# Wiring diagram: a 0.96" I2C OLED screen (SSD1306, pins GND VCC SCL SDA) on a 38-pin ESP32 dev
# board (NodeMCU-32S / DevKitC), drawn like sd_wiring_diagram_esp32.py: antenna end on the left,
# USB on the right, the GND · 23 · 22 header along the top, 3V3 · EN along the bottom.
import sys
TOP = ['GND', '23', '22', 'TX', 'RX', '21', 'GND', '19', '18', '5', '17', '16', '4', '0', '2', '15', 'D1', 'D0', 'CLK']
BOT = ['3V3', 'EN', 'VP', 'VN', '34', '35', '32', '33', '25', '26', '27', '14', '12', 'GND', '13', 'D2', 'D3', 'CMD', '5V']

def svg():
    W, H = 1200, 990
    bx, by, bw, bh = 170, 500, 860, 300
    px0 = bx + 70; pstep = (bw - 140) / 18
    pinx = lambda i: px0 + i * pstep
    topy = by + 28; boty = by + bh - 28
    # the screen module, pins along its top edge (as printed: GND VCC SCL SDA)
    sx, sy, sw, sh = 250, 165, 330, 250
    sd = [('GND', 'GND', '#9aa4ae', 'top', 0), ('VCC', '3V3', '#ff5a5a', 'bot', 0),
          ('SCL', '22', '#3aa0ff', 'top', 2), ('SDA', '21', '#4cd964', 'top', 5)]
    spx = lambda k: sx + 95 + k * 48
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Segoe UI,Roboto,Helvetica,Arial,sans-serif">',
         f'<rect width="{W}" height="{H}" fill="#0b0f14"/>',
         '<text x="40" y="44" fill="#fff" font-size="28" font-weight="700">0.96" OLED screen → The Shelf on an ESP32 (NodeMCU / DevKitC)</text>',
         '<text x="40" y="74" fill="#7f93a8" font-size="16">Four wires: GND, VCC to 3V3, SCL to GPIO 22, SDA to GPIO 21. The Shelf finds the screen by itself on the next start.</text>']
    # screen
    o.append(f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" rx="12" fill="#1d4f8f" stroke="#3a78c2" stroke-width="3"/>')
    o.append(f'<rect x="{sx+30}" y="{sy+70}" width="{sw-60}" height="150" rx="4" fill="#05070a" stroke="#2a2f36" stroke-width="3"/>')
    o.append(f'<text x="{sx+sw/2}" y="{sy+140}" fill="#7fd6ff" font-size="17" font-family="Consolas,monospace" text-anchor="middle">THE SHELF ESP32  SD</text>')
    o.append(f'<text x="{sx+sw/2}" y="{sy+170}" fill="#7fd6ff" font-size="15" font-family="Consolas,monospace" text-anchor="middle">http://10.0.0.126</text>')
    o.append(f'<text x="{sx+sw/2}" y="{sy+sh+24}" fill="#a9c7ee" font-size="14" text-anchor="middle">0.96" I2C OLED (SSD1306, 128×64)</text>')
    for k, (lab, _, col, _, _) in enumerate(sd):
        x = spx(k)
        o.append(f'<circle cx="{x}" cy="{sy+24}" r="10" fill="#d4af37" stroke="#8a6d1d" stroke-width="2"/>')
        o.append(f'<text x="{x}" y="{sy+56}" fill="#fff" font-size="14" font-weight="700" text-anchor="middle">{lab}</text>')
    # board
    o.append(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="12" fill="#141a22" stroke="#3a4a5c" stroke-width="3"/>')
    o.append(f'<rect x="{bx+bw-12}" y="{by+bh/2-32}" width="46" height="64" rx="8" fill="#9aa4ae" stroke="#5f6b78" stroke-width="3"/>')
    o.append(f'<text x="{bx+bw+11}" y="{by+bh/2+56}" fill="#7f93a8" font-size="14" text-anchor="middle">USB</text>')
    o.append(f'<rect x="{bx+90}" y="{by+70}" width="330" height="160" rx="6" fill="#b9c0c8" stroke="#7d8793" stroke-width="3"/>')
    o.append(f'<path d="M{bx+100},{by+90} h50 v40 h-30 v40 h30 v40 h-50" fill="none" stroke="#d4af37" stroke-width="5"/>')
    o.append(f'<text x="{bx+290}" y="{by+148}" fill="#2a2f36" font-size="20" font-weight="700" text-anchor="middle">ESP32-WROOM-32</text>')
    o.append(f'<text x="{bx+290}" y="{by+176}" fill="#4b5563" font-size="14" text-anchor="middle">antenna end</text>')
    o.append(f'<rect x="{bx+560}" y="{by+120}" width="40" height="40" rx="6" fill="#2b3a4b"/><text x="{bx+580}" y="{by+190}" fill="#7f93a8" font-size="13" text-anchor="middle">BOOT (IO0)</text>')
    o.append(f'<rect x="{bx+680}" y="{by+120}" width="40" height="40" rx="6" fill="#2b3a4b"/><text x="{bx+700}" y="{by+190}" fill="#7f93a8" font-size="13" text-anchor="middle">EN</text>')
    used = {(c[3], c[4]): c for c in sd}
    for row, names, y, ty in (('top', TOP, topy, topy + 32), ('bot', BOT, boty, boty - 20)):
        for i, name in enumerate(names):
            x = pinx(i); hit = (row, i) in used
            col = used[(row, i)][2] if hit else '#3a4a5c'
            o.append(f'<circle cx="{x}" cy="{y}" r="{11 if hit else 8}" fill="{col if hit else "#0b0f14"}" stroke="{col if hit else "#6b7d90"}" stroke-width="3"/>')
            o.append(f'<text x="{x}" y="{ty}" fill="{"#fff" if hit else "#6b7d90"}" font-size="{14 if hit else 12}" font-weight="{700 if hit else 400}" text-anchor="middle">{name}</text>')
    for k, (lab, pin, col, row, idx) in enumerate(sd):
        x1, y1 = spx(k), sy + 14
        if row == 'top':
            x2, y2 = pinx(idx), topy - 12
            # up out of the screen's header, over its edge, then down to the board
            ox = sx + sw + 40 + k * 26
            o.append(f'<path d="M{x1},{y1} L{x1},{sy-20-k*10} L{ox},{sy-20-k*10} L{ox},{by-40+k*0} C{ox},{by-10} {x2},{by-40} {x2},{y2}" fill="none" stroke="{col}" stroke-width="6" stroke-linecap="round" stroke-linejoin="round"/>')
        else:   # VCC to 3V3 on the far header: round the antenna end
            x2, y2 = pinx(idx), boty + 12; lx = bx - 50
            o.append(f'<path d="M{x1},{y1} L{x1},{sy-50} L{lx},{sy-50} L{lx},{by+bh+40} Q{lx},{by+bh+60} {lx+20},{by+bh+60} L{x2-20},{by+bh+60} Q{x2},{by+bh+60} {x2},{by+bh+40} L{x2},{y2}" fill="none" stroke="{col}" stroke-width="6" stroke-linecap="round" stroke-linejoin="round"/>')
    lx, ly = 40, 905
    x = lx
    for a, b, c, _, _ in sd:
        t = f'{a} → {("GPIO "+b) if b.isdigit() else b}'
        o.append(f'<rect x="{x}" y="{ly-14}" width="22" height="8" rx="4" fill="{c}"/>')
        o.append(f'<text x="{x+30}" y="{ly-6}" fill="#d7e3ee" font-size="16" font-weight="600">{t}</text>')
        x += 30 + len(t) * 8.8 + 26
    lines = ['Match the names printed by the pins (22 may read D22 or P22). Use 3V3, not 5V/VIN. The microSD card (pins 5, 17, 18, 19, 23) is unaffected.',
             'The screen sleeps after 5 minutes: a short press of BOOT wakes it (holding BOOT 6 s resets the Wi-Fi).']
    for n, l in enumerate(lines):
        o.append(f'<text x="40" y="{ly+28+n*20}" fill="#7f93a8" font-size="14">{l}</text>')
    o.append('</svg>')
    return '\n'.join(o)

out = sys.argv[1] if len(sys.argv) > 1 else '.'
open(f'{out}/oled-wiring-shelf-esp32.svg', 'w').write(svg())

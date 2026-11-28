# Draws SD Card Imager's window in its real states, from the exact layout in SD-Card-Imager.ps1
# (client area 560x350 at 96 DPI, Segoe UI 10 pt), and saves PNGs with headless Chrome.
import html, os, sys
from playwright.sync_api import sync_playwright

OUT = sys.argv[1] if len(sys.argv) > 1 else "../../docs/screenshots"
CARD = 'Disk 2: Generic STORAGE DEVICE USB Device, 119.1 GB, USB  (E:)'

CSS = """
* { box-sizing: border-box; }
body { margin: 0; background: #2b5797; font-family: 'Open Sans'; font-stretch: 90%; font-size: 13px; color: #000; }
.stage { position: relative; padding: 26px; display: inline-block;
         background: linear-gradient(135deg, #1f4e8c, #3a7bd5); }
.win { position: relative; width: 562px; border: 1px solid #1883d7; background: #f0f0f0;
       box-shadow: 0 8px 28px rgba(0,0,0,.45); }
.tb { height: 31px; background: #fff; position: relative; display: flex; align-items: center; }
.tb .ic { width: 16px; height: 16px; margin: 0 8px 0 8px; }
.tb .t { font-size: 12px; }
.tb .btns { position: absolute; right: 0; top: 0; display: flex; }
.tb .btns span { width: 46px; height: 31px; display: flex; align-items: center; justify-content: center; font-size: 15px; color: #000; font-family: 'DejaVu Sans'; font-weight: 300; }
.tb .btns span.off { color: #c8c8c8; }
.client { position: relative; width: 560px; height: 350px; }
.c { position: absolute; white-space: nowrap; }
.lbl { line-height: 18px; }
.btn { background: #e1e1e1; border: 1px solid #adadad; display: flex; align-items: center; justify-content: center; }
.btn.def { border: 2px solid #0078d7; }
.btn.dis { background: #cccccc; border-color: #bfbfbf; color: #838383; }
.combo { background: #fff; border: 1px solid #7a7a7a; height: 25px; padding: 0 22px 0 4px; display: flex; align-items: center; overflow: hidden; }
.combo.dis { background: #f0f0f0; border-color: #bfbfbf; color: #6d6d6d; }
.combo .arrow { position: absolute; right: 6px; top: 8px; width: 9px; height: 9px; border-right: 1px solid #333; border-bottom: 1px solid #333; transform: rotate(45deg) scale(.7); transform-origin: center; top: 5px; }
.combo.dis .arrow { border-color: #aaa; }
.chk { display: flex; align-items: center; gap: 5px; }
.chk .box { width: 13px; height: 13px; border: 1px solid #333; background: #fff; position: relative; }
.chk .box.on::after { content: ''; position: absolute; left: 3px; top: 0px; width: 4px; height: 8px; border-right: 2px solid #000; border-bottom: 2px solid #000; transform: rotate(40deg); }
.chk.dis { color: #6d6d6d; } .chk.dis .box { border-color: #bcbcbc; background: #f0f0f0; }
.chk.dis .box.on::after { border-color: #9a9a9a; }
.bar { background: #e6e6e6; border: 1px solid #bcbcbc; }
.bar .fill { height: 100%; background: #06b025; }
.status { white-space: normal; line-height: 18px; }
/* message box */
.shade { position: absolute; inset: 0; }
.mb { position: absolute; background: #fff; border: 1px solid #1883d7; box-shadow: 0 8px 28px rgba(0,0,0,.45); }
.mb .tb { border-bottom: 0; }
.mb .body { display: flex; gap: 14px; padding: 22px 26px 24px 22px; }
.mb .q { flex: none; width: 32px; height: 32px; border-radius: 50%; background: #2a73cc; color: #fff; font: bold 22px 'DejaVu Sans'; display: flex; align-items: center; justify-content: center; }
.mb .q.warn { border-radius: 0; background: none; width: 34px; }
.mb .text { line-height: 19px; white-space: pre-wrap; padding-top: 4px; }
.mb .text .cmd { font-family: 'DejaVu Sans Mono'; font-size: 12px; }
.mb .foot { background: #f0f0f0; padding: 11px 11px; display: flex; justify-content: flex-end; gap: 8px; }
.mb .foot .btn { width: 88px; height: 26px; position: static; }
"""

ICON = """<svg class="ic" viewBox="0 0 16 16"><path d="M3 1h7l3 3v11H3z" fill="#4a90d9" stroke="#1d5fa8"/><rect x="5" y="2" width="1.3" height="3" fill="#ffd34d"/><rect x="7" y="2" width="1.3" height="3" fill="#ffd34d"/><rect x="9" y="2" width="1.3" height="3" fill="#ffd34d"/></svg>"""

def esc(s): return html.escape(s)

def window(busy=False, bar=0, status='', stop=None, card=CARD):
    d = ' dis' if busy else ''
    stop_on = busy if stop is None else stop
    w = lambda x, y, wd, ht, cls, inner, extra='': f'<div class="c {cls}" style="left:{x}px;top:{y}px;width:{wd}px;height:{ht}px;{extra}">{inner}</div>'
    parts = [
        f'<div class="c lbl" style="left:16px;top:14px">Save an SD card to an image file, or write an image onto a card.</div>',
        f'<div class="c lbl" style="left:16px;top:52px">Card:</div>',
        w(64, 48, 380, 25, 'combo' + d, f'<span style="overflow:hidden;width:340px;display:block">{esc(card)}</span><svg style="position:absolute;right:6px;top:8px" width="10" height="8" viewBox="0 0 10 8"><path d="M1 2l4 4 4-4" fill="none" stroke="#333" stroke-width="1.2"/></svg>'),
        w(452, 47, 92, 30, 'btn' + d, 'Refresh'),
        f'<div class="c chk{d}" style="left:16px;top:88px"><span class="box on"></span>Compress the image (.img.gz: much smaller, empty space takes almost nothing)</div>',
        w(16, 124, 172, 40, 'btn' + d, 'Save card to image...'),
        w(194, 124, 172, 40, 'btn' + d, 'Write image to card...'),
        w(372, 124, 172, 40, 'btn' + d, 'Grow to fill card...'),
        w(16, 180, 528, 22, 'bar', f'<div class="fill" style="width:{bar * 100:.1f}%"></div>'),
        f'<div class="c status" style="left:16px;top:208px;width:528px;height:56px">{esc(status)}</div>',
        f'<div class="c chk{d}" style="left:16px;top:272px"><span class="box on"></span>Quick: only the parts in use (FAT32 cards)</div>',
        f'<div class="c chk{d}" style="left:16px;top:300px"><span class="box"></span>Show all drives</div>',
        w(432, 290, 112, 40, 'btn' + ('' if stop_on else ' dis'), 'Stop'),
    ]
    return f"""<div class="win"><div class="tb">{ICON}<span class="t">SD Card Imager</span>
      <div class="btns"><span>&#8211;</span><span class="off">&#9744;</span><span>&#10005;</span></div></div>
      <div class="client">{''.join(parts)}</div></div>"""

def msgbox(text_html, left, top, width):
    return f"""<div class="mb" style="left:{left}px;top:{top}px;width:{width}px">
      <div class="tb"><span class="t" style="margin-left:10px">SD Card Imager</span><div class="btns"><span>&#10005;</span></div></div>
      <div class="body"><div class="q">?</div><div class="text">{text_html}</div></div>
      <div class="foot"><div class="btn def">Yes</div><div class="btn">No</div></div></div>"""

def page(inner): return f'<!doctype html><html><head><style>{CSS}</style></head><body><div class="stage" id="s">{inner}</div></body></html>'

GROW_FAT = ("The image only uses part of this card. Grow the FAT32 partition from 29.7 GB to 119.1 GB, so the whole card can be used?\n\n"
            "Your files stay where they are. It only takes a minute. Keep the image you wrote until you've checked the card, just in case.")
GROW_PI = ("The image only uses part of this card. Grow the card's Linux (Raspberry Pi) partition from 29.2 GB to 118.6 GB?\n\n"
           "Windows can make the partition bigger but can't touch the files inside it, so after this, boot the Pi from the card once and run this in a Terminal:\n\n"
           "    <span class=\"cmd\">sudo resize2fs /dev/mmcblk0p2</span>\n\n(It takes a few seconds; no reboot needed.)")
WROTE = 'Complete. Written in 3 min. Take the card out and put it back in before using it.'

def overlay(box): return f'<div style="position:relative">{window(status=WROTE)}{box}</div>'

DIAGRAM = """
<div style="width:700px;padding:26px 30px;background:#fff;font-family:'Open Sans';color:#222">
 <div style="font-size:17px;font-weight:600;margin-bottom:14px">A 32 GB image written onto a 128 GB card</div>
 <div style="font-size:13px;margin-bottom:6px">After writing: the card still acts like a 32 GB card</div>
 <div style="display:flex;height:46px;border:1px solid #888;border-radius:4px;overflow:hidden;font-size:13px">
   <div style="width:25%;background:#3b82c4;color:#fff;display:flex;align-items:center;justify-content:center">your partition, 29.7 GB</div>
   <div style="flex:1;background:repeating-linear-gradient(45deg,#eee,#eee 8px,#ddd 8px,#ddd 16px);display:flex;align-items:center;justify-content:center;color:#666">89.4 GB nobody can use</div>
 </div>
 <div style="font-size:22px;text-align:center;margin:10px 0;color:#3b82c4">&#8595; Grow to fill card</div>
 <div style="display:flex;height:46px;border:1px solid #888;border-radius:4px;overflow:hidden;font-size:13px">
   <div style="flex:1;background:#2f9e5b;color:#fff;display:flex;align-items:center;justify-content:center">your partition, 119.1 GB: same files, all the space</div>
 </div>
</div>"""

shots = {
    'main': window(status='Pick the card, then save it or write an image to it.'),
    'saving': window(busy=True, bar=0.384, status='Saving (only the 3.1 GB in use) 11.4 GB of 29.7 GB  -  86 MB/s  -  about 4 min left'),
    'writing': window(busy=True, bar=0.41, status='Writing 1.2 GB of 2.9 GB  -  41 MB/s  -  about 1 min left'),
    'grow-ask': overlay(msgbox(esc(GROW_FAT), 60, 110, 460)),
    'grow-working': window(busy=True, stop=False, status='Moving 672 clusters out of the way...'),
    'grow-done': window(status='Done: the card now uses all 119.1 GB. Take it out and put it back in before using it.'),
    'grow-pi': overlay(msgbox(esc(GROW_PI).replace('&lt;span class=&quot;cmd&quot;&gt;', '<span class="cmd">').replace('&lt;/span&gt;', '</span>'), 50, 60, 480)),
}

with sync_playwright() as p:
    b = p.chromium.launch(executable_path='/usr/bin/google-chrome')
    pg = b.new_page(device_scale_factor=2, viewport={'width': 900, 'height': 700})
    for name, inner in shots.items():
        pg.set_content(page(inner)); pg.wait_for_timeout(150)
        pg.locator('#s').screenshot(path=os.path.join(OUT, f'sd-imager-{name}.png'))
    pg.set_content(f'<!doctype html><html><body style="margin:0"><div id="s" style="display:inline-block">{DIAGRAM}</div></body></html>')
    pg.locator('#s').screenshot(path=os.path.join(OUT, 'sd-imager-grow-diagram.png'))
    b.close()
print('ok')

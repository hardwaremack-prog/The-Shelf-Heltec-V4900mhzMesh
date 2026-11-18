"""Turns the simulator's 128x64 dumps into pictures of a two-color OLED (blue, with the yellow
strip at the bottom, as on a screen plugged in line and turned around), plus one sheet of them all."""
import sys, pathlib
from PIL import Image, ImageDraw, ImageFont
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
S = 4                                    # pixels per screen pixel
BLUE, YELLOW, OFF, BEZEL = (90, 200, 255), (255, 214, 60), (8, 12, 22), (24, 26, 30)
order = ["status", "now-playing", "game", "picture", "watch", "clock", "affirmation", "color", "temperature"]
titles = {"status": "Status", "now-playing": "Now playing", "game": "A game opened", "picture": "A picture on show",
          "watch": "Watch face", "clock": "Clock", "affirmation": "Today's affirmation", "color": "Color of the day",
          "temperature": "CPU temperature"}
tiles = []
for n in order:
    rows = (src / f"{n}.txt").read_text().split()
    im = Image.new("RGB", (128 * S + 24, 64 * S + 24), BEZEL)
    d = ImageDraw.Draw(im)
    d.rectangle([12, 12, 12 + 128 * S - 1, 12 + 64 * S - 1], fill=OFF)
    for y, r in enumerate(rows):
        on = YELLOW if y >= 48 else BLUE
        for x, c in enumerate(r):
            if c == "1":
                d.rectangle([12 + x * S, 12 + y * S, 12 + x * S + S - 2, 12 + y * S + S - 2], fill=on)
    im.save(out / f"{n}.png")
    tiles.append((n, im))
try: font = ImageFont.truetype("DejaVuSans-Bold.ttf", 22)
except OSError: font = ImageFont.load_default()
cols, tw, th = 3, tiles[0][1].width, tiles[0][1].height
sheet = Image.new("RGB", (cols * (tw + 30) + 30, ((len(tiles) + cols - 1) // cols) * (th + 70) + 30), (245, 245, 247))
d = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(tiles):
    x, y = 30 + (i % cols) * (tw + 30), 30 + (i // cols) * (th + 70)
    d.text((x, y), titles[n], fill=(30, 30, 35), font=font)
    sheet.paste(im, (x, y + 36))
sheet.save(out / "all-screens.png")
print("wrote", len(tiles), "screens and all-screens.png to", out)

"""Writes The Shelf's public/oled-sim-data.js: the board screen's U8g2 fonts (from fonts.c) and the
world map (ShelfV4/world_map.h), for the simulated screen on The Shelf's Home page.
    python3 tools/oled-web/make_data.py fonts.json ShelfV4/world_map.h <the-shelf>/public/oled-sim-data.js
"""
import json, re, sys
fonts = json.load(open(sys.argv[1]))
rows = re.findall(r"\{((?:0x[0-9a-f]{2},?){16})\}", open(sys.argv[2]).read())
world = ["".join(b[2:] for b in r.split(",")) for r in rows]
assert len(world) == 64
top = int(re.search(r"#define WORLD_TOP (-?\d+)", open(sys.argv[2]).read()).group(1))
bot = int(re.search(r"#define WORLD_BOTTOM \((-?\d+)\)", open(sys.argv[2]).read()).group(1))
with open(sys.argv[3], "w") as o:
    o.write("// Generated from The Shelf boards' firmware (tools/oled-web) -- the screen's fonts (U8g2, BSD)\n")
    o.write("// and world map (Natural Earth, public domain), for the simulated screen. Don't edit.\n")
    o.write("window.OLED_FONTS=" + json.dumps(fonts, separators=(",", ":")) + ";\n")
    o.write("window.OLED_WORLD={top:%d,bottom:%d,rows:%s};\n" % (top, bot, json.dumps(world)))
print("wrote", sys.argv[3])

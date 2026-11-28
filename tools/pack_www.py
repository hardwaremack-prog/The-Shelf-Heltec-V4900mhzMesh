#!/usr/bin/env python3
"""Pack The Shelf's web app (its public/ folder) into a LittleFS image for the board.

    python3 tools/pack_www.py <path-to-the-shelf> <out.bin> [size] [V4|V4MMC|V3|ESP32]

- Uses The Shelf's own index.html, app.js, style.css, pdf.js and Tidy Suite unchanged,
  except: the AI and Big Transfer tabs are taken out (they need a computer), and
  board-ui/board.css + board.js are added to the page.
- Everything is stored gzipped; the board serves it with Content-Encoding: gzip.
"""
import gzip, os, re, shutil, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
shelf = pathlib.Path(sys.argv[1]).resolve()
out_bin = pathlib.Path(sys.argv[2]).resolve()
size = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x200000
board = (sys.argv[4] if len(sys.argv) > 4 else "V4").upper()
# V4MMC: the Heltec V4 MMC build (4-bit SD/MMC card reader). It keeps the Big Transfer tab (its
# firmware stores big files on the card in 1 GB pieces) and has no GPS (the reader has its pins).
big_files = board == "V4MMC"
public = shelf / "public"
if not (public / "app.js").exists():
    sys.exit(f"Can't find The Shelf's public/app.js under {shelf}")

stage = ROOT / "build" / "www"
shutil.rmtree(stage, ignore_errors=True)
SKIP_DIRS = {"vendor/novnc", "vendor/shrink"}   # (vendor/shrink: the computer Shelf's offline copy of the shrinkers; boards load them from the CDN)
SKIP_FILES = {"vnc.html"}
for src in public.rglob("*"):
    rel = src.relative_to(public).as_posix()
    if src.is_dir() or any(rel == d or rel.startswith(d + "/") for d in SKIP_DIRS) or rel in SKIP_FILES:
        continue
    dst = stage / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)

# ---- small, targeted changes to The Shelf's page
app = (stage / "app.js").read_text(encoding="utf-8")
before = app
for key in (("ai", "mesh") if big_files else ("ai", "bigfiles", "mesh")):   # (mesh: the computer's way into Coms; boards have their own tab)
    app = re.sub(r"\n\s*\{ key: '%s',[^\n]*\},?" % key, "", app, count=1)
if app == before:
    sys.exit("Couldn't find the TOOLS list in app.js -- The Shelf's layout changed; update pack_www.py")
app = re.sub(r",(\s*\];\s*\nconst TOOL_LABELS)", r"\1", app)   # no trailing comma left behind
# add the board's own tab: Meshed Comms (drawn by board.js). A plain ESP32 has no LoRa radio.
# (a plain ESP32 has no LoRa radio: its Coms run over 2.4 GHz only, and the tab says so)
coms_label = "Meshed Comms"
app, n1 = re.subn(r"(\n(\s*)\{ key: 'tidysuite',)", r"\n\2{ key: 'coms', label: '" + coms_label + r"', color: '#E2725B' },\1", app, count=1)
app, n2 = re.subn(r"(else if \(key === 'tidysuite'\) loadTidySuiteView\(\);)", r"\1\n  else if (key === 'coms') window.loadComsView();", app, count=1)
# Meshed File Transfer, right after Meshed Comms
app, nx = re.subn(r"(\{ key: 'coms', label: '[^']*', color: '#E2725B' \},)", r"\1\n  { key: 'xfer', label: 'Meshed File Transfer', color: '#F2B441' },", app, count=1)
app, ny = re.subn(r"(else if \(key === 'coms'\) window\.loadComsView\(\);)", r"\1\n  else if (key === 'xfer') window.loadXferView();", app, count=1)
if not (nx and ny):
    sys.exit("Couldn't add the Meshed File Transfer tab to app.js -- The Shelf's layout changed; update pack_www.py")
if board == "V4":   # GPS, at the bottom: a V4 with Heltec's GPS kit on its GNSS connector (board.js draws it)
    app, n3 = re.subn(r"(\n(\s*)\{ key: 'tidysuite',[^\n]*\})", r"\1,\n\2{ key: 'gps', label: 'GPS', color: '#3AA0FF' }", app, count=1)
    app, n4 = re.subn(r"(else if \(key === 'coms'\) window\.loadComsView\(\);)", r"\1\n  else if (key === 'gps') window.loadGpsView();", app, count=1)
    if not (n3 and n4):
        sys.exit("Couldn't add the GPS tab to app.js -- The Shelf's layout changed; update pack_www.py")
if not (n1 and n2):
    sys.exit("Couldn't add the Meshed Comms tab to app.js -- The Shelf's layout changed; update pack_www.py")
# a seventh shelf, MIDI: listed, uploaded and played like Music (board-ui/midi.js plays the files)
def must(pattern, repl, count=1):
    global app
    app, n = re.subn(pattern, repl, app, count=count)
    if not n:
        sys.exit("Couldn't add the MIDI shelf (" + pattern[:40] + ") -- The Shelf's layout changed; update pack_www.py")
if 'isMusicShelf' not in app:          # The Shelf has had the MIDI shelf built in since 7b4777c
    must(r"(  Music: 'var\(--cat-Music\)',\n)", r"\1  MIDI: 'var(--cat-MIDI)',\n")
    must(r"(const PLAY_COUNT_CATEGORIES = \['Music',)", r"\1 'MIDI',")
    app = "const isMusicShelf = c => c === 'Music' || c === 'MIDI';\n" + app
    must(r"if \(state\.category !== 'Music' \|\| !state\.currentTrackName\) return;", "if (!isMusicShelf(state.category) || !state.currentTrackName) return;")
    must(r"if \(state\.category === 'Music'\) opts\.push\(\['recent'", "if (isMusicShelf(state.category)) opts.push(['recent'")
    must(r"el\('#shuffleAllBtn'\)\.hidden = state\.category !== 'Music'", "el('#shuffleAllBtn').hidden = !isMusicShelf(state.category)")
    must(r"\n  if \(state\.category === 'Music'\) \{", "\n  if (isMusicShelf(state.category)) {")
    must(r"recordPlay\('Music', f\.name\);", "recordPlay(String(f.url || '').startsWith('/shared/MIDI/') ? 'MIDI' : 'Music', f.name);")
# pictures, cover art and thumbnails load through board.js's queue (a few at a time) instead of all
# at once: a board can't send a screenful of full-size photos together
app, nimg = re.subn(r'<img src="(\$\{f\.(?:url|artUrl|thumbUrl)\})"( alt="[^"]*")? loading="lazy">', r'<img data-qsrc="\1"\2>', app)
if not nimg:
    print("note: no shelf images found to queue (The Shelf's layout changed?)")
# V4 MMC Big Transfer: 1 MB chunks (the board collects each one in PSRAM before writing it), a
# progress bar that moves while a chunk is going (not just after it), and the speed and time left.
if big_files:
    reps = [
        ("const BIGFILE_CHUNK_SIZE = 16 * 1024 * 1024;", "const BIGFILE_CHUNK_SIZE = 1024 * 1024;   // the board takes 1 MB at a time"),
        ("""      const res = await fetch(`/api/bigfiles/${u.uploadId}/chunk?offset=${u.uploadedBytes}`, {
        method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: chunk,
      });""", """      const res = await bigChunkXhr(`/api/bigfiles/${u.uploadId}/chunk?offset=${u.uploadedBytes}`, chunk, u);"""),
        ("""  const pct = u.size ? Math.round((u.uploadedBytes / u.size) * 100) : 0;
  const label = u.status === 'error'""", """  const live = Math.min(u.size || 0, u.uploadedBytes + (u.status === 'uploading' ? (u.liveBytes || 0) : 0));
  const pct = u.size ? Math.floor((live / u.size) * 1000) / 10 : 0;
  const label = u.status === 'error'"""),
        (""": `${humanFileSize(u.uploadedBytes)} of ${humanFileSize(u.size)} (${pct}%)`;""",
         """: `${humanFileSize(live)} of ${humanFileSize(u.size)} (${pct}%)${bigSpeedText(u, live)}`;"""),
    ]
    for a, b in reps:
        if a not in app:
            sys.exit("pack_www: Big Transfer code in app.js changed -- update the V4MMC patches (" + a[:50] + "...)")
        app = app.replace(a, b, 1)
    app += r"""

// ---- Heltec V4 MMC: Big Transfer progress while a chunk is on its way, with speed and time left
function bigChunkXhr(url, blob, u) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', url);
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    let last = 0;
    x.upload.onprogress = (e) => {
      u.liveBytes = e.loaded;
      bigSpeedSample(u, u.uploadedBytes + e.loaded);
      const now = performance.now();
      if (now - last > 250) { last = now; renderBigfileUploadCard(); }
    };
    x.onload = () => {
      u.liveBytes = 0;
      let body = {};
      try { body = JSON.parse(x.responseText || '{}'); } catch { /* not JSON */ }
      if (typeof body.receivedBytes === 'number') bigSpeedSample(u, body.receivedBytes);
      resolve({ ok: x.status >= 200 && x.status < 300, status: x.status, json: async () => body });
    };
    x.onerror = () => { u.liveBytes = 0; reject(new Error('The connection dropped.')); };
    x.onabort = () => { u.liveBytes = 0; reject(new Error('The upload was stopped.')); };
    x.send(blob);
  });
}
// keeps the last ~8 seconds of (time, bytes) to work out a steady speed
function bigSpeedSample(u, bytes) {
  const now = performance.now();
  const s = u.speedSamples || (u.speedSamples = []);
  s.push([now, bytes]);
  while (s.length > 2 && now - s[0][0] > 8000) s.shift();
}
function bigSpeedText(u, live) {
  if (u.status !== 'uploading') return '';
  const s = u.speedSamples || [];
  if (s.length < 2) return ' · measuring speed…';
  const dt = (s[s.length - 1][0] - s[0][0]) / 1000, db = s[s.length - 1][1] - s[0][1];
  if (dt < 1 || db <= 0) return ' · measuring speed…';
  const bps = db / dt;
  const left = Math.max(0, (u.size - live) / bps);
  const t = left >= 3600 ? `${Math.floor(left / 3600)} h ${Math.round((left % 3600) / 60)} min`
          : left >= 60 ? `${Math.round(left / 60)} min` : `${Math.round(left)} s`;
  return ` · ${humanFileSize(bps)}/s (${(bps * 8 / 1e6).toFixed(1)} Mbps) · about ${t} left`;
}
"""
(stage / "app.js").write_text(app, encoding="utf-8")
if "\x01" in app or "\x02" in app:
    sys.exit("app.js picked up a stray control character from a replacement -- check pack_www.py's regexes")

html = (stage / "index.html").read_text(encoding="utf-8")
html = html.replace('<link rel="stylesheet" href="style.css">',
                    '<link rel="stylesheet" href="style.css">\n<link rel="stylesheet" href="board.css">', 1)
midi_tag = '' if 'src="midi.js"' in html else '\n<script src="midi.js"></script>'
html = html.replace('<script src="app.js"></script>', '<script src="app.js"></script>\n<script src="board.js"></script>' + midi_tag, 1)
if '<option value="MIDI">' not in html:
    html = html.replace('<option value="Music">Music</option>', '<option value="Music">Music</option>\n      <option value="MIDI">MIDI</option>', 1)
if '<option value="MIDI">' not in html:
    sys.exit("Couldn't add MIDI to the upload shelf list in index.html")
label = "ESP32" if board == "ESP32" else "Heltec V4 MMC" if big_files else f"Heltec {board}"
html = html.replace('\n<script src="mesh-coms.js"></script>', '', 1)   # the computer's Mesh Coms (boards have Coms)
(stage / "mesh-coms.js").unlink(missing_ok=True)
html = html.replace("<title>The Shelf</title>", f"<title>The Shelf · {label}</title>", 1)
if "board.js" not in html or "board.css" not in html:
    sys.exit("Couldn't add board.js/board.css to index.html")
(stage / "index.html").write_text(html, encoding="utf-8")

# folder locks: on a board the "host" is the admin (a browser unlocked with the admin password)
lk = stage / "locks.js"
if lk.exists():
    t = lk.read_text(encoding="utf-8")
    for a, b in [
        ("Other devices can look once they enter it, but can never change locked folders — only this computer can. This computer is never asked.",
         "Others can look once they enter it, but can never change locked folders: only the admin can (a browser unlocked with the admin password), and the admin is never asked."),
        ("Backups are host-only while any lock is on.", "Card backups are admin-only while any lock is on."),
        # on a board it's "Read control": shelves (and tools) that need a password just to look at
        ("'Read-only password: on' : 'Read-only password: off'", "'Read control: on' : 'Read control: off'"),
        ("'\U0001F512 Folder locks'", "'\U0001F512 Read control'"),
        ("'\U0001F512 Folder locks\u2026'", "'\U0001F512 Read control: shelves\u2026'"),
        ("Tick what should need the viewing password.", "Tick the shelves and tools that need a password just to look at."),
    ]:
        if a not in t: sys.exit("pack_www: locks.js wording changed -- update the board's replacements")
        t = t.replace(a, b)
    lk.write_text(t, encoding="utf-8")

# ---- the board's own bits
for src in (ROOT / "board-ui").rglob("*"):
    if src.is_file():
        dst = stage / src.relative_to(ROOT / "board-ui")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

# name the right board in the page (board.js and the settings page say "Heltec V4")
if board != "V4":
    for f in (stage / "board.js", stage / "board" / "index.html"):
        t = f.read_text(encoding="utf-8")
        t = t.replace('<span class="chip">V4</span>', f'<span class="chip">{board}</span>')
        t = t.replace("Heltec V3 and V4", "Heltec V3 and V\x00")   # keep this one as is
        t = t.replace("Heltec V4", label).replace("Heltec V3 and V\x00", "Heltec V3 and V4")
        if board == "ESP32":   # its button is labelled BOOT, and there's no screen unless one is added
            t = t.replace("The screen sleeps after 5 minutes. Press the board's PRG button to wake it.",
                          "With a screen added (SSD1306 on GPIO 21/22), it sleeps after 5 minutes; press BOOT to wake it.")
            t = t.replace("Hold PRG for 6 seconds and let go to forget the Wi-Fi and go back to setup mode.",
                          "Hold BOOT for 6 seconds and let go to forget the home Wi-Fi and go back to its own Wi-Fi (named after the board, password theshelf) at http://10.1.1.250.")
            t = t.replace("<b>Meshed Comms</b> (its radio listens all the time;", "<b>Meshed Comms</b> (it keeps the Wi-Fi radio listening;")
            t = t.replace("Hold PRG", "Hold BOOT")
            t = t.replace("PRG", "BOOT")                    # (deep sleep and any other mention of the button)
            t = t.replace("const NOLORA = false;", "const NOLORA = true; ")
        f.write_text(t, encoding="utf-8")

# A plain ESP32's web partition is 1.2 MB: PDF.js (~400 KB) doesn't fit beside the rest, so it loads from the CDN there
# (Books covers and reading then need the internet; the rest of the page works offline as before).
if board == "ESP32":
    shutil.rmtree(stage / "vendor" / "pdfjs", ignore_errors=True)
    ix = stage / "index.html"
    t = ix.read_text(encoding="utf-8")
    cdn = "https://cdn.jsdelivr.net/npm/pdfjs-dist@3.11.174/build/"
    n = t.count("vendor/pdfjs/")
    t = t.replace("vendor/pdfjs/", cdn)
    if n < 2: sys.exit("pack_www: index.html's PDF.js lines changed -- update the ESP32 CDN switch")
    ix.write_text(t, encoding="utf-8")

try:
    rev = subprocess.run(["git", "-C", str(shelf), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
except Exception:
    rev = ""
(stage / "shelf-version.txt").write_text((rev or "unknown") + "\n")

# ---- every script must at least parse (a broken one leaves the page blank on the board)
if shutil.which("node"):
    for js in ("app.js", "board.js", "midi.js"):
        f = stage / js
        if f.exists():
            r = subprocess.run(["node", "--check", str(f)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"{js} doesn't parse after packing:\n{r.stderr[:800]}")

# ---- gzip everything
total = 0
for f in sorted(p for p in stage.rglob("*") if p.is_file()):
    data = f.read_bytes()
    gz = gzip.compress(data, 9, mtime=0)
    f.with_name(f.name + ".gz").write_bytes(gz)
    f.unlink()
    total += len(gz)
print(f"web app: {total/1024:.0f} KB gzipped (The Shelf {rev or '?'})")
if total > size * 0.9:
    sys.exit(f"Too big for the {size//1024} KB web partition")

# Needs mklittlefs 3.x built with LFS_NAME_MAX=64 to match the board (the copy bundled with
# the esp32 2.0.x core is too old for paths over 32 characters). See README.
mk = os.environ.get("MKLITTLEFS") or shutil.which("mklittlefs") or "mklittlefs"
r = subprocess.run([mk, "-c", str(stage), "-b", "4096", "-p", "256", "-s", str(size), str(out_bin)], check=True, capture_output=True, text=True)
if "Error" in (r.stdout + r.stderr):   # (it still writes an image, with the file that didn't fit left empty)
    sys.exit("mklittlefs couldn't fit everything in the web partition:\n" + (r.stdout + r.stderr)[-600:])
print(f"wrote {out_bin}")

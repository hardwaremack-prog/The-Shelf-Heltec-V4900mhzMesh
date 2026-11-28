# The Shelf for Heltec V4 (and V3, and plain ESP32 boards)

**[The Shelf](https://github.com/hardwaremack-prog/the-shelf), running on a Heltec WiFi LoRa 32 V4, a V3, or a plain ESP32 dev board (NodeMCU-32S / DevKitC).** The board serves The Shelf's own web app (the same page you get on a PC or a Pi), with your files on a microSD card. Anyone on your home Wi-Fi opens it in a browser, and it shows up in Messages on every other Shelf in the house.

### ➡️ Install from your browser: **https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/**

![The Shelf on a Heltec V4](docs/screenshot-pictures.png)

> Installing replaces Meshtastic on that board. You can put Meshtastic back any time from [flasher.meshtastic.org](https://flasher.meshtastic.org).

## What works on the board

| ✅ Included | 💻 Needs The Shelf on a computer |
|---|---|
| Pictures (grid, slideshow, photo editor), Music (shuffle, play counts, artist and cover art read from your MP3s' and Opus files' own tags), **MIDI** (its own shelf: `.mid`, `.midi`, `.kar` files played by a built-in General MIDI synth, with the same player, shuffle and play counts), Videos, Games (`.html` and zipped HTML games play in the page), Programs, Books (PDF reader and cover thumbnails) | The AI assistant and Create with AI |
| Home: bulletin board with notes, pasted pictures and videos, quick links, weather | Remote desktop (VNC) |
| Notes, Docs, Sheets, Slides, Draw | Online album-art lookup, tag editing, Standardize names, Remove duplicates, Export |
| Tidy Suite | Big Transfer (except on the V4 MMC), backup/restore zips, spreadsheet import |
| **Messages**: finds and chats with your other Shelves (PCs, Pis, other boards), pictures included | User accounts (the board has one admin password instead) |
| **Meshed Comms**: LoRa text mesh between boards, every board a relay, 12-hour catch-up, list of boards and remote admin | |
| **Read-only mode** with an admin password to unlock changes | |
| **Insane compression** (1.19): music and photos are shrunk in your browser on the way in, and the **Shrink…** button on the Music and Pictures shelves shrinks what's already there (optionally removing the originals). MP3s become Opus 48 kbps (3.8 MB → 1.4 MB, 62% smaller, tags and cover kept), photos and iPhone HEICs become AVIF at 2048 px (~3.5 MB → ~250 KB). The encoders load from the jsDelivr CDN, so the device you upload from needs the internet; otherwise files go up as they are. | |

The buttons for those computer-only features are hidden on the board, and the AI and Big Transfer tabs are left out (the Heltec V4 MMC keeps Big Transfer).

## Hardware

| Part | Notes |
|---|---|
| Heltec WiFi LoRa 32 **V4** or **V3** | The standard V4 (ESP32-S3R2, 2 MB PSRAM), the V4-R8 (8 MB PSRAM), or the V3 (8 MB flash, no PSRAM). Each has its own install button |
| 3.3 V SPI microSD module | The small blue 6-pin module whose pins read **3v3 · CS · MOSI · CLK · MISO · GND** (square pad = 3v3). It has no voltage regulator, so it goes to **3V3**, never 5V |
| microSD card | FAT32. A 32 GB card works out of the box |

### Wiring the microSD card (V4 and V3)

From **v1.6.0** the card uses four neighbouring pins on the J3 row, the same layout as Heltec V4 MP3 + BT:

![microSD wiring for The Shelf on Heltec V4](docs/sd-wiring-shelf-v4.png)

| microSD module | Heltec V4 / V3 |
|---|---|
| 3v3 | **3V3** |
| GND | GND |
| CS | GPIO 42 |
| MOSI | GPIO 41 |
| CLK (SCK) | GPIO 40 |
| MISO | GPIO 39 |

The V3's J3 row is laid out the same way ([V3 diagram](docs/sd-wiring-shelf-v3.png)). On a V4, GPIO 39–42 are shared with the GNSS connector, so **leave the GNSS kit unplugged**.

**Easier: plug it straight in.** CS · MOSI · CLK · MISO sit on 42 · 41 · 40 · 39 in the same order as the module's pins, so the module plugs straight into the header. Bend up (or clip) its 3v3 and GND pins so they skip GPIO 45 and 38, then add two short jumpers to the GND and 3V3 pins at the start of the row.

![Inline microSD mounting on the Heltec V4 / V3](docs/sd-inline-shelf.png)

**Heltec V4-R8:** its GPIO 40 switches the screen's power, so the R8 build keeps the older layout: CS 4, MISO 6, CLK 47, MOSI 48 ([R8 diagram](docs/sd-wiring-shelf-v4-r8.png)).

> **Upgrading from v1.5 or earlier on a V4 or V3?** The card moved from CS 4 · MISO 6 · CLK 47 · MOSI 48 to the pins above. Move the four signal wires, or the board won't find the card and will use its own flash instead.

With no card, The Shelf keeps files in the board's own flash: about 9 MB on a V4, about 2.5 MB on a V3. The screen shows **SD** or **FLASH** in the corner.

### Heltec V4 MMC (fast 4-bit card reader)

A separate build for a standard V4 (V4.3) with a 9-pin **SDIO card reader** (pins printed 3.3V · GND · D0 · CMD · CLK · D3 · D2 · D1 · CD) instead of the 6-pin SPI module. The board reads it on the ESP32-S3's own SD/MMC host, 4 data lines at 40 MHz, about three times as fast as SPI. Install it with **Install on V4 MMC** on the installer page; it updates over Wi-Fi from its own `shelf-v4-mmc` files, and shows up on the network as `shelf-v4mmc-xxxx`.

![4-bit SD/MMC reader in line on a Heltec V4.3](docs/sd-mmc-wiring-shelf-v4.png)

Lay the reader face down along J3 so **D0 · CMD · CLK · D3 · D2** drop straight into **GPIO 42 · 41 · 40 · 39 · 38**. Its other four pins would land on GPIO 46, 45, 1 and 2 (two boot-strap pins, the battery sense and the LoRa amplifier), so snip them and add three short wires:

| Reader pin | Heltec V4 |
|---|---|
| 3.3V | wire to **3V3** |
| GND | wire to **GND** |
| D0 · CMD · CLK · D3 · D2 | straight into **42 · 41 · 40 · 39 · 38** |
| D1 | wire to **GPIO 4** |
| CD | not used (snip it) |

It tries 4-bit at 40 MHz, then 20 MHz, then 1-bit (D0 only) so a card still works while a D1/D2/D3 wire is checked; the serial log says which one it found. GPIO 38–42 are the GNSS connector's pins, so this build has no GPS.

**Big Transfer** is back on this build, for a big card (say 128 GB): the same tab as The Shelf on a computer, for disk images, backups and whole video projects. Files come up in 1 MB chunks, four at a time over separate connections (the board's network stack only lets about 6 KB be in flight per connection, so one connection tops out around 7 Mbps), each written into its place in the file by a card-writer task on the other core so the Wi-Fi never waits on the card, with the speed and time left shown as it goes; a dropped connection only costs the chunk in flight, and dropping the same file in again later picks up where it stopped. The card is FAT32 (the board's FAT driver can't read exFAT, so format a 64 GB+ card as FAT32, for example with the SD Card Formatter's FAT32 option or `guiformat`), and FAT32 stops one file at 4 GB, so the board keeps anything over 1 GB in 1 GB pieces and joins them back together as you download. On the card they're in **Big Files**: small ones as ordinary files, bigger ones as a `name.parts` folder (`000`, `001`, …) that a PC can join with `copy /b 000+001+002 name` (Windows) or `cat 0* > name`. Speed is limited by the board's Wi-Fi (about 1–3 MB/s), so a 20 GB file takes a few hours each way.

### On a Heltec V3

The V3 gets the same Shelf, the same tabs and the same Meshed Comms. Its mesh settings match the V4's, so **V3 and V4 boards are on one mesh**, relaying and catching each other up. The differences:

- **Transmit power.** The V3 has no front-end amplifier, so its levels are Low ~11, Normal ~17 and High ~22 dBm (the V4 reaches ~27). The tab shows the right numbers for each board.
- **Memory.** The V3 has no PSRAM, so it keeps the newest 150 Coms messages on screen (the V4 keeps 300). The full log stays on the card. Give it a little longer with lots of people browsing at once.
- **Storage without a card** is about 2.5 MB, so fit a microSD card.
- **Screen** says **V3** in the top bar, and its address is `shelf-v3-xxxx.local`.

### On the card

Your shelves are normal folders at the top of the card: `Pictures`, `Music`, `MIDI`, `Videos`, `Games`, `Programs`, `Books`. Pop the card into a computer and copy music or photos straight in. Put files directly in those folders, because subfolders inside a shelf aren't shown (the same as The Shelf). Notes, docs, drawings, play counts, thumbnails and messages live in the hidden `.shelf` folder.

## First-time setup

1. Install with the [web installer](https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/) (Chrome or Edge).
2. Power the board. The screen shows **Join The-Shelf-XXXX**. Join it from your phone with the password **`theshelf`**.
3. The setup page opens (or go to `http://10.1.1.250`). Tap **Find networks**, pick your Wi-Fi and enter its password.
4. Back on your home Wi-Fi, open the address on the screen, or **`http://shelf-v4-xxxx.local`**.

### Meshed Comms (LoRa mesh between boards)

A **Meshed Comms** tab in The Shelf's Tools lets Shelf boards (V4 and V3) text each other over their LoRa radios, with no Wi-Fi or internet between them.

Every board also sends and hears the same packets over **2.4 GHz** (ESP-NOW broadcast frames, no router needed, phones can stay joined to a board's own Wi-Fi). Plain ESP32 boards mesh over 2.4 GHz only, through a **Meshed Comms** tab; Heltec boards send on both and relay between them, so it's one mesh. Over 2.4 GHz boards must share a Wi-Fi channel (channel 1 on their own Wi-Fi). Store and forward: besides asking for the last 12 hours when Coms comes on, a board asks again whenever it meets a board it hasn't heard for 20 minutes, so messages move with people carrying boards.

![Meshed Comms tab: on, with the message log, who's in range, and settings](docs/screenshots/coms-desktop.png)

<p align="center"><img src="docs/screenshots/coms-phone.png" width="300" alt="Meshed Comms on an iPhone"></p>

- **On/off switch.** When it's on, the tab shows the message log and a box to send to everyone on the mesh (180 characters per message).
- **Every board is a relay.** Anything new a board hears, it re-sends once after a short random wait, so messages hop board to board as far as the mesh reaches (no hop limit). Message IDs are remembered so nothing loops, and boards listen before talking.
- **"Received" receipts.** A **✓ Sending "received"** button in the tab, on by default. When it's on, the board answers every message it hears with a short receipt that hops back to the sender, so your own messages show **✓ Received by Garage V4, Truck**. Click it to stop this board sending receipts. You'll still see receipts from boards that have theirs on.
- **Catch-up.** When a board turns Coms on, it asks its neighbors for the last 12 hours. One of them replays what it's missing (up to 60 messages), marked "caught up", with their original times.
- **Display.** The tab shows who's in range with signal strength, plus counts of heard, relayed and caught-up messages. The OLED shows an antenna icon while Coms is on, and new messages for 15 seconds.
- **Settings.** You can set your name on the mesh and the transmit power. On a V4: Low ~11 dBm, Normal ~20 dBm (default), or High ~27 dBm, through its front-end amplifier. On a V3: ~11, ~17 or ~22 dBm.
- **Boards on the mesh.** Every board says hello when Coms comes on and every 15 minutes, so the tab lists every board on the mesh, even ones several hops away: its name, V3 or V4, how many hops, battery, and when it was last heard. **Refresh** asks them all to answer now.
- **Manage a board remotely.** Click a board in the list, type its admin password, and **Connect**. See [Remote admin over 900MHz](#remote-admin-over-900mhz).
- **Air settings.** 915.0 MHz, 125 kHz, SF9, CR 4/5, private sync word 0x12. This is its own channel, separate from Meshtastic, which it doesn't talk to.

**Fit the LoRa antenna before switching Coms on.** Transmitting without an antenna can damage the radio. The log is kept on the card in `.shelf/data/coms-log.jsonl`.

#### How the mesh works

1. You type a message. Your board sends it once over the radio, after checking that nobody else is transmitting.
2. Every board that hears it adds it to its log, then re-sends it once after a random 0.4–2 second wait. That way boards don't all talk at the same moment.
3. Boards farther away hear those re-sends and do the same, so the message spreads board to board as far as the mesh reaches.
4. Each board remembers the last 1,024 message IDs. When a copy it already has comes back around, it ignores it, so nothing loops and each board sends each message only once.
5. In the log, a message shows **direct** if it came straight from the sender, or **N hops** if it was relayed, along with the signal strength it arrived at.
6. With receipts on, each board that gets a message waits 2–5 seconds (so the relays go first), then sends a tiny "received" packet. It hops back across the mesh the same way, and only the original sender keeps it. Each receipt is a few bytes, but every board sends one for every message, so on a big, busy mesh you can turn receipts off to save airtime.

#### Catching up a new board

When a board turns Coms on (or powers up with it on), it broadcasts a short "catch me up" request. The request lists the newest messages it already has.

1. Each board in range picks a random wait of 0.5–3 seconds. The first one to finish waiting answers.
2. The others hear it answering and stay quiet, so there's only one answer.
3. It replays up to 60 messages from the last 12 hours that the new board is missing, oldest first, about one every 1.2 seconds.
4. Each replay carries the message's age rather than a clock time, so it shows at the right time even if the boards' clocks disagree. Replays are marked **caught up** and aren't relayed further.

The request goes out again after 2 minutes in case nobody heard the first one.

### Remote admin over 900MHz

Manage any board on the mesh from any other, with no Wi-Fi or internet between them, all night if you like.

![Boards on the mesh](docs/screenshots/coms-devices.png)

<p align="center"><img src="docs/screenshots/remote-admin.png" width="520" alt="Managing the Truck board over 900MHz"></p>

1. On the board you want to manage, set an **admin password** on its settings page (`/board/`).
2. On any other board's Shelf, open **Meshed Comms** and click that board under **Boards on the mesh**. Boards without an admin password show **No password**.
3. Type its admin password and press **Connect**. Your board sends a request over the radio, and the panel fills in when the answer comes back: a few seconds when it's direct, a few more for each hop.

From the panel you can see its battery, uptime, Wi-Fi, address, storage, mode and screen, and you can:
- Turn **read-only** on or off
- **Wake** or **sleep** its screen
- Set its **transmit power** and **receipts**, and rename it on the mesh
- **List the files** in one of its shelves
- **Ping** it to check the path
- Switch it to **its own Wi-Fi** or back to **home Wi-Fi**
- **Restart** it

How it's kept safe:
- **Signed, not encrypted.** Commands and replies go over the air readable, as the amateur rules require, so anyone listening on 915 MHz can see them. Each one carries a signature (HMAC-SHA256) made from the target board's admin password. The password itself is never sent, and a board only obeys commands signed with its own password.
- **No replays.** Every command carries a number that has to be higher than the last one that board accepted, so a recorded command can't be played back later.
- **Wrong passwords.** After 5 wrong passwords in a row, a board refuses everything for 5 minutes.

Every board relays these requests and replies the same way as messages, so you can reach a board as far away as the mesh goes.

### Admin password and read-only mode

On the board's settings page, set an **admin password**. Then you can switch The Shelf to **read-only**:

- Everyone on the Wi-Fi can still browse, play music and videos, play games, read books, and chat in Messages and Coms.
- Nobody can upload, edit, rename, move or delete anything, or change the board's settings.
- A **Read-only** switch shows in The Shelf's sidebar. Tap it and enter the admin password to unlock **Write mode** in that browser for 8 hours. Tap it again to lock.
- If someone tries to change something while it's locked, the password box pops up for them.

<p align="center"><img src="docs/screenshots/read-only-switch.png" width="240" alt="Read-only switch in the sidebar"> &nbsp; <img src="docs/screenshots/read-only-unlock.png" width="520" alt="Unlock prompt"></p>

You can turn read-only on or off remotely over Meshed Comms too. The password isn't stored on the board, only a key made from it.

**Forgot it?** Hold the board's **PRG** button for 12 seconds. That clears the admin password and turns read-only off. You need the board in your hand to do it.

#### Turned off

![Meshed Comms switched off](docs/screenshots/coms-off.png)

### Its own Wi-Fi (car, camping, anywhere)

On the settings page, choose **Use its own Wi-Fi**, then pick a network name and password. The board then runs its own network instead of joining yours, and The Shelf works fully on it. Join that network on your phone or laptop and open `http://10.1.1.250` (or `http://shelf-v4-xxxx.local`).

**Your phone keeps its cellular data.** The board's own network has no internet, and it says so. It hands out no gateway or DNS server, so an iPhone shows **"No Internet Connection"** under the network name. It stays joined for The Shelf (everything on 10.1.1.x) and sends everything else over cellular, so texts, iMessage, calls and apps keep working while you're connected. Android phones do the same. The only time the board acts as a sign-in page is first-time setup, for the minute or two it takes to pick your Wi-Fi. The board borrows the time from the first browser that opens it, so notes and files get the right dates. Messages with other Shelves needs your home Wi-Fi.

If the board is set to join your home Wi-Fi but can't find it at power-up, it starts its own Wi-Fi anyway and keeps looking for home.

<p align="center"><img src="docs/screenshots/settings-own-wifi.png" width="360" alt="Board settings: Use its own Wi-Fi, with network name and password"></p>

### The status light

The board's white LED (blue on a plain ESP32) shows what it's doing:

| Light | Means |
|---|---|
| Slow blink | Starting up |
| Slow, smooth fade in and out | Ready: the web page is up |
| Quick flicker | The microSD card is being read or written |
| Fast flash | Updating (firmware or web app being written) |

It goes dark while the board is powered off to save the battery. The orange light is the battery charger's own and can't be switched.

### The screen

![The board's OLED: on your Wi-Fi, on its own Wi-Fi, and at first power-up](docs/screenshots/oled-all.png)

*Left to right: on your home Wi-Fi with a Meshed Comms message showing, on its own Wi-Fi, and first power-up setup. These were drawn by the board's own screen code with the same U8g2 library and fonts, so they show what the OLED really draws, pixel for pixel.*

- **Top bar:** THE SHELF V4, SD or FLASH, an antenna icon while Meshed Comms is on, and the **battery meter** with percent (`USB` when no battery is plugged in)
- **Line 1:** the board's name, or its own Wi-Fi network's name
- **Line 2:** nearby Shelves or Wi-Fi strength (on its own Wi-Fi: the password). New Coms messages show here for 15 seconds, and long ones scroll.
- **Storage bar** with free space
- **Bottom line:** the **IP address** to open

#### Screen pages (v1.12 and later)

While it's on, the screen takes turns through these pages, about 6 to 10 seconds each (long text scrolls by once):

![The screen's pages: status, now playing, a game opened, a picture on show, watch face, clock, sunlit earth, outside temperature, today's affirmation, color of the day, CPU temperature](docs/screens/all-screens.png)

*Drawn by the firmware's own screen code with the real U8g2 library and fonts (`tools/screen-sim/render.sh`), shown on a two-color screen plugged in line on an ESP32: the bottom strip is yellow.*

- **Status:** as above. On a screen plugged in line, little arrows point at the **BOOT** (next page) and **EN** (restart) buttons beside it.
- **Now playing / Playing game / Program / Watching / Picture / Reading:** whatever was just opened on the board: a song, a game, a program, a video, a picture in the slideshow, or a book. It takes over the screen for 30 seconds (wakes it, too), with the name scrolling, then stays in the rotation for 10 minutes.
- **Watch face** with a sweeping second hand and the date, and a big **clock**. These need the time: from the internet, or from the first browser that opens The Shelf, which also tells the board its time zone.
- **Sunlit earth:** the world map with day and night as they are right now (land in daylight solid, twilight half-lit, night dim, the day/night line dotted across the oceans), the sun (a dot with rays) over the spot where it's straight overhead, and a flashing ring where the board is.
- **Outside, last 4 days:** the hourly outside temperature with a dashed freezing line (32 °F), each day's name, and the low and high. The board works out where it is from your internet address (or set it on its settings page, under Location) and fetches the hours from Open-Meteo once an hour, so this page needs a home Wi-Fi with internet. The settings page shows how many hours it has.
- **News:** the top 6 headlines (NPR, or BBC News if NPR doesn't answer), one after another, each scrolling by once. Fetched every 30 minutes on a home Wi-Fi with internet (and passed on by any browser with The Shelf open).
- **Today's affirmation** and the **color of the day**: the same ones The Shelf's Home page shows that day.
- **CPU temperature** in °C and °F, how long the board has been on, and free memory.

**Screen sleep:** by default the screen goes dark after 5 minutes (a song, game or picture opening restarts that). A short press of **PRG** (BOOT on an ESP32) wakes it, and so does **Wake** from a remote board over Coms. To keep it on all the time, set **Screen** to **Stays on** on the board's settings page. While it's on, one press stays on the current page until you press again, and two quick presses show the next page.

The **Heltec V4** card at the bottom of The Shelf's sidebar opens the board's settings page (`/board/`). There you can change Wi-Fi, rename the board, see storage, and restart it. There's also the admin password and read-only switch.

**PRG button:** a short press wakes the screen. While it's on, one press stays on the current page until the next press, two quick presses show the next page. Hold it 6 seconds and let go to forget the Wi-Fi and go back to setup mode. Keep holding to 12 seconds to clear the admin password and turn read-only off.

## More screenshots

| | |
|:---:|:---:|
| ![Home](docs/screenshots/home.png) | ![Music](docs/screenshots/music.png) |
| **Home**: bulletin board, links, weather | **Music**: artists and art from your MP3s' tags |
| ![Messages](docs/screenshots/messages.png) | ![Tidy Suite](docs/screenshots/tidy-suite.png) |
| **Messages** with your other Shelves | **Tidy Suite** |

*Screenshots use sample files and messages.*

## Power saving

On by default (board settings page, **Power**). When nobody has used the board for 90 seconds (no page open, nothing streaming or updating), the CPU drops from 240 to 80 MHz, and on a home Wi-Fi the Wi-Fi radio sleeps between the router's beacons, waking for each one (not while Coms is on, since boards hear each other over 2.4 GHz only while it listens). The first web request brings full speed straight back; the CPU temperature screen page shows the speed. On battery, the board switches itself off at 5% (for two minutes, so a brief dip doesn't count), waking every hour to look again and staying off until it's charged past 10%, or until PRG is pressed. The rest of the power goes to Meshed Comms (its radio listens all the time), the board's own Wi-Fi (it can't nap while phones may join) and the screen.

## Meshed File Transfer

Right under Meshed Comms: send a file (up to 5 MB) to a board this one hears directly. Pick the board, then the file from a little file manager (pictures as pictures, songs with their album art, PDFs, HTML games and programs with their thumbnails; double-click to open one first). The two boards agree over Meshed Comms, then move to a fast link of their own: on 900 MHz, 923.3 MHz at 500 kHz and SF7 (about 2 KB/s: a 1 MB picture in about 9 minutes, 5 MB in about 45, with less range than chat), or over 2.4 GHz when a plain ESP32 is on either end (much faster, about Wi-Fi's range). It goes in windows of 32 packets; only missing ones are sent again; a CRC-32 checks the whole file, and it lands on the same shelf on the other board. Chat pauses on those two boards while it runs. **Taking files from other boards** can be switched off.

## GPS (Heltec V4)

With Heltec's L76K GPS kit plugged into the V4's GNSS connector, the **GPS** tab (at the bottom of the tools) shows a map of where the board is and the trail it's taken (OpenStreetMap, when the browser has internet; the trail on its own when it doesn't), plus latitude and longitude, altitude, speed and top speed, heading, satellites used and in view with the accuracy, the Maidenhead grid square, sunrise and sunset, and the time from the satellites. **Download trail (GPX)** saves the trail for mapping apps. While it has a fix, the board's location (earth map, weather) follows the GPS, and its clock is set from it if nothing else has. The screen gets a GPS page too.

The GNSS connector uses GPIO 34 and 38–42, so the microSD card can't be on the in-line spot (42 · 41 · 40 · 39): wire it to CS 4, MISO 6, CLK 47, MOSI 48 instead (1.18 and later look for it there too).

![microSD wiring on a Heltec V4 with the GPS kit](docs/sd-wiring-shelf-v4-gps.png)

[The same, seen from underneath](docs/sd-wiring-shelf-v4-gps-bottom.png). The V4-R8 can't power the GPS kit (GPIO 34 belongs to its memory).

## Meshed Comms bands (Heltec V3 / V4)

A Heltec talks on two bands at once: 900 MHz LoRa (long range) and 2.4 GHz (ESP-NOW, which reaches the plain ESP32 boards). In the Meshed Comms tab, the **915.0 MHz** and **2.4 GHz** chips are switches: tap one to turn that band off or back on (one has to stay on). With 2.4 GHz off, the board's Wi-Fi can save power again while it's idle.

## Folder locks

The same folder locks as The Shelf on a computer: put chosen shelves (say Pictures) and tools (Notes, Docs, Sheets, Slides, Draw, Messages) behind a **viewing password**. Anyone else has to enter it to look, for 2 hours at a time, and locked folders are always read-only for them. On a board, the **admin** is the host: set an admin password on the board's settings page and unlock that browser as the admin, then use the **Read control** switch and **Read control: shelves…** at the top of The Shelf's sidebar (on a board, folder locks are called Read control). Card backups are admin-only while any lock is on. Clearing the admin password with the button also turns the locks off.

## Messages between Shelves

The board talks to other Shelves the same way they talk to each other: `shelf-presence` broadcasts on UDP port 41234, a network scan that asks every address on port 8080 "are you a Shelf?", and direct HTTP for messages. That's why the board answers on **port 8080** as well as port 80. Click the board in another Shelf's Messages list to chat, or choose **Connect Shelf** to open it.

## Updates over Wi-Fi

From v1.9 the board has two firmware slots. On its settings page, **Update firmware** (or `http://<board>/update`) checks the [installer site](https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/) for a newer version and installs it over Wi-Fi: the new firmware goes into the spare slot and the web app is rewritten, then the board restarts into it. Files, settings, Wi-Fi and the admin password stay. Without internet (the board's own Wi-Fi), pick the board's `-main.bin` from the installer page with **Update from a file**. The update page is built into the firmware, so it works even if a web-app update was cut off. Read-only mode blocks updates until write mode is unlocked.

> **Coming from v1.8 or earlier:** install v1.9 once over USB from the installer page. That one install changes the flash layout (adds the second slot), which clears files kept on the board's own flash; files on a microSD card are untouched.

`build.sh` also writes `docs/ota/<board>.json` plus gzipped `-app.bin.gz` and `-www.bin.gz` pieces, which is what the update page downloads.

## Plain ESP32 boards (NodeMCU-32S, ESP32-DevKitC)

The same Shelf runs on an ESP32-WROOM dev board with 4 MB of flash. Install it with **Install on ESP32** on the installer page (hold **BOOT** while connecting if the port won't open). What's different:

- **Its own Wi-Fi, no setup.** It has no screen, so it starts straight on its own network, named after the board (**shelf-esp32-xxxx**, the same as its name on the network), password `theshelf`, at **http://10.1.1.250** (phones get 10.1.1.100 and up). Joining a home Wi-Fi is optional, from its settings page. Holding BOOT 6 s puts it back on its own Wi-Fi with the default password. Every board's own Wi-Fi is named after the board, so renaming a board renames its network too.
- **Meshed Comms** instead of Meshed Comms (no LoRa radio): the same mesh, catch-up and remote admin over ESP-NOW, and Heltec boards bridge it to 900 MHz. No Bluetooth: Wi-Fi and Bluetooth share one radio and too little memory on this chip, so Bluetooth is left off and its memory goes to the web server.
- **It needs a microSD card.** The 4 MB of flash holds The Shelf (2.3 MB slot), a small update loader (`ShelfLoader/`, 384 KB) and the web app, with the top 64 KB left empty (on some boards the chip's last sector doesn't keep what the installer writes). An update over Wi-Fi saves the new firmware on the card; the board restarts into the loader, which installs it and starts The Shelf again.
- **No screen or battery meter** unless you add an SSD1306 128×64 screen on GPIO 21 (SDA) / 22 (SCL), which it finds on its own. The BOOT button does what PRG does on a Heltec (hold 6 s: forget Wi-Fi; 12 s: clear the admin password).
- Its name on the network is `shelf-esp32-xxxx.local`; the setup Wi-Fi is `The-Shelf-XXXX` as usual.

| microSD module | ESP32 pin |
|---|---|
| 3v3 | 3V3 |
| GND | GND |
| CS | GPIO 5 |
| MOSI | GPIO 23 |
| CLK | GPIO 18 |
| MISO | GPIO 19 |

![microSD wiring on an ESP32](docs/sd-wiring-shelf-esp32.png)

Wiring it with the board upside down (pins pointing up)? The rows swap places:

![microSD wiring on an ESP32, board flipped over](docs/sd-wiring-shelf-esp32-flipped.png)

**In line, one jumper.** On a 38-pin board the module can plug straight onto five header pins in a row, GND · 19 · 18 · 5 · 17, turned around so its GND lands on GND. Its 3v3 pin would land on GPIO 16, so bend that one pin up (or cut it short) and run a single jumper from it to 3V3. The firmware tries the usual wiring first, then this one (MOSI 5, CS 17), so either works.

![microSD plugged in line on an ESP32 with one jumper](docs/sd-wiring-shelf-esp32-inline.png)

The same, with the module on the bottom side of the board (seen from underneath). This is the recommended way, and the one the installer page shows first:

![microSD plugged in line on the bottom side of an ESP32](docs/sd-wiring-shelf-esp32-inline-flipped.png)

**Adding a screen:** a 0.96" I2C OLED (SSD1306, pins GND VCC SCL SDA) takes four wires: GND to GND, VCC to 3V3, SCL to GPIO 22, SDA to GPIO 21. The Shelf finds it by itself.

![0.96 inch OLED wiring on an ESP32](docs/oled-wiring-shelf-esp32.png)

**Screen in line, no wires:** on a 38-pin board the screen can plug straight onto 27 · 14 · 12 · GND on the bottom row, turned around (lying over the board). The firmware powers it from GPIO 12 and turns the picture round; it looks on 21/22 first, then here.

![0.96 inch OLED plugged in line on an ESP32](docs/oled-wiring-shelf-esp32-inline.png)

**Copying a card to another card (Windows):** [SD Card Imager](docs/SD-Card-Imager.zip) (source in `tools/sd-card-imager/`) saves a whole card to one compressed `.img.gz` and writes an image back onto a card. Unzip it and double-click *SD Card Imager.cmd*; it asks for admin rights, which Windows needs for reading or writing a whole card. Only SD cards and USB drives are listed. Moved to a bigger card? **Grow to fill card** makes the card use all its space. Features, uses and screenshots: [tools/sd-card-imager](tools/sd-card-imager/README.md), also in its own repo: [sd-card-imager](https://github.com/hardwaremack-prog/sd-card-imager).

## Backups

- **The whole board, over USB.** On the [installer page](https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/#backup), **Back up board** reads all of the board's flash (16 MB on a V4, 8 MB on a V3) into one `.bin.gz`: firmware, Wi-Fi, admin password, Coms history and any on-board files. **Restore from file** writes it back to the same board or a replacement of the same kind. Chrome or Edge on a computer.
- **The microSD card, over Wi-Fi.** On the board's settings page, **Back up card** saves every file on the card as one `.tar.gz` (7-Zip, Windows, macOS and Linux all open it). **Restore from a backup** puts the files back on this card or a new one, skipping any that are already there. Handy for moving to a bigger or newer card.

## Good to know

- **No accounts.** Anyone on your Wi-Fi can open it. Set an admin password and turn on read-only mode so only you can change things.
- Expect roughly 0.5–1 MB/s through the SPI card. Songs, photos, documents and games are quick; big videos are slow to upload.
- Weather needs the browser to share its location. Browsers only allow that on `https` or `localhost` pages, so on the board it usually stays hidden, the same as The Shelf does on a LAN.

## Building

`build.sh` packs The Shelf's current `public/` folder (cloned from [the-shelf](https://github.com/hardwaremack-prog/the-shelf), or set `SHELF=/path/to/the-shelf`) into a LittleFS image, compiles the firmware for both V4 versions, the V3 (`-DSHELF_V3`, `partitions/v3_8mb.csv`) and plain ESP32 boards (`-DSHELF_ESP32`, `partitions/esp32_4mb.csv`), and writes each as two pieces to `docs/firmware/`: `<name>-boot.bin` (bootloader and partition table, at 0x0) and `<name>-main.bin` (everything from 0xE000). Installing skips 0x9000–0xE000, where the board keeps its Wi-Fi and admin password, so a reinstall keeps them.

Needs:
- arduino-cli with **esp32 core 2.0.17**
- **ESPAsyncWebServer 3.3.23** + **AsyncTCP 3.2.14** (mathieucarbou), **ArduinoJson 7**, **U8g2**
- **RadioLib 7** (the V3/V4 LoRa radio; 7.1.2 works), and `ctags` (arduino-cli uses it for the .ino)
- for the plain ESP32 build: **ESP32-A2DP 1.8** (pschatzmann) and **ESP8266Audio 1.9**
- **mklittlefs 3.x** built with `LFS_NAME_MAX=64` to match the board. The copy bundled with the 2.0.x core breaks on paths over 32 characters (`make CPPFLAGS="-D LFS_NAME_MAX=64"`, or edit its Makefile)

`tools/pack_www.py` makes a few changes to The Shelf's page for the board:
- removes the AI and Big Transfer tabs
- adds `board-ui/board.css`, which hides computer-only buttons
- adds `board-ui/board.js`, which shows the board card and its address

Rebuild after updating The Shelf to pick up its newest page.

### Layout

```
ShelfV4/
  main.cpp         Wi-Fi + setup mode, storage, web servers (80 and 8080), screen, button
  api_files.cpp    the six shelves: list, upload, rename, move, delete, play counts,
                   thumbnails, photo editor, zipped games, Range-capable file serving
  api_tools.cpp    Home board, Notes/Docs/Slides/Sheets, Draw, Visitors, weather, settings
  messaging.cpp    Shelf-to-Shelf discovery and chat
  coms.cpp         Coms: LoRa + 2.4 GHz (ESP-NOW) mesh, catch-up, receipts, hellos, signed remote admin
  admin.cpp        admin password, read-only mode, write-unlock cookies, signing keys
  music_meta.cpp   artist, title and cover art from ID3 tags
  util.cpp         JSON bodies (spooled to the card when big), streaming responses
board-ui/          the board's settings page and the small additions to The Shelf's page
                   (midi.js: MIDI playback, synth from MIDI Vault)
tools/pack_www.py  builds the web app image
docs/              web installer (GitHub Pages) and firmware
```

Flash layout (16 MB): 3 MB app, 2 MB web app (LittleFS), about 10.9 MB storage for when no card is fitted.

# flipper-term

A **work-in-progress**, offline X11 desktop simulator inspired by Flipper Zero. It
uses Xlib for its window and draws a 128×64 monochrome framebuffer into a
clickable device-shaped case. It is **not the Flipper firmware**, a hardware
emulator, or a drop-in replacement for a device.

## Browser version / GitHub Pages

The browser edition is a separate JavaScript implementation, not the native
X11 executable compiled for the web. It is a static site: `index.html` and
`web/` can be hosted directly on GitHub Pages, without a server, Python, build
step, runtime downloads, or external libraries. To publish it, push these
files to GitHub, then in the repository's **Settings → Pages** select **Deploy
from a branch**, choose the branch containing them (usually `main`) and the
`/ (root)` folder, and save. GitHub will provide the URL once deployed. To try
it locally, open `index.html` in a browser or serve the repository root with
`python3 -m http.server`; on GitHub Pages the relative asset paths also work
under a project subpath.

Use the on-screen controls or arrow keys, Enter/Space and Escape/Backspace.
The browser simulator includes the embedded firmware-derived font and dolphin
frames, simulated device menus, Snake, tic-tac-toe, settings and simulated ID
saving. It stores settings, IDs and the Snake high score in that browser's
`localStorage`, not in the native app's `vm_sd/`; clearing site data deletes
them. If storage is blocked, it works for the current session without
persistence. The browser cannot access the native executable, host files, GPIO
or radio hardware. In particular, Bad USB script import and execution are not
implemented in the browser edition. Like the native version, the browser UI is
a work in progress, not the real firmware or an exact device replica.

## Build and run on Linux Mint

Keep `flipper-term.cpp`, `device_fonts.h` and `dolphin_frames.h` together. No
runtime downloads or third-party UI libraries are needed.

```sh
g++ --version
ls /usr/include/X11/Xlib.h
g++ -std=c++17 -O2 -Wall -o flipper-term flipper-term.cpp -lX11
./flipper-term
```

The X11 build needs a C++17 compiler and the X11 **development** headers and
linker library; they are not necessarily included in a default Mint install. If
missing, `sudo apt install g++ libx11-dev` works only if the packages are
already available locally (or via installation media). An offline machine
without those packages needs them supplied separately. An X11 session or a
working XWayland display is required. If no display can be opened, the program
prints an error and exits with status 1.

### Console framebuffer alternative

On Linux systems with `/dev/fb0`, supported packed-pixel true-color 16-, 24- or
32-bit framebuffer hardware, and the Linux kernel headers:

```sh
g++ -std=c++17 -O2 -Wall -DUSE_FB0 -o flipper-term-fb flipper-term.cpp
./flipper-term-fb
```

Use a virtual console. If permissions prevent access, configure them or use
`sudo ./flipper-term-fb` with care. This mode draws just the LCD into fb0, not
the case, and uses keyboard input; it may overwrite existing console graphics.
It does not run in an ordinary terminal emulator without a framebuffer. Running
as root can create root-owned files in `vm_sd/`.

## Controls

| Input | Action |
|---|---|
| Arrow keys | Direction pad |
| Enter / Space | OK |
| Backspace / Esc | Back |
| Mouse | Press and release on the same drawn button |
| `+` / `-` | Change X11 zoom (3×, 4×, 5×) |
| `Q` | Quit |
| fb0 only: `W`, `A`, `S`, `D` | Alternative direction keys |

The X11 animation pauses while the window is unfocused. Settings can also change
the zoom, dim timeout and beep. The factory-reset confirmation removes the
**entire** `vm_sd/` directory next to the executable, then recreates it with
defaults. Back cancels; OK confirms. Keep anything valuable elsewhere.

## What works and what is not yet faithful

- Embedded dolphin frames are the firmware's `L1_Waves_128x50` animation,
  converted from its four source PNGs; the player uses a simplified fixed
  frame order, not firmware scheduling, interactions or bubbles. **Other asset
  packs, PNGs, BAM/BM files and manifests are not loaded.**
- The primary and secondary ASCII glyphs are rasterized from the firmware's
  `u8g2_font_helvB08_tr` and `u8g2_font_haxrcorp4089_tr`. Layout, spacing,
  emoji and non-ASCII coverage are not firmware-exact.
- Boot, status bar, device artwork, menu layout and slide effects are
  approximations. The battery/Bluetooth/lock values are simulated, charging is
  not modeled, and some of the real firmware's menus and scenes are absent.
- Sub-GHz, RFID, NFC, Infrared, GPIO and iButton are UI simulations. They do
  **not** access radio hardware, scan for real signals, emulate devices, or
  transmit. Generated IDs and saved files are explicitly marked simulated and
  are **not** device-compatible file formats. Sub-GHz/Infrared saved files can
  be browsed as plain text, not decoded into functional signals. The spectrum
  is synthetic. Some menu items report “Not simulated”.
- Bad USB displays `.txt` files in `vm_sd/scripts/` or `scripts/` beside the
  executable; it never executes scripts or types on the host.
- Snake and tic-tac-toe are playable; Snake's high score, settings and saved
  simulated IDs persist in `vm_sd/` beside the executable. The executable's
  directory must be writable.
- The requested software-3D view is not implemented. The framebuffer console
  backend, X11 layout and desktop behavior have not been verified against
  physical Mint/X11 or fb0 hardware in this environment.

## Adding an app

Derive from `App`, implement `handle_input(InputEvent, long long)` and
`draw(Canvas&, long long)`, and register a label and factory in
`make_registry()`. For example, once corresponding `App` subclasses exist,
these would be registry hooks for three potential apps (not implemented):

```cpp
r.push_back({"U2F", [] { return std::make_unique<U2fApp>(); }});
r.push_back({"Archive", [] { return std::make_unique<ArchiveApp>(); }});
r.push_back({"Passport", [] { return std::make_unique<PassportApp>(); }});
```

## Test

```sh
g++ -std=c++17 -O1 -g -Wall -fsanitize=address,undefined -o /tmp/flipper-test tests/smoke.cpp
/tmp/flipper-test
node tests/web_smoke.js  # optional Node.js test for the static site
```

The smoke test exercises framebuffer drawing, settings, simulated reads and
saves, script preview, Snake and reset in a temporary directory. It does not
exercise an actual X server or fb0 device. The web smoke test runs in a mocked
DOM and does not replace testing in a real browser. The generated browser assets
can be rebuilt with `python3 tools/build_web_assets.py`.

## Sources and attribution

The embedded dolphin frames and font data were taken from Flipper Devices'
[flipperzero-firmware](https://github.com/flipperdevices/flipperzero-firmware)
commit `a8a24de3ff394f77d599ea8ba9c073398670c880`, in
`assets/dolphin/external/L1_Waves_128x50/` and
`lib/u8g2/u8g2_fonts.c`, respectively. The repository's GPL-3.0 license is
included as `LICENSE`; see the upstream file for font-specific copyright
notices. The frame order and app entry labels were informed by upstream source,
but do not imply an exact reimplementation.

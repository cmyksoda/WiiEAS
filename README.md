# WiiEAS

Live **Emergency Alert System** alerts from across the United States on the Nintendo Wii, drawn to resemble a **DASDEC** (Digital Alert Systems Decoder) emergency alert screen, the hardware used by many US cable companies — purple-blue field, red border, white monospaced text, page flips for long messages.

Data comes from the free public [GlobalEAS Central Alert Repository](https://alerts.globaleas.org/) API. Audio is the official MP3 hosted on their CDN.

> **Idea credit:** [RetrokVR](https://gbatemp.net/threads/idea-simple-eas-alert-app-channel-using-globaleas-api.682538/) floated this as a homebrew wish-list item in June 2026. This project picks that idea up; they offered to help test on hardware.

## ⚠️ IMPORTANT SAFETY WARNING & DISCLAIMER

**This app plays live, authentic Emergency Alert System (EAS) audio tones and data headers.**

*   **Do not play this audio over public speakers, microphones, or live streams.**
*   **Do not use this app near open microphones, radios, or smart home devices.**
*   **Keep your TV volume low, or use headphones.**

The audio signals streamed in this app contain unmodified SAME data codes, which if picked up by a nearby microphone, smart speaker, ham radio, or PC running decoder software, could accidentally trigger real emergency alert systems. Broadcasting these tones over the internet or over radio waves may violate federal laws and result in severe penalties.

You are solely responsible for how and where the audio is played. The author is not liable for misuse of the tones provided by this application.

## Features

- **DASDEC-style** full-screen layout
- **Active alerts** from across the United States
- **Details panel** (sent time, expiration time, alert type, callsign, originator)
- **Consistently polls** the API every 60s (same cadence as the CAR website)
- **Auto play**: if left on, new alerts will automatically play as they are retrieved

## Build

Requires [devkitPro](https://devkitpro.org/) with:

- `devkitPPC`, `libogc`, `wii-grrlib` (and FreeType/png/jpeg)
- mbedTLS for PPC is **vendored** in `libs/mbedtls/` (headers + static libs) — nothing extra to install
  
```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

Produces `boot.dol` / `boot.elf` for the Homebrew Channel.

### Install on SD

Copy `WiiEAS` folder to your `apps` folder on the root of your SD card.

## Controls/Usage

| Action            | Wii Remote (Nunchuk optional)     | Classic Controller        | GameCube   |
|-------------------|-------------|----------------|------------|
| Prev / next alert | ← → / Nunchuk stick | ← → / stick | ← → / stick |
| Page up / down    | 1 2 / ↑ ↓ / Nunchuk stick | Y X / ↑ ↓ / stick | Y X / L R / ↑ ↓ / stick |
| Details panel     | +           | +              | Start (tap) |
| Play / pause audio| A           | A              | A          |
| Toggle auto-play  | B           | B              | B          |
| Quit              | hold HOME   | hold HOME      | hold Start |

The app itself keeps one small status line on the top bezel, which stays empty unless there's something to report (downloading audio, fetch results, errors).  
A second status line is persistently shown on the bottom bezel, which shows how many alerts are currently available, which one you are on, and whether auto-play is active.

## API

Documented at https://alerts.globaleas.org/swagger/index.html:

- `GET /api/v2/alerts/active` → array of alerts  
- Each alert has `translation` (display text) and `audioUrl` (MP3)

## Notice

This is a **fan / homebrew viewer** of publicly syndicated EAS data. It is **not** an official warning device, not a substitute for NOAA Weather Radio / WEA / local broadcast, and not affiliated with GlobalEAS, Digital Alert Systems, or any government agency.

## Credits

- **GlobalEAS Central Alert Repository**, the free public API behind the alerts and their official audio ([alerts.globaleas.org](https://alerts.globaleas.org/))
- **Luxi Mono Bold** by Kris Holmes and Charles Bigelow (Bigelow & Holmes Inc.), with hinting instructions by URW++, the font used for the on-screen text. © 2001 Bigelow & Holmes Inc. Luxi is a registered trademark of Bigelow & Holmes Inc.
- **Mbed TLS** by the Mbed TLS Contributors, vendored in `libs/mbedtls/` (version 3.6.3), for HTTPS
- **libmad** by Underbit Technologies, Inc., for decoding the alert MP3s
- **GRRLIB** by The GRRLIB Team, for drawing the screen, with **FreeType** for the text
- **libogc** by Michael Wiedenbauer (shagkur) and Dave Murphy (WinterMute), and **devkitPPC** by devkitPro, with **ASND** by Hermes for sound playback

## License

The code in this repo is free software under the [GNU General Public License, version 3 or later](LICENSE). Some parts use other licenses: the font is Luxi Mono Bold (Bigelow & Holmes - Luxi License), the vendored Mbed TLS is under the Apache License 2.0 or GPL version 2 or later, and the other libraries keep their own licenses.

GlobalEAS data/audio: as published by GWES (public alert archive).

---

*This project was made with AI assistance. For more information, see [my AI usage statement](https://github.com/cmyksoda/cmyksoda/blob/main/AI_USAGE.md).*

## More controllers

On top of the Wii Remote (with Nunchuk and Classic Controller) and GameCube pads the program already reads, these work with no mapping of their own, using the same buttons as the Wii Remote:

- **USB mouse, pen tablet or uDraw GameTablet.** Left click, pen tip or uDraw pen is A, right click or the lower pen switch / rocker is B, and middle click or the upper pen switch / rocker is 2. While one is in use it replaces the Wii Remote's pointer where the program has one. Wacom, Huion, XP-Pen, Gaomon and similar tablets work; the uDraw plugs into any Wii Remote.
- **GameCube controller adapter for Wii U** (and third-party ones that copy it): its four ports read as GameCube pads, rumble aside.
- **DualShock 3 / Sixaxis** over USB, **Wii U Pro Controller** and **Wii U GamePad** (vWii): Cross is A, Circle is B, Triangle is 2, Square is 1, Start / R1 / R2 is +, Select / L1 / L2 is -, PS is Home, and the left stick presses the D-pad. These need their libraries, which `scripts/build_controller_portlibs.sh` builds into `./portlibs/ppc` (run it once before `make`); without them the program builds as before, minus those three.

The drivers are from [libwiiinput](https://github.com/quatric/libwiiinput) and live in `source/libwiiinput/`; `extrainput_compat.h` wraps the program's own `WPAD_*` and `PAD_*` calls, so no input code in the program had to change. The GameCube adapter driver is GPL-3.0-or-later (from RiftWii) and the rest AGPL-3.0, which are the terms of the files in that folder.

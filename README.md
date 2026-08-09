# WiiEAS

Live **Emergency Alert System** alerts on the Nintendo Wii, drawn to resemble a **DASDEC** emergency alert screen — purple-blue field, red border, white monospaced text, page flips for long messages.

Data comes from the free public [GlobalEAS Central Alert Repository](https://alerts.globaleas.org/) API. Audio is the official MP3 hosted on their CDN.

> **Idea credit:** [RetrokVR](https://gbatemp.net/threads/idea-simple-eas-alert-app-channel-using-globaleas-api.682538/) floated this as a homebrew wish-list item in June 2026. This project picks that idea up; they offered to help test on hardware.

## Features

- **DASDEC-style** full-screen layout
- **Active alerts** from across the United States
- **Details panel** (sent time, expiration time, alert type, callsign, originator)
- **Consistently polls** the API every 60s (same cadence as the CAR website)
- **Auto play** if left on, new alerts will automatically play as they are retrieved.

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

Copy `WiiEAS` folder to your `apps` folder.

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

- `GET /api/v1/alerts/active` → array of alerts  
- Each alert has `translation` (display text) and `audioUrl` (MP3)

## Disclaimer

This is a **fan / homebrew viewer** of publicly syndicated EAS data. It is **not** an official warning device, not a substitute for NOAA Weather Radio / WEA / local broadcast, and not affiliated with GlobalEAS, Digital Alert Systems, or any government agency.

## License

Code in this repo: GPLv3. Font is Luxi Mono Bold (Bigelow & Holmes - Luxi License).  
GlobalEAS data/audio: as published by GWES (public alert archive).

# Cardputer ADV Browser

Text-mode web browser for the M5Stack Cardputer ADV. Everything (WiFi, URLs, settings) is done on the device keyboard.

Two ways to render, switch any time with `m`:
- **Direct** - the Cardputer downloads the page and renders it itself (streaming HTML parser, HTTPS supported, no cert checking).
- **Proxy** - a tiny Wasmer Edge app fetches and simplifies the page; the Cardputer just displays it. Better for big or messy sites.

No images, JavaScript or forms. Search = type words after `g`.

## Build

**PlatformIO (local or GitHub):** `pio run -t upload`. Pushing to GitHub runs `.github/workflows/build.yml`, which builds and uploads `cardputer-browser-merged.bin` as an artifact. Flash it at offset 0x0:
`esptool.py --chip esp32s3 write_flash 0x0 cardputer-browser-merged.bin` (or any web flasher).

**Arduino IDE:** open the `CardputerBrowser/` folder (the `.ino` is empty; code is in `browser.cpp`). Install the M5Stack board package, libraries *M5Cardputer* (new enough for the ADV keyboard) + *M5Unified* + *M5GFX*, choose board **M5Cardputer** (or M5StampS3), enable **USB CDC On Boot**, 8MB flash.

## Proxy on Wasmer
```
cd proxy
# edit KEY in src/index.js (optional secret) and the package name in wasmer.toml
wasmer login
wasmer deploy
```
Then on the device: `s` > Proxy URL = `https://<your-app>.wasmer.app`, key if you set one, and `m` for proxy mode.
Test locally: `node proxy/src/index.js` then open `http://localhost:8787/r?u=https://example.com`.
(The Wasmer config follows the WinterJS template; if `wasmer deploy` complains, generate a fresh WinterJS app with Wasmer's template and drop `src/index.js` into it.)

## Keys
`; .` scroll | `Space`/`p` page | `Tab` `/` next link, `,` prev link | `Enter` open | `g` URL/search | `e` edit URL |
`b`/`Del` back | `f` forward | `r` reload | `o` home | `d` bookmark | `k` bookmarks | `w` WiFi | `m` mode | `s` settings | `h` help.
Text boxes: Enter ok, Tab clear, Fn+` cancel.

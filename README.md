# Audio Stream — Schwung module for Ableton Move

An `audio_fx` module for [Schwung](https://schwung.dev) that passes audio through
untouched and streams it over WiFi as an endless HTTP WAV stream
(44.1 kHz, 16-bit, stereo, port 8800 by default).

```
ffplay http://move.local:8800/
vlc    http://move.local:8800/
```

Status: verified on a Move (valid WAV, real-time, no dropouts, 4 simultaneous clients,
install via Schwung Manager). Items marked **unverified** below still need a device test.

## Streaming everything vs. one track
An audio FX only hears the signal of the place it is loaded:

| Where you load it | What it hears |
|---|---|
| A track slot's FX | only that Schwung slot |
| **Master FX** slot | the full mix Schwung sees |

To stream **all four Move tracks**: load the module in a **Master FX** slot **and** enable
Schwung's **Move → Schwung** audio routing (Move's per-track audio is fed in via Link Audio).
Whether this captures all four native tracks is **unverified** — see `docs/NOTES.md`.
The volume knob: **unverified** whether the stream level follows it.

## Parameters (shown on the Move screen)
| Param | Meaning |
|---|---|
| Stream | Off / On. Off closes the listener and disconnects clients |
| Latency | Normal (~370 ms backlog) / Low (~93 ms backlog, more dropouts on bad WiFi) |
| Port | Start port (default 8800); changing it rebinds live |
| Status | `:8800 2 clients`, `OFF`, `PORT 8800 IN USE`, `BIND ERR n` |
| Clients | Listeners currently streaming (max 4) |
| Drops | Times a slow listener (or the ring) had to skip ahead |

Config JSON (module config): `port`, `port_span` (default 16), `stall_ms` (default 3000).

## Server behaviour
- Serves only `GET /` (404 other paths, 405 other methods, 400 junk, 408 if no request in 5 s).
- 5th listener gets `503 Busy`. A listener that stops reading for `stall_ms` is dropped.
- A listener that falls behind skips to the newest audio (frame-aligned) instead of clicking at random.
- If the port is busy it tries the next ports (up to `port_span`), so two instances do not collide; `Status` shows the real port. If none is free it retries every second and says `PORT n IN USE`.
- The audio thread only copies samples into a lock-free ring; networking runs on a demoted worker thread off core 3.

## Low-latency playback
Most of the delay is the *player's* buffer. Examples:
```
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 http://move.local:8800/
vlc --network-caching=50 http://move.local:8800/
```
Set the module's **Latency** to Low as well. Measured end-to-end numbers: **unverified**.

## Build
Needs an aarch64 Linux cross compiler. The binary needs only **glibc ≥ 2.17**.

Linux/macOS (with `gcc-aarch64-linux-gnu`): `make` → `dist/audio-stream-module.tar.gz`

Windows (no make needed): install [Zig](https://ziglang.org/), then
```
python scripts/build.py
```
(uses `zig cc -target aarch64-linux-gnu.2.17`; set `CC` to override the compiler).
Packaging uses Python's `tarfile` with explicit modes (dirs 755, `.so` 755, `module.json` 644),
fixing the read-only-directory extract error from Windows `tar`.

`make test` runs the host-side suite (Linux only: dlopen + sockets).

## Install / remove
- Schwung Manager (http://move.local:7700) → Install Custom Module → upload the tarball; or copy
  `audio-stream/` to `/data/UserData/schwung/modules/audio_fx/` and restart Schwung.
- **Remove:** in the slot/Master FX, highlight the module and press **Delete + jog click**
  (the module page has no other exit).

`src/schwung/*.h` are vendored unmodified from [charlesvestal/schwung](https://github.com/charlesvestal/schwung) (MIT).

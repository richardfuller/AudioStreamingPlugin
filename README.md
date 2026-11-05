# Audio Stream — Schwung module for Ableton Move

An `audio_fx` module for [Schwung](https://schwung.dev) that passes audio through
untouched and streams it over WiFi as an endless HTTP WAV stream
(44.1 kHz, 16-bit, stereo, port 8800 by default).

```
ffplay http://move.local:8800/
vlc    http://move.local:8800/
```

Verified on a Move: valid WAV, real-time, no dropouts, 4 simultaneous clients, install via Schwung Manager,
stall eviction, request parsing. Items marked **unverified** still need a device test (Mix source, IPv6,
parameter page, `enabled`/port/latency changes, measured latency).

## Capturing Move's own tracks
**Tested on a Move:** putting an audio FX in the **Master FX** slot, even with *Move→Schwung* (Link Audio
routing) on, did **not** capture native Move tracks — only Schwung's own synths were heard. An FX slot or
Master FX sits on Schwung's bus; Move's native output is left untouched. So the module has two sources
(param **Source**):

| Source | Hears | Needs |
|---|---|---|
| **FX in** | whatever reaches the slot the module is loaded in (Schwung audio only, unless Schwung routes a Move track into that slot) | nothing |
| **Mix** | Move's **full mix** (native tracks + Schwung), after the master filter. All four tracks verified on a Move | Schwung's capture ring must be on (below) |
| **Auto** (default) | Mix when the ring is live, otherwise FX in | – |

**Mix** reads Schwung's capture ring (`/dev/shm/schwung-audio-live`, the same audio Skipback and the sampler record)
from the module's worker thread. Schwung only fills that ring while its **display mirror** is on:
**Schwung Manager → Config → Mirror Display** (stored as `"display_mirror_enabled": true` in
`/data/UserData/schwung/config/features.json`), then **restart Move**. It is off by default.
`Status` on the Move shows `MIX`, `FX (no tap)` (Auto fell back: ring absent), `FX`, or `NO MIX TAP`
(Mix selected but the ring is not live → turn the mirror on). `GET /status` says which of the two it is.

**Level and the volume knob.** Schwung's source says this ring is the "unity" mix, independent of the volume
knob, but on a Move the stream was measured **following the knob**. The module cannot change that: it only reads
what the shim writes. Schwung builds the ring one of two ways: with Link Audio routing active it composites the
tracks at unity; otherwise it divides Move's already-attenuated output by a smoothed copy of its *own* idea of the
master volume (capped at 50×), and that copy is only as good as Schwung's volume tracking. If that copy is stale or
stepped, the level follows the knob, and at volume 0 nothing can be recovered. To measure it, sweep the knob while
polling `GET /status` and watch `level_1s.peak_in` (the raw tap level, before this module's gain). Workarounds:
set **Gain** to compensate for a fixed knob position, or keep the knob where you want the stream level.

Caveats:
- **Unverified on a device:** the Mix source was built from Schwung's source and tested against a fake ring only.
- With no Schwung slot loaded, Schwung un-scales Move's output by 1/master-volume (max 50×) for this ring, so very low
  volume settings get noisier and volume 0 is silent. With a slot loaded the ring is exact.
- Any instance can use Mix; you only need one. Per-slot instances in *FX in* mode each hear only their slot.

## Parameters (shown on the Move screen)
| Param | Meaning |
|---|---|
| Source | Auto / FX in / Mix (see above) |
| Gain | Stream-only gain, −24…+12 dB (default 0). Does not change what you hear on the Move |
| Limit | On (default): smooth soft limiter on the stream. Below 85% full scale and at 0 dB it is bit-exact; above that it rolls off toward, but never reaches, full scale |
| Stream | Off / On. Off closes the listener and disconnects clients |
| Latency | Normal (~370 ms backlog) / Low (~93 ms backlog, more dropouts on bad WiFi) |
| Port | Start port (default 8800); changing it rebinds live |
| Status | `:8800 2 cl MIX`, `OFF`, `PORT 8800 IN USE`, `BIND ERR n` |
| Clients | Listeners currently streaming (max 4) |
| Drops | Times a slow listener (or the ring) had to skip ahead |

Config JSON (module config): `port`, `port_span` (default 16), `stall_ms` (default 3000).

## Server behaviour
- Serves `GET /` (the stream), `GET /status` (JSON) and `GET /log` (text); 404 other paths, 405 other methods, 400 junk, 408 if no request in 5 s).
- 5th listener gets a readable `503 Busy` (the request is read first, then the 503 is sent).
- Listens on IPv6 and IPv4 (dual-stack), so `move.local` works even when it resolves to IPv6 (**unverified on a device**; if VLC says "unable to open MRL", use the Move's IP address). A listener that stops reading for `stall_ms` is dropped.
- A listener that falls behind skips to the newest audio (frame-aligned) instead of clicking at random.
- If the port is busy it tries the next ports (up to `port_span`), so two instances do not collide; `Status` shows the real port. If none is free it retries every second and says `PORT n IN USE`.
- Quiet when idle: with no listener the worker sleeps in `poll()` (100 ms housekeeping), at `nice 10`, off core 3. Only streaming wakes it every 5 ms.
- Diagnostics go to Schwung Manager → System → Logs (prefix `[audio-stream]`): bind result, source, client connect/drop, rebinds. None are written from the audio thread.
- The audio thread only copies samples into a lock-free ring; networking runs on a demoted worker thread off core 3.

## Diagnostics without SSH
```
curl http://move.local:8800/status     # or http://<move-ip>:8800/status
curl http://move.local:8800/log
```
`/status` is JSON and does not use a stream slot: version, uptime, port, clients, drops, `source_in_use`
(`mix`/`fx`/`none`), `tap` (`present`, `live`, `write_pos`), gain/limiter, 1-second `level_1s`
(`peak_in`, `peak_out`, `rms_in`, `clipped_in_samples`, `limited_samples`; linear 0–1), `clipped_in_total`, a plain-English
`hint`, and the last 32 module log lines. The same log lines go to Schwung's logger, but that logger only writes while
`/data/UserData/schwung/debug_log_on` exists and it re-reads that flag by call count, so a module that logs a few
lines at startup can lose them all; `/log` is the reliable copy.

**Clipping.** If `clipped_in_samples` is non-zero, the samples were already at full scale when they left Schwung
(the shim clamps its mix), so they cannot be repaired here; **Limit** only stops this module adding more.
Lower Gain (−3…−6 dB) if you see `limited_samples` often.

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
- **Remove:** **Shift + jog click → None** on the slot, or the module's **Module** page → **Remove Module**.
  Do **not** use Delete + jog click: Move's native Delete handler also sees it and deletes the selected clip.

`src/schwung/*.h` are vendored unmodified from [charlesvestal/schwung](https://github.com/charlesvestal/schwung) (MIT).

# Audio Stream — Schwung module for Ableton Move

An `audio_fx` module for [Schwung](https://schwung.dev) that passes audio through
untouched and streams it over WiFi as an endless HTTP WAV stream
(44.1 kHz, 16-bit, stereo, port 8800 by default).

```
ffplay http://move.local:8800/
vlc    http://move.local:8800/
```

## How it works
- `process_block` copies each block into a lock-free ring buffer (no alloc, locks, logging or I/O on the SPI callback).
- A worker thread (demoted to `SCHED_OTHER`, pinned off core 3) runs a tiny HTTP server and feeds up to 4 clients. Slow clients lose data rather than ever blocking the audio thread.
- Expect ~0.3–1 s latency from TCP and player buffering; this is for monitoring/recording, not live playing.
- Config: `{"port": 8800}` in the module config; params `enabled`, `port`, `clients`, `status`.

## Important limitation
As an audio FX, the module hears only the signal of the chain/slot it is inserted in (or the master FX bus, if Schwung offers master-FX slots), not necessarily Move's whole native output. Putting it in a master FX slot is the way to stream everything.

## Build / install
```
sudo apt install gcc-aarch64-linux-gnu
make            # -> dist/audio-stream-module.tar.gz
make test       # host-side load + stream test
```
Install via Schwung Manager ("Install Custom Module", http://move.local:7700), or copy `dist/audio-stream/` to
`/data/UserData/schwung/modules/audio_fx/audio-stream/` on the device and restart Schwung.

`src/schwung/*.h` are vendored unmodified from [charlesvestal/schwung](https://github.com/charlesvestal/schwung) (MIT).

## Status
Built against the real headers and tested on the host only. **Not yet tested on a Move.** `chain_params`/UI metadata is not implemented yet.

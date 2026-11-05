# Design notes and open investigations

## Capturing Move's full mix
Device result: Master FX + Move→Schwung did **not** capture native tracks (Master FX sits on Schwung's own bus; Move's
output is untouched).

Sources read in the Schwung repo (`shadow_constants.h`, `schwung_shim.c`, `audio_live_shm.h`, `link_subscriber.cpp`):
- **`/dev/shm/schwung-audio-live`** — chosen. Public header, versioned (`AUDLV1`), single-producer ring of 16384
  stereo frames + `write_pos`, "any number of readers", with a documented lap check. Content = the capture view
  (full mix at unity, after master filter, what Skipback records); on the no-slot fast path it is Move's mailbox
  un-scaled by 1/master-volume (capped 50×). **Only written while `display_mirror` is on**
  (`features.json: display_mirror_enabled`, default false).
- `/schwung-movein` — raw copy of Move's mailbox audio for the shim; no public layout, written every block, but
  already scaled by master volume. Not used.
- `/schwung-link-in` — per-channel ring filled by the `link-subscriber` sidecar (Link Audio `Main`, tracks). Needs
  Link Audio enabled and an undocumented struct; not used.
- `overtake_fx_end_of_chain` — runs an *overtake* FX on the final Move+ME mix; it is for overtake modules, not Chain FX.
- `host_api_v1_t` exposes `mapped_memory` (the SPI mailbox, "use with care") but the fields are not a supported tap.

Open on device: confirm `Mix` carries all four tracks, check level vs. the volume knob, and whether Schwung Manager
has a toggle for the display mirror.

Not done: a global mixer combining per-slot instances (the Mix source gives the whole mix from one instance).

## Latency (issue 12)
Done: `CLIENT_BUF` backlog cap with drop-to-latest, Low latency mode (16 KB backlog, 8 KB socket
send buffer), TCP_NODELAY, documented player flags.
Not done, on purpose:
- *eventfd/condvar wake-up instead of the 5 ms poll*: waking the worker means a `write()` syscall
  from `process_block`, which Schwung's realtime contract forbids. The 5 ms poll adds at most 5 ms,
  small next to player buffering.
- *UDP/RTP or WebSocket for <100 ms*: plausible next step. RTP/UDP needs the listener to know the
  address (SDP) and has no loss recovery on WiFi; WebSocket needs a browser page. A good follow-up is
  an optional "UDP target IP:port" parameter (raw PCM or RTP L16) for ffplay/GStreamer receivers.
- Measured end-to-end latency has not been taken.

## CPU / Manager responsiveness
A report that Schwung Manager (:7700) became unreachable while the plugin was active could not be reproduced. Reduced
what the module could contribute: idle wake-ups (was every 5 ms always; now a 100 ms housekeeping poll with no
listener, 250 ms when disabled), `nice 10` on the worker, no work on core 3, diagnostics in System → Logs. If it recurs,
check Manager's CPU view and the `[audio-stream]` log lines around the time.

## Device UI (issues 10, 15)
Parameters and a `ui_hierarchy` are served from `get_param("chain_params")` / `("ui_hierarchy")`,
the documented dynamic route for audio FX. The host appends its own "My Presets" page; the module
does not implement presets (**unverified** whether "No presets" still shows on the device).

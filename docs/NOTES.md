# Design notes and open investigations

## Capturing Move's full mix (issue 11)
What the Schwung docs (ARCHITECTURE.md) say:
- A `link-subscriber` sidecar subscribes to Move's per-track Link Audio channels
  (`1-MIDI`, `2-MIDI`, `3-MIDI`, `4-Audio`, `Main`) and writes int16 audio into the
  `/schwung-link-in` shared-memory ring. The shim routes each Move track through the matching
  slot's audio FX **when Move→Schwung routing is enabled**.
- `/schwung-movein` is described only as "Move's audio for shadow processing".
- Master FX runs in-process on the shadow mix.

Recommended path (no extra code): Master FX slot + Move→Schwung on. Needs a device test:
play on all four tracks and check each is audible on the stream; repeat with the volume knob down.

Not implemented: reading `/schwung-movein` or the Link Audio `Main` channel directly from the
module. Their binary layout is not documented in the files I could read, mapping foreign
shared memory from a plugin is fragile across Schwung versions, and a Master FX already receives
the mix through `process_block`. Revisit only if the device test shows Master FX misses tracks;
then read `src/host/shadow_constants.h` and the Link Audio wire-format doc in the Schwung repo first.

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

## Device UI (issues 10, 15)
Parameters and a `ui_hierarchy` are served from `get_param("chain_params")` / `("ui_hierarchy")`,
the documented dynamic route for audio FX. The host appends its own "My Presets" page; the module
does not implement presets (**unverified** whether "No presets" still shows on the device).

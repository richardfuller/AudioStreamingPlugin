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

## Level follows the volume knob (device report)
Shim code (`schwung_shim.c`): the ring is pushed from `unity_view` (full path) or from the Move mailbox un-scaled by
`1/mv_live_smoothed` (fast path, no Schwung slot active); both clamp. `mv` is `shadow_master_volume`, read from
`Settings.json` at start and then from `move_model_sync_master_volume()` or, as a fallback, a scan of Move's OLED
volume bar. If that value is stale/stepped the tap tracks the knob. The module cannot read it (not in `host_api_v1_t`),
so it adds a static Gain and exposes `level_1s.peak_in` in `/status` to measure it. Open: confirm which path is active
(look for "Master volume:" lines in Schwung's debug log) and report upstream.

## Other
- A report of Schwung Manager being unreachable may be a DHCP change (e.g. .44 -> .45) or `move.local` resolution
  rather than the module; `/status` on the stream port is a quick way to tell whether the module is alive.
- Parameter page now lists: Stream, Source, Gain, Limit, Latency, Port, Status, Clients, Drops. Record what the
  device actually shows (and whether "No presets" appears) after the next install.

## Where the tap can clip (read from schwung_shim.c / mix_soft_clip.h)
- **Rebuild path** (Link Audio routing on, >= 4 channels): tracks, slots, overtake DSP and send returns are summed in
  int32 and converted once through `mix_soft_clip` (identity to -1 dBFS, tanh shoulder, never quite full scale).
  Schwung added that after measuring 3272 clipped samples in 30 s there. The tap `unity_view` is a copy of that mailbox
  taken before master volume. (The line-in "jack" mix just before it still hard-clamps per add.)
- **Non-rebuild path:** `unity_view[i] = clamp(native_move[i] * inv_mv + me_unity[i])` with `inv_mv = 1/smoothed_mv`
  capped at 50, then a **hard clamp** to int16. Any real Move level times a small `mv` overshoots and flat-tops, and the
  level follows `mv`.
- **Fast path** (nothing of Schwung's running): the mailbox un-scaled by the same 1/mv and clamped by `audio_live_push`.
Which path is live decides the cause; the sweep in the README distinguishes them. Reported upstream in `docs/UPSTREAM.md` #3.

## Device observation: only native Move audio follows the knob
Streamed to VLC with Mix: turning the volume knob did **not** change Schwung's own synth but **did** change the native
Move instruments. So the ME (Schwung) component reaches the tap at unity while the Move component is still attenuated.
That points at the Move-side un-scaling: either `shadow_master_volume` is stale when `inv_mv` is computed (non-rebuild
path), or Move's Link Audio track feeds are already post-volume (rebuild path). The module cannot fix this (a Gain
applies to the whole mix and the module cannot read the knob). Discriminating tests:
1. Schwung debug log (needs `debug_log_on`): "Master volume: ..." lines while turning the knob; if none appear, the value
   is not being updated.
2. Toggle Link Audio routing (Manager > Config) off/on and repeat: if natives stop following the knob in one setting,
   that setting's path is the correct one and the other has the bug.
3. `/status` `level_1s.peak_in` with only a native track playing vs. only a Schwung synth, at several knob positions.

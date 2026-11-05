# Notes to send upstream (charlesvestal/schwung)
Drafts — not filed. This session cannot post to that repository.

## 1. Bug: Delete + jog click on a loaded effect also triggers Move's native Delete
Selecting a chain FX and pressing Delete + jog click removes the module, but the same Delete press also reaches Move's
native handler, which deletes the currently selected clip. Expected: the press is consumed by the shadow UI.

## 2. Question: supported tap for Move's full mix from a module?
We stream Move's output from an `audio_fx` module. Master FX did not receive native Move tracks even with Move→Schwung on.
We now read `/dev/shm/schwung-audio-live` (needs `display_mirror_enabled`). Is that a supported interface for modules?
Would you consider (a) an always-on tap independent of the display mirror, or (b) a host capability (dlsym export) that
returns Move's pre-volume mix for Master FX? Also: does Master FX receive routed Move-track audio under any setting?

## 3. Question/bug: capture ring level tracks the volume knob
`audio_live_shm.h` says the ring is the unity mix, independent of the volume knob. On a Move the ring level followed the
knob (module reading `/dev/shm/schwung-audio-live`, Schwung with Link Audio routing on). Which path fills the ring in that
configuration (`rebuild_from_la` vs. the 1/mv path), and can `shadow_master_volume` be stale there? Also: `host->log` from a
chain FX appears only when `debug_log_on` exists, and `unified_log_enabled()` re-reads the flag by call count — lines logged
shortly after load are lost.

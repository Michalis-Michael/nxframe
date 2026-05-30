# NxFrame playout layer

The `playout/` folder contains receiver-side timing code used before frames are
sent to SDI output. It does not own transport sockets, decoders, or DeckLink
hardware. Its job is to convert decoded media timestamps into a stable playout
schedule.

## Pipeline position

```text
receiver transport -> demuxer -> decoders -> AvSyncController -> DeckLink output
```

## Files

### `av_sync_controller.h` / `av_sync_controller.cpp`

`AvSyncController` is the receiver A/V synchronization scheduler. It accepts
already-decoded `VideoFrame` and `AudioFrame` objects with media PTS values,
keeps bounded queues, and decides when enough media exists to start or continue
SDI playout.

Important responsibilities:

- convert FFmpeg timestamps to microseconds;
- maintain the active media anchor;
- align audio and video around the same media clock;
- report whether playout is waiting, locked, underrunning, or resyncing;
- avoid using transport packet arrival time as the media clock.

### `receiver_clock_policy.h`

Documents the receiver clock policy. NxFrame treats decoded media PTS as the
A/V alignment timeline and DeckLink scheduled playback as the SDI output clock.
Transport arrival timing is intentionally excluded from media synchronization.

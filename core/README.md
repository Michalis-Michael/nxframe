# NxFrame core types

The `core/` folder contains small, reusable building blocks shared by sender, receiver, encoder, muxer, output, and playout modules. These files are intentionally low-level and should not depend on application-level code.

## Pipeline position

```text
input/receiver/app modules
  -> core frame, packet, queue, metadata and stop primitives
  -> encoder/output/playout modules
```

The core layer defines ownership, timing, metadata, packet lifetime, queue behaviour, and shutdown primitives used by the live media pipeline.

## File-by-file

### `bounded_queue.h`

Thread-safe bounded queue used between live pipeline stages.

It supports:

- blocking push;
- non-blocking try-push;
- drop-oldest live-latency policy;
- drop-newest policy;
- timed push with overflow policy;
- stop/wake behaviour for clean shutdown.

For live media, `DropOldest` is usually preferable when stale data would increase latency. `Block` is useful only where preserving every item is more important than latency.

### `frame.h`

Defines `VideoFrame` and `AudioFrame`.

`VideoFrame` carries:

- shared payload ownership;
- FFmpeg-style plane pointers and line sizes;
- timestamps and time bases;
- interlace flags;
- colorimetry and HDR metadata;
- broadcast metadata sidecar.

`AudioFrame` carries shared PCM payload ownership, channel/sample metadata, and audio timestamps.

### `frame_pool.h`

Small shared byte-buffer pool used by hot-path media stages. Outstanding buffers keep the old pool state alive safely, so the pool can be reset without invalidating already queued frames.

### `metadata.h`

Broadcast metadata sidecar structures for SMPTE timecode and ANC/VANC packet foundations.

This file intentionally stays focused on SDI/broadcast contribution metadata. Private/proprietary metadata and non-broadcast workflows should not be added here unless they become an explicit NxFrame feature.

### `packet_item.h`

RAII and pooling wrapper for FFmpeg `AVPacket` objects.

Encoded packets move through queues as `AVPacketPtr` so packet lifetime is explicit and hot-path allocation is reduced.

### `pipeline_telemetry.h`

Shared atomic counters for live diagnostics:

- input/encoded frame rates;
- queue depth and peak queue depth;
- push failures and dropped frames;
- mux/send failures;
- recovery/keyframe gating;
- backpressure drops;
- timestamp repairs;
- idle counters.

Telemetry is observational. Relaxed atomics are used because exact synchronization is not required for status output.

### `stop_token.h`

Small cooperative stop flag used by worker loops. It provides deterministic shutdown without introducing a larger threading framework.

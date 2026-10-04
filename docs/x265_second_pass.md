# HEVC wrapper second pass — FFmpeg 8.1 / x265 4.2+18

Based on nxframe(20261002-200728).zip. Test with the existing superfast,
1080p50, 10-bit 4:2:2, 35 Mbps preset. This patch leaves its encoder search,
quality, lookahead, VBV and thread defaults to the preset and current options.

## Changes

- Native shared input no longer allocates an unused pixel-copy working buffer
  at startup (roughly 8 MiB at 1080p). The borrowed-pointer compatibility path
  allocates that buffer on first use; ownership/copy safety is preserved.
- A dedicated receive AVPacket is reused. The output packet pool is touched
  only when FFmpeg returns a real packet, avoiding acquire/release mutex work
  on empty/EAGAIN receives. Packet references are moved, never payload-copied.
- Contiguous input allocation size is cached at initialization. Per-frame plane
  bounds/stride validation and shared AVBufferRef ownership remain in place.
- Scoped frame cleanup releases the input wrapper on every exit, including
  prepare/metadata/submit failure paths.
- Missing PTS and odd-address 10-bit inputs are rejected. Positive/reference
  bounds and gop.closed types are validated before allocating working state.
- threads is treated as the legacy frame-threads alias. Conflicting aliases
  are rejected; frame_threads is also recognized. This build accepts 0 (auto)
  or 1..15 frame threads. Pool worker counts are separate from frame threads.

## Timing and latency

The current preset is fast, but an encode API-call duration is not the latency
of an individual picture. Matching x265 source/logs confirm superfast defaults
to lookahead depth 10; B-frames=0 does not remove that lookahead. In the synthetic
benchmark, first output arrived after 16 submitted inputs with auto threading.
Do not infer 3–5 ms end-to-end latency from the dashboard's encode-call timings.
The same preset settings and first-output buffering remain available here.

Three alternating 150-frame 1920x1080 tests on this shared server returned
150 packets in every run. Median of the three wrapper-overhead measurements,
excluding FFmpeg send/receive calls, was 15.6 us before and 11.9 us after.
The delivered harness later measured 15.4 us after, showing host/timing noise.
These are microsecond-scale measurements, not proof of a material codec-speed
increase. No production throughput or CPU-percentage gain is claimed.
Codec-call tails were variable; these tests are synthetic, unpaced moving
ramps, not SDI footage or the user's Ryzen 7 9700X. The allocation/pool-operation
reductions are concrete, but their live impact must be measured on that machine.

## Validation

Strict C++14 compilation and regression tests pass with FFmpeg 8.1 and the
matching x265 commit 6fdfffe8d. Tests cover native/converted output, padded
planes, borrowed/shared inputs, delayed B-frames, timestamp rescaling, captions,
forced IDRs, HLG/PQ static HDR metadata, flushing and invalid inputs/options.
A new regression holds 48 packets across repeated receives/flush to catch
scratch-packet aliasing, and verifies preset-only superfast adds no speed or
thread overrides. AddressSanitizer/UndefinedBehaviorSanitizer tests pass;
LeakSanitizer is disabled because this environment cannot inspect /proc tasks.
Full application/DeckLink production validation is still performed on the
broadcast machine. Packet-pool lifetime contracts are unchanged.

## Optional benchmark

The benchmark target is excluded from normal builds and CTest:

```bash
cmake --build build --target bench_encoder_x265 -j"$(nproc)"
./build/bench_encoder_x265
./build/bench_encoder_x265 /path/to/your_superfast_preset.json 150
```

Default: the provided superfast-only video configuration, 150 frames. An input
preset path and count (10..10000) are optional. Frames are generated outside the
timed region and independently owned. Results include submit-call average,
median/p95/max, first-packet input count, flush time and process peak RSS.
This is an isolated encoder benchmark, not an SDI, mux, transport or receiver test.

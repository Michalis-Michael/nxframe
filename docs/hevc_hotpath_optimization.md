# HEVC hot-path optimization and controlled measurements

Patch baseline: `nxframe(20261002-203621).zip`. C++14; FFmpeg 8.1;
x265 4.2+18-6fdfffe8d, including its 10-bit library.

## Changes

- Dashboard state updates no longer share a mutex with terminal writes or
  formatting. Rendering snapshots state/events, then releases their lock.
  A separate output mutex serializes rendering, raw redirected output, navigation
  and shutdown. TTY log events bypass that output mutex and only update the
  event queue, so they can proceed while terminal writes are blocked. The layout, alternate terminal buffer and one-second refresh stay
  the same. Capture formats display strings outside the state lock.
- Capture timing handles are registered once per call site instead of looking
  them up under the registry mutex every frame, even with timing disabled.
- The sender reuses an output packet vector for HEVC. The existing vector-return
  and single-packet compatibility APIs remain available. The new output overload
  clears previous results before each call, including invalid input calls.
- `additional_options.asm` accepts `auto`, `avx2`, or `avx512`. `auto` leaves the
  codec's defaults untouched. Explicit modes are checked against FFmpeg CPU/OS
  capabilities before forwarding them to x265. `avx512` invokes x265's own checked
  AVX-512 detection; inspect its `using cpu capabilities` startup line to confirm
  what the linked x265 build actually enables. Unsupported requests fail clearly.
- Shared AVX2/AVX-512 row pools cannot have their job pointers overwritten by
  simultaneous capture instances. A second caller runs the serial SIMD kernel
  instead of waiting for the current job. This is a correctness improvement;
  concurrent streams may warrant per-device pools after measurement.
- `x265_convert` measures the actual libswscale call separately. It is absent
  from the native 10-bit 4:2:2 path because no chroma/bit-depth conversion runs.

No preset file is edited. No tune/lookahead override is introduced. The current
superfast baseline remains the reference. Input AVBufferRef/shared_ptr ownership
and x265's internal input copying remain intact. Small ownership allocations are
still present; removing them requires a separate lifetime design, not borrowing
pixels across asynchronous encoding. No new custom chroma kernel is introduced:
libswscale already dispatches SIMD, and this preset bypasses conversion entirely.

## Build and regressions

From the repository root, with the existing build configuration:

```sh
cmake --build build -j"$(nproc)"
ctest --test-dir build -R '^(encoder_x265|sender_dashboard|v210_scalar_vs_avx2)$' --output-on-failure
cmake --build build --target bench_encoder_x265 -j"$(nproc)"
```

The benchmark target requires `NXFRAME_BUILD_TESTS=ON` and
`NXFRAME_BUILD_FFMPEG_TESTS=ON`. It remains excluded from normal builds and CTest.

## Benchmark your working preset

`-` selects the built-in 1920x1080p50, 35 Mbps, superfast, main422-10, GOP 50,
B-frames 0 baseline with no tune or additional options. Substitute the path to
**your actual working superfast preset** to test that file; repository presets
with the same descriptive filename may have different settings.

```sh
./build/bench_encoder_x265 - 500 --realtime
./build/bench_encoder_x265 - 500 --realtime --asm=avx512
./build/bench_encoder_x265 - 500 --realtime --frame-threads=2 --pools=8
NXFRAME_V210_THREADS=0 ./build/bench_encoder_x265 - 500 --realtime --unpack=avx512
./build/bench_encoder_x265 - 500 --realtime --output=42010
```

The v210 fixture supports widths divisible by six. Normal encoder calls have no
such extra restriction. `--output=42010` and `--output=4208` are separate format
experiments with corresponding HEVC profiles; they do not preserve 4:2:2 quality.
Each fixture includes a 100-frame repeating content pattern. It is synthetic,
not a substitute for sports footage, noise, graphics or the original loop scene.

To run all experiments sequentially, three repeats in shuffled order:

```sh
python3 scripts/bench_hevc_matrix.py --binary ./build/bench_encoder_x265 --preset - --frames 500 --repeats 3 --results hevc_benchmark.jsonl
```

On a 16-logical-CPU machine this is 15 cases x 3 repeats, roughly 7.5 minutes plus
startup/drain. Cases compare the unmodified baseline, x265 assembly, frame/pool
threads, unpack AVX2/AVX-512 with 0/1/2 helpers, and 4:2:0 conversions. Restrict the
run using `--groups asm,threads` or `--groups unpack`. Pool sizes are experiments
based on process affinity, not recommended production settings. Each subprocess
creates a fresh unpack pool so `NXFRAME_V210_THREADS` takes effect. Existing
CPU profiles and machine load must remain comparable throughout the run.

The script writes JSONL measurements and a companion codec log. Unsupported SIMD
cases are marked `unsupported`; other failures cause a nonzero exit. It overwrites
those result paths when explicitly run. It never modifies the input preset.

## Interpret the measurements

- Encode-call p50/p95/p99/max are API wall time. They include waiting for work from
  previously submitted frames; they do not establish picture latency by themselves.
- `input_to_packet_p95_ms` matches output PTS to the timestamp immediately before
  unpack/submission. It includes codec buffering and unpack, excludes SDI capture,
  MPEG-TS, SRT and receiver latency. The short flush tail runs without pacing.
- `late_inputs_gt1ms` counts synthetic source deadlines missed by more than 1 ms.
  Generation and scalar fixture packing are outside the measured stages but can
  cause these misses. This is not a production capture-drop counter.
- `fixture_throughput_fps` includes fixture creation, pacing and drain. In paced
  mode it should be near or below the configured rate; it is not maximum capacity.
  Omit `--realtime` for an unpaced synthetic capacity experiment.
- `conversion_calls` must be zero for native 4:2:2 10-bit output; conversion timing
  measures only `sws_scale`, not frame allocation or preparation.
- Unpack timing includes helper wake/wait overhead. It runs on the submitting
  thread while x265 workers may be active; hardware capture uses another thread.
- Wrapper overhead excluding FFmpeg calls is diagnostic, noisy at microsecond
  scale, and includes conversion and instrumentation. Peak RSS is process-wide.

Select candidates from repeated results, then validate them with actual DeckLink
capture, sustained output cadence, capture drops, quality and end-to-end latency.
Do not combine every apparently faster setting without retesting the combination.

## Validation scope

The isolated regression suite uses the matching FFmpeg/x265 libraries. It covers
conversion, native/padded input ownership, captions, IDR, timestamps, delayed output,
flush, supported SIMD modes, packet-vector reuse and a blocked output stream that
must not block dashboard state updates. SIMD reference tests also exercise
simultaneous callers. Strict C++14 builds and AddressSanitizer/UndefinedBehaviorSanitizer
checks pass in the isolated wrapper suite. CMake/CTest dashboard and SIMD
regressions pass, as do pseudo-terminal checks with piped stdout and deliberate
terminal backpressure. LeakSanitizer is disabled in this execution
container due to its `/proc` restrictions. No hardware SDI/SRT test or complete
NxFrame application link is claimed here; production performance improvements
must be measured on the target machine.

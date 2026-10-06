# x265 wrapper rework

Applies to the NxFrame snapshot dated 2026-10-02 06:01:59. The application-facing
encoder API is retained. Preset bitrates, quality presets, GOPs and thread pools
are not retuned. This changes the wrapper around x265, not x265's compression algorithms.

## Changes

- Shared native-format input is retained through a read-only AVBufferRef holding
  its shared_ptr. Converted input goes directly from validated source planes to
  the reusable output frame. The previous intermediate pixel copy is removed.
- Conversion no longer allocates an unused full-resolution input pixel buffer.
  At 1920x1080, the internal 10-bit 4:2:2 bus occupies 8,294,400 bytes per frame.
  Avoiding one copy at 50 fps removes 414.72 MB/s of copied payload, consisting
  of both a read and a write. This is not an end-to-end FPS guarantee.
- Dimensions, integer cadence, GOP, profiles, output format, rate-control modes,
  supported options and input plane bounds are checked before use. Initialization
  is one-shot and repeated successful initialization is idempotent.
- Floating-point options reach x265, including the existing vbv-init=0.75 and
  fractional CRF. GOP scenecut is honored. Additional supported controls include
  strict-cbr, wpp, ctu, aq-strength, psy-rd and psy-rdoq.
  The supplied numeric vbv-init previously fell back to x265's default; applying
  0.75 intentionally changes that startup rate-control behaviour.
- Submission drains and retries on EAGAIN. Flush is idempotent, failed codecs
  stop accepting input, compatibility APIs retain extra packets, and failed
  preparation/submission preserves a pending keyframe request.
- Explicit keyframe requests use FFmpeg's forced-idr option. A/53 captions are
  attached to the submitted frame, cleared on reuse, and explicitly enabled.
- VideoFrame timestamps are rescaled from time_base into the encoder clock.
  Preset VUI remains authoritative for the stream's static color signalling.

## Deliberate compatibility limits

Interlaced presets and interlaced input are rejected. The old wrapper passed a
whole woven picture to field-oriented x265 encoding. Correct support requires
field splitting, field-safe chroma conversion, timing and receiver handling.
The existing x265_1080i50 preset remains in the tree but cannot initialize this wrapper.

Supported output profiles are main (8-bit 4:2:0), main10 (10-bit 4:2:0) and
main422-10 (10-bit 4:2:2). An explicit incompatible profile is rejected rather
than silently replaced. Cadence remains integral; fractional cadence needs a
separate rational-rate change throughout the sender.

Supported rate_control values are cbr, abr, vbr and crf. The first three use
x265's ABR controls with the configured VBV; the label cbr alone does not imply
constant transport bitrate or filler. strict-cbr and nal-hrd are explicit controls.
QP mode and arbitrary x265 CLI passthrough are not supported. Unknown options,
duplicate aliases and conflicting positive/negative boolean options are rejected.
The deblock option supports boolean values in this wrapper.

HDR10 mastering/content-light SEI comes from static preset parameters. Per-frame
HDR side data is retained but this patch does not promise dynamic HDR updates.
Colour primaries, transfer and range must match the input signal; changing VUI
does not perform SDR/HDR or gamut conversion.

Encode/flush calls must be serialized. requestKeyFrame is safe across threads.
Shared input pixels must remain immutable while references exist. The legacy
raw-pointer API assumes a sufficiently large contiguous buffer. Returned pooled
packets must be released before destroying the encoder, as in the existing API.

## Validation

Standalone regression tests compile with -Wall -Wextra -Wpedantic -Werror and
run against FFmpeg 8.1 and x265 revision 6fdfffe8d (4.2+18), GCC 13.3.0, using
an isolated 8/10/12-bit build. Its 12-bit assembly was disabled to resolve
unavailable assembly symbols; tested encoder paths use 8-bit and 10-bit builds
with assembly enabled. This is not a copy of the production dependency build.

Tests cover actual HEVC encode/decode, native 4:2:2, converted 4:2:0, padded
strides, immediately released caller buffers, captions without carryover,
forced IDRs, B-frames/lookahead, both legacy APIs, timestamp rescaling,
fractional CRF, HLG/PQ VUI, static HDR10 SEI, rejected inputs/presets and repeated
flush. All five supplied progressive presets initialize at their configured
resolution; the interlaced preset is rejected. This is not a full application,
SDI/network integration, sustained production or picture-quality validation.

AddressSanitizer and UndefinedBehaviorSanitizer pass for the wrapper/test code
on that stack. Dependency libraries were not instrumented. LeakSanitizer could
not run under the execution environment's process tracing, so leak detection
was disabled; no claim of a leak-free dependency stack is made.

An additional synthetic benchmark encoded 150 repeated 1920x1080 frames in each
of three paired runs, using ultrafast/zerolatency, pools=2, frame-threads=1 and
the same effective options on both wrappers. Before/after encoded packet hashes
matched in every run. Median wall times were:

| Path | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Converted 8-bit 4:2:0 | 2.66715 s | 2.61848 s | 1.8% |
| Native 10-bit 4:2:2 | 2.57883 s | 2.55934 s | 0.8% |

Run-to-run variation was larger than these median gains. This benchmark does
not establish a reliable production speedup, quality comparison or motion-scene
capacity. Measure real SDI material on the production CPU before retuning presets.

Build the new test in your configured project build, then run:

```sh
cmake --build build --target test_encoder_x265
ctest --test-dir build -R '^encoder_x265$' --output-on-failure
```

The test returns skip code 77 if FFmpeg has no libx265 encoder. To check preset
initialization too, pass the desired JSON preset paths to test_encoder_x265.
Repeat the established production soak and recovery tests after applying the patch.

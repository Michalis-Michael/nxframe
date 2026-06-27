# NxFrame tests

The `tests/` folder contains small regression tests that protect important
runtime contracts without requiring the full appliance to run.

## Test groups

Pure tests can build without FFmpeg, SRT or DeckLink. FFmpeg-backed tests verify
muxer, demuxer and playout behavior when development dependencies are available.

## Files

- `test_v210_scalar_vs_avx2.cpp` verifies that the AVX2 v210 unpacker matches
  the scalar reference implementation.
- `test_signal_generator_layout.cpp` verifies the generated `YUV422P10LE` plane
  layout used by the test input source.
- `test_bounded_queue_policy.cpp` verifies live queue overflow behavior.
- `test_av_sync_controller_lock.cpp` verifies receiver A/V lock behavior.
- `test_demuxer_ts_health.cpp` verifies basic MPEG-TS health counter behavior.
- `test_muxer_session_anchor_state.cpp` verifies muxer timestamp/session anchor
  reset behavior.

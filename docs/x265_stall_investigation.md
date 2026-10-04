# HEVC submission stalls: comparison and replay

The x264 and x265 wrappers both retain reference-counted native input and use
FFmpeg send/receive. Neither requires chroma conversion for native 422 10-bit
output. The x265 wrapper preserves preset defaults unless explicitly overridden;
automatic threading is not changed by this patch. The underlying codecs have
different buffering, CPU demand and scheduling. Similar API-call averages do
not mean similar total CPU work or worst-case picture delivery.

Production logs show long libx265 submission calls while packet enqueueing,
muxing and SRT remain fast. This patch deliberately adds evidence rather than
claiming that wrapper cleanup will eliminate library/worker stalls. It does not
change quality, presets, lookahead, GOP, threading, capture buffering or input
ownership. The library's default picture copy remains enabled.

## Additional timing evidence

With timing enabled, the first packet drained after a successful non-flush
submission classifies that submission's wall duration into:

- `x265_key_output_submit`: call followed by a key packet.
- `x265_inter_output_submit`: call followed by a non-key packet (including B
  pictures when enabled).
- `x265_submit_caller_cpu`: CPU time spent by the submitting thread for these
  calls. This excludes x265 worker-thread CPU; it is NOT total encoder CPU time.

These appear in the diagnostic file's normal interval timing records. As with
existing timings, averages/counts are interval values; maxima are session values.
Flush calls are excluded from these three groups. The submitting frame PTS and
returned packet PTS can differ because x265 buffers pictures. Group names describe
the output associated with an API call, not the total encoding time of that
picture. Calls with no returned packet have no output classification. The
existing `x265_send_frame` timer still covers all submission calls.

Over-budget calls also emit `[EncoderX265][DIAG]` with wall/caller-CPU time,
submitted/output PTS, output key flag and packet bytes. These are retained in the
separate sender diagnostic log without requiring verbose terminal details.
Large wall time with small caller CPU can indicate waiting or descheduling;
combine it with process/system CPU and worker evidence. It cannot distinguish
all internal x265 waits by itself.

## Benchmark with actual content

Build the optional benchmark:

```sh
cmake --build build --target bench_encoder_x265 -j"$(nproc)"
```

Replay a tightly packed little-endian planar `yuv422p10le` file with dimensions
matching the preset. Input files contain Y, U, V planes per frame, with two bytes
per sample. The codec's configured frame rate determines replay cadence. The
file loops at EOF; partial trailing frames are rejected. Example from `build`:

```sh
./bench_encoder_x265 ../preset/04_hdr_wcg/x265_1080p50_10bit_422_pcm_test.json 3000 --realtime --input-yuv422p10=/tmp/hevc_test_42210.yuv
```

A local video file can be converted to this raw format using FFmpeg. Match the
preset's dimensions/frame rate and preserve its intended colour characteristics.
Disk reads happen outside measured encode calls; wall throughput and late-input
counts still include disk/fixture overhead. Use local fast storage and inspect
late-input counts. This benchmark bypasses DeckLink, queues, muxing and SRT.
Do not run a competing decoder during an encoder-isolation benchmark.

Results now include actual compressed video Mbps, output bytes, key packet count,
source type/frame count and the three diagnostic groups. Synthetic ramp input
remains the default for compatibility. Its low bitrate/easy content is not a
production performance estimate. Real-content replay provides a stronger check
of whether the same stalls occur without the live pipeline.

Regression coverage checks timing enabled/disabled yields byte-identical output
under a deterministic single-thread test preset, preserves PTS/order and excludes
flush from key/non-key classification. Existing encode/decode, ownership,
conversion, metadata and option-validation tests remain in place.

# NxFrame receiver layer

The `receiver/` folder contains transport ingest, MPEG-TS demuxing, decoding and
receiver-side media queues. SDI output scheduling itself lives in `output/`, and
A/V playout timing lives in `playout/`.

## Pipeline position

```text
SRT/UDP input -> DemuxerTS -> DecoderVideo/DecoderAudio -> Receiver media queues
```

## Files

### `receiver.h` / `receiver.cpp`

`Receiver` orchestrates the live receive pipeline. It starts the selected
transport, feeds MPEG-TS data to the demuxer, starts decoders when streams are
available, and exposes decoded video/audio frames to the playout layer.

### `srt_input.h` / `srt_input.cpp`

SRT transport input with caller, listener and rendezvous modes. It owns the SRT
socket lifecycle, reconnect behavior and received TS packet queue.

### `udp_input.h` / `udp_input.cpp`

UDP/RTP transport input. It owns the socket, optional multicast join, reconnect
state and received packet queue.

### `demuxer_ts.h` / `demuxer_ts.cpp`

MPEG-TS demuxer built on FFmpeg. It converts incoming TS bytes into demuxed
`AVPacket` objects, tracks stream snapshots and exposes lightweight health
counters for receiver recovery logic.

### `decoder_video.h` / `decoder_video.cpp`

Video decoder. It receives demuxed video packets, decodes with FFmpeg, converts
or copies decoded frames into NxFrame `VideoFrame` objects, and publishes them to
a bounded decoded-video queue.

### `decoder_audio.h` / `decoder_audio.cpp`

Audio decoder and resampler. It receives demuxed audio packets, normalizes audio
into the configured receiver output format, and publishes decoded `AudioFrame`
objects.

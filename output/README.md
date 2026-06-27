# NxFrame output subsystem

The `output/` directory contains the sender transport path and the receiver SDI playout path. Its job is to take already-encoded packets or already-decoded receiver frames and deliver them to the selected external output.

There are two main runtime paths:

```text
Sender path:
Encoded video/audio packets
  -> MuxerTS
  -> MPEG-TS chunks
  -> SRT, UDP, or RTP transport
```

```text
Receiver playout path:
Receiver decoded frames/audio
  -> AV sync controller
  -> DeckLinkOutput
  -> v210 SDI video buffer + embedded audio
  -> DeckLink scheduled playback
```

The output subsystem does not perform video encoding or audio encoding. It owns muxing, network transport, and SDI output scheduling.

## File overview

### `output_manager.h` / `output_manager.cpp`

High-level coordinator for all output modes.

Main responsibilities:

- load output-related settings from the active preset;
- configure MPEG-TS service metadata;
- initialize the `MuxerTS` instance from encoder codec contexts;
- choose sender transport: SRT, UDP, or RTP;
- run the sender output thread that drains encoded packet queues;
- coordinate transport reconnect/recovery behavior;
- initialize and run DeckLink receiver playout.

`OutputManager` is deliberately a coordinator. It should not perform codec work, packet parsing, or SDI frame conversion directly.

### `muxer_ts.h` / `muxer_ts.cpp`

MPEG-TS muxer built around FFmpeg `libavformat` with a custom memory-backed AVIO callback.

Main responsibilities:

- clone video and audio codec parameters from the encoder layer;
- create MPEG-TS streams with stable service metadata;
- normalize packet timestamps to a live-session base;
- repair missing DTS/PTS pairs where safe;
- mux encoded video/audio packets into MPEG-TS;
- expose ready MPEG-TS chunks from a low-allocation chunk pool;
- optionally capture the generated transport stream to disk for analysis.

This module should receive encoded `AVPacket` objects only. Raw frame processing belongs in the input, encoder, receiver, or playout modules.

### `srt.h` / `srt.cpp`

SRT transport wrapper around libsrt.

Main responsibilities:

- initialize caller, listener, or rendezvous SRT sockets;
- apply latency, buffer, bandwidth, encryption, stream ID, and timeout options;
- handle reconnect attempts and external stop requests;
- send MPEG-TS payloads using the SRT message API;
- expose runtime connection state and periodic statistics.

This module does not know about codecs or MPEG-TS internals. It only receives byte payloads from the sender path.

### `udp_streamer.h` / `udp_streamer.cpp`

UDP and RTP/MP2T transport sender.

Main responsibilities:

- create a UDP socket for unicast or multicast output;
- configure multicast TTL and loopback behavior;
- optionally packetize MPEG-TS payloads into RTP with payload type 33;
- optionally pace datagrams to reduce burst pressure on receivers and NIC buffers;
- expose transport state and last-error reporting.

`payload_size` refers to MPEG-TS payload bytes. In RTP mode, the fixed 12-byte RTP header is added on top.

### `decklink_output.h` / `decklink_output.cpp`

DeckLink SDI playout implementation for the receiver side.

Main responsibilities:

- open and configure a DeckLink output device;
- select output display mode from the receiver video format;
- maintain a pool of DeckLink output frames;
- convert NxFrame internal `YUV422P10LE` receiver frames into DeckLink v210 output buffers;
- generate black fallback frames during source loss;
- schedule video frames with DeckLink playback timestamps;
- schedule embedded SDI audio using an audio cadence helper;
- track callback completion and recover from schedule pressure.

This path is not zero-copy from decoded receiver frames to SDI. The final SDI device buffer must be v210, so NxFrame converts the internal planar 10-bit frame directly into the DeckLink output buffer before scheduling it.

### `sdi_audio_cadence.h`

Header-only helper for SDI embedded audio pacing.

Main responsibilities:

- calculate how many 48 kHz audio samples belong with each video frame;
- support common integer frame rates such as 25, 50, 30, 24, and 60 fps;
- support fractional rates such as 30000/1001, 24000/1001, and 60000/1001 through repeating cadence patterns;
- provide a frame-index to audio-sample position mapping.

### Removed unused implementation stubs

`output/sdi_audio_cadence.cpp` was only an empty include-only translation unit. The helper is implemented inline in `sdi_audio_cadence.h`, so the empty `.cpp` file can be removed and omitted from the build.

## Ownership and threading notes

- `MuxerTS` owns FFmpeg muxing state and pooled output chunks.
- `SRTStreamer` and `UDPStreamer` own transport sockets.
- `DeckLinkOutput` owns DeckLink COM objects, output frame pool state, and callback synchronization.
- `OutputManager` owns the sender output thread and performs lifecycle coordination.

The sender hot path should remain:

```text
encoded packet queue -> muxer -> byte chunks -> transport
```

The receiver SDI path should remain:

```text
receiver decoded frame/audio -> sync controller -> DeckLink scheduling
```

Avoid mixing codec conversion, muxing, and transport responsibilities in one class. Keeping these boundaries clear makes release testing, latency tuning, and future output modules safer.

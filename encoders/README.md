# Encoders

The `encoders/` folder contains the codec-facing part of the NxFrame sender pipeline. It receives normalized video and audio frames from the sender workers, applies the selected preset, and returns encoded packets for muxing/output.

## Pipeline role

```text
InputManager / sender workers
  -> EncoderManager
  -> video encoder: EncoderX264 or EncoderX265
  -> audio encoder legs: EncoderAAC or EncoderPCM
  -> encoded packet queues
  -> OutputManager / MuxerTS / transport
```

`EncoderManager` is the public coordinator. The rest of the application should normally talk to `EncoderManager`, not directly to a specific codec class.

## Video format contract

The production sender path is built around the internal video bus format:

```text
AV_PIX_FMT_YUV422P10LE
10-bit planar 4:2:2
```

For x264, this allows the current zero-copy-oriented path where the encoder wraps the existing `VideoFrame` buffer and keeps the shared owner alive through FFmpeg's `AVBufferRef` lifetime mechanism.

If a preset requests a different output pixel format, the encoder may need a conversion step. That is not zero-copy and should be treated as a compatibility path rather than the main low-latency broadcast path.

## Audio format contract

Audio arrives through `AudioFrame` and is routed by preset configuration.

- `EncoderAAC` is for compressed AAC contribution audio.
- `EncoderPCM` is for PCM/SMPTE ST 302M-style MPEG-TS carriage and protected passthrough-style routing.

Both audio encoders may internally buffer samples because audio codecs and TS carriage require fixed-size packetization that does not always match incoming callback sizes.

## File overview

| File | Purpose |
| --- | --- |
| `encoder_manager.h/.cpp` | Preset-driven coordinator for video and audio encoder instances. Owns the selected codec objects and exposes the sender-facing encode API. |
| `encoder_x264.h/.cpp` | H.264 encoder implementation using FFmpeg/libx264. Contains the main low-latency 10-bit 4:2:2 path and packet-drain logic. |
| `encoder_x265.h/.cpp` | HEVC encoder implementation using FFmpeg/libx265. Supports the same manager-facing API, with more conservative frame ownership because HEVC lookahead/reference behavior may retain input longer. |
| `encoder_aac.h/.cpp` | AAC audio encoder with channel mapping, resampling, FIFO buffering, discontinuity handling, and packet drain support. |
| `encoder_pcm.h/.cpp` | PCM/ST 302M-style audio path with channel mapping, protected routing validation, fixed packet cadence, and packet drain support. |

## Ownership rules

- Encoded packets returned as `AVPacketPtr` are owned by the caller through the packet wrapper/pool contract.
- Raw `AVPacket*` audio packets returned by the AAC/PCM classes are transferred to the caller and must be released after muxing.
- `VideoFrame` and `AudioFrame` input buffers remain owned by the pipeline frame objects. Encoders must not keep raw pointers past the lifetime they explicitly retain.
- Zero-copy video is only safe when the encoder attaches an owner object to the `AVFrame` buffer reference.

## Notes for contributors

- Keep preset parsing defensive. Missing or invalid fields should fail with clear logs or fall back only where the behavior is explicitly intentional.
- Do not hide expensive format conversion in the hot path without logging it clearly.
- Keep codec-specific behavior inside the codec class and keep application orchestration inside `EncoderManager`.
- Avoid adding low-level tuning knobs to user-facing configuration unless they are required for broadcast operation.

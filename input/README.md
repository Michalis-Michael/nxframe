# NxFrame input subsystem

The `input/` directory contains the capture and generated-source side of the NxFrame sender pipeline. Its job is to normalize every supported video source into the internal NxFrame video bus and push timestamped `VideoFrame` / `AudioFrame` objects into the live sender queues.

Current primary production path:

```text
DeckLink SDI input
  -> Blackmagic callback thread
  -> v210 / UYVY access from the DeckLink frame
  -> normalized internal bus: AV_PIX_FMT_YUV422P10LE
  -> shared VideoFrame buffer
  -> sender video queue
  -> encoder worker
```

The input subsystem intentionally keeps the sender-facing video format simple: planar 10-bit 4:2:2 (`YUV422P10LE`). This lets the encoder path wrap the same shared buffer when the selected encoder preset also targets 10-bit 4:2:2.

## Zero-copy boundary

DeckLink input is not zero-copy from the Blackmagic-owned frame memory because SDI cards normally expose 10-bit YUV as packed v210. NxFrame must unpack that into its planar internal bus.

After that normalization step, the DeckLink path keeps the frame in a shared pooled buffer and passes the same ownership object through the sender queue. In the normal 10-bit 4:2:2 x264 path, the encoder can wrap this buffer instead of copying the image again.

In short:

```text
DeckLink v210 buffer -> one required unpack/normalization -> shared YUV422P10LE buffer -> queue/encoder ownership transfer
```

## File overview

### `decklink.h` / `decklink.cpp`

DeckLink SDI capture implementation.

Main responsibilities:

- enumerate and open a DeckLink input device by index;
- enable video input with automatic format detection;
- enable embedded audio input as 48 kHz, 32-bit container samples with 24 valid bits;
- receive video/audio from the Blackmagic callback thread;
- normalize video to `AV_PIX_FMT_YUV422P10LE`;
- unpack 10-bit v210 through AVX2 when available, otherwise scalar fallback;
- up-convert 8-bit UYVY input to the same internal 10-bit planar bus;
- extract SMPTE timecode where available;
- publish a latest-frame pointer for legacy callers;
- push `VideoFrame` and `AudioFrame` objects directly into the sender queues;
- publish black fallback frames when the SDI signal is missing or frame memory cannot be accessed;
- handle format-change requests outside the DeckLink callback thread.

Important classes and functions:

- `DeckLinkCaptureCallback` implements `IDeckLinkInputCallback` and forwards Blackmagic SDK callbacks into `DeckLinkCapture`.
- `DeckLinkCapture::init()` opens the requested DeckLink device, queries `IDeckLinkInput`, installs the callback, detects AVX2 support, and prepares initial pools.
- `DeckLinkCapture::startCapture()` enables video/audio input and starts the streams.
- `DeckLinkCapture::onVideoFrameArrived()` is the video hot path. It obtains the DeckLink frame memory, normalizes the pixel format, builds `VideoFrame` metadata, and pushes the frame to the sender queue.
- `DeckLinkCapture::onAudioPacketArrived()` copies embedded audio from the DeckLink packet into a pooled buffer and pushes an `AudioFrame`.
- `DeckLinkCapture::buildVideoFrameMetadata()` maps the planar Y/U/V pointers into the shared buffer without copying image data.
- `DeckLinkCapture::v210_to_yuv422p10le_dispatch()` chooses AVX2 or scalar v210 unpacking at runtime.
- `DeckLinkCapture::reconfigureWorkerLoop()` and `performPendingReconfigure()` restart DeckLink input outside the callback path after a detected input-mode change.

### `input_manager.h` / `input_manager.cpp`

Input source selector and sender-facing input coordinator.

Main responsibilities:

- choose between `decklink` and generated test signal input;
- initialize the selected source;
- expose basic input properties such as width, height, frame rate, time base, interlace flag, and audio format;
- attach DeckLink capture directly to the sender queues;
- run a synthetic producer thread for the test signal path.

For DeckLink, `InputManager::startProducer()` does not create a separate producer thread. It attaches the DeckLink callback path directly to the live pipeline queues so the callback can push normalized frames as soon as they arrive.

For the test signal path, `InputManager::producerLoop()` creates synthetic `VideoFrame` and `AudioFrame` objects at the configured frame cadence.

### `test_signal_generator.h` / `test_signal_generator.cpp`

Development and fallback source used for lab tests when real SDI is not available or when fallback is explicitly enabled.

Main responsibilities:

- generate planar `YUV422P10LE` video frames;
- generate interleaved signed 16-bit PCM audio;
- provide deterministic frame/audio content for development tests.

This source should not be treated as a silent production fallback for failed DeckLink input. DeckLink fallback must be explicitly allowed by the CLI/application layer.

### `simd_v210_avx2.h`

Public declarations for CPU feature detection and v210 unpacking implementations.

Main responsibilities:

- expose `cpu_has_avx2()`;
- expose scalar and AVX2 v210-to-`YUV422P10LE` conversion entry points.

### `simd_v210_avx2.cpp`

AVX2-oriented v210 unpacking implementation and runtime CPU capability detection.

Main responsibilities:

- verify AVX2 availability and OS support for XMM/YMM state;
- unpack packed v210 rows into planar 10-bit Y, U, and V output planes;
- provide a faster hot path for supported x86 CPUs.

### `simd_v210_scalar.cpp`

Portable scalar v210 unpacking implementation.

Main responsibilities:

- provide the fallback implementation for systems without AVX2;
- provide a correctness reference for the AVX2 implementation and regression tests.

## Internal video bus contract

The input subsystem outputs video as:

```text
AV_PIX_FMT_YUV422P10LE
Y plane: width x height 16-bit little-endian samples carrying 10-bit values
U plane: width/2 x height 16-bit little-endian samples carrying 10-bit values
V plane: width/2 x height 16-bit little-endian samples carrying 10-bit values
```

The current buffer layout is tightly packed:

```text
[Y plane][U plane][V plane]
```

with linesizes:

```text
Y: width * 2 bytes
U: (width / 2) * 2 bytes
V: (width / 2) * 2 bytes
```

## Audio contract

DeckLink embedded audio is captured as:

```text
sample rate: 48000 Hz
channels: 16
container: 32-bit signed integer
valid bits: 24
layout: interleaved samples
```

The audio packet is copied into an owned pooled buffer before it is queued. This copy is intentional because the DeckLink audio packet memory belongs to the callback lifetime, while the sender queue needs independent ownership.

## Signal loss behavior

When the DeckLink frame reports no input source, or when frame memory cannot be accessed, the capture path publishes and queues a black `YUV422P10LE` fallback frame. This keeps the live sender pipeline moving and avoids null-frame handling in downstream stages.

The fallback black frame uses broadcast-range values:

```text
Y = 64
U = 512
V = 512
```

## Threading model

DeckLink video and audio callbacks are invoked by the Blackmagic SDK. NxFrame keeps the callback hot path focused on:

1. access input memory;
2. normalize/copy into owned pooled memory;
3. build metadata;
4. push to the bounded live queues.

Format reconfiguration is intentionally moved to a worker thread. Restarting DeckLink streams directly inside the callback path is avoided because it can block capture delivery and increase the risk of driver-level instability.

## Comments and release hygiene

Input-source comments should explain ownership, timing, format conversion, and callback-safety decisions. Avoid stale development notes, emojis, and comments that only repeat the obvious.

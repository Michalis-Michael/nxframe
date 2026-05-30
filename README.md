# NxFrame

NxFrame is a Linux-based broadcast contribution encoder/decoder for low-latency SDI-over-IP workflows.

It captures SDI video/audio from Blackmagic DeckLink cards, encodes the signal, muxes it into MPEG-TS, sends it over IP using SRT, UDP, or RTP, and can receive/decode the stream back to SDI output.

NxFrame is designed for broadcast engineering, contribution links, lab testing, and development of SDI-over-IP workflows.

## What NxFrame does

- Captures SDI input from Blackmagic DeckLink cards
- Normalizes DeckLink v210 input to an internal 10-bit 4:2:2 video format
- Encodes video using FFmpeg/libx264 or FFmpeg/libx265
- Encodes audio using FFmpeg/libfdk-aac or carries PCM/S302M audio
- Muxes audio/video into MPEG-TS
- Sends MPEG-TS over:
  - SRT
  - raw UDP
  - RTP payload type 33
- Receives SRT/UDP/RTP transport streams
- Demuxes and decodes received streams
- Outputs decoded video/audio to DeckLink SDI output
- Provides JSON presets for common sender and receiver workflows

## Validated dependency baseline

This release is built and tested with:

- Linux x86_64
- GCC or Clang with C++14 support
- CMake 3.16 or newer
- pkg-config
- FFmpeg 8.1
- x264
- x265
- libfdk-aac
- Haivision SRT
- Blackmagic Desktop Video / DeckLink SDK 15.3.x

FFmpeg 8.1 must be built with:

```text
--enable-gpl
--enable-nonfree
--enable-shared
--enable-pic
--enable-libx264
--enable-libx265
--enable-libfdk_aac
--enable-libsrt
```

Required FFmpeg libraries:

```text
libavutil
libavcodec
libavformat
libavdevice
libswscale
libswresample
```

Distribution FFmpeg packages may not include the required codec, bit-depth, chroma-format, SRT, or FDK-AAC support. For the validated NxFrame build, use FFmpeg 8.1 built from source with the options above.

## Hardware requirements

For SDI workflows:

- Blackmagic DeckLink card supported by the installed Desktop Video driver
- CPU with AVX2 support
- Modern Intel Core i7/i9, Intel Core Ultra 7/9, or AMD Ryzen 7/9 recommended for realtime 1080i50 / 1080p50 testing
- 16 GB RAM minimum; 32 GB recommended
- NVMe SSD recommended
- Stable PCIe bandwidth for DeckLink input/output
- Reliable cooling for long-duration encode tests

Older CPUs may work for development and functional testing, but realtime performance depends on preset, resolution, frame rate, x264 settings, audio mode, transport mode, and whether format conversion is enabled.

## DeckLink SDK

The Blackmagic DeckLink SDK is not bundled in this repository.

Install Blackmagic Desktop Video and download the DeckLink SDK separately. Then pass the SDK include path to CMake:

```bash
-DNXFRAME_DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include
```

## Quick build

From the project root:

```bash
mkdir build
cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include

cmake --build . -j$(nproc)
```

Run tests:

```bash
ctest --output-on-failure
```

## Quick use

### Send DeckLink SDI input over SRT

```bash
./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

### Receive SRT and output to DeckLink SDI

```bash
./NxFrame play srt://SENDER_IP:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

### Send test signal over SRT

```bash
./NxFrame send test to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

### Receive SRT to test output

```bash
./NxFrame play srt://127.0.0.1:5000 to test --receiver-preset receiver_audio_route_example
```

### RTP multicast example

Sender:

```bash
./NxFrame send decklink 0 to rtp://239.10.10.5:5004 encoder preset x264_1080i50_aac_lowlatency
```

Receiver:

```bash
./NxFrame play rtp://239.10.10.5:5004 to decklink 1 --receiver-preset receiver_audio_route_example
```

## Transport modes

```text
srt:// = MPEG-TS over Haivision SRT
udp:// = raw MPEG-TS over UDP
rtp:// = MPEG-TS over RTP, payload type 33
```

## Presets

Presets are stored in:

```text
preset/
```

Use the preset filename without `.json`.

Example:

```bash
./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

## Project status

NxFrame is currently an alpha-stage broadcast engineering project. It is suitable for development, lab testing, and controlled evaluation. It is not yet a fully certified production appliance.

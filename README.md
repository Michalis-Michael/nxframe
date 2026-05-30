# NxFrame

NxFrame is a Linux-based broadcast contribution encoder/decoder project for low-latency SDI-over-IP workflows.

It is built around:

- Blackmagic DeckLink SDI input/output
- FFmpeg-based video/audio encode and decode
- MPEG-TS muxing/demuxing
- Haivision SRT transport
- raw MPEG-TS over UDP
- MPEG-TS over RTP

NxFrame is currently an alpha-stage engineering project. It is suitable for controlled lab testing, interoperability experiments, and limited beta-candidate evaluation. It is not yet a certified production appliance.

## Current focus

The primary validated workflow is:

```text
DeckLink SDI input
-> internal 10-bit 4:2:2 YUV bus
-> x264 10-bit 4:2:2 contribution encode
-> MPEG-TS
-> SRT / UDP / RTP
-> NxFrame receiver
-> DeckLink SDI output
```

The recommended real-time contribution path is x264 10-bit 4:2:2 over SRT with PCM/SMPTE 302M or AAC audio, depending on the selected preset.

## Feature status

| Area | Status |
|---|---|
| DeckLink SDI input | Implemented |
| DeckLink SDI output | Implemented |
| Internal video bus | 10-bit YUV 4:2:2 planar, `yuv422p10le` |
| v210 unpacking | AVX2 path with scalar fallback |
| x264 | Primary real-time path; 10-bit 4:2:2 High 4:2:2 when FFmpeg/libx264 supports it |
| x265 | Functional but experimental and hardware-dependent |
| MPEG-TS mux/demux | Implemented through FFmpeg libavformat |
| SRT input/output | Implemented with Haivision SRT |
| UDP transport | Implemented as raw MPEG-TS over UDP |
| RTP transport | Implemented as MPEG-TS over RTP, payload type 33 |
| PCM/SMPTE 302M audio | Implemented |
| AAC-LC audio | Implemented when FFmpeg includes AAC encoder support |
| Receiver A/V sync | Implemented with decoded media PTS as the media clock |
| HDR/WCG metadata path | Experimental |
| Timecode / CC / SCTE / KLV | Future work |

## Engineering contracts

The source tree uses folder-level README files and release headers on NxFrame-owned source files. Comments are focused on runtime ownership, format boundaries, shutdown order, and real-time behavior.

Important contracts:

- DeckLink v210 input is normalized to the internal `yuv422p10le` bus before encoding.
- The x264 real-time path is optimized for 10-bit 4:2:2. Presets that request another target format may trigger conversion and are not the clean zero-copy-oriented path.
- Receiver SDI output converts decoded/internal video into DeckLink v210 output buffers at the SDI boundary.
- Transport arrival time is not used as the receiver media clock. Decoded media PTS drives A/V sync, and DeckLink scheduled output is the playout clock.
- Low-level queue sizes, recovery thresholds, and timing constants are internal engineering values unless deliberately promoted to preset-level controls.

## Repository layout

```text
NxFrame/
|-- app/                  Top-level sender/play command runners
|-- cli/                  Command-line parsing and transport URL helpers
|-- config/               Preset validation and configuration checks
|-- core/                 Shared frame, packet, queue, and telemetry structures
|-- docs/                 Validation procedures and engineering notes
|-- input/                DeckLink capture, test signal, and v210 conversion
|-- encoders/             x264, x265, AAC, PCM, and encoder manager
|-- output/               MPEG-TS muxer, SRT/UDP output, DeckLink SDI output
|-- receiver/             SRT/UDP/RTP input, demuxer, audio/video decoders
|-- sender/               Sender pipeline and encode workers
|-- playout/              Receiver A/V sync controller
|-- preset/               Supported sender and receiver JSON presets
|-- tests/                Validation tests
|-- scripts/              Release/source-tree maintenance helpers
|-- CMakeLists.txt        Root build definition
|-- main.cpp              Process entry point
`-- stage_timing.h        Optional lightweight stage timing registry
```

Most subsystem folders include a local `README.md`. Read the folder README first, then inspect the individual source files for implementation details.

## Requirements

### Operating system

Recommended development environment:

- Linux x86_64
- GCC or Clang with C++14 support
- CMake 3.16 or newer
- pkg-config

### Hardware for SDI workflows

- Blackmagic DeckLink card supported by the installed Desktop Video driver
- CPU with AVX2 support for the optimized v210 conversion path
- Stable PCIe bandwidth for DeckLink I/O
- Reliable cooling for long-duration real-time encode/decode tests

The scalar v210 conversion path is retained for correctness testing and fallback, but the real-time SDI path is intended for AVX2-capable systems.

### External dependencies

NxFrame expects these libraries to be available through pkg-config:

- Haivision SRT
- FFmpeg libraries:
  - libavutil
  - libavcodec
  - libavformat
  - libavdevice
  - libswscale
  - libswresample
- FFmpeg with libx264 support for H.264 presets
- FFmpeg with libx265 support for HEVC presets, where HEVC presets are used
- Optional FFmpeg AAC encoder support for AAC presets

### DeckLink SDK

NxFrame uses the Blackmagic DeckLink SDK headers and `DeckLinkAPIDispatch.cpp`, but the public repository does **not** bundle the SDK files. This avoids redistributing third-party SDK material inside the GPL source tree.

Install Blackmagic Desktop Video for the runtime driver support, then download the matching Blackmagic DeckLink SDK / Desktop Video SDK from Blackmagic Design. Point CMake to the SDK Linux include directory:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include
```

Alternatively, set an environment variable before running CMake:

```bash
export DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

The selected directory must contain:

```text
DeckLinkAPI.h
DeckLinkAPIDispatch.cpp
```

Do not commit the SDK directory to this repository unless you have independently verified that redistribution is permitted by Blackmagic Design's SDK license.

## Dependency setup

The exact FFmpeg/x264/x265 build can vary by system. The important requirement is that the resulting FFmpeg installation exposes the required libraries through pkg-config and includes the encoder support required by the presets in use.

### Basic packages

On Debian/Ubuntu-style systems:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git nasm yasm
sudo apt install -y autoconf automake libtool openssl libssl-dev nlohmann-json3-dev
```

### Haivision SRT

```bash
git clone https://github.com/Haivision/srt.git
cmake -S srt -B srt/build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build srt/build -j$(nproc)
sudo cmake --install srt/build
sudo ldconfig
```

Verify:

```bash
pkg-config --modversion srt
pkg-config --libs srt
```

### x264

```bash
git clone --depth 1 https://code.videolan.org/videolan/x264.git
cd x264
./configure --enable-shared --enable-pic --prefix=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
```

After FFmpeg is built or rebuilt, verify that libx264 exposes the required pixel format:

```bash
ffmpeg -hide_banner -h encoder=libx264 | grep yuv422p10le
```

### x265

HEVC 10-bit 4:2:2 requires an x265 build that supports the required bit depth and chroma format. Standard distribution packages may not provide every required mode.

After FFmpeg is built or rebuilt, verify the required pixel format:

```bash
ffmpeg -hide_banner -h encoder=libx265 | grep yuv422p10le
```

Treat x265 10-bit 4:2:2 as experimental until validated on the target CPU and receiver hardware.

### FFmpeg

Build or install FFmpeg with the features required by the selected presets.

Typical source-build configuration for the current workflows:

```bash
./configure \
  --prefix=/usr/local \
  --enable-gpl \
  --enable-shared \
  --enable-pic \
  --enable-libx264 \
  --enable-libx265 \
  --enable-libsrt
```

If using libfdk_aac, FFmpeg also requires:

```text
--enable-libfdk_aac
--enable-nonfree
```

Verify the resulting installation:

```bash
ffmpeg -hide_banner -version
ffmpeg -hide_banner -h encoder=libx264 | grep yuv422p10le
ffmpeg -hide_banner -h encoder=libx265 | grep yuv422p10le
ffmpeg -hide_banner -protocols | grep srt
pkg-config --modversion libavcodec libavformat libavutil libswscale libswresample srt
```

## Build

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

When the DeckLink SDK is outside the repository:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include

cmake --build build -j$(nproc)
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

### Build options

| Option | Default | Description |
|---|---:|---|
| `NXFRAME_BUILD_APP` | `ON` | Build the main `NxFrame` application |
| `NXFRAME_BUILD_TESTS` | `ON` | Build validation tests |
| `NXFRAME_BUILD_PURE_TESTS` | `ON` | Build tests that do not require FFmpeg/SRT/DeckLink |
| `NXFRAME_BUILD_FFMPEG_TESTS` | `ON` | Build tests that require FFmpeg libraries |
| `NXFRAME_ENABLE_AVX2` | `ON` | Compile the AVX2 v210 converter source |
| `NXFRAME_ENABLE_RELEASE_TUNING` | `ON` | Apply release compile tuning on Release builds |
| `NXFRAME_SANITIZE` | `OFF` | Enable AddressSanitizer/UBSan on supported compilers |
| `NXFRAME_WARNINGS_AS_ERRORS` | `OFF` | Treat warnings as errors |
| `NXFRAME_INSTALL` | `ON` | Enable install rules |

Pure tests only:

```bash
cmake -S . -B build-pure \
  -DNXFRAME_BUILD_APP=OFF \
  -DNXFRAME_BUILD_FFMPEG_TESTS=OFF

cmake --build build-pure -j$(nproc)
ctest --test-dir build-pure --output-on-failure
```

Sanitizer build:

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNXFRAME_SANITIZE=ON

cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

### Install

Install the built binary, presets, and docs using CMake install rules:

```bash
sudo cmake --install build
```

By default this installs the application into the platform binary directory, usually `/usr/local/bin`, and installs presets/docs under the CMake data directory. To install into a staging directory for packaging:

```bash
cmake --install build --prefix /tmp/nxframe-package
```

If NxFrame uses locally built FFmpeg/SRT libraries in `/usr/local/lib`, ensure the runtime linker can find them:

```bash
sudo ldconfig
```

Check linked runtime libraries:

```bash
ldd build/NxFrame | grep -E "avcodec|avformat|avutil|swscale|swresample|srt|x264|x265"
```

## Quick start

A minimal local functional test can be done without SDI input by using the generated test signal as the sender source. One terminal starts the sender in listener mode:

```bash
./build/NxFrame send test to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

A second terminal starts a receiver on the same machine and sends decoded output to the test sink:

```bash
./build/NxFrame play srt://127.0.0.1:5000 to test --receiver-preset receiver_audio_route_example
```

For SDI workflows, replace `test` with the DeckLink device index. Device numbering follows the order exposed by the installed Blackmagic Desktop Video driver.

## Usage

NxFrame exposes two main workflows:

```text
send    SDI/test input -> encode -> mux -> transport
play    transport input -> demux -> decode -> SDI/test output
```

### Sender listener, receiver caller

Sender:

```bash
./build/NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

Receiver on the same machine:

```bash
./build/NxFrame play srt://127.0.0.1:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

Receiver on another machine:

```bash
./build/NxFrame play srt://SENDER_IP:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

Do not use `srt://0.0.0.0:5000` as a caller destination. `0.0.0.0` is a bind/listener address.

### Receiver listener, sender caller

Receiver:

```bash
./build/NxFrame play srt://0.0.0.0:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

Sender:

```bash
./build/NxFrame send decklink 0 to RECEIVER_IP:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

### Test signal sender

```bash
./build/NxFrame send test to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

### Receive SRT and play to test output

```bash
./build/NxFrame play srt://127.0.0.1:5000 to test --receiver-preset receiver_audio_route_example
```

## Transport modes

NxFrame separates transport URL schemes:

```text
srt:// = MPEG-TS over Haivision SRT
udp:// = raw MPEG-TS over UDP
rtp:// = MPEG-TS over RTP, RTP payload type 33
```

RTP uses:

```text
12-byte RTP header + 1316-byte MPEG-TS payload
```

RTP multicast example:

```bash
./build/NxFrame send decklink 0 to rtp://239.10.10.5:5004 encoder preset x264_1080i50_aac_lowlatency
./build/NxFrame play rtp://239.10.10.5:5004 to decklink 1 --receiver-preset receiver_audio_route_example
```

Raw UDP multicast example:

```bash
./build/NxFrame send decklink 0 to udp://239.10.10.5:5004 encoder preset x264_1080i50_aac_lowlatency
./build/NxFrame play udp://239.10.10.5:5004 to decklink 1 --receiver-preset receiver_audio_route_example
```

## Validation

Recommended validation before a release tag:

```text
1. x264 1080p50 DeckLink input -> SRT -> NxFrame receiver -> DeckLink output
2. x264 1080i50 DeckLink input -> SRT -> NxFrame receiver -> DeckLink output
3. x264 1080i50 DeckLink input -> RTP multicast -> NxFrame receiver -> DeckLink output
4. x264 1080i50 DeckLink input -> UDP multicast -> NxFrame receiver -> DeckLink output
5. Receiver starts before sender
6. Sender starts before receiver
7. Multiple receiver reconnect cycles
8. Sender restart while receiver remains open
9. RTP sender restart while receiver remains open
10. Signal loss / signal return
11. Audio pair routing verification
12. Invalid preset validation test
13. 30 minute, 2 hour, and 8 hour soak tests
14. TSDuck MPEG-TS analysis for UDP/RTP multicast validation
```

### RTP / UDP validation with TSDuck

For multicast analysis:

```bash
tsp -I ip --buffer-size 4194304 239.10.10.5:5004 -P analyze -O drop
```

A clean steady-state result should report:

```text
With invalid sync: 0
With transport error: 0
Suspect and ignored: 0
Unreferenced PID's: 0
Unexpected discontinuities: 0
PCR leaps: 0
PTS leaps: 0
```

To verify RTP packetization:

```bash
sudo tcpdump -n -i any udp port 5004 -c 1 -XX
```

Expected RTP packet layout:

```text
UDP payload length: 1328 bytes
80 21 ...        RTP version 2, payload type 33
47 ...           MPEG-TS sync byte after the 12-byte RTP header
```

For raw UDP mode, the UDP payload normally starts directly with the MPEG-TS sync byte and has a 1316-byte payload.

## UDP/RTP receive buffer

NxFrame requests a larger UDP receive buffer for RTP/UDP receiver mode. On Linux, the requested value may be capped by `net.core.rmem_max` unless NxFrame has permission to use `SO_RCVBUFFORCE`.

Development example:

```bash
sudo setcap cap_net_admin+ep ./build/NxFrame
getcap ./build/NxFrame
```

Expected output:

```text
./build/NxFrame cap_net_admin=ep
```

Reapply the capability after rebuilding if it is lost. For appliance deployment, prefer systemd service capabilities or OS-level sysctl tuning during installation.

## Useful sender flags

```text
--copy                 Use legacy copy path instead of default zero-copy-oriented path
--allow-test-fallback  Allow test generator fallback when input is not available
--timing               Enable timing information
--timing-verbose       Enable more detailed timing output
--ts-debug             Enable MPEG-TS debug output
--ts-capture <file.ts> Capture outgoing transport stream to a file
```

## Useful receiver flags

```text
--packed-audio-channels <n>  Number of packed audio output channels
--max-audio-pairs <n>        Maximum audio pairs to route
--audio-route <csv>          Audio pair routing map
--timing                     Enable timing information
--timing-verbose             Enable more detailed timing output
```

## Presets

Presets live in `preset/` and are selected by filename without `.json`.

The public preset tree is organized by workflow class:

```text
preset/01_low_latency_contribution/
preset/02_quality_contribution/
preset/03_cpu_safe_development/
preset/04_hdr_wcg/
```

Example:

```bash
./build/NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

The command-line resolver scans the preset tree recursively, so future folders can be added without code changes.

Primary presets include:

```text
x264_1080p50_pcm_cpu_safe
x264_1080p50_pcm_balance
x264_1080p50_pcm_lowlatency
x264_1080p50_pcm_quality
x264_1080i50_pcm_dolby_cpu_safe
x264_1080i50_pcm_dolby_lowlatency
x264_1080i50_pcm_dolby_quality
x264_1080p50_aac_lc_lowlatency
x264_1080p50_aac_lc_mpeg2_lowlatency
x264_1080p50_aac_lc_mpeg4_lowlatency
receiver_audio_route_example
```

Experimental / hardware-dependent presets include:

```text
x265_1080p50_10bit_422_pcm_test
x265_1080p50_pcm_cpu_safe
x265_1080i50_8bit_420_pcm_cpu_safe
x265_1080p50_hlg_main10_bt2020_lowlatency
x265_1080p50_hlg_main10_bt709_lowlatency
x265_1080p50_pq_main10_bt2020_lowlatency
```

For HDR/WCG presets, see:

```text
preset/README_HDR_WCG.md
```

## Video notes

- x264 10-bit 4:2:2 is the preferred real-time live contribution path.
- x265 10-bit 4:2:2 is CPU-heavy and should be treated as experimental unless the target hardware proves real-time performance.
- HEVC 4:2:2 10-bit may be reported by tools as HEVC Range Extensions / `Rext`.
- HDR/WCG support is experimental until validated on real HDR-capable SDI equipment and monitoring/analyzer hardware.
- Current public workflows focus on 50 Hz operation: 1080i50 and 1080p50.

## Audio notes

NxFrame's SDI audio path is designed around 48 kHz embedded SDI audio.

Implemented audio modes:

- SMPTE 302M uncompressed LPCM carriage for selected stereo pairs
- Protected SMPTE 302M carriage for Dolby E-style pairs where NxFrame must not decode, resample, remix, or alter the payload
- AAC-LC encoding for selected stereo pairs when FFmpeg has AAC encoder support
- Split-pair audio routing through presets and receiver route configuration

AAC notes:

- AAC-LC is the only AAC profile currently exposed through public presets.
- AAC input/output is fixed to 48 kHz for SDI contribution workflows.
- `aac_lc`, `aac_lc_mpeg4`, and `aac_lc_mpeg2` are all AAC-LC modes.
- MP2 / MPEG Layer II audio is not implemented.
- True MPEG-4 LATM/LOAS TS signalling, full AC3/E-AC3 passthrough, and complete Dolby metadata workflows are future work.

## Experimental HDR / WCG status

NxFrame includes an experimental HDR/WCG metadata pipeline. This work is compile/path validated, but still requires real HDR-capable SDI input/output devices, displays, and/or waveform/analyzer validation before it should be considered production-proven.

Implemented HDR/WCG components:

- BT.709 SDR signalling
- BT.2020 / WCG signalling
- HLG transfer signalling
- PQ / ST 2084 transfer signalling
- 10-bit validation for HDR presets
- `VideoFrame` color metadata propagation
- HDR10 mastering-display metadata structure
- MaxCLL / MaxFALL content-light metadata structure
- x265 HDR10 parameter support for PQ presets
- Decoder-side metadata propagation when metadata exists in the decoded stream
- DeckLink output metadata wrapper for Rec.709 / Rec.2020 and SDR / HLG / PQ signalling

Known HDR limitations:

- HDR/WCG SDI output has not yet been validated on real HDR monitoring equipment.
- PQ/HDR10 mastering metadata has not yet been verified with a downstream HDR analyzer/display.
- Real HDR camera/source input has not yet been validated.
- HDR behaviour may vary depending on DeckLink card model, Desktop Video driver version, and connected monitor/analyzer.

## Known limitations

- Alpha-stage/beta-candidate project; not yet certified as a production appliance.
- RTP input/output is implemented for MPEG-TS over RTP using payload type 33, but RTCP, SDP generation, FEC, retransmission, and SMPTE 2022-style protection are not implemented.
- x265 10-bit 4:2:2 is CPU-heavy and not real-time on all systems.
- HDR/WCG support is compile/path validated but not hardware-analyzer validated.
- DeckLink HDR metadata behaviour may depend on card model and Desktop Video driver version.
- Fractional frame rates such as 29.97/59.94 need a more complete rational frame-rate configuration model.
- True slice-based capture and encode, where encoding begins before a full frame is captured, is not implemented.
- Timecode, closed-caption pass-through, SCTE-35/SCTE-104, KLV, MISB metadata, RTCP, RTP FEC, and RTP retransmission workflows are future milestones.
- Full production readiness requires longer-duration soak testing, signal-loss/recovery testing, interoperability testing, and analyzer validation across more hardware.

## Release hygiene

Before tagging a release:

```bash
./scripts/clean_release_tree.sh
find preset -name '*.json' -print0 | xargs -0 -n1 python3 -m json.tool >/dev/null
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
ctest --test-dir build-release --output-on-failure
```

Do not commit generated build directories, transport captures, core dumps, local test media, editor swap files, or machine-local configuration files.

Recommended source checks:

```bash
git status --short
git diff --check
find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.md' -o -name '*.json' \) | sort
```

## Development notes

- Keep low-level pipeline tuning values internal unless they are intentionally promoted to product-level preset controls.
- Prefer clean presets for public workflows and keep experimental presets clearly marked.
- Treat DeckLink SDK updates as separate milestones so SDK changes do not mix with encoder/receiver changes.
- Commit large feature areas separately: encoder, receiver, DeckLink output, presets, documentation.
- Keep machine-specific paths out of CMake, presets, and README examples.

## License

NxFrame-owned source files are released under GPL-3.0-or-later unless a file states otherwise.

Third-party SDKs and libraries remain under their own licenses. In particular, review the Blackmagic DeckLink SDK license before redistributing SDK files.

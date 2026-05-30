# nxframe

nxframe is a Linux-based broadcast contribution encoder/decoder for low-latency SDI-over-IP workflows.

It captures SDI video/audio from Blackmagic DeckLink cards, encodes the signal, muxes it into MPEG-TS, sends it over IP using SRT, UDP, or RTP, and can receive/decode the stream back to SDI output.

nxframe is designed for broadcast engineering, contribution links, lab testing, and development of SDI-over-IP workflows.

## What nxframe does

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
- Provides JSON presets for sender and receiver workflows

## Project status

nxframe is currently an alpha-stage broadcast engineering project. It is suitable for development, lab testing, and controlled evaluation. It is not yet a fully certified production appliance.

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

Distribution FFmpeg packages may not include the required codec, bit-depth, chroma-format, SRT, or FDK-AAC support. For the validated nxframe build, use FFmpeg 8.1 built from source with the options above.

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

## Build dependencies from source

Build the third-party libraries wherever you prefer. Do not build them inside the nxframe source tree.

The examples below install the validated dependency stack under `/usr/local`. If you install to a different prefix, make sure `PKG_CONFIG_PATH` points to the correct `pkgconfig` directory before configuring FFmpeg and nxframe.

### 1. Install basic system packages

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  git \
  cmake \
  pkg-config \
  nasm \
  yasm \
  autoconf \
  automake \
  libtool \
  libssl-dev \
  nlohmann-json3-dev
```

### 2. Build and install Haivision SRT

```bash
git clone https://github.com/Haivision/srt.git
cd srt

mkdir -p build
cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local

cmake --build . -j$(nproc)
sudo cmake --install .
sudo ldconfig
```

Verify:

```bash
pkg-config --modversion srt
pkg-config --libs srt
```

### 3. Build and install x264

```bash
git clone https://code.videolan.org/videolan/x264.git
cd x264

./configure \
  --prefix=/usr/local \
  --enable-shared \
  --enable-pic

make -j$(nproc)
sudo make install
sudo ldconfig
```

Verify:

```bash
pkg-config --modversion x264
pkg-config --libs x264
```

### 4. Build and install x265 multilib

nxframe keeps x265 support for experimental HEVC presets. Use a multilib / 10-bit capable x265 build.

```bash
git clone https://bitbucket.org/multicoreware/x265_git.git x265
cd x265/build/linux

chmod +x multilib.sh
MAKEFLAGS="-j$(nproc)" ./multilib.sh
```

Install the generated x265 files:

```bash
sudo cp /path/to/x265/build/linux/8bit/x265 /usr/local/bin/
sudo cp /path/to/x265/source/x265.h /usr/local/include/
sudo cp /path/to/x265/build/linux/8bit/x265_config.h /usr/local/include/
sudo cp /path/to/x265/build/linux/8bit/x265.pc /usr/local/lib/pkgconfig/
sudo cp /path/to/x265/build/linux/8bit/libx265.a /usr/local/lib/

if ls /path/to/x265/build/linux/8bit/libx265.so* >/dev/null 2>&1; then
  sudo cp /path/to/x265/build/linux/8bit/libx265.so* /usr/local/lib/
  cd /usr/local/lib
  sudo ln -sf $(ls libx265.so.* | sort -V | tail -1) libx265.so
fi

sudo ldconfig
```

Verify:

```bash
x265 --version
pkg-config --modversion x265
pkg-config --libs x265
```

The x265 version output should show 10-bit support, for example:

```text
8bit+10bit
```

or:

```text
8bit+10bit+12bit
```

### 5. Build and install libfdk-aac

```bash
git clone https://github.com/mstorsjo/fdk-aac.git
cd fdk-aac

autoreconf -fiv

./configure \
  --prefix=/usr/local \
  --enable-shared \
  --enable-static

make -j$(nproc)
sudo make install
sudo ldconfig
```

Verify:

```bash
pkg-config --modversion fdk-aac
pkg-config --libs fdk-aac
```

### 6. Build and install FFmpeg 8.1

```bash
git clone https://github.com/FFmpeg/FFmpeg.git ffmpeg
cd ffmpeg

git checkout n8.1
```

Configure FFmpeg:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

./configure \
  --prefix=/usr/local \
  --enable-gpl \
  --enable-nonfree \
  --enable-shared \
  --enable-pic \
  --enable-libx264 \
  --enable-libx265 \
  --enable-libfdk_aac \
  --enable-libsrt \
  --extra-cflags="-I/usr/local/include" \
  --extra-ldflags="-L/usr/local/lib" \
  --extra-libs="-lpthread -lm"
```

Build and install:

```bash
make -j$(nproc)
sudo make install
sudo ldconfig
```

Verify FFmpeg:

```bash
ffmpeg -hide_banner -version
ffmpeg -hide_banner -h encoder=libx264 | grep yuv422p10le
ffmpeg -hide_banner -h encoder=libx265 | grep yuv422p10le
ffmpeg -hide_banner -h encoder=libfdk_aac
ffmpeg -hide_banner -protocols | grep srt

pkg-config --modversion \
  libavcodec \
  libavformat \
  libavutil \
  libavdevice \
  libswscale \
  libswresample \
  srt
```

If `yuv422p10le`, `libfdk_aac`, or `srt` are missing from the verification output, the dependency stack is not correct for the validated nxframe build.

## DeckLink SDK

Install the Blackmagic Desktop Video driver for your DeckLink card.

The Blackmagic DeckLink SDK is required to build DeckLink SDI input/output support.

Recommended public-source approach:

- Do not download the DeckLink SDK into the nxframe repository.
- Keep the SDK outside the repository, for example under `~/src/Blackmagic_DeckLink_SDK_15.3/`.
- Pass the SDK include path to CMake.

Example:

```bash
-DNXFRAME_DECKLINK_SDK_DIR=$HOME/src/Blackmagic_DeckLink_SDK_15.3/Linux/include
```

### Optional bundled DeckLinkAPI folder

Some DeckLink API header/source files contain permissive license text. If you decide to include a minimal `DecklinkAPI/` folder in the root of the project, only include the files needed to build nxframe and keep the original Blackmagic copyright and license text inside those files.

Recommended root layout if you bundle the minimal API files:

```text
nxframe/
|-- DecklinkAPI/
|   |-- DeckLinkAPI.h
|   |-- DeckLinkAPIConfiguration.h
|   |-- DeckLinkAPIDiscovery.h
|   |-- DeckLinkAPIModes.h
|   |-- DeckLinkAPITypes.h
|   |-- DeckLinkAPIVersion.h
|   `-- DeckLinkAPIDispatch.cpp
|-- CMakeLists.txt
|-- README.md
`-- ...
```

If `DecklinkAPI/` exists in the project root, CMake can use it directly:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
```

If `DecklinkAPI/` is not bundled, pass the external SDK path:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=$HOME/src/Blackmagic_DeckLink_SDK_15.3/Linux/include
```

Before publishing a repository that includes `DecklinkAPI/`, verify that the exact SDK files you include are allowed to be redistributed under their included license text.

## Build nxframe

Clone nxframe:

```bash
git clone https://github.com/Michalis-Michael/nxframe.git
cd nxframe
```

Configure:

```bash
mkdir -p build
cd build

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

If `DecklinkAPI/` is bundled in the repository root:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release
```

If using an external DeckLink SDK:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=$HOME/src/Blackmagic_DeckLink_SDK_15.3/Linux/include
```

Build:

```bash
cmake --build . -j$(nproc)
```

Run tests:

```bash
ctest --output-on-failure
```

Check linked libraries:

```bash
ldd ./NxFrame | grep -E "avcodec|avformat|avutil|swscale|swresample|srt|x264|x265|fdk"
```

## Quick use

The examples below assume you are inside the nxframe build directory:

```bash
cd /path/to/nxframe/build
```

### Send DeckLink SDI input over SRT

```bash
./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe
```

### Receive SRT and output to DeckLink SDI

```bash
./NxFrame play srt://SENDER_IP:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

Replace `SENDER_IP` with the IP address of the sender machine.

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

### UDP multicast example

Sender:

```bash
./NxFrame send decklink 0 to udp://239.10.10.5:5004 encoder preset x264_1080i50_aac_lowlatency
```

Receiver:

```bash
./NxFrame play udp://239.10.10.5:5004 to decklink 1 --receiver-preset receiver_audio_route_example
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

## Release cleanup

Before committing or tagging a release:

```bash
./scripts/clean_release_tree.sh
git status
```

Do not commit generated build directories, compiled binaries, logs, transport captures, local configuration files, or SDK files that are not allowed to be redistributed.# NxFrame

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

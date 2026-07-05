# NxFrame

NxFrame is a Linux-based broadcast contribution encoder/decoder for low-latency SDI-over-IP workflows.

It captures SDI video/audio from Blackmagic DeckLink cards, encodes the signal, muxes it into MPEG-TS, sends it over IP using SRT, UDP, or RTP, and can receive/decode the stream back to SDI output.

NxFrame is designed for broadcast engineering, contribution links, lab testing, and controlled evaluation of SDI-over-IP workflows.

## What NxFrame does

- Captures SDI input from Blackmagic DeckLink cards
- Normalizes DeckLink v210 input to an internal 10-bit 4:2:2 video format using a custom SIMD/AVX2 conversion path 
- Encodes video using FFmpeg/libx264 up to 10-bit 4:2:2 1080p50/60
- Keeps FFmpeg/libx265 support for experimental HEVC testing
- Encodes audio using FFmpeg/libfdk-aac, or carries PCM/S302M audio including Dolby-E passthrough, with audio carried either in separate PIDs or packed together
- Muxes audio/video into MPEG-TS
- Sends MPEG-TS over:
  - SRT
  - raw UDP
  - RTP payload type 33
- Receives SRT/UDP/RTP transport streams
- Demuxes and decodes received streams
- Outputs decoded video/audio to DeckLink SDI output
- Provides JSON presets for sender and receiver workflows
- Provides optional CPU profile handling for controlled real-time encoding

## Reference 1U build

NxFrame has been tested in a compact 1U SDI contribution encoder build. This is not required hardware, but it documents one validated direction for a small, high-performance, low-power, low-noise broadcast appliance.

Tested 1U build:

- 1U mini-ITX chassis
- AMD Ryzen 7 9700X, using Eco mode and BIOS power limits around 65-75 W
- Mini-ITX AM5 motherboard
- 32 GB DDR5 memory
- SATA SSD (less heat than NVMe)
- Blackmagic DeckLink Duo 2 via PCIe riser
- Dynatron A45 1U AM5 CPU cooler
- 5 x 40 mm chassis fans fixed at 4,200 RPM to push enough air through the case without becoming loud
- 350 W Flex ATX power supply

The main idea of this build is not maximum CPU boost. The goal is stable real-time x264 contribution encoding inside a very small chassis by controlling power, temperature, and fan noise.

For a single real-time x264 contribution stream, the CPU profile can cap the Ryzen 7 9700X around 4.0 GHz. In testing, this kept CPU package power around 40-45 W while maintaining real-time encoding. Leaving the CPU fully automatic can allow boosts around 5.4-5.5 GHz, which may raise package power to around 75 W and make 1U cooling significantly louder.

In one 2-hour 1080i50 10-bit 4:2:2 encode test, with room ambient temperature around 30°C, CPU temperature remained stable around 65°C, CPU package power was around 45 W, and the fans remained relatively quiet.

This makes the CPU profile useful for appliance-style deployments where predictable thermals and acoustics are more important than maximum benchmark performance.


## Project status

NxFrame is currently in active testing and controlled field-evaluation stage.

The project has moved beyond basic lab proof-of-concept testing and is now being validated with real DeckLink SDI hardware, real-time x264 contribution presets, SRT/UDP/RTP transport, receiver workflows, audio routing, CPU power profiles, and compact 1U hardware testing.

NxFrame is suitable for development, lab testing, controlled field tests, and engineering evaluation. It should not yet be treated as a fully certified production appliance.

The main validated real-time path is x264-based contribution encoding. x265/HEVC support is kept in the tree for experimental testing and future development, but it is not currently the primary validated real-time path.

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
- Blackmagic Desktop Video driver
- Bundled `DecklinkAPI/` source/header folder, or an external DeckLink SDK include path

FFmpeg 8.1 must be built with:

```bash
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
- Modern Intel Core i7/i9, Intel Core Ultra 7/9, or AMD Ryzen 7/9 recommended for real-time 1080i50 / 1080p50 testing
- 16 GB RAM minimum; 32 GB recommended
- NVMe SSD recommended
- Stable PCIe bandwidth for DeckLink input/output
- Reliable cooling for long-duration encode tests

Older CPUs may work for development and functional testing, but real-time performance depends on preset, resolution, frame rate, x264 settings, audio mode, transport mode, and whether format conversion is enabled.

## Build dependencies from source

Build the third-party libraries wherever you prefer. Do not build them inside the NxFrame source tree.

The examples below install the validated dependency stack under `/usr/local`. If you install to a different prefix, make sure `PKG_CONFIG_PATH` points to the correct `pkgconfig` directory before configuring FFmpeg and NxFrame.

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

NxFrame keeps x265 support for experimental HEVC presets. Use a multilib / 10-bit capable x265 build.

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

If `yuv422p10le`, `libfdk_aac`, or `srt` are missing from the verification output, the dependency stack is not correct for the validated NxFrame build.

## DeckLink support

NxFrame uses Blackmagic DeckLink hardware for SDI input and SDI output.

The repository includes only the minimal `DecklinkAPI/` source/header files required by the NxFrame build. The full Blackmagic SDK is not bundled.

Expected folder layout:

```text
DecklinkAPI/
├── DeckLinkAPI.h
├── DeckLinkAPIConfiguration.h
├── DeckLinkAPIDiscovery.h
├── DeckLinkAPIModes.h
├── DeckLinkAPITypes.h
├── DeckLinkAPIVersion.h
└── DeckLinkAPIDispatch.cpp
```

These files are used at compile time only. The machine running NxFrame still needs the Blackmagic Desktop Video driver installed for the DeckLink card to work.

NxFrame is currently developed and tested with Blackmagic Desktop Video / DeckLink API 15.3.x.

If `DecklinkAPI/` exists in the project root, configure normally:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release
```

If the bundled `DecklinkAPI/` folder is removed, pass an external SDK include path instead:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXFRAME_DECKLINK_SDK_DIR=$HOME/src/Blackmagic_DeckLink_SDK_15.3/Linux/include
```

Do not commit SDK installers, archives, examples, documentation, binaries, generated folders, or unrelated platform files. Keep the original Blackmagic copyright and license text inside any bundled DeckLink API source/header files.

## Build NxFrame

Clone NxFrame:

```bash
git clone https://github.com/Michalis-Michael/nxframe.git
cd nxframe
```

Configure:

```bash
mkdir -p build
cd build

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

cmake .. \
  -DCMAKE_BUILD_TYPE=Release
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

The examples below assume you are inside the NxFrame build directory:

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

Video/audio presets control codec settings such as resolution, frame rate, bitrate, GOP, chroma format, bit depth, audio mode, and low-latency behavior.

## CPU profile

NxFrame includes optional CPU profile support for more predictable real-time encode testing.

A CPU profile is separate from the encoder preset. The encoder preset controls video/audio encoding. The CPU profile controls selected Linux CPU frequency/governor behavior before the sender pipeline starts.

CPU profile support is intended for compact broadcast systems where thermal and power behavior must be controlled, for example a 1U Ryzen or Intel system running real-time x264 contribution encoding.

In the tested 1U Ryzen 7 9700X build, a single x264 contribution encode can run with the CPU capped around 4.0 GHz, keeping CPU package power around 40-45 W without affecting the observed real-time encode performance. Without the cap, the CPU may boost automatically to around 5.4-5.5 GHz, increasing package power to around 75 W and making the small 1U cooling system louder.

The CPU profile helper can:

- Apply a CPU governor
- Apply CPU frequency limits
- Cap maximum CPU frequency for controlled temperature and power behavior
- Store the previous CPU settings
- Restore the previous settings on normal exit or Ctrl+C

Example:

```bash
sudo ./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe -cpu_profile profile_1
```

Alternative long-form flag:

```bash
sudo ./NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe --cpu-profile profile_1
```

`profile_1` is intended as a safe real-time profile. It can cap the CPU maximum frequency, for example around 3.8 GHz, while allowing the minimum frequency to remain automatic.

Because CPU profiles write to Linux CPU frequency/governor sysfs paths, they normally require root privileges.

Typical sysfs paths used by Linux CPU frequency control are:

```text
/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
/sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq
/sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
```

### Avoiding a sudo password every time

Recommended safe options:

1. Apply the CPU policy once at boot using system tools, then run NxFrame normally without `-cpu_profile`.
2. Allow passwordless sudo only for the exact NxFrame binary path.
3. Do not use broad passwordless sudo rules such as `NOPASSWD: ALL`.

Example limited sudoers rule:

```bash
sudo visudo -f /etc/sudoers.d/nxframe
```

Add:

```text
michalis ALL=(root) NOPASSWD: /home/michalis/nxframe/build/NxFrame
```

Then run the exact binary path:

```bash
sudo /home/michalis/nxframe/build/NxFrame send decklink 0 to 0.0.0.0:5000 encoder preset x264_1080p50_pcm_cpu_safe -cpu_profile profile_1
```

For production-style systems, the cleaner long-term design is a small privileged helper or boot-time service that applies the CPU policy, while the NxFrame process itself runs without full root privileges.

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

## License

NxFrame is released under the license included in this repository.

Third-party components keep their own licenses. When bundling DeckLink API files, keep the original Blackmagic copyright and license text inside those files.

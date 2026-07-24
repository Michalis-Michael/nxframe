# NxFrame GUI control plane

`NxFrameWeb` is the lightweight C++ backend for the browser-based NxFrame appliance GUI. It is intentionally separate from the real-time sender/receiver process so the management interface can remain available while media pipelines are restarted or updated.

## Current features

- Responsive browser dashboard with SDI 1, SDI 2, SDI 3, SDI 4, and Admin tabs
- Linux network-interface discovery using `getifaddrs()` and `/sys/class/net`
- Management/control and streaming role assignment
- DHCP or static IPv4 configuration fields
- Four DeckLink SDI role assignments: sender, receiver, or disabled
- Protected sender templates loaded from `gui/gui_encoder_presets/`
- Sender configuration for broadcast video format, mandatory interlaced field order, bitrate, CBR/VBR, chroma, bit depth, and H.264 level (Auto or validated override)
- MPEG-TS service metadata, constant/non-constant TS selection, and automatic or operator-selected constant mux rate
- SDI audio channel count and separate audio streams per pair, with independent codecs including Dolby E passthrough
- SRT, UDP, and RTP addressing and operator-facing parameters
- Automatic x264 profile selection plus hidden VBV maximum rate, VBV buffer, and HRD/filler behavior
- Appliance-wide CPU performance profile selection from `config/cpu_profiles.json`
- The first GUI sender worker applies the selected CPU profile; it remains active across concurrent sender workers and is restored after the last sender stops
- Debounced autosave of complete per-channel presets to `config/channels/sdi1.json` through `sdi4.json`
- Start/stop control that launches the existing NxFrame CLI as a separate process per SDI channel
- No external web framework or JavaScript package manager

The browser never edits the protected template directly. The C++ backend clones it, applies only the approved operator fields, calculates dependent values, validates the complete result, and then saves the channel preset. Filesystem paths and derived encoder values are not exposed in the browser UI.

The current stage stores network configuration but does not yet modify NetworkManager, netplan, or Linux routes. Sender processes are launched through the existing CLI, keeping the real-time pipeline separate from the GUI process.

## Build the GUI application

```bash
sudo apt install nlohmann-json3-dev

cmake -S . -B gui_app \
  -DNXFRAME_BUILD_APP=OFF \
  -DNXFRAME_BUILD_TESTS=OFF \
  -DNXFRAME_BUILD_WEB=ON

cmake --build gui_app -j"$(nproc)"
```

## Run from the repository

```bash
./gui_app/NxFrameWeb \
  --bind 127.0.0.1 \
  --port 8080 \
  --config config/system.json \
  --web-root gui/static \
  --encoder-presets gui/gui_encoder_presets \
  --channel-config-root config/channels \
  --nxframe-executable build/NxFrame \
  --cpu-profile-config config/cpu_profiles.json
```

Open `http://127.0.0.1:8080`.

To expose it on the management LAN, bind explicitly to the management-interface address, for example:

```bash
./gui_app/NxFrameWeb \
  --bind 192.168.1.50 \
  --port 8080 \
  --config config/system.json \
  --web-root gui/static \
  --encoder-presets gui/gui_encoder_presets \
  --channel-config-root config/channels \
  --nxframe-executable build/NxFrame \
  --cpu-profile-config config/cpu_profiles.json
```

Applying a non-default CPU profile writes Linux cpufreq sysfs controls. The
GUI refuses to start the worker if those writes are not permitted; configure a
restricted service/helper or suitable sysfs permissions rather than exposing
the whole web service as a privileged public process.

Authentication and TLS are not implemented yet. Do not expose the service directly to the public internet.

## Sender API

- `GET /api/sender/templates` — list protected templates and their approved editable values
- `GET /api/sender/channels/sdi1` — load the saved channel configuration
- `POST /api/sender/validate/sdi1` — derive and validate without writing a file
- `PUT /api/sender/channels/sdi1` — derive, validate, and atomically save the complete preset
- `GET /api/sender/status/sdi1` — read sender process state
- `POST /api/sender/start/sdi1` — start the saved SDI sender through the NxFrame CLI
- `POST /api/sender/stop/sdi1` — request a graceful stop

The same channel endpoints are available for `sdi2`, `sdi3`, and `sdi4`.

## Next control-plane stage

Add live encoder, muxer, transport, and DeckLink telemetry plus receiver configuration and control.

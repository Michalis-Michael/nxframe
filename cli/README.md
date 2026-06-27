# NxFrame CLI layer

The `cli/` folder contains command-line parsing helpers shared by the sender and receiver application entry points. These files do not open DeckLink devices, sockets, encoders, or decoders directly. Their job is to convert user-facing command arguments into validated runtime configuration.

## Pipeline position

```text
main.cpp
  -> app/nxframe_app.cpp
     -> cli helpers validate syntax, presets, URLs and routing
     -> app/send_app.cpp or app/play_app.cpp starts the actual pipeline
```

Keeping this layer lightweight makes it safe to extend the CLI without touching the media hot path.

## File-by-file

### `cli_utils.cpp` / `cli_utils.h`

Common command-line helpers:

- print supported sender/receiver command forms;
- validate integer arguments with strict range checks;
- trim strings and check file paths;
- resolve short preset names such as `x264_1080i50_aac_lowlatency` into a real JSON file path.

Preset resolution searches common source/build/install locations and is depth-limited so the CLI remains convenient without scanning the whole filesystem.

### `receiver_cli.cpp` / `receiver_cli.h`

Receiver-specific CLI and preset helpers:

- parse `--audio-route` CSV values;
- load optional receiver audio settings from JSON;
- validate packed audio channel count and maximum audio-pair count;
- apply validated options to `Receiver::Config`.

The receiver audio route is intentionally validated before playout starts so invalid channel mapping cannot reach the SDI output path.

### `transport_url.cpp` / `transport_url.h`

Transport URL helpers:

- parse `srt://host:port`, `udp://host:port`, `rtp://host:port`, and bare `host:port`;
- detect listener-style addresses such as `0.0.0.0` and `*`;
- detect IPv4 multicast groups;
- map the parsed URL into `Receiver::Config`.

`rtp://` currently maps to the UDP receiver with RTP depacketization enabled. UDP and RTP receiver modes are listener-style inputs. SRT can be caller or listener depending on the host address.

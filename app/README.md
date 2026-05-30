# NxFrame app layer

The `app/` folder contains the top-level application runners used by `main.cpp`.
It is intentionally thin: command-line parsing, preset validation, sender/receiver
configuration, and lifecycle coordination live here, while media processing remains
inside the `input/`, `sender/`, `encoders/`, `receiver/`, and `output/` modules.

## Pipeline role

`main.cpp` installs process signal handling and then calls `runNxFrameApp()`.
The app layer parses the command, validates presets, and dispatches to one of two
runtime paths:

```text
send ... -> runSendApp() -> SenderPipeline
play ... -> runPlayTest() / runPlayDeckLink() -> Receiver + OutputManager
```

This separation keeps process-level concerns outside the hot media loops. The app
code should not encode, decode, mux, demux, convert video formats, or perform SDI
I/O directly.

## File-by-file overview

### `nxframe_app.h` / `nxframe_app.cpp`

Application entry dispatcher. It parses global options, loads optional receiver
preset settings, validates command shape, and forwards execution to the sender or
receiver app runner.

Main responsibilities:

- parse global flags such as copy mode, timing, transport-stream debug, and live
  receiver options;
- keep the user-facing CLI grammar in one place;
- resolve and validate receiver presets before receiver startup;
- dispatch `send` commands to `runSendApp()`;
- dispatch `play` commands to either test playout or DeckLink SDI playout.

### `send_app.h` / `send_app.cpp`

Sender runtime bootstrap. It validates the destination transport URL, resolves and
validates the sender preset, reports the selected sender mode, builds
`SenderPipeline::Config`, and starts the sender pipeline.

Main responsibilities:

- reject invalid sender transport addresses early;
- prevent UDP/RTP output from using listener-style addresses such as `0.0.0.0`;
- resolve and validate sender presets;
- map URL schemes to `OutputManager::SenderTransport`;
- pass the external shutdown flag into the sender pipeline.

### `play_app.h` / `play_app.cpp`

Receiver runtime bootstrap. It validates the input transport URL, configures the
receiver, and either drains decoded frames in test mode or forwards synchronized
frames to DeckLink SDI playout through `OutputManager`.

Main responsibilities:

- configure receiver transport and audio-routing options;
- run the receiver in test mode without hardware output;
- run receiver-to-SDI playout through `OutputManager`;
- publish lightweight `/tmp` JSON control/state files for live audio-route
  inspection and updates during receiver operation.

## Ownership and shutdown rules

- The app layer passes `std::atomic<bool>& shutdownRequested` into long-running
  components instead of owning their worker loops directly.
- Sender media ownership is handled by `SenderPipeline` and downstream queues.
- Receiver media ownership is handled by `Receiver` and `OutputManager`.
- On receiver SDI playout, the app layer starts the receiver first, initializes
  DeckLink output second, then stops the control thread, receiver, and output in
  a deterministic order.

## Release notes

The app layer is a coordination boundary. Keep comments here focused on CLI
behavior, lifecycle order, and module responsibilities. Avoid adding media-format
or codec implementation details here unless they affect command behavior.

# NxFrame sender layer

The `sender/` folder contains the runtime sender pipeline. It connects the input
source, encoders, packet queues, muxer and network output while keeping each
module responsible for its own work.

## Pipeline position

```text
InputManager -> video/audio encode workers -> encoded packet queues -> OutputManager
```

## Files

### `sender_pipeline.h` / `sender_pipeline.cpp`

`SenderPipeline` is the top-level lifecycle object for sender mode. It starts the
input source, initializes the encoder manager, starts the transport/output layer,
launches worker threads and performs orderly shutdown.

The sender pipeline does not perform encoding or muxing directly. It coordinates
ownership and timing between the modules.

### `video_encode_worker.h` / `video_encode_worker.cpp`

Consumes `VideoFrame` objects from the live video queue and submits them to the
selected video encoder. In the normal DeckLink path, the worker passes the frame
object through to the encoder so the encoder can wrap the existing internal
`YUV422P10LE` buffer instead of copying it. The explicit copy path is kept for
debugging and validation.

### `audio_encode_worker.h` / `audio_encode_worker.cpp`

Consumes `AudioFrame` objects and submits them to the configured audio encoders.
It handles codecs that may buffer input internally, such as AAC, and publishes
all returned packets to the audio encoded-packet queue.

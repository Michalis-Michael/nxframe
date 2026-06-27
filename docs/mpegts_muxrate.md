# MPEG-TS muxrate / transport pacing

NxFrame sender presets can optionally request a stable transport rate:

```json
"mpegts": {
  "service_provider": "NxFrame",
  "service_name": "Studio Link A",
  "muxrate": 60000000
}
```

`muxrate` is **bits per second**, so `60000000` means **60 Mbps**. String values are also accepted:

```json
"muxrate": "60Mbps"
```

Aliases inside the `mpegts` object are also accepted:

```json
"ts": "60Mbps"
"ts_rate": 60000000
"ts_bitrate": 60000000
"transport_rate": 60000000
```

## Production-safe behavior

When `mpegts.muxrate` is set, NxFrame currently uses it as the **transport pacing rate**:

1. SRT application-side pacing is enabled at the same bitrate.
2. SRT `inputbw` is set to the muxrate if the preset did not already set it.
3. UDP/RTP pacing uses the same bitrate when applicable.
4. SRT messages are payloadized into MPEG-TS-aligned payloads, normally 1316 bytes = 7 TS packets.

NxFrame intentionally does **not** pass this value to FFmpeg's MPEG-TS `muxrate` option yet. In the current live timestamp model, FFmpeg's internal muxrate/null-packet mode can move PCR ahead of DTS and produce repeated:

```text
[mpegts] dts < pcr, TS is invalid
```

So the production-safe behavior is:

```text
mpegts.muxrate = clean user setting for transport pacing
FFmpeg muxrate = disabled
```

True CBR MPEG-TS null-packet padding should be implemented explicitly inside NxFrame later, after PCR/DTS ownership is fully controlled by NxFrame.

## Recommended values

For a 45 Mbps H.264 10-bit 4:2:2 feed with three S302M/PCM audio pairs, use:

```json
"muxrate": 60000000
```

For a slightly lower overhead target:

```json
"muxrate": 56000000
```

If the value is accidentally too small, for example `560000`, NxFrame prints a warning because that means 560 kbps, not 56 Mbps.


## True-CBR null stuffing (experimental)

NxFrame can optionally make `mpegts.muxrate` the actual wire bitrate by inserting
PID `0x1FFF` null TS packets in the NxFrame output layer, while still keeping
FFmpeg's internal `muxrate` option disabled:

```json
"mpegts": {
  "service_provider": "NxFrame",
  "service_name": "Studio Link A",
  "muxrate": 60000000,
  "null_stuffing": true
}
```

With `null_stuffing: false` or omitted, NxFrame keeps the stable transport-pacing
behaviour: the stream is paced but not padded to a fixed TS bitrate.

With `null_stuffing: true`, the output manager sends fixed-size TS payloads
(normally 1316 bytes = 7 TS packets) at the muxrate interval. If FFmpeg has not
produced enough media TS packets for a given slot, NxFrame fills the slot with
standards-compliant PID `0x1FFF` null packets.

Do not enable FFmpeg's own MPEG-TS `muxrate` option in this mode. FFmpeg still
creates PAT/PMT/PCR/PES; NxFrame only performs the final CBR output shaping.

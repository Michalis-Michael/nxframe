<!--
NxFrame - broadcast contribution encoder/decoder
SPDX-License-Identifier: GPL-3.0-or-later

This validation note is part of the NxFrame source tree. Use, redistribution,
and modification are governed by the project license and any written EULA or
commercial license agreement supplied with the project.
-->

# NxFrame Transport Comparison Procedure: RTP multicast, RTP unicast, and SRT

This procedure is intended for receiver transport validation after encoder, muxer, receiver, or transport changes. Keep the same source, encoder preset, receiver preset, DeckLink devices, and network path as much as possible.

## Scope

Use this document to compare the transport layer only. Keep the same input signal, encoder preset, receiver preset, DeckLink devices, and network path wherever possible so differences are not caused by unrelated preset or hardware changes.

## Pass / fail counters

For every run, record the final sender and receiver logs. A clean run should keep these at zero, except for startup-only anchor drops:

- Sender: `muxfail=0`, `sendfail=0`, `tsrepair[v_dts/v_pts]=0/0`, `pushfail=0/0/0/0`.
- Receiver playout: `late_v_drop=0`, `fallback_v=0`, `fallback_a=0`, `dropped_v=0`, `dropped_a=0`, `dup_v=0`, `dup_a=0`.
- RTP/UDP receiver: `udp_drop=0`, `udp_ts_sync=0`; isolated `rtp_gap` / `udp_ts_cc` may be logged as `rx_soft_loss` and should not cause `hard_resync_total` to increase.
- SRT receiver: no reconnects or transport queue drops during a local clean test.

## Test A: RTP multicast

Sender:

```bash
./NxFrame send decklink 0 to rtp://239.10.10.5:5004 encoder preset x264_1080i50_aac_lowlatency
```

Receiver:

```bash
./NxFrame play rtp://239.10.10.5:5004 to decklink 1 --receiver-preset receiver_audio_route_example
```

Watch for: `rtp_gap`, `udp_ts_cc`, `rx_soft_loss`, `rx_hard_loss`, and `hard_resync_total` in `[PLAY-DECKLINK]`. If only multicast shows gaps, investigate switch IGMP snooping, NIC receive buffer limits, multicast flooding, and receiver CPU scheduling.

## Test B: RTP unicast

Use the receiver host IP instead of the multicast group. Example receiver IP: `192.168.10.20`.

Sender:

```bash
./NxFrame send decklink 0 to rtp://192.168.10.20:5004 encoder preset x264_1080i50_aac_lowlatency
```

Receiver:

```bash
./NxFrame play rtp://0.0.0.0:5004 to decklink 1 --receiver-preset receiver_audio_route_example
```

Interpretation: if unicast is clean while multicast has `rtp_gap`, the encoder/muxer path is likely healthy and the multicast network path needs tuning.

## Test C: SRT listener/caller

Listener receiver:

```bash
./NxFrame play srt://0.0.0.0:5000 to decklink 1 --receiver-preset receiver_audio_route_example
```

Sender caller:

```bash
./NxFrame send decklink 0 to srt://<receiver-ip>:5000 encoder preset x264_1080i50_aac_lowlatency
```

Interpretation: if SRT is clean while RTP has soft loss, the remaining issue is UDP/RTP packet delivery rather than encoding, muxing, decoding, or SDI playout.

## Suggested run lengths

- Quick confidence test: 5 minutes per transport.
- Pre-tag soak: 30 minutes per transport.
- Pre-beta soak: 4 to 8 hours on the preferred production transport.

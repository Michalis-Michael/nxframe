# Sender TS timing diagnostics

Add `"timing_diagnostics": true` to the active preset's `mpegts` object
(or supported `output.mpegts` object). The default is false. No existing
presets are changed by this patch. Restore false or remove the field to disable.
It must be a JSON boolean. The existing PID validation test covers this setting.

The sender prints one `[OutputManager][TS-TIMING]` summary approximately every
two seconds, including when the dashboard is enabled. No per-packet logging is
performed. Enabling diagnostics adds clock reads, TS header inspection and queue
snapshots; summary output can block like the existing diagnostic console output.
The disabled path performs none of these operations. No packet bytes, timestamp
values, queue contents, encoder settings, pacing or recovery decisions are changed.

All timing fields are nanoseconds; each range has count/min/max. A zero count
means unavailable, so its min/max must be ignored.

- `schedule_lateness_ns`: transport call start minus the existing CBR deadline.
  Unavailable for the unpaced/VBR path.
- `call_duration_ns`: elapsed time in the transport API, including failed calls.
- `pcr_value_gap_ns`: successive accepted source PCR increments, handling wrap.
- `pcr_call_gap_ns`: time between the API calls containing those PCRs. Several
  PCRs in one payload share a call time. This is not NIC or receiver arrival time.
- `pcr_vs_packet_clock_ns`: elapsed PCR time minus elapsed accepted TS-byte time
  at the configured CBR muxrate, anchored to the first accepted PCR in a session.
  It includes null packets and PCR byte positions. Positive means PCR has advanced
  more than the byte clock. It is unavailable without constant-rate stuffing.
- `last_pcr_27m`, `pes_pts_90k`, `pes_dts_90k`: values parsed from accepted payloads.
  `mux_input_dts_90k` separately shows the latest DTS submitted to the muxer.
  Missing timestamp values are -1. PES parsing covers headers within one TS packet.
- `ts_queue`, `ts_queue_max`, `video_queue`, `audio_queue`: post-call queue snapshots
  (TS packets versus encoded packet queue entries). The maximum covers this window.
- `clock_events`: session changes, announced discontinuities, or backward PCRs;
  these restart the timing comparison. Natural PCR wrap does not restart it.
- `failed`, `invalid`, `unaligned`: failed transport calls and malformed TS input.
  Failed calls do not advance the accepted-byte clock or supply PCR/PES samples.

Summary counters/ranges reset after each report; clock anchors persist until a
clock event. The first interval includes startup. Diagnostics apply to SRT, UDP
and RTP at the TS payload boundary, excluding transport headers. Successful SRT
calls mean library acceptance, not delivery. These logs cannot certify on-wire
PCR accuracy, transport recovery success, or receiver buffering compliance.

## Focused validation

The new `ts_timing_diagnostics` test covers PCR/PES decoding, byte-clock phase,
API timing, 10,000 paced payloads, unchanged packet bytes, failed sends, stalls,
wraparound, discontinuities, session changes, backward clocks, malformed headers,
adaptation-only packets and the unpaced path. It passed alongside
`mpegts_pid_validation`, `muxer_ts_options`, `muxer_session_anchor_state` and
`muxer_ts_null_packet`. OutputManager passed a C++14 syntax check.
The diagnostic parser also passed AddressSanitizer and UndefinedBehaviorSanitizer
with leak checking disabled because this execution environment prevents it.
Live SRT/UDP/RTP integration and NIC/receiver timing were not exercised.


## Phase A: pacer observability (no scheduling changes)

When `mpegts.timing_diagnostics=true`, the existing periodic `TS-TIMING` line also
reports the following **transport API submission** statistics:

- `send_begin_gap_ns`: elapsed monotonic time between consecutive transport
  API call starts (includes failed attempts); `send_gap_lt100us` and
  `send_gap_gt2ms` count unusually close/long intervals.
- `send_spacing_error_ns`: observed API-call spacing minus corresponding
  scheduled-deadline spacing; the quantity is signed.
- `schedule_lateness_ns` and `call_duration_ns`: existing deadline miss and
  transport-API call duration statistics.
- `cbr_recovery`, `vbr_recovery`, `resyncs`, `recovery_lateness_ns`:
  events where the existing scheduler resets its deadline after exceeding
  the internal recovery window. Counters are per reporting interval.
- `ts_queue_avg`, `ts_queue_max`, `video_queue_max`, `audio_queue_max`:
  sampled queue occupancy (not actual enqueue-to-dequeue residence time).
- `estimated_ts_queue_delay_ns`: TS queue occupancy converted to duration
  using the configured **constant** transport bitrate; unavailable for VBR.
- `pcr_schedule_gap_ns`: elapsed scheduled deadline between successive
  PCR-bearing **accepted transport API payloads**, when both have deadlines.
- `pcr_call_vs_value_gap_error_ns`: elapsed PCR-bearing API-call-start time
  minus the elapsed time encoded in successive PCR values. This is a useful
  *clock divergence indicator*, **not** a DVB PCR accuracy measurement on the
  wire. When PCRs are present in the same datagram, their actual transmission
  times cannot be resolved from API-call timestamps.

Important: SRT owns its network scheduler and retransmissions. These stats do
not measure physical NIC departure, Wi-Fi delivery, SRT retransmission timing,
or actual media residence time. Capture PCAP data independently for that.

## Phase B: conservative clock-recovery guard

On a true-CBR scheduler reset, the next scheduled application submission
remains behind the last successfully completed submission by at least the
profile's existing minimum-gap guard. This does **not** drop queued media,
retime MPEG-TS packets or PCRs, add buffering, or control libsrt retransmits.

`recovery_delay_total_ns` sums the positive scheduling lateness at recovery
resets in each reporting interval. It is **not** additional buffering latency,
and must not be interpreted as elapsed PCR drift. The PCR byte clock remains
anchored to transmitted TS bytes.

Phase B PCR retiming is intentionally deferred: API-call clocks cannot safely
be substituted for the muxer PCR clock without also addressing media timestamp
continuity, and SRT API acceptance is not wire departure time.

### CBR queue direction (Phase B diagnosis)

`ts_queue_min`, `ts_queue_growing_samples`, `ts_queue_draining_samples`,
`ts_queue_flat_samples`, `ts_queue_max_growth`, and `ts_queue_max_drain`
are derived from queue depths sampled at consecutive transport API calls.
They are not per-packet enqueue/dequeue counters or packet residence times.
The previous queue depth is retained across reporting intervals.

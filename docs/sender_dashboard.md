# Sender CLI dashboard

The sender uses the approved SYSTEM-first dashboard, refreshed in place once
per second. Labels and values share fixed columns throughout, with separators
between input, encoder, timing, cadence, transport and metadata/health.
Two related values share each line. Timing has separate average/max columns.
OS name and CPU model are sampled at startup, alongside core topology.

A terminal approximately 96 columns by 48 rows shows the entire dashboard.
Smaller terminals keep the same layout in a fixed viewport; Up/Down,
Page Up/Page Down and Home/End move the view without adding terminal scrollback.
The footer shows the visible row range. Widening the terminal adjusts the
column widths. Long first-column values end with `~` when shortened.
Keyboard handling preserves Ctrl+C and restores the original terminal mode
on normal shutdown. It does not change encoding or transport settings.

Rendering goes to `/dev/tty`, so it works with stdout piped through `tee`.
Without a controlling terminal, plain-text snapshots are emitted every two
seconds without control sequences. Dashboard frames are not written to the
`tee` log when a controlling terminal is available.

Run the existing sender command; no additional dashboard flag is required.
`--timing-verbose` retains diagnostics in recent events on a terminal, or in
plain logs without a terminal. Sender timing is automatically enabled;
receiver behavior is unchanged. `--timing` remains accepted.

## Measurements

- Capture rate counts input frames. Encoded rate counts output packets, not
  necessarily pictures when using other codec/reordering arrangements.
- Rates are normalized by actual elapsed time; a partial shutdown interval is
  not assumed to last one second.
- Timing averages use samples recorded since the previous refresh. Stage
  snapshots are observational relaxed-atomic counters, not tracing-grade
  percentiles. Maximum values are session maxima and clearly labelled.
- A missing/no-sample metric is not presented as a measured zero.
- Capture drops count overflow evictions in all DropOldest queue APIs. Normal
  consumption and queue stop do not increment this count.
- Encoding over-budget calls are wall-clock API calls, including codec waiting;
  this is not isolated CPU cost or a per-picture worker-thread measurement.
- Recovery discards, keyframe-wait discards, encoded overflow and capture
  evictions are separate counters.
- SRT loss/retransmission/drop values belong to the current socket and can reset
  after reconnect. The dashboard displays sample age. Reconnect/error counters
  remain separately labelled. No network measurement is invented for UDP/RTP.
- Detected format comes from capture metadata. Encoder format/colour and audio
  output channels come from opened codec contexts. Preset/profile/thread/level
  descriptions are configured values, not a claim that auto-thread selection
  has been queried. A signal with no recent frames is marked stale.
- ANC capability is sampled directly from DeckLink queries. Timecode and caption
  presence refer to the current observed frame. No timecode is synthesized.
- Credentials, passphrases and stream IDs are never copied into dashboard fields.

## CPU, memory and reference

CPU and memory counters are read once per reporting interval from Linux `/proc`.
No external commands or frame-callback work are added. System CPU is the busy
percentage across all online CPUs; idle and I/O wait are excluded. NxFrame CPU
sums all process threads: 100% means one logical CPU, so it can exceed 100%.
Physical and online logical counts describe the host; affinity is the number
of logical CPUs allowed to this process. Topology is read only at startup.
NxFrame memory is resident memory (RSS); system used RAM is MemTotal minus
MemAvailable, allowing for reclaimable memory. These are host measurements,
not container CPU-quota or memory-limit measurements.

Genlock is read from DeckLink status interfaces once per reporting interval.
The monitor owns SDK interface references independently of capture shutdown.
Reference lock is separate from SDI input lock. PAL/NTSC or a detected display
mode is shown when available. The SDK mode does not identify sync waveform;
HD is not automatically labelled tri-level. An unlocked reference with no
reported mode is labelled absent or undetected, because the API cannot prove
that a cable is missing. Unsupported hardware and unavailable queries have
separate states. A generated test source shows not applicable.

## Output and lifecycle

C++ application logs are collected into recent events. Recurring diagnostic
noise is suppressed unless verbose mode is enabled. Native library writes to
stderr may appear outside the dashboard; the next refresh redraws it. Ctrl+C
uses existing shutdown and restores the terminal/cursor, then prints a session
summary. SIGKILL cannot restore the terminal; use `reset` if needed.

Validation for this incremental patch: strict C++14 renderer/resource tests,
80x24, 94x34 and 100x50 PTY redraw/navigation/terminal-mode restoration
tests with direct and piped stdout,
and syntax compilation of sender/capture/input translation units against
FFmpeg 8.1 and the bundled DeckLink API. Live reference lock transitions require
validation on the DeckLink machine; no hardware test is claimed.

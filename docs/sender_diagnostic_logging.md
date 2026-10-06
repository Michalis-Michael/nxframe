# Sender diagnostic history

The sender now prints its diagnostic filename at startup and writes a separate
plain-text file, default `sender_diagnostics_<PID>.log` in the working directory.
The live dashboard retains its alternate-screen display, layout and navigation.
`tee` still captures ordinary startup/shutdown output; use the diagnostic file
for per-second performance history. Logging does not require `--timing-verbose`.

Each reporting interval (approximately one second) retains capture/encoded rates,
raw/packet queue depths, capture drop delta and total, over-budget encode calls,
transport/recovery counters, CPU/RSS and stage timing samples. Timing averages
and call counts cover the reporting interval; `max_session_ms` is explicitly a
session maximum, not an interval maximum. Worker cadence summaries and >40 ms
encode/input/packet/mux gaps are also retained even with display details off.
Initial codec buffering and shutdown draining can affect packet-rate intervals;
API call duration is not end-to-end encoding latency.

Logs start when the sender dashboard starts, before capture initialization.
Native x265/FFmpeg C-library stderr output is not intercepted by this file;
keep `sender.log` from `tee` alongside it. On normal shutdown the logger drains
its queue and adds a session summary plus its own dropped-record/write-error
counts. Forced termination cannot guarantee a complete log.

Override the path (choose a distinct path for each concurrent sender):

```sh
sudo env NXFRAME_DIAGNOSTIC_LOG=/tmp/hevc_diagnostics.log ./NxFrame send decklink 0 to srt://0.0.0.0:5000 encoder preset x265_1080p50_10bit_422_pcm_test --timing 2>&1 | tee -i sender.log
```

Files are appended and created with permissions 0600. When started using sudo,
new files are assigned to the invoking user/group (`SUDO_UID`/`SUDO_GID`), so
that user can open/upload them without changing permissions. Ordinary user runs
retain their normal owner; direct root/service runs retain root ownership.
Existing files retain their ownership and permissions. If an older root-owned
log needs to be reused, change its ownership once or choose a fresh path.
Malformed sudo IDs or ownership-assignment failures disable logging with the
existing warning; streaming continues. Devices, FIFOs and symlinks are rejected. Failure to open the log
warns but does not stop streaming. Files do not rotate automatically; remove or
archive them between runs. A bounded queue of 256 records drops new records
rather than waiting for disk I/O; counts are reported. Regular-file I/O occurs
only on the logging thread; normal shutdown joins that thread and can still
wait on an unusually slow or stalled filesystem. Use a local filesystem.

This change does not tune HEVC, change threading in x265, alter frame ownership,
modify queue policies, or change transport/receiver behavior. It adds one
background file writer and enables existing sender cadence diagnostics for it.

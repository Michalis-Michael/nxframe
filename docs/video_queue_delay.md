# Capture queue comparison

Default raw video queue capacity remains two frames. Select either capacity:

```
--capture-queue-frames 2
--capture-queue-frames 4
```

No JSON, codec quality, codec threading, audio queue or encoded packet queue settings change. Raw capture retains the existing drop-oldest policy. The CLI rejects missing values and values other than 2 or 4 before starting capture. Other SenderPipeline callers retain the default of two.

The existing dashboard refresh behavior remains; one Queue wait row is added. Diagnostic INTERVAL records include capacity, sampled session queue peak, queue delay average, nearest-rank p95, maximum, sample count and omitted sample count. Delay statistics reset each reporting interval; the peak is the existing sampled session high-water observation, not an exact push-time peak.

Queue wait measures steady-clock elapsed time from immediately before publishing the normalized frame to the raw queue until the worker starts its encode call. It includes queue locking and worker preparation. It excludes capture normalization, codec buffering, transport and receiver delay. Generated test input uses a blocking push, so its measurement also includes producer backpressure. This is not end-to-end latency. Existing video_q_wait still measures worker waiting for input.

Only frames reaching an encode call contribute samples; evicted frames remain visible in capture_drop_delta/total. Collection stores at most 4096 samples per interval without producer allocation; additional samples are explicitly reported as queue_wait_omitted. Sorting happens in the reporting thread outside the collection lock. With omissions, statistics describe retained samples.

Run the same scene sequence and CPU profile for at least 60 seconds with each capacity. Close NxFrame playback and VLC; use the same stream-copy receiver for both tests. Compare capture_drop_delta/total, encoded_pps, over_budget, queue_wait_avg_ms/p95_ms/max_ms, and system CPU usage. Four frames can absorb longer temporary stalls at the cost of more waiting; it cannot fix sustained encoding overload. At 50 fps, two additional slots represent 40 ms of additional queue capacity, not a guaranteed end-to-end latency limit.

Build and tests from the repository:

```
cmake --build build -j
ctest --test-dir build --output-on-failure -R 'video_queue_delay|sender_dashboard|pipeline_telemetry_report|bounded_queue_policy'
```

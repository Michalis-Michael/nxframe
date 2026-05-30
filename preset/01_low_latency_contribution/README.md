# 01_low_latency_contribution

Curated low-latency contribution presets.

These are the normal starting point for live contribution testing. They prioritize:

- 10-bit 4:2:2 x264 contribution where applicable
- CBR/VBV/HRD behaviour
- closed GOP, no B-frames, no scenecut/open-GOP
- BT.709 limited-range signalling
- predictable low-latency encode behaviour

For alpha.12, this folder is the canonical source for low-latency contribution presets. The CLI still accepts short preset names without the folder or `.json` suffix.

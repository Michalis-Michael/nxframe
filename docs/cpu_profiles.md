# NxFrame CPU profiles

NxFrame can optionally apply a Linux CPU frequency profile while the sender is running.
This is useful when the encoder is already comfortably real-time, but the CPU keeps boosting to unnecessarily high clocks and power.

Example:

```bash
sudo ./NxFrame send decklink 0 to srt://192.168.1.50:9000 encoder preset x264_1080i50_pcm_dolby_quality -cpu_profile profile_1
```

Equivalent long option:

```bash
sudo ./NxFrame send decklink 0 to srt://192.168.1.50:9000 encoder preset x264_1080i50_pcm_dolby_quality --cpu-profile profile_1
```

By default NxFrame looks for:

- `config/cpu_profiles.json`
- `config/cpu_profile.json`
- the same paths relative to the executable
- `/usr/local/share/nxframe/config/cpu_profiles.json`
- `/usr/share/nxframe/config/cpu_profiles.json`

You can override this:

```bash
sudo ./NxFrame send decklink 0 to srt://192.168.1.50:9000 encoder preset x264_1080i50_pcm_dolby_quality --cpu-profile profile_1 --cpu-profile-config /etc/nxframe/cpu_profiles.json
```

## JSON format

```json
{
  "profiles": {
    "profile_1": {
      "description": "1080i50 quality cap",
      "max_frequency": 3.8,
      "min_frequency": 0,
      "restore_on_exit": true
    }
  }
}
```

Frequency values can be written in several formats:

- `"max_frequency": 3.8` means 3.8 GHz.
- `"max_frequency_ghz": 3.8` means 3.8 GHz.
- `"max_frequency_mhz": 3800` means 3800 MHz.
- `"max_frequency_khz": 3800000` means 3800000 kHz.
- `"max_frequency": "3.8GHz"` or `"3800MHz"` also work.

`min_frequency: 0` means auto: NxFrame does not force a minimum frequency unless it must temporarily lower the current minimum so the requested maximum can be accepted by cpufreq.

## Restore behavior

NxFrame captures the current `scaling_min_freq`, `scaling_max_freq`, and `scaling_governor` for each CPU before applying the profile. When the sender exits normally or after Ctrl+C, the previous values are restored automatically.

This cannot protect against `SIGKILL`, a kernel crash, or sudden power loss. If that happens, restore manually or reboot.

## Permissions

The helper writes to Linux cpufreq sysfs files under:

```text
/sys/devices/system/cpu/cpu*/cpufreq/
```

That normally requires root. For development, run the sender with `sudo`. For production, prefer a small privileged launcher, systemd service policy, or udev/sysfs permission rule rather than running the whole encoder as root.

# Changelog

## 0.1.1

### Changed

- Added startup MP3 prebuffer.
- Listener now receives approximately 750 ms of valid MP3 frames before realtime streaming.
- Keeps stream/server alive during supported track/sample-rate changes.

### Verified

- AIMP DSP
- HTTP streaming
- VLC playback
- Start/Stop cycles
- Pause/Play
- 44.1 → 48 kHz transition
- Multiple Next operations
- Port collision handling

Version 0.1.0 was confirmed working in ETS2 by a real user. Version 0.1.1 retains the same URL and protocol path while changing startup buffering. Version 0.1.1 was not independently rerun in ETS2 on the release-build machine.

# Validation summary for 0.1.1

Validation date: 2026-09-14

This is the path-scrubbed public summary. It intentionally omits local installation paths, usernames, raw logs, and user files.

## Environment

- Windows 11 24H2 x64, build 26100
- AIMP 5.40.2700 x64
- LAME 3.100 supplied by AIMP
- VLC 3.0.23
- GCC/MinGW-w64 16.2.0 via w64devkit 2.9.1

## Startup diagnosis and change

The 0.1.0 encoder produced its first valid MP3 frame quickly, but a newly connected listener initially received small realtime writes with no retained MP3 reserve. Version 0.1.1 adds a bounded rolling queue of complete LAME output blocks with a 750 ms target. Early listeners wait for readiness; later listeners receive the retained blocks immediately before joining realtime delivery.

Measured with real AIMP playback, the first PCM arrived 16 ms after Start, the first valid MP3 frame at 47 ms, and the prebuffer became ready at 672 ms with 24,576 bytes. The callback, encoder format, HTTP endpoint, pause behavior, and Start/Stop lifecycle were otherwise unchanged.

## Completed checks

- Release x64 build and CTest artifact check
- PE32+ x86-64 DLL and version resource 0.1.1.0
- `winampDSPGetHeader2` export and AIMP plug-in load
- HTTP 200 and `audio/mpeg` at the exact `/stream` path
- Valid MP3 frames from byte offset 0 for early and late listeners
- Approximately 24–25 KB startup reserve
- Two simultaneous listeners
- Three repeated Start → Stop → Start cycles on one loaded DLL
- Pause/Play while keeping the connection alive
- One connection across a 44.1 → 48 kHz source-rate change
- One real-AIMP listener across four Next operations without reconnecting
- VLC decode as 48 kHz stereo MP3 at 256 kbps
- Port collision error with Winsock code 10048
- `/stream/` returning 404 by design
- Windows Defender custom scan of the tested DLL with no detected threat

## ETS2 status

Version 0.1.0 was confirmed working in ETS2 by a real user, including track switching. The earlier failure was caused by saving `/stream/` instead of the exact `/stream` endpoint.

Version 0.1.1 keeps the same URL and protocol while changing startup buffering. It was not rerun in ETS2 on the release-build machine because a working local game executable was unavailable. No `live_streams.sii` or other user game file was changed during validation.

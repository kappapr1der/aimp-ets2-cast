# AIMP ETS2 Cast

A small Windows DSP plugin that turns whatever AIMP is currently playing into a local MP3 radio stream for Euro Truck Simulator 2.

Основная русская инструкция: [README.md](README.md)

## What it does

```text
AIMP
  ↓
DSP PCM
  ↓
MP3 encoder
  ↓
local HTTP stream
  ↓
ETS2 Internet Radio
```

AIMP remains the player and the only place where you control music. The plugin receives decoded PCM from AIMP's Winamp-compatible DSP callback, encodes it as MP3, and serves it at one loopback-only URL:

```text
http://127.0.0.1:6969/stream
```

## Features

- Uses the current AIMP playlist.
- No copying music into the ETS2 music folder.
- Shuffle, next, previous, repeat, pause, and play remain controlled by AIMP.
- Local-only HTTP server bound to `127.0.0.1`.
- 48 kHz stereo MP3 CBR stream at 256 kbps.
- Approximately 750 ms startup prebuffer to prevent initial underruns.
- Keeps the listener connected across track and supported sample-rate changes where possible.
- Sends the current artist and track title as ICY metadata to compatible listeners.
- Clients that do not request ICY metadata continue receiving the original raw MP3 stream.
- A bounded per-listener output queue absorbs transient local-socket
  backpressure without blocking the DSP or dropping the connection.
- No virtual audio cable.
- No external Icecast or Shoutcast server.
- No separate music player.

## Requirements

The 0.1.2 release was built and validated with:

- Windows 11 24H2 x64 (build 26100).
- AIMP 5.40.2700 x64.
- The x64 `libLAME.dll` supplied with that AIMP installation (LAME 3.100).
- A free TCP port 6969 on loopback.

This release is x64 only. It will not load into a 32-bit AIMP process. VLC is not required; VLC 3.0.23 was used only as an independent playback check.

The release does not bundle AIMP, LAME, BASS, BASSenc, or BASSenc_MP3. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Installation

1. Download `aimp_ets2_cast-0.1.2.aimppack` from the GitHub Release.
2. Open the file and confirm installation in AIMP.
3. Start or restart AIMP.
4. Open AIMP Preferences, go to **Plugins**, and enable **AIMP ETS2 Cast** if it is not already enabled.
5. Open the plugin's DSP configuration window from AIMP's sound-effects/DSP controls.

The `.aimppack` contains the plugin DLL in AIMP's required `x64` package directory. The release also provides a manual-install ZIP; copy its `aimp_ets2_cast` folder into AIMP's `Plugins` directory while AIMP is closed.

## Usage

1. Start AIMP and play a normal local track or playlist.
2. Enable **AIMP ETS2 Cast** as the DSP plugin.
3. Open its small configuration window and click **Start**.
4. Use exactly:

   ```text
   http://127.0.0.1:6969/stream
   ```

> **Do not add a trailing slash.**
>
> Correct: `http://127.0.0.1:6969/stream`
>
> Wrong: `http://127.0.0.1:6969/stream/`

Version 0.1.2 intentionally exposes `/stream`; `/stream/` may return `404 Not Found`.

Click **Stop** to close the encoder, listener connections, and HTTP server. Repeated Start → Stop → Start cycles are supported without restarting AIMP.

## ETS2 setup

Back up `Documents\Euro Truck Simulator 2\live_streams.sii` before editing it. Do not replace the whole file or remove existing stations.

The exact `live_streams.sii` wrapper and station count vary between game versions. The following is an **example station record only**, not a complete universal file:

```text
stream_data[N]: "http://127.0.0.1:6969/stream|Дальнобой FM|Local|RU|256|0"
```

Use the next valid index instead of `N` and update the file's existing `stream_data` count as required by its current syntax. Preserve every existing station. If your ETS2 version provides an in-game station editor, prefer adding the exact URL there.

Version 0.1.0 was confirmed working in ETS2 by a real user, including track switching. Version 0.1.2 keeps the same URL and MP3 format. ICY metadata is inserted only when a client explicitly requests it, preserving compatibility with existing listeners.

## Known limitations

- ETS2 1.61 plays the stream but does not display the current ICY track title. Compatible external players still receive metadata; this limitation does not affect ETS2 audio.
- The endpoint is intentionally local only; other computers and phones cannot connect.
- The release is x64 only.
- `/stream/` is not equivalent to `/stream` in 0.1.2.
- The DLL is not digitally signed, so Windows may display a publisher warning.

## Troubleshooting

### Port 6969 is already in use

Stop the other program using port 6969, then click **Start** again. The plugin intentionally does not choose another port.

### The stream works in a browser or VLC but not in ETS2

Check the saved station URL character by character. The most common cause is an accidental trailing slash. Also make sure AIMP is playing and broadcasting before selecting the station in ETS2.

### The plugin is not visible in AIMP

Confirm that AIMP is x64, restart AIMP after installation, and check that the plugin is enabled under Preferences → Plugins. A 32-bit AIMP process cannot load this release.

### There is no audio

Play a local track in AIMP, open the plugin window, and verify that its status changes to **Broadcasting**. Test the exact URL in another player. AIMP's DSP input must be 16-bit PCM; unsupported input is reported in the log.

### Log location

The diagnostic log is written to:

```text
%APPDATA%\AIMP\AIMP-ETS2-Cast\ets2cast.log
```

If the primary directory cannot be used, the plugin tries `%LOCALAPPDATA%`, TEMP, and finally the DLL directory. It logs lifecycle, PCM parameters, startup timing, listeners, and errors, but never logs PCM payloads or music files.

## Building from source

The project uses C++17, CMake, Ninja, and an x64 MinGW-w64 compiler. No encoder SDK or import library is needed because `libLAME.dll` is resolved dynamically at runtime.

```powershell
.\build.ps1 -ToolchainBin ".\w64devkit\bin" -Configuration Release -Clean
```

The script runs CTest and creates the two release packages in `dist/`. Build products and release binaries are intentionally ignored by git.

## Validation

The validation report for version 0.1.2 is in [docs/VALIDATION_0.1.2.md](docs/VALIDATION_0.1.2.md). Version history is in [CHANGELOG.md](CHANGELOG.md).

## Credits

- Original concept: [@nuclearsunrise](https://github.com/nuclearsunrise)
- Production and project development: [@kappapr1der](https://github.com/kappapr1der)

## License

The independently written AIMP ETS2 Cast source code is licensed under the [MIT License](LICENSE). Third-party programs and libraries keep their own licenses and are not relicensed by this repository.

Artem Izmaylov's [AIMP LanCast](https://github.com/ArtemIzmaylov/aimp_lancast) (MPL-2.0) was used only as a technical reference for AIMP's use of the standard Winamp DSP callback ABI. No LanCast Delphi source, UI resources, BASS_WMA code, or compiled binaries are included or adapted here. Details are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

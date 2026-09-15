# Third-party notices

This file describes software that AIMP ETS2 Cast interoperates with or used as a development reference. No third-party runtime binary is stored in this repository or bundled in the release assets.

## AIMP ETS2 Cast source

The C++ source in this repository was written independently and is licensed under the MIT License in `LICENSE`.

## LAME / libmp3lame

AIMP ETS2 Cast dynamically resolves the LAME encoder API from the x64 `libLAME.dll` already supplied by the user's AIMP installation. The tested AIMP 5.40.2700 installation supplies LAME 3.100.

LAME 3.100 project materials identify the GNU Library General Public License, version 2. See the LAME source archive's `LICENSE` and `COPYING` files and <https://lame.sourceforge.io/>.

This repository and its release assets do **not** contain, modify, or redistribute `libLAME.dll`. Users obtain it as part of their own AIMP installation and should also consult the notices shipped with AIMP.

## AIMP

AIMP is the required host application. No AIMP source code, executable, DLL, skin, configuration, or user data is included. AIMP's name is used only to describe compatibility. See <https://www.aimp.ru/>.

## BASS, BASSenc, and BASSenc_MP3

These libraries were evaluated during prototyping but are not used by the 0.1.1 implementation. The plugin does not link to them, dynamically load them, or redistribute them. The `bass.dll` present in the tested AIMP installation is not a dependency of this plugin.

BASS and its add-ons have their own licensing terms; see <https://www.un4seen.com/bass.html>. Because no BASS binary or code is used or distributed, those terms do not license the source in this repository.

## AIMP LanCast technical reference

Artem Izmaylov's [AIMP LanCast](https://github.com/ArtemIzmaylov/aimp_lancast) is licensed under the Mozilla Public License 2.0. Reference revision: `c643fc1052cb8c2d556c03f097ae2530053633a7` (2024-02-28).

It was inspected to confirm how AIMP calls the standard Winamp DSP `ModifySamples` callback and supplies PCM. The implementation in this repository is independent C++ code. No LanCast Delphi source, ACL code, form resources, WMA/MMS server implementation, BASS_WMA integration, or compiled binary was copied or adapted. MPL-2.0 therefore applies to the referenced LanCast project, not to AIMP ETS2 Cast.

## Winamp DSP ABI

The project declares the small, standard Winamp DSP plug-in ABI needed by AIMP: the public header/module layouts, callback signatures, header version, and `winampDSPGetHeader2` export. No Winamp implementation or binary is bundled.

## Build toolchain

The validated release build uses w64devkit 2.9.1 with GCC/MinGW-w64 16.2.0, CMake 4.4.2, and Ninja. These are build-time tools only and are not included in the repository or release assets. See <https://github.com/skeeto/w64devkit> for their component licenses. The DLL statically links the GCC and MinGW runtime portions permitted by their applicable runtime exception and licenses.

Windows SDK headers and Windows system libraries are used through the normal platform toolchain and operating-system interfaces.

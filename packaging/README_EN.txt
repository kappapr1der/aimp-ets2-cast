AIMP ETS2 Cast 0.1.2 x64

1. Start or restart AIMP.
2. Enable AIMP ETS2 Cast in AIMP Preferences > Plugins.
3. Select it in the DSP/sound-effects controls.
4. Open the plug-in window and click Start.
5. Use exactly:
   http://127.0.0.1:6969/stream

Do not add a trailing slash. /stream/ may return 404.

Compatible players receive the current track title through ICY metadata.
Clients that do not request ICY metadata receive the original raw MP3 stream.

The plug-in is x64 only and uses libLAME.dll from the installed AIMP. No
third-party DLL is included in this package. The DLL is not digitally signed.

Log:
%APPDATA%\AIMP\AIMP-ETS2-Cast\ets2cast.log

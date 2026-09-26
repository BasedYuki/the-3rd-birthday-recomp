# Third-party code

- `at3_standalone/`: the ATRAC3 / ATRAC3+ decoders from FFmpeg, as extracted to standalone
  C++ by PPSSPP (`ext/at3_standalone`, PPSSPP commit cae623f). LGPL-2.1-or-later (see the
  file headers), which is compatible with this project's GPL-2.0-or-later. Copied unmodified
  (minus its CMakeLists.txt); `at3_shim/` supplies the three PPSSPP headers it includes.

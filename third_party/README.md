# third_party

| Folder | What | Version | Licence | Local changes |
|---|---|---|---|---|
| `ft8_lib/` | FT8 and FT4 encoder/decoder by Kārlis Goba, https://github.com/kgoba/ft8_lib: `ft8/`, `common/monitor.*`, `common/common.h` | commit 9fec6ca39886edbf96f4f5e71edc76da5074e871 (2025-08-24) | MIT (`ft8_lib/LICENSE`) | Four, each marked "Local change (SDRconnect_classifier)": a fixed array instead of a variable-length one in `ft8/decode.c`; no `stpcpy` in `ft8/message.c`; work buffers allocated in `monitor_init` instead of variable-length arrays in `monitor_process` (these three for MSVC); `common/monitor.c` logs warnings only, not the FFT sizes of every slot. The repository's `ft4_ft8_public/` folder (WSJT-X Fortran) is not copied |
| `ft8_lib/fft/` | KISS FFT by Mark Borgerding, as bundled with ft8_lib; also used for this program's own FFTs | as in ft8_lib | BSD-3-Clause (file headers) | none |
| `sqlite/` | SQLite amalgamation, https://www.sqlite.org | 3.50.4 | Public domain | none |

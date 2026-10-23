# eboot_decrypt

Decrypts the game program (PSP_GAME/SYSDIR/EBOOT.BIN) from the player's own disc image, so the
launcher can build game.dll without PPSSPP. Built by scripts/make_kit.sh into the kit.

- `PrxDecrypter.cpp/.h`: from PPSSPP (https://github.com/hrydgard/ppsspp, Core/ELF), GPL-2.0-or-later
- `libkirk/`: from PPSSPP (ext/libkirk), GPL-3.0-or-later
- `shim/`: small stand-ins for the PPSSPP headers those files include
- `eboot_decrypt.cpp`: ISO reading and the command line (this project)

The resulting program is GPL-3.0-or-later.
- PrxDecrypter.cpp change: its includes of "ext/libkirk/..." read "libkirk/..."

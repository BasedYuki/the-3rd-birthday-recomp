# Unlock data

`ULUS10567SYSDATA/` is the game's system data from a fully cleared game (plain, as the port
saves it). The 3rd Birthday has no DLC: costumes, weapons, DNA boards and the Cheat Codes menu
unlock through this system data by finishing the game. The launcher's **Unlock all extras**
button installs it into `memstick/PSP/SAVEDATA/ULUS10567SYSDATA/`, backing up your current
system data to `memstick/BACKUP/` first. Your game saves and story progress are not touched;
the in-game Config settings go back to their defaults.

To do it by hand: copy the three files into `memstick/PSP/SAVEDATA/ULUS10567SYSDATA/`.

Source: a public clear-game save (CheatHappens), decrypted with `tools/save_import.py`.

// eboot_decrypt: the decrypted game program from the player's own disc image.
//
//   eboot_decrypt <game.iso> <out.elf>
//
// Reads PSP_GAME/SYSDIR/EBOOT.BIN from the ISO9660 image, decrypts the ~PSP module with
// PPSSPP's PRX decrypter (PrxDecrypter.cpp, GPL-2.0-or-later) and libkirk (GPL-3.0-or-later),
// and writes the ELF. This program as a whole is therefore GPL-3.0-or-later.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Core/ELF/PrxDecrypter.h"

static bool read_at(FILE *f, uint64_t off, void *buf, size_t n) {
    return _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

// Find a file by path in an ISO9660 image. Returns false if absent.
static bool iso_find(FILE *f, const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t pvd[2048];
    if (!read_at(f, 16 * 2048, pvd, sizeof(pvd)) || memcmp(pvd + 1, "CD001", 5)) return false;
    uint32_t dlba = pvd[156 + 2] | pvd[156 + 3] << 8 | pvd[156 + 4] << 16 | (uint32_t)pvd[156 + 5] << 24;
    uint32_t dsize = pvd[156 + 10] | pvd[156 + 11] << 8 | pvd[156 + 12] << 16 | (uint32_t)pvd[156 + 13] << 24;
    std::string rest = path;
    while (!rest.empty()) {
        size_t sl = rest.find('/');
        std::string name = rest.substr(0, sl);
        rest = sl == std::string::npos ? "" : rest.substr(sl + 1);
        std::vector<uint8_t> dir(dsize);
        if (!read_at(f, (uint64_t)dlba * 2048, dir.data(), dsize)) return false;
        bool found = false;
        for (uint32_t o = 0; o < dsize;) {
            uint8_t len = dir[o];
            if (!len) { o = (o / 2048 + 1) * 2048; continue; }
            uint8_t nl = dir[o + 32];
            std::string n((const char *)&dir[o + 33], nl);
            size_t semi = n.find(';'); if (semi != std::string::npos) n.resize(semi);
            if (_stricmp(n.c_str(), name.c_str()) == 0) {
                dlba = dir[o + 2] | dir[o + 3] << 8 | dir[o + 4] << 16 | (uint32_t)dir[o + 5] << 24;
                dsize = dir[o + 10] | dir[o + 11] << 8 | dir[o + 12] << 16 | (uint32_t)dir[o + 13] << 24;
                found = true;
                break;
            }
            o += len;
        }
        if (!found) return false;
    }
    *lba = dlba; *size = dsize;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: eboot_decrypt <game.iso> <out.elf>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    uint32_t lba, size;
    if (!iso_find(f, "PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &size)) {
        fprintf(stderr, "no PSP_GAME/SYSDIR/EBOOT.BIN in %s (not a PSP game disc image?)\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> in(size), out(size + 0x1000);
    if (!read_at(f, (uint64_t)lba * 2048, in.data(), size)) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
    uint32_t outsize = size;
    if (memcmp(in.data(), "~PSP", 4) == 0) {
        int n = pspDecryptPRX(in.data(), out.data(), size);
        if (n <= 0) { fprintf(stderr, "decryption failed (%d)\n", n); return 1; }
        outsize = (uint32_t)n;
    } else if (memcmp(in.data(), "\x7f" "ELF", 4) == 0) {
        memcpy(out.data(), in.data(), size);   // already plain
    } else { fprintf(stderr, "EBOOT.BIN is neither ~PSP nor ELF\n"); return 1; }
    if (memcmp(out.data(), "\x7f" "ELF", 4) != 0) { fprintf(stderr, "decrypted data is not an ELF\n"); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(out.data(), 1, outsize, o) != outsize) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fclose(o);
    printf("decrypted %u bytes -> %s\n", outsize, argv[2]);
    return 0;
}

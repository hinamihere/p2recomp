// Minimal PSP PRX/EBOOT decrypter built on pspdecrypt's PrxDecrypter + libkirk.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "PrxDecrypter.h"
extern "C" {
#include "libkirk/kirk_engine.h"
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: prxdec in.bin out.bin\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<u8> in(size), out(size + 0x1000);
    fread(in.data(), 1, size, f); fclose(f);
    kirk_init();
    int n = pspDecryptPRX(in.data(), out.data(), (u32)size, nullptr, true);
    if (n <= 0) { fprintf(stderr, "decrypt failed: %d\n", n); return 2; }
    f = fopen(argv[2], "wb");
    fwrite(out.data(), 1, n, f); fclose(f);
    printf("decrypted %d bytes\n", n);
    return 0;
}

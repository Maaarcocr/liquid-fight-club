/*
 * Replay fuzz inputs through eval/fuzz.c against a plain release library
 * (no sanitizers, no libFuzzer), to triage timeouts and OOMs.
 *   replay FILE...
 * Link: clang -O2 eval/fuzz.c eval/replay.c build/libliquid_fc.a -lpthread -ldl -lm
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    LLVMFuzzerInitialize(&argc, &argv);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n + 1);
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, got);
        free(buf);
    }
    return 0;
}

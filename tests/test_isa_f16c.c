/* isa_rewrite F16C stub (issue #7): vcvtph2ps patched with its stub gives the same bits as the
 * real instruction (this host has F16C), for xmm and ymm destinations and registers 8-15, and
 * leaves the other vector registers alone. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "isa_rewrite.h"

typedef void (ABI *Snippet)(const uint16_t *in, uint32_t *out);

/* vmovdqu xmm0,[rdi] ; vcmpps ymm1,ymm1,ymm1,15 (garbage) ; vcvtph2ps ymm1,xmm0 ; vmovups [rsi],ymm1 ;
 * vzeroupper ; ret     -> site at +0x0a, ymm destination */
static const uint8_t YMM_CODE[] = {0xc5, 0xfa, 0x6f, 0x07, 0xc5, 0xf4, 0xc2, 0xc9, 0x0f, 0x90,
                                   0xc4, 0xe2, 0x7d, 0x13, 0xc8, 0xc5, 0xfc, 0x11, 0x0e, 0xc5, 0xf8, 0x77, 0xc3};
/* vmovdqu xmm8,[rdi] ; vcmpps ymm9,ymm9,ymm9,15 ; vmovaps ymm2,ymm9 ; vcvtph2ps xmm9,xmm8 ;
 * vmovups [rsi],ymm9 ; vmovups [rsi+32],ymm2 ; vzeroupper ; ret  -> site at +0x0f, xmm destination
 * (its upper half must become zero) and ymm2 must survive */
static const uint8_t XMM_CODE[] = {0xc5, 0x7a, 0x6f, 0x07, 0xc4, 0x41, 0x34, 0xc2, 0xc9, 0x0f, 0xc5, 0x7c, 0x28, 0xd1, 0x90,
                                   0xc4, 0x42, 0x79, 0x13, 0xc8, 0xc5, 0x7c, 0x11, 0x0e, 0xc5, 0xfc, 0x11, 0x56, 0x20,
                                   0xc5, 0xf8, 0x77, 0xc3};

static int run(const uint8_t *code, size_t size, uint32_t at, int ymm, int dst, int src) {
    unsigned char *region = VirtualAlloc(NULL, 2 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *image = region, *stubs = region + 4096;
    memcpy(image, code, size);
    static const uint16_t in[8] = {0x3c00, 0xc000, 0x0001, 0x03ff, 0x7c00, 0xfc00, 0x7e01, 0x7c01};
    uint32_t native[16], patched[16];
    memset(native, 0, sizeof native);
    memset(patched, 0, sizeof patched);
    ((Snippet)(void *)image)(in, native);
    IsaSite site;
    memset(&site, 0, sizeof site);
    site.offset = at; site.length = 5; site.kind = ISA_VCVTPH2PS; site.width = (uint8_t)ymm; site.mode = ISA_MODE_JMP;
    site.dst = (uint8_t)dst; site.src1 = (uint8_t)src; site.src2 = ISA_NO_REG; site.base = site.index = ISA_NO_REG;
    memcpy(site.original, image + at, 5);
    uint64_t hits = 0;
    IsaTable table = {.sites = &site, .hits = &hits, .count = 1, .image_size = 4096};
    IsaReport report = isa_rewrite_apply(&table, image, stubs, CPU_SET_F16C);
    assert(report.jmp == 1 && image[at] == 0xe9);
    ((Snippet)(void *)image)(in, patched);
    int ok = memcmp(native, patched, sizeof native) == 0 && hits == 1;
    printf("%s dst %d src %d: %s\n", ymm ? "ymm" : "xmm", dst, src, ok ? "same bits" : "DIFFERENT");
    for (int i = 0; i < 16 && !ok; i++) printf("  %2d %08x %08x\n", i, native[i], patched[i]);
    return ok;
}

int main(void) {
    int ok = run(YMM_CODE, sizeof YMM_CODE, 0x0a, 1, 1, 0);
    ok &= run(XMM_CODE, sizeof XMM_CODE, 0x0f, 0, 9, 8);
    return ok ? 0 : 1;
}

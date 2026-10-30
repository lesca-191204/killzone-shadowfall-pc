/* SPDX-License-Identifier: GPL-2.0-or-later
 * Sites, atlas layout and checks adapted from pearlxcore/KillzoneShadowFall-PC
 * (src/kzsf_prompt_pc_win.h, kzsf_prompt_trace_win.h); hooked here with stubs instead of INT3. */
#ifndef GOW3_PROMPT_HOOK_H
#define GOW3_PROMPT_HOOK_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prompt_art.h"

/* Killzone Shadow Fall CUSA00190 v01.81, native PC button prompts.
 *
 * The gameplay button icons are quads cut from a 2048x1024 atlas of 27 PS4 glyphs. A glyph
 * collection (rdi) reaches one of three icon lookups, by button 0xdee8a0, by axis 0xdee930 and by
 * game function 0xdee9c0 (pearlx's trace sites); widgets can hold their own collection and atlas
 * texture, so each distinct atlas gets its PC copy (up to GOW3_PROMPT_ATLASES). +0x18 count (27),
 * +0x20 entries (0x30 bytes: +0 button icon, +4 axis, +0x10 rect, +0x28 icon object whose +0x20
 * holds the rect), +0x40 UV quads (0x40 bytes each), +0x50 texture holder (texture first, its
 * vtable at image+0x14e0420, +0x18 width/height). The first time one is seen, the loader draws a
 * copy of the atlas with key caps and a mouse in the same rectangles and creates it as a game
 * texture (new 0x321970, constructor 0x2cea70, external-pixel init 0x2d0db0, as the generated
 * font texture creator 0x31a540 does).
 *
 * 0xfc4d90 (rdi command, rsi x | y << 32, rdx width | height << 32, rcx 16 UV floats, r8 colour;
 * 29 callers) appends a quad, six 0x24-byte vertices (+0xc u, +0x10 v), to a command of one
 * texture: +0x40 vertex count, +0x48 vertices, +0x58 texture holder. The HUD batches several
 * atlas glyphs in one command (pearlx swapped only empty commands, so its reload prompt stayed
 * PS4). A stub at its entry points a command of the original atlas at the PC holder when this
 * quad and every quad already in it are glyphs with PC art, and back at the original holder when
 * a glyph without (a stick, the touchpad) joins, so nothing is drawn from an empty rectangle.
 *
 * Rich text (menus, the death screen) adds inline images through 0x51f4c0 (xmm0 advance, rdi the
 * text builder, r8 texture, r9 its int rect, which the run copies). <function-icon name=...> takes
 * a game function's glyph from the button collection at builder+0xb0 through a fourth lookup,
 * 0xdeebd0; <icon name=...> takes it from the ImageMap of the builder's current style (style stack
 * +0x30 depth, +0x38 styles; style +0x30 ImageMap: +0x18 count, +0x20 {String id, image}; image
 * +0x18 texture, +0x20 rect), the menu legends with ids from ELegendButton. A stub at its entry
 * swaps an atlas glyph's texture for the PC atlas, and an ImageMap button for its sheet's copy,
 * keys drawn in the same rectangles, so the line keeps its layout.
 *
 * TextureWidgets (the L1/R1 of the menu tabs) draw their own quads: 0x42d7d0 sets +0x340 to the
 * render texture of their TextureWidgetResource (+0x328; +0x20 texture resource, +0x38 count and
 * +0x40 Elements of 0x40 bytes, an int rect at +0 or an image at +0x10 whose +0x20 is the rect),
 * and 0x42d370 (rdi the widget) draws with [+0x340]. A stub at its entry points +0x340 at a
 * sheet's copy when every element is a button with a key, and back at the sheet otherwise. */
enum {
    GOW3_PROMPT_ATLAS_WIDTH = 2048, GOW3_PROMPT_ATLAS_HEIGHT = 1024,
    GOW3_PROMPT_ATLAS_BYTES = GOW3_PROMPT_ATLAS_WIDTH * GOW3_PROMPT_ATLAS_HEIGHT * 4,
    /* Glyphs of a collection at most: 27 in the US English one, other languages' (Russian, #91)
     * may hold more; each glyph is still checked against its rectangle. */
    GOW3_PROMPT_GLYPHS = 48, GOW3_PROMPT_ICONS = 16,
    GOW3_PROMPT_DRAW = 0xfc4d90, GOW3_PROMPT_ATLASES = 4, GOW3_PROMPT_HOLDERS = 8,
    GOW3_PROMPT_ENTRY_LENGTH = 6, /* push rbp; mov rbp,rsp; push r15 (draw, function lookup) */
    GOW3_PROMPT_ATLAS_VTABLE = 0x14e0420,
    GOW3_PROMPT_NEW = 0x321970, GOW3_PROMPT_CTOR = 0x2cea70, GOW3_PROMPT_TEXTURE_INIT = 0x2d0db0,
    GOW3_PROMPT_LOOKUPS = 4, GOW3_PROMPT_TEXT_IMAGE = 0x51f4c0, GOW3_PROMPT_TEXTURE_WIDGET = 0x42d370,
    GOW3_PROMPT_SHEET_MAX = 4096, GOW3_PROMPT_MENU_IMAGES = 64, GOW3_PROMPT_SHEETS = 4,
    GOW3_PROMPT_MENUS = 4,
    /* Icon sets, each with its own copies: 0 keys and mouse, 1 Xbox buttons. */
    GOW3_PROMPT_SET_KEYBOARD = 0, GOW3_PROMPT_SET_XBOX = 1, GOW3_PROMPT_SETS = 2,
};

typedef struct { uint32_t address; unsigned length; unsigned char bytes[27]; } Gow3PromptCode;
/* The four lookups and the bytes their stubs replay: push rbx; mov r10d,[rdi+0x18] for the
 * button and axis ones, push rbp; mov rbp,rsp; push r15 for the game-function ones. */
static const struct { uint32_t address; unsigned replay; } gow3_prompt_lookups[GOW3_PROMPT_LOOKUPS] = {
    {0xdee8a0, 5}, {0xdee930, 5}, {0xdee9c0, 6}, {0xdeebd0, 6}};
static const Gow3PromptCode gow3_prompt_code[] = {
    /* The draw: prologue (the 6 bytes its stub replays) and its first stack store. */
    {GOW3_PROMPT_DRAW, 27, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
        0x48,0x81,0xec,0x18,0x01,0x00,0x00,0x4c,0x89,0x85,0xd8,0xfe,0xff,0xff}},
    {0xdee8a0, 21, {0x53,0x44,0x8b,0x57,0x18,0x45,0x31,0xc0,0x49,0xc7,0xc1,0xf0,0xff,0xff,0xff,
        0x41,0xbb,0xfc,0xff,0xff,0xff}},
    {0xdee930, 21, {0x53,0x44,0x8b,0x57,0x18,0x45,0x31,0xc0,0x49,0xc7,0xc1,0xf0,0xff,0xff,0xff,
        0x41,0xbb,0xfc,0xff,0xff,0xff}},
    {0xdee9c0, 21, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
        0x50,0x4d,0x89,0xc6,0x48,0x89,0x4d,0xd0}},
    {0xdeebd0, 14, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50}},
    /* The inline image of rich text, and its calls for <icon> and <function-icon>. */
    {GOW3_PROMPT_TEXT_IMAGE, 20, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
        0x48,0x81,0xec,0x98,0x00,0x00,0x00}},
    {GOW3_PROMPT_TEXTURE_WIDGET, 20, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
        0x48,0x81,0xec,0x18,0x01,0x00,0x00}},
    {0xe7a5ec, 5, {0xe8,0xcf,0x4e,0x6a,0xff}},
    {0xe7a7a9, 5, {0xe8,0x12,0x4d,0x6a,0xff}},
    {GOW3_PROMPT_NEW, 5, {0xe9,0x5b,0x7a,0x00,0x00}},
    {GOW3_PROMPT_CTOR, 24, {0x48,0x8d,0x05,0x69,0x24,0x21,0x01,0x48,0x83,0xc0,0x10,0x48,
        0x89,0x07,0xb8,0x00,0x00,0x00,0xf8,0x23,0x47,0x08,0x0d,0x00}},
    {GOW3_PROMPT_TEXTURE_INIT, 24, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xec,0xf8,0x00,0x00,0x00,0x44,0x89,0x8d,0x40}},
};

/* The code above is there. */
static inline int gow3_prompt_code_match(const unsigned char *image, uint64_t size) {
    for (unsigned i = 0; i < sizeof(gow3_prompt_code) / sizeof(*gow3_prompt_code); ++i) {
        const Gow3PromptCode *c = &gow3_prompt_code[i];
        if (c->address > size || c->length > size - c->address ||
            memcmp(image + c->address, c->bytes, c->length)) return 0;
    }
    return 1;
}

/* A stub that runs `args` and calls the helper with every argument register (and xmm0-7) saved
 * and restored, then replays `replay` and jumps to `target`. Entered by a CALL or at a function's
 * entry, so rsp is 8 mod 16; seven pushes realign it for the call. In `args` the saved r8 and r9
 * are at [rsp+0x88] and [rsp+0x80], xmm0 at [rsp]. Returns its length. */
static inline unsigned gow3_prompt_build_stub_args(unsigned char *out, uintptr_t helper,
                                                   const unsigned char *args, unsigned args_length,
                                                   const unsigned char *replay, unsigned replay_length,
                                                   uintptr_t target) {
    static const unsigned char save[] = {
        0x50, 0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, /* push rax rdi rsi rdx rcx r8 r9 */
        0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00};           /* sub rsp, 0x80 */
    static const unsigned char restore[] = {
        0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,           /* add rsp, 0x80 */
        0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f, 0x58}; /* pop r9 r8 rcx rdx rsi rdi rax */
    unsigned n = 0;
    memcpy(out + n, save, sizeof(save)); n += sizeof(save);
    for (unsigned i = 0; i < 8; ++i) {                     /* movdqu [rsp+16*i], xmm_i */
        const unsigned char op[] = {0xf3, 0x0f, 0x7f, (unsigned char)(0x44 | i << 3), 0x24,
                                    (unsigned char)(i * 16)};
        memcpy(out + n, op, sizeof(op)); n += sizeof(op);
    }
    memcpy(out + n, args, args_length); n += args_length;
    static const unsigned char call[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                 /* movabs rax, helper */
        0xff, 0xd0};                                        /* call rax */
    memcpy(out + n, call, sizeof(call));
    memcpy(out + n + 2, &helper, 8);
    n += sizeof(call);
    for (unsigned i = 0; i < 8; ++i) {                     /* movdqu xmm_i, [rsp+16*i] */
        const unsigned char op[] = {0xf3, 0x0f, 0x6f, (unsigned char)(0x44 | i << 3), 0x24,
                                    (unsigned char)(i * 16)};
        memcpy(out + n, op, sizeof(op)); n += sizeof(op);
    }
    memcpy(out + n, restore, sizeof(restore)); n += sizeof(restore);
    memcpy(out + n, replay, replay_length); n += replay_length;
    static const unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0}; /* jmp [rip+0] */
    memcpy(out + n, jump, sizeof(jump)); n += sizeof(jump);
    memcpy(out + n, &target, 8);
    return n + 8;
}

/* helper(rdi, rdx, rcx). */
static inline unsigned gow3_prompt_build_stub(unsigned char *out, uintptr_t helper,
                                              const unsigned char *replay, unsigned replay_length,
                                              uintptr_t target) {
    static const unsigned char args[] = {
        0x48, 0x89, 0xd6,                                   /* mov rsi, rdx */
        0x48, 0x89, 0xca};                                  /* mov rdx, rcx */
    return gow3_prompt_build_stub_args(out, helper, args, sizeof(args), replay, replay_length, target);
}

/* helper(rdi, &r8, &r9): the helper may change the texture and rect the image is added with. */
static inline unsigned gow3_prompt_build_text_stub(unsigned char *out, uintptr_t helper,
                                                   const unsigned char *replay, unsigned replay_length,
                                                   uintptr_t target) {
    static const unsigned char args[] = {
        0x48, 0x8d, 0xb4, 0x24, 0x88, 0x00, 0x00, 0x00,     /* lea rsi, [rsp+0x88] */
        0x48, 0x8d, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00};    /* lea rdx, [rsp+0x80] */
    return gow3_prompt_build_stub_args(out, helper, args, sizeof(args), replay, replay_length, target);
}

/* Reads guest memory without faulting on a bad pointer. */
typedef int (*Gow3PromptRead)(uintptr_t address, void *out, size_t size);
/* Key label (kind 1), mouse button (2 left, 3 right) or Xbox button label (4) bound to a prompt
 * icon; 0: keep PS4. */
typedef int (*Gow3PromptBinding)(unsigned icon, char *label, size_t size);
/* Creates a width x height RGBA game texture from the pixels; 0 on failure. */
typedef uint64_t (*Gow3PromptCreate)(const uint32_t *pixels, unsigned width, unsigned height);

typedef struct {
    uint64_t original_texture;
    uint64_t holder[9];        /* the game's holder with the PC atlas as its texture */
    /* One quad per glyph with PC art: a button can have several glyphs (pearlx kept one per
     * button, so the others, the reload prompt's among them, stayed PS4). */
    float uv[GOW3_PROMPT_GLYPHS][16];
    unsigned glyphs;
    /* The game's holders of this atlas seen so far (the HUD and the prompts may each have one),
     * each paired with a copy of `holder` for its commands. */
    uint64_t original_holders[GOW3_PROMPT_HOLDERS];
    uint64_t pc_holders[GOW3_PROMPT_HOLDERS][9];
    uint32_t supported;        /* icons with PC art */
    /* Rich text holds a texture resource whose +0x18 is the render texture (0x518420 reads its
     * size there), so its images get this one: the original's header, our texture at +0x18. */
    uint64_t resource[4];
    /* A sheet's copy (Gow3PromptMenu) has no holder to start from: each pc_holders[k] is copied
     * from the first command holder claiming it, then flagged ready. */
    int lazy_holders;
    int holder_ready[GOW3_PROMPT_HOLDERS];
} Gow3PromptAtlas;

/* Draws the PC atlas over the collection's glyph rectangles and creates it. 1 when ready. */
static inline int gow3_prompt_prepare(Gow3PromptAtlas *pc, uintptr_t base, uintptr_t collection,
                                      Gow3PromptRead read, Gow3PromptBinding binding,
                                      Gow3PromptCreate create) {
    int32_t count = 0;
    uint64_t entries = 0, quads = 0, source = 0, vtable = 0;
    uint32_t dims[2], flags = 0;
    if (!read(collection + 0x18, &count, 4) || count <= 0 || count > GOW3_PROMPT_GLYPHS ||
        !read(collection + 0x20, &entries, 8) || !read(collection + 0x40, &quads, 8) ||
        !read(collection + 0x50, &source, 8) || !read(source, &vtable, 8) ||
        vtable != base + GOW3_PROMPT_ATLAS_VTABLE || !read(source + 8, &flags, 4) ||
        ((flags >> 15) & 255) != 14 || !read(source + 0x18, dims, sizeof(dims)) ||
        dims[0] != GOW3_PROMPT_ATLAS_WIDTH || dims[1] != GOW3_PROMPT_ATLAS_HEIGHT) return 0;
    Gow3PromptAtlas atlas;
    memset(&atlas, 0, sizeof(atlas));
    if (!read(collection + 0x50, atlas.holder, sizeof(atlas.holder))) return 0;
    uint32_t *pixels = calloc(1, GOW3_PROMPT_ATLAS_BYTES);
    if (!pixels) return 0;
    for (int i = 0; i < count; ++i) {
        unsigned char item[0x30];
        int32_t icon, axis, rect[4];
        uint64_t object;
        if (!read(entries + (uintptr_t)i * 0x30, item, sizeof(item))) goto invalid;
        memcpy(&icon, item, 4);
        memcpy(&axis, item + 4, 4);
        memcpy(&object, item + 0x28, 8);
        if (axis != -1 || icon < 0 || icon >= GOW3_PROMPT_ICONS) continue;
        char label[16];
        const int kind = binding((unsigned)icon, label, sizeof(label));
        if (!kind) continue;
        memcpy(rect, item + 0x10, sizeof(rect));
        if (object && !read(object + 0x20, rect, sizeof(rect))) goto invalid;
        float *uv = atlas.uv[atlas.glyphs];
        if (!read(quads + (uintptr_t)i * 0x40, uv, 64)) goto invalid;
        /* The quad must be exactly the rectangle (x, y per corner, two unused floats). */
        const float w = GOW3_PROMPT_ATLAS_WIDTH, h = GOW3_PROMPT_ATLAS_HEIGHT;
        const float expected[8] = {rect[0] / w, rect[1] / h, rect[2] / w, rect[1] / h,
                                   rect[2] / w, rect[3] / h, rect[0] / w, rect[3] / h};
        for (unsigned v = 0; v < 4; ++v)
            for (unsigned a = 0; a < 2; ++a)
                if (!isfinite(uv[v * 4 + a]) || fabsf(uv[v * 4 + a] - expected[v * 2 + a]) > 0.000001f)
                    goto invalid;
        if (!gow3_prompt_art(pixels, GOW3_PROMPT_ATLAS_WIDTH, GOW3_PROMPT_ATLAS_HEIGHT, rect, kind, label))
            goto invalid;
        atlas.supported |= 1u << icon;
        ++atlas.glyphs;
    }
    if (!atlas.supported) goto invalid;
    const uint64_t texture = create(pixels, GOW3_PROMPT_ATLAS_WIDTH, GOW3_PROMPT_ATLAS_HEIGHT);
    free(pixels);
    if (!texture) return 0;
    atlas.original_texture = source;
    atlas.holder[0] = texture;
    memset(&atlas.holder[6], 0, 3 * sizeof(uint64_t)); /* no borrowed program pointer or slots */
    for (unsigned k = 0; k < GOW3_PROMPT_HOLDERS; ++k)
        memcpy(atlas.pc_holders[k], atlas.holder, sizeof(atlas.holder));
    *pc = atlas;
    return 1;
invalid:
    free(pixels);
    return 0;
}

/* Whether the quad whose UV corners are (u0, v0)-(u1, v1) lies in a glyph with PC art (the HUD
 * may inset or scale its UVs, so its center is tested against each glyph rectangle). */
static inline int gow3_prompt_has_art(const Gow3PromptAtlas *pc, float u0, float v0, float u1, float v1) {
    const float u = (u0 + u1) * 0.5f, v = (v0 + v1) * 0.5f;
    for (unsigned i = 0; i < pc->glyphs; ++i) {
        const float *g = pc->uv[i];
        if (u >= fminf(g[0], g[8]) && u <= fmaxf(g[0], g[8]) && v >= fminf(g[1], g[9]) && v <= fmaxf(g[1], g[9]))
            return 1;
    }
    return 0;
}

/* Before a quad is appended to `command` (see above). Returns 1 when the command draws from the
 * PC atlas. Runs for every UI quad, so it reads directly: the append reads the same memory. */
static inline int gow3_prompt_swap(Gow3PromptAtlas *pc, uintptr_t command, uint64_t size,
                                   uintptr_t uv_address) {
    const int32_t width = (int32_t)size, height = (int32_t)(size >> 32);
    if (!command || !uv_address || width <= 0 || height <= 0) return 0;
    const uint64_t holder = *(const uint64_t *)(command + 0x58);
    if (!holder) return 0;
    const float *uv = (const float *)uv_address;
    const int art = gow3_prompt_has_art(pc, uv[0], uv[1], uv[8], uv[9]);
    for (unsigned k = 0; k < GOW3_PROMPT_HOLDERS; ++k) {
        if (holder != (uintptr_t)pc->pc_holders[k]) continue;
        if (art) return 1;
        *(uint64_t *)(command + 0x58) = pc->original_holders[k];
        return 0;
    }
    if (!art || *(const uint64_t *)holder != pc->original_texture) return 0;
    const int32_t count = *(const int32_t *)(command + 0x40);
    const unsigned char *vertices = *(const unsigned char *const *)(command + 0x48);
    if (count < 0 || (count && !vertices)) return 0;
    for (int32_t q = 0; q + 5 < count; q += 6) {
        const float *a = (const float *)(vertices + (size_t)q * 0x24 + 0xc);       /* first corner */
        const float *b = (const float *)(vertices + (size_t)(q + 2) * 0x24 + 0xc); /* opposite one */
        if (!gow3_prompt_has_art(pc, a[0], a[1], b[0], b[1])) return 0;
    }
    for (unsigned k = 0; k < GOW3_PROMPT_HOLDERS; ++k) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&pc->original_holders[k], &expected, holder, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (pc->lazy_holders) {
                memcpy(pc->pc_holders[k], (const void *)holder, sizeof(pc->pc_holders[k]));
                pc->pc_holders[k][0] = pc->holder[0];
                memset(&pc->pc_holders[k][6], 0, 3 * sizeof(uint64_t));
                __atomic_store_n(&pc->holder_ready[k], 1, __ATOMIC_RELEASE);
            }
        } else if (expected != holder) {
            continue;
        } else if (pc->lazy_holders && !__atomic_load_n(&pc->holder_ready[k], __ATOMIC_ACQUIRE)) {
            return 0;
        }
        *(uint64_t *)(command + 0x58) = (uintptr_t)pc->pc_holders[k];
        return 1;
    }
    return 0;
}

/* Points a command drawing from one of pc's holders back at the game's holder (another icon set
 * is wanted now). */
static inline void gow3_prompt_release(const Gow3PromptAtlas *pc, uintptr_t command) {
    if (!command) return;
    const uint64_t holder = *(const uint64_t *)(command + 0x58);
    for (unsigned k = 0; k < GOW3_PROMPT_HOLDERS && holder; ++k)
        if (holder == (uintptr_t)pc->pc_holders[k] && pc->original_holders[k]) {
            *(uint64_t *)(command + 0x58) = pc->original_holders[k];
            return;
        }
}

/* A rich-text image cut from the atlas (<function-icon>; render: its resource's render texture,
 * rect: its int rectangle): whether the glyph has PC art, for pc->resource. */
static inline int gow3_prompt_text_texture(const Gow3PromptAtlas *pc, uint64_t render, const int32_t *rect) {
    if (render != pc->original_texture) return 0;
    const float w = GOW3_PROMPT_ATLAS_WIDTH, h = GOW3_PROMPT_ATLAS_HEIGHT;
    return gow3_prompt_has_art(pc, rect[0] / w, rect[1] / h, rect[2] / w, rect[3] / h);
}

/* The EIconType of an ImageMap image id, -1 for other images. The legends use the ELegendButton
 * names (ok/cancel are the cross/circle images; the game already swaps them where circle
 * confirms), any case. Other images (the menu tabs, the death screen) name a button inside a
 * longer id: each word of it (split at non-alphanumerics) is tried against the names no
 * decoration would use, a shoulder or stick name also as a word's start (l1button). */
static inline int gow3_prompt_legend_icon(const char *id) {
    static const struct { const char *id; int icon, in_words; } ids[] = {
        {"up", 12, 0}, {"down", 14, 0}, {"left", 15, 0}, {"right", 13, 0}, {"ok", 2, 0}, {"cancel", 1, 0},
        {"select", 9, 0}, {"start", 8, 0},
        {"triangle", 0, 1}, {"circle", 1, 1}, {"cross", 2, 1}, {"square", 3, 1}, {"touchpad", 9, 1},
        {"options", 8, 1}, {"l2", 4, 1}, {"r2", 5, 1}, {"l1", 6, 1}, {"r1", 7, 1}, {"l3", 10, 1}, {"r3", 11, 1}};
    const unsigned n = sizeof(ids) / sizeof(*ids);
    char word[64];
    int whole = 1;
    for (const char *p = id; *p; whole = 0) {
        unsigned len = 0;
        while (*p && !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p, whole = 0;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            if (len + 1 < sizeof(word)) word[len++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
            ++p;
        }
        word[len] = 0;
        whole = whole && !*p;
        if (len < 2) continue;
        for (unsigned i = 0; i < n; ++i) {
            if (!whole && !ids[i].in_words) continue;
            if (!strcmp(word, ids[i].id)) return ids[i].icon;
            if (ids[i].in_words && ids[i].id[1] >= '1' && ids[i].id[1] <= '3' && !ids[i].id[2] &&
                !strncmp(word, ids[i].id, 2) && !(word[2] >= '0' && word[2] <= '9'))
                return ids[i].icon;
        }
    }
    return -1;
}

/* A style's ImageMap with its buttons redrawn as keys in a copy of its button sheet, same size
 * and same rectangles, so text keeps its layout and other widgets drawing from the sheet (the
 * menu tabs, the death screen) can be switched in the quad draw like an atlas. */
typedef struct {
    /* One copy per sheet its buttons are cut from (the legends' and larger ones for the menu
     * tabs and the death screen): original_texture the sheet, uv the button rectangles. */
    Gow3PromptAtlas sheet[GOW3_PROMPT_SHEETS];
    unsigned sheets, images;
    uint64_t image[GOW3_PROMPT_MENU_IMAGES];
    unsigned char image_sheet[GOW3_PROMPT_MENU_IMAGES];
    int32_t rect[GOW3_PROMPT_MENU_IMAGES][4];
    char others[256];          /* ids of the images left as they are, for the log */
    char buttons[768];         /* id@x,y of the buttons with keys, for the log */
} Gow3PromptMenu;

/* Reads a Decima String (pointer to {refcount, hash, length, characters at +0x10}). */
static inline int gow3_prompt_read_string(uint64_t string, char *out, size_t size, Gow3PromptRead read) {
    int32_t length = 0;
    if (!string || !read(string + 8, &length, 4) || length <= 0 || (size_t)length >= size ||
        !read(string + 0x10, out, (size_t)length)) return 0;
    out[length] = 0;
    return 1;
}

/* Prepares `menu` for the ImageMap at `map`. 1 when at least one button got a key. */
static inline int gow3_prompt_prepare_menu(Gow3PromptMenu *menu, uint64_t map, Gow3PromptRead read,
                                           Gow3PromptBinding binding, Gow3PromptCreate create) {
    int32_t count = 0;
    uint64_t list = 0;
    if (!read(map + 0x18, &count, 4) || count <= 0 || count > 256 || !read(map + 0x20, &list, 8)) return 0;
    Gow3PromptMenu result;
    memset(&result, 0, sizeof(result));
    uint32_t *pixels[GOW3_PROMPT_SHEETS] = {NULL};
    uint32_t dims[GOW3_PROMPT_SHEETS][2];
    uint64_t resources[GOW3_PROMPT_SHEETS] = {0};
    for (int i = 0; i < count && result.images < GOW3_PROMPT_MENU_IMAGES; ++i) {
        uint64_t item[2], resource = 0, render = 0;
        char id[64], label[16];
        int32_t rect[4];
        if (!read(list + (uintptr_t)i * 16, item, sizeof(item)) || !item[1] ||
            !gow3_prompt_read_string(item[0], id, sizeof(id), read)) continue;
        const int icon = gow3_prompt_legend_icon(id);
        if (icon < 0) {
            const size_t used = strlen(result.others);
            if (used + strlen(id) + 2 < sizeof(result.others))
                memcpy(result.others + used, id, strlen(id) + 1), strcat(result.others, " ");
            continue;
        }
        if (!read(item[1] + 0x20, rect, sizeof(rect)) || !read(item[1] + 0x18, &resource, 8) || !resource ||
            !read(resource + 0x18, &render, 8) || !render) continue;
        unsigned s = 0;
        while (s < result.sheets && result.sheet[s].original_texture != render) ++s;
        if (s == result.sheets) { /* a sheet not seen yet */
            if (s == GOW3_PROMPT_SHEETS || !read(render + 0x18, dims[s], sizeof(dims[s])) || !dims[s][0] ||
                !dims[s][1] || dims[s][0] > GOW3_PROMPT_SHEET_MAX || dims[s][1] > GOW3_PROMPT_SHEET_MAX ||
                !(pixels[s] = calloc(1, (size_t)dims[s][0] * dims[s][1] * 4))) continue;
            result.sheet[s].original_texture = render;
            resources[s] = resource;
            ++result.sheets;
        }
        Gow3PromptAtlas *sheet = &result.sheet[s];
        const int kind = binding((unsigned)icon, label, sizeof(label));
        if (!kind || sheet->glyphs == GOW3_PROMPT_GLYPHS || rect[0] < 0 || rect[1] < 0 || rect[2] <= rect[0] ||
            rect[3] <= rect[1] || (uint32_t)rect[2] > dims[s][0] || (uint32_t)rect[3] > dims[s][1] ||
            !gow3_prompt_art(pixels[s], dims[s][0], dims[s][1], rect, kind, label)) continue;
        float *uv = sheet->uv[sheet->glyphs++];
        uv[0] = (float)rect[0] / dims[s][0]; uv[1] = (float)rect[1] / dims[s][1];
        uv[8] = (float)rect[2] / dims[s][0]; uv[9] = (float)rect[3] / dims[s][1];
        sheet->supported |= 1u << icon;
        result.image_sheet[result.images] = s;
        memcpy(result.rect[result.images], rect, sizeof(rect));
        {
            char entry[96];
            snprintf(entry, sizeof(entry), "%s@%d,%d ", id, rect[0], rect[1]);
            if (strlen(result.buttons) + strlen(entry) < sizeof(result.buttons)) strcat(result.buttons, entry);
        }
        result.image[result.images++] = item[1];
    }
    int ready = 0;
    for (unsigned s = 0; s < result.sheets; ++s) {
        Gow3PromptAtlas *sheet = &result.sheet[s];
        const uint64_t texture = sheet->glyphs ? create(pixels[s], dims[s][0], dims[s][1]) : 0;
        free(pixels[s]);
        if (!texture || !read(resources[s], sheet->resource, 0x18)) {
            sheet->glyphs = 0; /* its images are left as they are */
            continue;
        }
        sheet->resource[3] = texture;
        sheet->holder[0] = texture;
        sheet->lazy_holders = 1;
        ready = 1;
    }
    if (!ready) return 0;
    *menu = result;
    return 1;
}

/* The button image cut from `render` at `rect` (by value: some tags pass a copy of the image's
 * rectangle): the resource of its sheet's copy, or 0. */
static inline uint64_t gow3_prompt_menu_image(const Gow3PromptMenu *menu, uint64_t render, const int32_t *rect) {
    for (unsigned i = 0; i < menu->images; ++i) {
        const Gow3PromptAtlas *sheet = &menu->sheet[menu->image_sheet[i]];
        if (sheet->glyphs && sheet->original_texture == render && !memcmp(menu->rect[i], rect, 16))
            return (uintptr_t)sheet->resource;
    }
    return 0;
}
/* Whether every element of a TextureWidget's resource drawn from `sheet` (width x height) is a
 * button with a key (see above). */
static inline int gow3_prompt_widget_art(const Gow3PromptAtlas *sheet, uint32_t width, uint32_t height,
                                         uint64_t resource, Gow3PromptRead read) {
    int32_t count = 0;
    uint64_t elements = 0;
    if (!sheet->glyphs || !width || !height || !read(resource + 0x38, &count, 4) || count <= 0 || count > 16 ||
        !read(resource + 0x40, &elements, 8) || !elements) return 0;
    for (int32_t i = 0; i < count; ++i) {
        uint64_t element = elements + (uint64_t)i * 0x40, image_rect = 0;
        int32_t rect[4];
        if (!read(element + 0x10, &image_rect, 8)) return 0;
        if (!read(image_rect ? image_rect + 0x20 : element, rect, sizeof(rect)) ||
            !gow3_prompt_has_art(sheet, (float)rect[0] / width, (float)rect[1] / height, (float)rect[2] / width,
                                 (float)rect[3] / height)) return 0;
    }
    return 1;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Continuous Tactical Echo and the local-player check adapted from pearlxcore/KillzoneShadowFall-PC
 * (src/kzsf_cheats.h, kzsf_campaign_cheats_win.h). */
#ifndef GOW3_CHEAT_HOOK_H
#define GOW3_CHEAT_HOOK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "signature.h"

/* Killzone Shadow Fall CUSA00190 v01.81. GOW3_CHEAT_FREE_CAMERA is the free camera's switch
 * (src/free_camera_hook.h): it shares the menu's cheat flags but is never saved. */
enum { GOW3_CHEAT_GOD, GOW3_CHEAT_AMMO, GOW3_CHEAT_NO_RELOAD, GOW3_CHEAT_SPRINT, GOW3_CHEAT_ECHO,
       GOW3_CHEAT_CHAPTERS, GOW3_CHEAT_FREE_CAMERA, GOW3_CHEAT_COUNT };

/* The game keeps its developers' DebugSettings object (exported to scripts as gDebugSettings:
 * SetGodMode, EnableInfiniteAmmo, SetSPAllUnlocked...) at a global the retail code still reads.
 * Each reader below tests the local player first (+0xa0 set, +0xa1 clear), so the switches only
 * change the player. The loader finds the global through the god mode reader and writes the
 * fields; the game does the rest. Every reader must match, or no field is written. */
typedef struct { const char *name; unsigned field, size; Gow3Signature site; } Gow3DebugCheat;
static const Gow3DebugCheat gow3_debug_cheats[] = {
    /* 0xa665e9 (player IsGodMode, 0xa665c0): GodMode int, 0 off, 1 on (2 adds a check of its own,
     * 0xa66600, left unused); 0xa71037 makes the player's damage events void. */
    {"god mode", 0x24, 4,
     {"80 b9 a1 00 00 00 00 75 15 48 8d 05 ?? ?? ?? ?? 48 8b 80 b0 07 00 00 83 78 24 00 0f 9f c0", 9}},
    /* 0xb4ea16 (player HasInfiniteAmmo, 0xb4e9e0): reserve ammo is not taken. */
    {"infinite ammo", 0x28, 1,
     {"80 b9 a1 00 00 00 00 b0 01 75 15 48 8d 05 ?? ?? ?? ?? 48 8b 80 b0 07 00 00 80 78 28 00", 11}},
    /* 0xca5ae8 (magazine use, 0xca5a20): the magazine is not emptied, so no reload. */
    {"no reload", 0x30, 1,
     {"74 14 48 8d 05 ?? ?? ?? ?? 48 8b 80 b0 07 00 00 80 78 30 00 75 04 45 29 75 1c", 2}},
    /* 0xa66679 (player unlimited stamina, 0xa66650): the sprint update (0xb38950) refills. */
    {"unlimited sprint", 0x29, 1,
     {"80 b9 a1 00 00 00 00 75 15 48 8d 05 ?? ?? ?? ?? 48 8b 80 b0 07 00 00 80 78 29 00 0f 95 c0", 9}},
    /* 0x8efef9 (chapter select, 0x8efef0 / 0x8efe30 / 0x8f4050): every chapter and section
     * counts as unlocked at every difficulty; the save's own progress is not written. */
    {"all chapters", 0x1c, 1,
     {"80 bf 40 fa 00 00 00 75 15 48 8d 05 ?? ?? ?? ?? 48 8b 80 b0 07 00 00 80 78 1c 00", 9}},
};
enum { GOW3_DEBUG_CHEAT_COUNT = sizeof(gow3_debug_cheats) / sizeof(gow3_debug_cheats[0]) };
static const unsigned gow3_debug_cheat_ids[GOW3_DEBUG_CHEAT_COUNT] = {
    GOW3_CHEAT_GOD, GOW3_CHEAT_AMMO, GOW3_CHEAT_NO_RELOAD, GOW3_CHEAT_SPRINT, GOW3_CHEAT_CHAPTERS};
/* "lea rax,[global]; mov rax,[rax+0x7b0]" at the site: the image offset of the object's pointer. */
static inline uint64_t gow3_debug_settings_slot(const unsigned char *image, uint64_t at) {
    int32_t disp;
    memcpy(&disp, image + at + 3, 4);
    return at + 7 + (int64_t)disp + 0x7b0;
}

/* Continuous Tactical Echo: an int3 at "mov r14,rdi" (0xc1a860) in the SpotAndMark ability's
 * update. State 2 is an expanding scan, 3 waits between scans; keeping two pulses queued and the
 * scan's own input flag held stops it expiring. Idle, cooldown and scripted cancels stay the
 * game's. The player must use Tactical Echo once after loading. */
enum { GOW3_ECHO_LENGTH = 3 };
static const Gow3Signature gow3_echo_site = {
    "49 89 f7 49 89 fe 48 8b 05 ?? ?? ?? ?? 48 8b 00 48 89 45 e0 e8 ?? ?? ?? ?? 49 8b 9e e0 01 00 00", 3};

/* God mode without hits: int3s at "push rbp" of two functions; with god mode on they return at
 * once. The game's own god mode keeps the health, but both still run.
 * - The health component's damage function (0x690cf0, rdi = component, [rdi+0x38] = its
 *   entity): skipped for the player only. Adapted from pearlxcore (kzsf_cheats_win.h).
 * - SoldierRep::OnHitEffects (0xb9d090, reached only for the local player through 0xb820e0):
 *   FirstPersonHitEffectsResource's blood screen, hit direction and camera shake. */
static const Gow3Signature gow3_god_hit_site = {
    "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 18 01 00 00 49 89 f6 48 89 fb 48 8b 05 ?? ?? ?? ?? "
    "48 8b 00 48 89 45 d0 48 8b 43 38 48 8b 80 30 02 00 00", 0};
static const Gow3Signature gow3_god_effects_site = {
    "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 c5 fa 11 4d b4 48 89 4d a8 c5 fa 11 45 a4 49 89 f6", 0};

typedef int (*Gow3CheatRead)(uintptr_t address, void *out, size_t size);
typedef int (*Gow3CheatWrite)(uintptr_t address, const void *in, size_t size);
/* Entity_ExportedIsPlayer (0x65dac0) dispatches vtable+0x140; these are its 01.81 cases, read
 * without calling guest code. Unknown implementations and unreadable pointers are not the player. */
static inline int gow3_player_entity(uintptr_t entity, uintptr_t base, Gow3CheatRead read, unsigned depth) {
    uintptr_t vtable, method, controller = 0;
    unsigned char flag = 0;
    if (!entity || depth >= 4 || !read(entity, &vtable, 8) || !read(vtable + 0x140, &method, 8)) return 0;
    if (method == base + 0xa7ab20) {
        if (!read(entity + 0x282, &flag, 1)) return 0;
        if (flag) return 1;
        if (!read(vtable + 0x138, &method, 8) || method != base + 0xb67220) return 0;
        return read(entity + 0x948, &controller, 8) && controller != 0;
    }
    const size_t user = method == base + 0xa74980 ? 0x598 : method == base + 0xadb8f0 ? 0x580 : 0;
    if (!user || !read(entity + user, &controller, 8) || controller < 0x48) return 0;
    return gow3_player_entity(controller - 0x48, base, read, depth + 1);
}
static inline int gow3_echo_continue(uintptr_t ability, uintptr_t base, Gow3CheatRead read, Gow3CheatWrite write) {
    uintptr_t owner = 0;
    int state = 0, count = 0;
    const unsigned char held = 1;
    if (!ability || ability > UINTPTR_MAX - 0x2c7 || !read(ability + 0x1e0, &owner, 8) ||
        !gow3_player_entity(owner, base, read, 0) || !read(ability + 0x2b8, &state, 4) ||
        (state != 2 && state != 3) || !read(ability + 0x2c0, &count, 4) || count < 0 || count > 10000)
        return 0;
    if (count < 2) {
        count = 2;
        if (!write(ability + 0x2c0, &count, 4)) return 0;
    }
    return write(ability + 0x2c6, &held, 1);
}
#endif

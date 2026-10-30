#ifndef GOW3_RUNTIME_H
#define GOW3_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in gow3_longjmp
 * (gpu/shim/gow3_toggles.h uses the same buffer type). */
typedef gow3_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) gow3_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Called at the end of runtime_report (the loader adds its own counters). */
void runtime_set_report_hook(void (*hook)(void));
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
/* The loaded guest image (eboot and linked modules), for diagnostics. */
extern uintptr_t runtime_image_start, runtime_image_end;
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
void runtime_set_guest_fpu(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
/* 1 when the guest TSC runs at the PS4 rate (default; GOW3_PS4_TSC=0 turns it off): inline rdtsc
 * must then be patched (tsc_patch.c) to return (host TSC * *runtime_tsc_multiplier()) >> 32. */
int runtime_tsc_calibrate(void);
const uint64_t *runtime_tsc_multiplier(void);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
uintptr_t runtime_network_resolve(const char *name);
int32_t runtime_network_wake_udp(int fd,const void *data,uint32_t size,uint16_t *port);
int32_t runtime_network_prepare_udp(int fd);
uintptr_t runtime_http_resolve(const char *name);
uintptr_t runtime_rudp_resolve(const char *name);
uintptr_t runtime_np_webapi_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
/* AjmDecMp3ParseFrame (guest layout). */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
typedef struct Mp3Decoder Mp3Decoder;
int runtime_mp3_parse(const unsigned char *data, uint32_t size, int parse_ofl, int layer3_only, Mp3Frame *frame);
Mp3Decoder *runtime_mp3_open(void);
void runtime_mp3_close(Mp3Decoder *decoder);
void runtime_mp3_reset(Mp3Decoder *decoder);
int runtime_mp3_decode(Mp3Decoder *decoder, const unsigned char *data, int size, float *pcm, int max_samples, int *channels);
uintptr_t runtime_videodec_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* probe --gamepad: prints one "Gamepad:" line for the first pad found (returns 1) or none (0). */
int runtime_pad_describe(void);
/* Button prompts (src/prompt_hook.h): the icon set wanted now (0 keyboard/mouse, 1 Xbox, -1 PS4);
 * key or Xbox button bound to a prompt icon. */
int runtime_pad_prompt_set(void);
int runtime_pad_prompt_binding(unsigned icon, char *label, size_t size);
int runtime_pad_prompt_xbox_binding(unsigned icon, char *label, size_t size);
/* Free camera (src/free_camera_hook.h): while active the game reads a neutral pad and the camera
 * takes the input. move: right, up, forward (-1..1); look: yaw, pitch (rad, since the last call)
 * and yaw/pitch rates (rad/s); speed: m/s. keys: bit 0 F9 (toggle) pressed, bit 1 F10 (recenter)
 * pressed since the last call. Returns 0 while the window or the menu has the input. */
void runtime_pad_free_camera(int active);
int runtime_pad_free_camera_input(float move[3], float look[4], float *speed, unsigned *keys);
/* Guest flexible memory (CPU and GPU read/write) for pixels the loader hands to the game; NULL
 * when the pool is full. size: whole pages. */
void *runtime_memory_map_host_pixels(uint64_t size);
uintptr_t runtime_rtc_resolve(const char *name);
/* A module linked into the image that the game loads itself (link_modules.py --dynamic). */
typedef struct { char nid[16]; uint64_t address; } RuntimeDynamicExport;
typedef struct {
    char path[64];
    uint64_t base, size, export_count;
    RuntimeDynamicExport *exports; /* sorted by NID; addresses are image offsets */
} RuntimeDynamicModule;
void runtime_module_register(const RuntimeDynamicModule *module, unsigned char *image);
uintptr_t runtime_module_resolve(const char *name);
uintptr_t runtime_font_resolve(const char *name);
uintptr_t runtime_png_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_thread_attach_host(const char *name);
#endif

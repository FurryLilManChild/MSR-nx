// MSR-nx: the GPU's VRAM kernels (vram_ops.inc) compiled for the Switch's CPU, so the start-up
// self-test (dk_pica.c dkr_scaler_selftest) can compare what the GPU does with what the same code
// gives on the CPU, on the Switch itself.
#include <stdint.h>
typedef uint32_t u32;
typedef uint32_t uint;
struct UV4 { u32 x, y, z, w; };
static UV4 q[4];
static u32* g_vr;
static const u32* g_src;
static u32 g_src_w;
static u32* g_dst;
static u32 g_dst_w;
static u32* g_out;
#define LDW(i) g_vr[(i)]
#define STW(i, v) (g_vr[(i)] = (v))
#define IMG_READ(x, y) g_src[(y) * g_src_w + (x)]
#define IMG_WRITE(x, y, v) (g_dst[(y) * g_dst_w + (x)] = (v))
#define OUTW(i, v) (g_out[(i)] = (v))
static inline uint min(uint a, uint b) { return a < b ? a : b; }
#include "vram_ops.inc"

extern "C" void vref_setup(const u32 qq[16], u32* vram, const u32* src, u32 src_w, u32* dst, u32 dst_w, u32* out) {
    for (int k = 0; k < 4; k++) q[k] = UV4{qq[4 * k], qq[4 * k + 1], qq[4 * k + 2], qq[4 * k + 3]};
    g_vr = vram; g_src = src; g_src_w = src_w; g_dst = dst; g_dst_w = dst_w; g_out = out;
}
// the kernel by name as in the GPU build (mkshaders.py KERNELS)
extern "C" int vref_run(const char* name, u32 n) {
    void (*f)(uint) = 0;
    const char* s = name;
    if (!s) return 0;
    struct { const char* n; void (*f)(uint); } t[] = {{"imgcopy", k_imgcopy}, {"texup", k_texup}, {"hires", k_hires}, {"dump", k_dump},
                                                      {"store", k_store}, {"load", k_load}, {"copy_img", k_copy_img}, {"transfer_img", k_transfer_img}};
    for (auto& e : t) {
        const char *a = e.n, *b = s;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) f = e.f;
    }
    if (!f) return 0;
    for (u32 i = 0; i < n; i++) f(i);
    return 1;
}

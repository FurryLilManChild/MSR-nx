// MSR-nx: CPU state and helpers used by the generated code.
//
// Game memory is one 4 GiB window: guest address A lives at g_mem + A, so every guest load and
// store is a plain host load and store. Registers live in locals inside each generated function
// and in Cpu only across calls, returns and exits.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef struct Cpu {
    u32 r[16];
    u32 n, z, c, v, q;
    u32 ge;          // GE[3:0] for the parallel add/subtract instructions
    u32 pc;          // where control goes when a function returns or exits
    u32 fpscr;
    u32 tls;         // CP15 c13 c0 3 (thread local storage pointer)
    u32 excl_addr;   // ldrex/strex monitor
    u32 excl_valid;
    union {
        u32 s[32];
        double d[16];
        u64 dw[16];
    } vfp;
} Cpu;

extern u8* g_mem;

#define MSR_INLINE static inline __attribute__((always_inline))

MSR_INLINE u32 rd32(u32 a) { u32 v; memcpy(&v, g_mem + a, 4); return v; }
MSR_INLINE u32 rd16(u32 a) { u16 v; memcpy(&v, g_mem + a, 2); return v; }
MSR_INLINE u32 rd8(u32 a) { return g_mem[a]; }
MSR_INLINE void wr32(u32 a, u32 v) { memcpy(g_mem + a, &v, 4); }
MSR_INLINE void wr16(u32 a, u32 v) { u16 h = (u16)v; memcpy(g_mem + a, &h, 2); }
MSR_INLINE void wr8(u32 a, u32 v) { g_mem[a] = (u8)v; }

// Shifts with ARM semantics for amounts of any size (register-specified shifts use the low byte).
MSR_INLINE u32 lsl_(u32 x, u32 n) { return n >= 32 ? 0 : x << n; }
MSR_INLINE u32 lsr_(u32 x, u32 n) { return n >= 32 ? 0 : x >> n; }
MSR_INLINE u32 asr_(u32 x, u32 n) { return n >= 32 ? (u32)((s32)x >> 31) : (u32)((s32)x >> n); }
MSR_INLINE u32 ror_(u32 x, u32 n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

// Same, also producing the shifter carry out (C is left alone when the amount is 0).
MSR_INLINE u32 lsl_c(u32 x, u32 n, u32* c) {
    if (n == 0) return x;
    if (n < 32) { *c = (x >> (32 - n)) & 1; return x << n; }
    *c = n == 32 ? (x & 1) : 0;
    return 0;
}
MSR_INLINE u32 lsr_c(u32 x, u32 n, u32* c) {
    if (n == 0) return x;
    if (n < 32) { *c = (x >> (n - 1)) & 1; return x >> n; }
    *c = n == 32 ? (x >> 31) : 0;
    return 0;
}
MSR_INLINE u32 asr_c(u32 x, u32 n, u32* c) {
    if (n == 0) return x;
    if (n < 32) { *c = ((s32)x >> (n - 1)) & 1; return (u32)((s32)x >> n); }
    *c = x >> 31;
    return (u32)((s32)x >> 31);
}
MSR_INLINE u32 ror_c(u32 x, u32 n, u32* c) {
    if (n == 0) return x;
    n &= 31;
    u32 r = n ? (x >> n) | (x << (32 - n)) : x;
    *c = r >> 31;
    return r;
}
MSR_INLINE u32 rrx_c(u32 x, u32* c) {
    u32 r = (x >> 1) | (*c << 31);
    *c = x & 1;
    return r;
}

// Flag-setting add with carry in; sets N Z C V.
MSR_INLINE u32 adds_(u32 a, u32 b, u32 cin, u32* N, u32* Z, u32* C, u32* V) {
    u64 w = (u64)a + b + cin;
    u32 r = (u32)w;
    *N = r >> 31;
    *Z = r == 0;
    *C = (u32)(w >> 32);
    *V = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}
// a - b - !cin, ARM style (C = no borrow).
MSR_INLINE u32 subs_(u32 a, u32 b, u32 cin, u32* N, u32* Z, u32* C, u32* V) {
    return adds_(a, ~b, cin, N, Z, C, V);
}

MSR_INLINE float sf(const Cpu* c, int i) { float f; memcpy(&f, &c->vfp.s[i], 4); return f; }
MSR_INLINE void set_sf(Cpu* c, int i, float f) { memcpy(&c->vfp.s[i], &f, 4); }
MSR_INLINE double df(const Cpu* c, int i) { double d; memcpy(&d, &c->vfp.dw[i], 8); return d; }
MSR_INLINE void set_df(Cpu* c, int i, double d) { memcpy(&c->vfp.dw[i], &d, 8); }
#ifdef MSR_NANCHECK
// Debug build: reports the first functions that produce a NaN in a VFP register.
void msr_nan_seen(const char* fn, int reg, int dbl);
#define set_sf(c, i, f) do { float nan_v_ = (f); if (nan_v_ != nan_v_) msr_nan_seen(__func__, (i), 0); set_sf((c), (i), nan_v_); } while (0)
#define set_df(c, i, d) do { double nan_v_ = (d); if (nan_v_ != nan_v_) msr_nan_seen(__func__, (i), 1); set_df((c), (i), nan_v_); } while (0)
#endif

// VFP compare: FPSCR NZCV as the ARM ARM defines it (unordered = 0011).
MSR_INLINE u32 vcmp_flags(double a, double b) {
    if (a != a || b != b) return 0x3u << 28;
    if (a == b) return 0x6u << 28;
    if (a < b) return 0x8u << 28;
    return 0x2u << 28;
}

// VFP to integer conversions with round toward zero and saturation, as ARM does.
MSR_INLINE s32 vcvt_s32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 0x7FFFFFFF;
    if (x <= -2147483648.0) return (s32)0x80000000;
    return (s32)x;
}
MSR_INLINE u32 vcvt_u32(double x) {
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (u32)x;
}


// Register locals and their sync with Cpu (generated code only).
#ifdef MSR_TRACE
extern const char* msr_trace_ring[64];
extern unsigned msr_trace_pos;
#define MSR_TRACE_ENTER msr_trace_ring[msr_trace_pos++ & 63] = __func__
#else
#define MSR_TRACE_ENTER (void)0
#endif
#define DECLARE_REGS MSR_TRACE_ENTER; u32 r0 = c->r[0]; u32 r1 = c->r[1]; u32 r2 = c->r[2]; u32 r3 = c->r[3]; u32 r4 = c->r[4]; u32 r5 = c->r[5]; u32 r6 = c->r[6]; u32 r7 = c->r[7]; u32 r8 = c->r[8]; u32 r9 = c->r[9]; u32 r10 = c->r[10]; u32 r11 = c->r[11]; u32 r12 = c->r[12]; u32 r13 = c->r[13]; u32 r14 = c->r[14]; u32 N = c->n; u32 Z = c->z; u32 C = c->c; u32 V = c->v; u32 Q = c->q; u32 GE = c->ge;
#define SYNC_OUT do { c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->n = N; c->z = Z; c->c = C; c->v = V; c->q = Q; c->ge = GE; } while (0)
#define SYNC_IN do { r0 = c->r[0]; r1 = c->r[1]; r2 = c->r[2]; r3 = c->r[3]; r4 = c->r[4]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7]; r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11]; r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; N = c->n; Z = c->z; C = c->c; V = c->v; Q = c->q; GE = c->ge; } while (0)

// Runtime entry points the generated code calls.
void msr_svc(Cpu* c, u32 imm);
void msr_call_indirect(Cpu* c, u32 target);   // a call through a register
void msr_jump_indirect(Cpu* c, u32 target);   // a jump through a register that is not a return
void msr_unknown(Cpu* c, u32 addr, const char* what);
// A call returned somewhere other than its return address (logged; control unwinds).
void msr_unwind(Cpu* c, u32 call_site, u32 expected);

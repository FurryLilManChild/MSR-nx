// MSR-nx: the GPU-side copies of the 3DS's VRAM operations (fills, texture copies, display
// transfers, render target stores and loads, texture decoding, screen output).
//
// Written in the part of the language GLSL and C++ share, so tools/tests/vram_ops_test.cpp can run
// exactly this code on the CPU and compare it, byte for byte, with the reference implementations
// the probe uses (sw_raster.c, hle_hid_gsp.c). The including file defines:
//   LDW(i)        the 32-bit word i of VRAM (byte address i * 4, relative to the VRAM base)
//   STW(i, v)     store a word
//   IMG_READ(x, y)     a pixel of the bound image as r | g << 8 | b << 16 | a << 24
//   IMG_WRITE(x, y, v) store one
//   OUTW(i, v)    store a word of the screen output buffer
//   q[4]          the parameters (uvec4 in GLSL, a struct of four uints in C++)
// Addresses below are byte offsets from the start of VRAM (guest 0x1F000000).

// Division by a value that is not a constant: the Switch GPU has no integer divide, and uam's
// replacement goes through floats and can be off by one. Take the float estimate and correct it.
uint udiv(uint a, uint b) {
    uint d = uint(float(a) / float(b));
    if (d * b > a) d--;
    if (d * b > a) d--;
    if ((d + 1u) * b <= a) d++;
    if ((d + 1u) * b <= a) d++;
    return d;
}
uint umod(uint a, uint b) { return a - udiv(a, b) * b; }

uint ld8(uint a) { return (LDW(a >> 2u) >> ((a & 3u) * 8u)) & 0xFFu; }
uint ld16(uint a) { return ld8(a) | (ld8(a + 1u) << 8u); }

uint morton(uint x, uint y) {
    return (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) | ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
}
uint tiled_index(uint x, uint y, uint w) { return (y & ~7u) * w + (x & ~7u) * 8u + morton(x & 7u, y & 7u); }
uint demorton_x(uint m) { return (m & 1u) | ((m >> 1u) & 2u) | ((m >> 2u) & 4u); }
uint demorton_y(uint m) { return ((m >> 1u) & 1u) | ((m >> 2u) & 2u) | ((m >> 3u) & 4u); }

uint pack4(uint r, uint g, uint b, uint a) { return r | (g << 8u) | (b << 16u) | (a << 24u); }
uint ch(uint c, uint k) { return (c >> (8u * k)) & 0xFFu; }

uint fb_bpp(uint fmt) { return fmt == 0u ? 4u : fmt == 1u ? 3u : 2u; }

// Framebuffer color formats as the display transfer widens them (sw_raster.c px_read: v / 31 * 255
// rounded to nearest).
uint x5(uint v) { return (v * 510u + 31u) / 62u; }
uint x6(uint v) { return (v * 510u + 63u) / 126u; }
// The same from the pixel's bytes (low byte first, as px_encode gives them).
uint px_from_bits(uint e, uint fmt) {
    if (fmt == 0u) return pack4(ch(e, 3u), ch(e, 2u), ch(e, 1u), ch(e, 0u));
    if (fmt == 1u) return pack4(ch(e, 2u), ch(e, 1u), ch(e, 0u), 255u);
    uint h = e & 0xFFFFu;
    if (fmt == 2u) return pack4(x5((h >> 11u) & 31u), x5((h >> 6u) & 31u), x5((h >> 1u) & 31u), (h & 1u) != 0u ? 255u : 0u);
    if (fmt == 3u) return pack4(x5((h >> 11u) & 31u), x6((h >> 5u) & 63u), x5(h & 31u), 255u);
    return pack4(((h >> 12u) & 15u) * 17u, ((h >> 8u) & 15u) * 17u, ((h >> 4u) & 15u) * 17u, (h & 15u) * 17u);
}
uint px_read(uint a, uint fmt) {
    if (fmt == 0u) return pack4(ld8(a + 3u), ld8(a + 2u), ld8(a + 1u), ld8(a));
    if (fmt == 1u) return pack4(ld8(a + 2u), ld8(a + 1u), ld8(a), 255u);
    uint h = ld16(a);
    if (fmt == 2u) return pack4(x5((h >> 11u) & 31u), x5((h >> 6u) & 31u), x5((h >> 1u) & 31u), (h & 1u) != 0u ? 255u : 0u);
    if (fmt == 3u) return pack4(x5((h >> 11u) & 31u), x6((h >> 5u) & 63u), x5(h & 31u), 255u);
    return pack4(((h >> 12u) & 15u) * 17u, ((h >> 8u) & 15u) * 17u, ((h >> 4u) & 15u) * 17u, (h & 15u) * 17u);
}

// Render target and texture color formats as dk_pica.c decode_color_px widens them (truncating).
uint px_decode(uint a, uint fmt) {
    if (fmt == 0u) return pack4(ld8(a + 3u), ld8(a + 2u), ld8(a + 1u), ld8(a));
    if (fmt == 1u) return pack4(ld8(a + 2u), ld8(a + 1u), ld8(a), 255u);
    uint h = ld16(a);
    if (fmt == 2u) return pack4(((h >> 11u) & 31u) * 255u / 31u, ((h >> 6u) & 31u) * 255u / 31u, ((h >> 1u) & 31u) * 255u / 31u, (h & 1u) * 255u);
    if (fmt == 3u) return pack4(((h >> 11u) & 31u) * 255u / 31u, ((h >> 5u) & 63u) * 255u / 63u, (h & 31u) * 255u / 31u, 255u);
    return pack4((h >> 12u) * 17u, ((h >> 8u) & 15u) * 17u, ((h >> 4u) & 15u) * 17u, (h & 15u) * 17u);
}

// The bytes of one pixel in a framebuffer format, low byte first, as a word (encode_color_px, px_write).
uint px_encode(uint c, uint fmt) {
    uint r = ch(c, 0u), g = ch(c, 1u), b = ch(c, 2u), a = ch(c, 3u);
    if (fmt == 0u) return pack4(a, b, g, r);
    if (fmt == 1u) return b | (g << 8u) | (r << 16u);
    if (fmt == 2u) return ((r >> 3u) << 11u) | ((g >> 3u) << 6u) | ((b >> 3u) << 1u) | (a >> 7u);
    if (fmt == 3u) return ((r >> 3u) << 11u) | ((g >> 2u) << 5u) | (b >> 3u);
    return ((r >> 4u) << 12u) | ((g >> 4u) << 8u) | ((b >> 4u) << 4u) | (a >> 4u);
}

// Up to 16 bytes of output built one byte at a time, then stored as words; a byte not set keeps
// what memory had (the words are read first).
uint g_w0, g_w1, g_w2, g_w3;
void put8(uint pos, uint v) {
    uint sh = (pos & 3u) * 8u, m = ~(0xFFu << sh), bits = (v & 0xFFu) << sh;
    uint k = pos >> 2u;
    if (k == 0u) g_w0 = (g_w0 & m) | bits;
    else if (k == 1u) g_w1 = (g_w1 & m) | bits;
    else if (k == 2u) g_w2 = (g_w2 & m) | bits;
    else g_w3 = (g_w3 & m) | bits;
}
void group_load(uint word, uint nwords) {
    g_w0 = LDW(word);
    g_w1 = nwords > 1u ? LDW(word + 1u) : 0u;
    g_w2 = nwords > 2u ? LDW(word + 2u) : 0u;
    g_w3 = nwords > 3u ? LDW(word + 3u) : 0u;
}
void group_store(uint word, uint nwords) {
    STW(word, g_w0);
    if (nwords > 1u) STW(word + 1u, g_w1);
    if (nwords > 2u) STW(word + 2u, g_w2);
    if (nwords > 3u) STW(word + 3u, g_w3);
}

// A pixel of the bound render target image at native position (x, y) of a w x h target. The
// image can be larger (resolution scaler): q[3].x, q[3].y = its size (0: the same). Twice the size
// averages the 2 x 2 pixels; other sizes take the nearest one.
uint img_read_s(uint x, uint y, uint w, uint h) {
    uint iw = q[3].x, ih = q[3].y;
    if (iw == 0u || (iw == w && ih == h)) return IMG_READ(x, y);
    if (iw == 2u * w && ih == 2u * h) {
        uint a = IMG_READ(2u * x, 2u * y), b = IMG_READ(2u * x + 1u, 2u * y);
        uint c = IMG_READ(2u * x, 2u * y + 1u), d = IMG_READ(2u * x + 1u, 2u * y + 1u);
        uint r = 0u;
        for (uint k = 0u; k < 4u; k++) r |= (((ch(a, k) + ch(b, k) + ch(c, k) + ch(d, k) + 2u) >> 2u) << (8u * k));
        return r;
    }
    return IMG_READ(udiv((2u * x + 1u) * iw, 2u * w), udiv((2u * y + 1u) * ih, 2u * h));
}

// ---- MemoryFill: q[0] = start, end, value, width (bit 0: 24 bit, bit 1: 32 bit, neither: 16 bit).
// One word per invocation. start and end are word aligned.
void k_fill(uint gid) {
    uint start = q[0].x, end = q[0].y, value = q[0].z, width = q[0].w;
    uint a = (start & ~3u) + gid * 4u;
    if (a >= end) return;
    if ((width & 2u) != 0u) { if (a + 4u <= end) STW(a >> 2u, value); return; }
    if (width == 0u) {
        uint v = value & 0xFFFFu;
        if (a + 4u <= end) STW(a >> 2u, v | (v << 16u));
        else if (a + 2u <= end) STW(a >> 2u, (LDW(a >> 2u) & 0xFFFF0000u) | v);
        return;
    }
    group_load(a >> 2u, 1u);
    for (uint k = 0u; k < 4u; k++) {
        uint o = a + k - start, t = o / 3u;
        if (start + t * 3u + 3u <= end) put8(k, value >> (8u * (o - t * 3u)));
    }
    group_store(a >> 2u, 1u);
}

// ---- TextureCopy: q[0] = in, out, size, 0; q[1] = input width, input gap, output width, output gap
// (bytes). One word per invocation; everything is a multiple of 16 bytes.
void k_copy(uint gid) {
    uint size = q[0].z & ~15u, b = gid * 4u;
    if (b >= size) return;
    uint iw = q[1].x != 0u ? q[1].x : size, ow = q[1].z != 0u ? q[1].z : size;
    uint bi = udiv(b, iw), bo = udiv(b, ow);
    uint ia = q[0].x + bi * (iw + q[1].y) + (b - bi * iw);
    uint oa = q[0].y + bo * (ow + q[1].w) + (b - bo * ow);
    STW(oa >> 2u, LDW(ia >> 2u));
}

// ---- DisplayTransfer: q[0] = in, out, input dims, output dims; q[1].x = flags.
// Four output pixels (consecutive in memory) per invocation.
void transfer_body(uint gid, uint from_img) {
    uint inp = q[0].x, outp = q[0].y, flags = q[1].x;
    uint iw = q[0].z & 0xFFFFu, ih = q[0].z >> 16u;
    uint flip = flags & 1u, in_linear = (flags >> 1u) & 1u, same = (flags >> 5u) & 1u;
    uint out_tiled = in_linear != same ? 1u : 0u;
    uint fi = (flags >> 8u) & 7u, fo = (flags >> 12u) & 7u, down = (flags >> 24u) & 3u;
    uint sx = down != 0u ? 2u : 1u, sy = down == 2u ? 2u : 1u;
    uint lx = sx - 1u, ly = sy - 1u;   // sizes divide by 1 or 2: shifts
    uint ow = (q[0].w & 0xFFFFu) >> lx, oh = (q[0].w >> 16u) >> ly;
    uint cw = ow < (iw >> lx) ? ow : (iw >> lx), chh = oh < (ih >> ly) ? oh : (ih >> ly);
    uint bi = fb_bpp(fi), bo = fb_bpp(fo), n = sx * sy, hn = n / 2u;
    uint m0 = gid * 4u;
    if (m0 >= ow * oh) return;
    uint word = (outp + m0 * bo) >> 2u, nwords = bo;   // four pixels: 16, 12 or 8 bytes
    group_load(word, nwords);
    for (uint k = 0u; k < 4u; k++) {
        uint m = m0 + k, x, dy;
        if (out_tiled != 0u) {
            uint tile = m >> 6u, tiles_x = ow / 8u;
            uint tr = udiv(tile, tiles_x);
            x = (tile - tr * tiles_x) * 8u + demorton_x(m & 63u);
            dy = tr * 8u + demorton_y(m & 63u);
        } else { dy = udiv(m, ow); x = m - dy * ow; }
        if (x >= cw || dy >= chh) continue;
        uint y = flip != 0u ? chh - 1u - dy : dy;
        uint ar = 0u, ag = 0u, ab = 0u, aa = 0u;
        for (uint j = 0u; j < sy; j++)
            for (uint i = 0u; i < sx; i++) {
                uint xx = x * sx + i, yy = y * sy + j;
                uint c;
                if (from_img != 0u) c = px_from_bits(px_encode(img_read_s(xx, ih - 1u - yy, iw, ih), fi), fi);   // as k_store, then px_read
                else c = px_read(inp + (in_linear != 0u ? yy * iw + xx : tiled_index(xx, yy, iw)) * bi, fi);
                ar += ch(c, 0u); ag += ch(c, 1u); ab += ch(c, 2u); aa += ch(c, 3u);
            }
        uint ln = lx + ly;   // n is 1, 2 or 4
        if (n > 1u) { ar = (ar + hn) >> ln; ag = (ag + hn) >> ln; ab = (ab + hn) >> ln; aa = (aa + hn) >> ln; }
        uint e = px_encode(pack4(ar, ag, ab, aa), fo);
        for (uint t = 0u; t < bo; t++) put8(k * bo + t, e >> (8u * t));
    }
    group_store(word, nwords);
}
void k_transfer(uint gid) { transfer_body(gid, 0u); }
// The same with the input read from the render target's image (bound as the source) instead of
// VRAM, where the image is newer: what k_store would have put there, without storing it. The input
// is tiled and is exactly the render target (dk_pica.c checks).
void k_transfer_img(uint gid) { transfer_body(gid, 1u); }

// ---- TextureCopy from a render target's image (bound as the source), as k_store then k_copy.
// q[2] = the render target: addr, w, h, fmt; the input lies within it.
void k_copy_img(uint gid) {
    uint size = q[0].z & ~15u, b = gid * 4u;
    if (b >= size) return;
    uint iw = q[1].x != 0u ? q[1].x : size, ow = q[1].z != 0u ? q[1].z : size;
    uint bi = udiv(b, iw), bo = udiv(b, ow);
    uint ia = q[0].x + bi * (iw + q[1].y) + (b - bi * iw);
    uint oa = q[0].y + bo * (ow + q[1].w) + (b - bo * ow);
    uint sa = q[2].x, w = q[2].y, h = q[2].z, fmt = q[2].w, bpp = fb_bpp(fmt), tiles_x = w / 8u;
    uint v = 0u;
    for (uint t = 0u; t < 4u; t++) {
        uint o = ia + t - sa, m = udiv(o, bpp), k = o - m * bpp;
        uint tile = m >> 6u, tr = udiv(tile, tiles_x);
        uint x = (tile - tr * tiles_x) * 8u + demorton_x(m & 63u), y = tr * 8u + demorton_y(m & 63u);
        uint e = px_encode(img_read_s(x, h - 1u - y, w, h), fmt);
        v |= ((e >> (8u * k)) & 0xFFu) << (8u * t);
    }
    STW(oa >> 2u, v);
}

// ---- Render target to VRAM: q[0] = addr, w, h, fmt. The image row for memory row y is h - 1 - y.
// Four pixels (consecutive in memory) per invocation.
void k_store(uint gid) {
    uint addr = q[0].x, w = q[0].y, h = q[0].z, fmt = q[0].w, bpp = fb_bpp(fmt);
    uint m0 = gid * 4u;
    if (m0 >= w * h) return;
    uint word = (addr + m0 * bpp) >> 2u, nwords = bpp;
    group_load(word, nwords);
    for (uint k = 0u; k < 4u; k++) {
        uint m = m0 + k, tile = m >> 6u, tiles_x = w / 8u;
        uint tr = udiv(tile, tiles_x);
        uint x = (tile - tr * tiles_x) * 8u + demorton_x(m & 63u), y = tr * 8u + demorton_y(m & 63u);
        uint e = px_encode(img_read_s(x, h - 1u - y, w, h), fmt);
        for (uint t = 0u; t < bpp; t++) put8(k * bpp + t, e >> (8u * t));
    }
    group_store(word, nwords);
}

// ---- VRAM to render target: q[0] = addr, w, h, fmt. One pixel per invocation.
// A larger image (q[3] its size, as img_read_s): one image pixel per invocation, from the native
// pixel under it.
void k_load(uint gid) {
    uint addr = q[0].x, w = q[0].y, h = q[0].z, fmt = q[0].w;
    uint iw = q[3].x != 0u ? q[3].x : w, ih = q[3].x != 0u ? q[3].y : h;
    if (gid >= iw * ih) return;
    uint Y = udiv(gid, iw), X = gid - Y * iw;   // image pixel; its row Y is native row h - 1 - y
    uint x = iw == w ? X : udiv(X * w, iw), yi = ih == h ? Y : udiv(Y * h, ih), y = h - 1u - yi;
    IMG_WRITE(X, Y, px_decode(addr + tiled_index(x, y, w) * fb_bpp(fmt), fmt));
}

// ---- The top screen at the scaler's size, straight from the render target image (bound as the
// source) that a display transfer just sent to the framebuffer: q[0].x = 1 for a flipped transfer;
// q[3] = the image's size (240 s x 400 s). Writes the dst image, 400 s x 240 s, its row 0 the
// bottom of the screen like the screens main_nx.c shows (k_screen's row order). One pixel each.
void k_hires(uint gid) {
    uint iw = q[3].x, ih = q[3].y, OW = ih, OH = iw;
    if (gid >= OW * OH) return;
    uint R = udiv(gid, OW), X = gid - R * OW;   // output row (bottom first) and column
    uint c = IMG_READ(R, q[0].x != 0u ? X : ih - 1u - X);
    IMG_WRITE(X, R, c | 0xFF000000u);
}

// ---- Texture in VRAM to an image: q[0] = addr, w, h, fmt (PICA texture format). One texel per
// invocation; the image row for memory row y is h - 1 - y (dk_pica.c decode_texture).
uint etc_mod(uint table, uint big) {
    if (table == 0u) return big != 0u ? 8u : 2u;
    if (table == 1u) return big != 0u ? 17u : 5u;
    if (table == 2u) return big != 0u ? 29u : 9u;
    if (table == 3u) return big != 0u ? 42u : 13u;
    if (table == 4u) return big != 0u ? 60u : 18u;
    if (table == 5u) return big != 0u ? 80u : 24u;
    if (table == 6u) return big != 0u ? 106u : 33u;
    return big != 0u ? 183u : 47u;
}
uint sext3(uint v) { return (v & 4u) != 0u ? v | 0xFFFFFFF8u : v; }
uint clamp8(int v) { return v < 0 ? 0u : v > 255 ? 255u : uint(v); }
uint etc1_texel(uint blk, uint x, uint y) {
    uint lo = LDW(blk >> 2u), hi = LDW((blk >> 2u) + 1u);
    uint flip = hi & 1u, diff = (hi >> 1u) & 1u;
    uint sub = flip != 0u ? (y >= 2u ? 1u : 0u) : (x >= 2u ? 1u : 0u);
    int r, g, b;
    if (diff != 0u) {
        int r0 = int((hi >> 27u) & 31u), g0 = int((hi >> 19u) & 31u), b0 = int((hi >> 11u) & 31u);
        if (sub != 0u) { r0 += int(sext3((hi >> 24u) & 7u)); g0 += int(sext3((hi >> 16u) & 7u)); b0 += int(sext3((hi >> 8u) & 7u)); }
        r = (r0 << 3) | (r0 >> 2); g = (g0 << 3) | (g0 >> 2); b = (b0 << 3) | (b0 >> 2);
    } else {
        uint s = sub != 0u ? 0u : 4u;
        r = int((hi >> (24u + s)) & 15u); g = int((hi >> (16u + s)) & 15u); b = int((hi >> (8u + s)) & 15u);
        r |= r << 4; g |= g << 4; b |= b << 4;
    }
    uint table = sub != 0u ? (hi >> 2u) & 7u : (hi >> 5u) & 7u;
    uint i = x * 4u + y;
    int m = int(etc_mod(table, (lo >> i) & 1u));
    if (((lo >> (16u + i)) & 1u) != 0u) m = -m;
    return pack4(clamp8(r + m), clamp8(g + m), clamp8(b + m), 255u);
}
uint tex_bits(uint fmt) {
    if (fmt == 0u) return 32u;
    if (fmt == 1u) return 24u;
    if (fmt <= 6u) return 16u;
    if (fmt <= 9u) return 8u;
    if (fmt <= 12u) return 4u;
    return 8u;
}
void k_texdecode(uint gid) {
    uint addr = q[0].x, w = q[0].y, h = q[0].z, fmt = q[0].w;
    if (gid >= w * h) return;
    uint y = udiv(gid, w), x = gid - y * w, c;
    if (fmt == 12u || fmt == 13u) {
        uint bs = fmt == 13u ? 16u : 8u, tx = x & ~7u, ty = y & ~7u;
        uint sub = (((y - ty) >> 2u) << 1u) | ((x - tx) >> 2u);
        uint blk = addr + (((ty / 8u) * (w / 8u) + tx / 8u) * 4u + sub) * bs;
        uint px = x & 3u, py = y & 3u;
        c = etc1_texel(blk + (fmt == 13u ? 8u : 0u), px, py);
        if (fmt == 13u) {
            uint sh = 4u * (px * 4u + py);
            uint aw = sh < 32u ? LDW(blk >> 2u) >> sh : LDW((blk >> 2u) + 1u) >> (sh - 32u);
            c = (c & 0x00FFFFFFu) | (((aw & 15u) * 17u) << 24u);
        }
    } else {
        uint t = tiled_index(x, y, w), p = addr + (t * tex_bits(fmt)) / 8u;
        if (fmt <= 4u) c = px_decode(p, fmt);
        else if (fmt == 5u) c = pack4(ld8(p + 1u), ld8(p + 1u), ld8(p + 1u), ld8(p));
        else if (fmt == 6u) c = pack4(ld8(p + 1u), ld8(p), 0u, 255u);
        else if (fmt == 7u) c = pack4(ld8(p), ld8(p), ld8(p), 255u);
        else if (fmt == 8u) c = pack4(0u, 0u, 0u, ld8(p));
        else if (fmt == 9u) { uint v = ld8(p); c = pack4((v >> 4u) * 17u, (v >> 4u) * 17u, (v >> 4u) * 17u, (v & 15u) * 17u); }
        else {
            uint v = (t & 1u) != 0u ? ld8(p) >> 4u : ld8(p) & 15u;
            c = fmt == 10u ? pack4(v * 17u, v * 17u, v * 17u, 255u) : pack4(0u, 0u, 0u, v * 17u);
        }
    }
    IMG_WRITE(x, h - 1u - y, c);
}

// ---- A screen for the Switch: q[0] = framebuffer addr, stride (bytes per column), format, width
// (400 or 320); q[1].x = first output word, q[1].y = 1 to store rows bottom to top. One pixel per
// invocation. The LCD framebuffer is stored column by column, bottom of the column first (main_nx.c
// convert_screen).
void k_screen(uint gid) {
    uint addr = q[0].x, fmt = q[0].z & 7u, W = q[0].w, H = 240u;
    if (gid >= W * H) return;
    uint y = udiv(gid, W), x = gid - y * W, bpp = fb_bpp(fmt);
    uint stride = q[0].y != 0u ? q[0].y : 240u * bpp;
    uint p = addr + x * stride + (H - 1u - y) * bpp;
    uint r, g, b;
    if (fmt == 0u) { r = ld8(p + 3u); g = ld8(p + 2u); b = ld8(p + 1u); }
    else if (fmt == 1u) { r = ld8(p + 2u); g = ld8(p + 1u); b = ld8(p); }
    else {
        uint hh = ld16(p);
        if (fmt == 3u) { r = ((hh >> 11u) & 31u) << 3u; g = ((hh >> 5u) & 63u) << 2u; b = (hh & 31u) << 3u; }
        else if (fmt == 2u) { r = ((hh >> 11u) & 31u) << 3u; g = ((hh >> 6u) & 31u) << 3u; b = ((hh >> 1u) & 31u) << 3u; }
        else { r = (hh >> 12u) << 4u; g = ((hh >> 8u) & 15u) << 4u; b = ((hh >> 4u) & 15u) << 4u; }
    }
    uint row = q[1].y != 0u ? H - 1u - y : y;
    OUTW(q[1].x + row * W + x, pack4(r, g, b, 255u));
}

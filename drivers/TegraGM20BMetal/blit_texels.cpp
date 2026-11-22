// SPDX-License-Identifier: MIT
// The blit encoder's remapped texel copies (gpu.h BlitEncoder): between textures, a stencil aspect through its temp, and
// between a texture and a buffer's rows
#include "gpu.h"
#include "blit_remap.h"

#include <algorithm>
#include <cstdio>

namespace gm20b {

void BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                             uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint8_t aspect)
{
    cb_->use(src, false);
    cb_->use(dst, true);
    room(32);
    // an eight-byte texel's first or second word alone: Depth32Float_Stencil8's depth or stencil word, RGB10A8_2P_XR10's
    // colour word or alpha word
    const uint32_t remap = aspect == kAspectStencil ? remapOf(kRemapNoWrite, 1, kRemapNoWrite, kRemapNoWrite, 4, 2, 2)
                           : aspect == kAspectDepth ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 2)
                                                    : texelRemap(src.bpp);
    copyRemapped(p_, sideOf(src, sx, sy, sz), sideOf(dst, dx, dy, dz), w, h, remap,
                 aspect == kAspectWhole ? src.bpp : 4, dev_->stats);
#if SWITCH_METAL_DIAGNOSTICS
    char detail[64];
    snprintf(detail, sizeof detail, " from %ux%u f%u at %u,%u", src.width, src.height, src.format, sx, sy);
    logEdgeMiss("copy", dst, {dx, dy, dx + w, dy + h}, detail);
#endif
}

void BlitEncoder::copyStencilTexels(const Texture &zeta, uint32_t x, uint32_t y, uint32_t z, const Texture &bytes,
                                    uint32_t bx, uint32_t by, uint32_t bz, uint32_t w, uint32_t h, bool intoZeta)
{
    if (!zeta.stencilTemp || !w || !h)
        return;
    const Texture &s = *zeta.stencilTemp;
    cb_->use(zeta, intoZeta);
    cb_->use(bytes, !intoZeta);
    cb_->use(s, true);
    // the temp is the texture's level 0 alone: a larger region (a multisampled one's sample grid) goes through it in parts.
    // Every launch waits for the copies before it: the temp is shared by every stencil copy of the texture, which the
    // encoder's access() orders by the textures' names alone
    const CopySide temp{s.va, s.pitch};
    const uint32_t tw = std::max(1u, std::min(w, s.width)), th = std::max(1u, std::min(h, s.height));
    for (uint32_t ty = 0; ty < h; ty += th)
        for (uint32_t tx = 0; tx < w; tx += tw) {
            const uint32_t cw = std::min(tw, w - tx), ch = std::min(th, h - ty);
            const CopySide texels = sideOf(zeta, x + tx, y + ty, z), plain = sideOf(bytes, bx + tx, by + ty, bz);
            room(64);
            if (intoZeta) {
                copyRemapped(p_, plain, temp, cw, ch, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2),
                             1, dev_->stats, true);
                copyRemapped(p_, temp, texels, cw, ch, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4),
                             2, dev_->stats, true);
            } else {
                copyRemapped(p_, texels, temp, cw, ch, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1),
                             2, dev_->stats, true);
                copyRemapped(p_, temp, plain, cw, ch, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1),
                             1, dev_->stats, true);
            }
        }
}

void BlitEncoder::copyFirstByte(const Texture &from, const Texture &to)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyRemapped(p_, sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height),
                 remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, from.bpp, to.bpp), 1, dev_->stats);
}

void BlitEncoder::copyFirstTwoBytes(const Texture &from, const Texture &to)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyRemapped(p_, sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height), remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, 1, from.bpp, to.bpp), 2,
                 dev_->stats);
}

void BlitEncoder::copyArgb8(const Texture &from, const Texture &to, bool toStandIn)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    // ARGB8Unorm's bytes are alpha, red, green, blue; the stand-in's red, green, blue, alpha
    const uint32_t remap = toStandIn ? remapOf(1, 2, 3, 0, 1, 4, 4) : remapOf(3, 0, 1, 2, 1, 4, 4);
    copyRemapped(p_, sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height), remap, 4, dev_->stats);
}

void BlitEncoder::copyRedBlueSwapped(const Texture &from, const Texture &to, uint32_t w, uint32_t h)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyRemapped(p_, sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), w, h, remapOf(2, 1, 0, 3, 1, 4, 4), 4, dev_->stats);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, bool toTexture, uint8_t aspect)
{
    if (t.xrPlanes && t.luma)  // plane 0's words alone (xr.h bufferTexelBytes): the alpha plane stays as it is
        return copyBufferTexels(va, pitch, *t.luma, x, y, z, w, h, toTexture, aspect);
    if (!toTexture)
        cb_->bufferWrites_.note(va, uint64_t(pitch) * h + uint64_t(w) * 16);
    cb_->use(t, toTexture);
#if SWITCH_METAL_DIAGNOSTICS
    if (toTexture && !t.zeta)
        logEdgeMiss("upload", t, {x, y, x + w, y + h}, "");
#endif
    room(48);
    const CopySide rows{va, pitch}, texels = sideOf(t, x, y, z);
    const CopySide &from = toTexture ? rows : texels, &to = toTexture ? texels : rows;
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectDepth) {
        // the texel's first word: depth
        copyRemapped(p_, from, to, w, h,
                     toTexture ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 1, 2)
                               : remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 1),
                     4, dev_->stats);
        return;
    }
    if (aspect == kAspectStencil && t.stencilTemp && ((t.zeta && t.format == kZetaFormat) || twoPlaneXr(t.format))) {
        // its fifth byte, stencil (an RGB10A8_2P_XR10 texel's alpha, whose sixth byte is 0): as the third of its two-byte
        // components to or from the temp's two bytes a texel, the buffer's byte and a 0, and those two bytes' first to or
        // from the buffer's byte; each launch after the copies before it, which may share the temp
        const Texture &s = *t.stencilTemp;
        cb_->use(s, true);
        const CopySide temp{s.va + uint64_t(y) * s.pitch + 2ull * x, s.pitch};
        if (toTexture) {
            const bool xr = twoPlaneXr(t.format);
            if (xr) {
                room(64);
                p_.m(CE_SET_REMAP_CONST_A, 0, SUBC_COPY);
            }
            copyRemapped(p_, rows, temp, w, h,
                         remapOf(0, xr ? kRemapConstA : kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2),
                         xr ? 2 : 1, dev_->stats, true);
            copyRemapped(p_, temp, texels, w, h, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4), 2,
                         dev_->stats, true);
        } else {
            copyRemapped(p_, texels, temp, w, h, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1), 2,
                         dev_->stats, true);
            copyRemapped(p_, temp, rows, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1), 1,
                         dev_->stats, true);
        }
        return;
    }
    if (twoPlaneXr(t.format)) {
        // four-byte components: the texel's color word to the buffer's word, or into the texel with its alpha word NO_WRITE (6)
        constexpr uint32_t kOut = 3u << 16 | 1u << 20, kIn = 6u << 4 | 3u << 16 | 1u << 24;
        copyRemapped(p_, from, to, w, h, toTexture ? kIn : kOut, bufferTexelBytes(t.format, t.bpp), dev_->stats);
        return;
    }
    copySides(p_, from, to, w, h, t.bpp, dev_->stats);
}

}  // namespace gm20b

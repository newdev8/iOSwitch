// SPDX-License-Identifier: MIT
// A turned RGB10A8_2P_XR10 drawn in its planes: gm20b::expandXrPlanes makes its word's and its alpha's targets its planes,
// which hold no rotation of their own, as the passes that merge, split or fill them draw them straight. A turned pass
// takes its rotation from its targets (turned_pass.h), so each plane target goes in turned as the texture is: a copy
// in the slot's level target, which neither slot holds (the word's slot is the texture's, whole; the alpha's has none). Planes
// whose rows overlap are drawn in their pools (xr_pools.h), turned alike.
#pragma once
#include <cstdint>

namespace gm20b {

template <typename Texture, typename Pass> void turnXrPlaneTargets(const Texture &x, Pass &p, Texture (&targets)[8])
{
    if (!x.xrPlanes || !x.rotation)
        return;
    for (uint32_t k = 0; k < p.count && k < 8; k++) {
        const auto *c = p.color[k].texture;
        const bool pool = (x.luma && c == x.luma->sampled) || (x.chroma && x.chroma->sampled && c == x.chroma->sampled);
        if (c && (c == x.luma || c == x.chroma || pool)) {
            targets[k] = *p.color[k].texture;
            targets[k].rotation = x.rotation;
            p.color[k].texture = &targets[k];
        }
    }
}

}  // namespace gm20b

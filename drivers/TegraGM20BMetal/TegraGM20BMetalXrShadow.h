// SPDX-License-Identifier: MIT
// The command buffer's extended-range shadows (xr_shadow.h): the open render encoder's draws as they are recorded, and each
// ended pass that holds shadows with the shadowed variant of every state its draws take there.
#pragma once
#include <cstdint>
#include <vector>

#include "cubic.h"
#include "variant.h"
#include "written_textures.h"
#include "xr_shadow.h"

@class SwitchMetalPipeline;

namespace gm20b {
struct Pipeline;
}

// A state the open pass draws that reads or writes one of its eligible colours, as its draws record it
struct SwitchMetalXrShadowDraw {
    __unsafe_unretained SwitchMetalPipeline *pipe;
    gm20b::Variant<gm20b::Pipeline> *base;  // the variant its draws record (null: the state's own record)
    gm20b::CubicBinds binds;
    uint8_t tests;
    bool planes;
    uint16_t xrSlots;
    std::vector<gm20b::WrittenTexture> written;
    uint8_t touched;  // the eligible colours it reads or writes
};

struct SwitchMetalXrShadowOpen {
    bool open = false;
    uint32_t passAt = 0;
    size_t renderAt = 0;
    gm20b::XrShadowPass pass;
    std::vector<SwitchMetalXrShadowDraw> draws;
};

struct SwitchMetalXrShadowTwin {
    const void *base;  // its draws' variant, or its SwitchMetalPipeline where they record none
    __unsafe_unretained SwitchMetalPipeline *pipe;  // the command buffer retains it with its tape
    gm20b::Variant<gm20b::Pipeline> *shadowed;
    uint64_t refusal;  // its refusal line's key: the state's and the variant's forms, kept by a variant made again
};

// A pass that holds shadows: its OpRender in the plan, its SwitchMetalPass, and the shadowed variant of each state it draws
struct SwitchMetalXrShadowTwins {
    size_t renderAt = 0;
    uint32_t passAt = 0;
    std::vector<SwitchMetalXrShadowTwin> twins;
};

// The pipeline a draw of `variant`'s state (or of `pipe`'s own record) takes in a pass of `twins`: its shadowed variant's,
// else `drawn`
inline gm20b::Pipeline *gm20bXrShadowed(const SwitchMetalXrShadowTwins *twins, const void *pipe,
                                        gm20b::Variant<gm20b::Pipeline> *variant, gm20b::Pipeline *drawn)
{
    if (!twins)
        return drawn;
    const void *base = variant ? static_cast<const void *>(variant) : pipe;
    for (const SwitchMetalXrShadowTwin &t : twins->twins)
        if (t.base == base)
            return t.shadowed->value();
    return drawn;
}

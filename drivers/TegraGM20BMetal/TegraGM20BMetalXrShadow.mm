// SPDX-License-Identifier: MIT
// Extended-range colours' half shadows (xr_shadow.h): which colours a pass holds them of, decided from its draws when its
// encoder ends; the shadowed variants its draws take, which the queue head waits for; the shadows filled before the pass.
#import "TegraGM20BMetalInternal.h"
#include "xr_shadow.h"

void gm20bXrShadowBegin(SwitchMetalCommandBuffer *cb, SwitchMetalRenderEncoder *e, const SwitchMetalPass &facts)
{
    SwitchMetalXrShadowOpen &o = cb->_xrShadowOpen;
    o.open = false;
    o.pass = {};
    o.draws.clear();
    // a parallel encoder's sub-encoders record apart from the command buffer that runs them
    if (cb->_commandLog || facts.layers > 1 || e->_samples > 1 || e->_turn)
        return;
    for (uint32_t c = 0; c < 8; c++) {
        const SwitchMetalPass::Color &a = facts.color[c];
        __unsafe_unretained SwitchMetalTexture *x = (SwitchMetalTexture *)a.texture;
        if (!x || ![(id)x isKindOfClass:[SwitchMetalTexture class]] || !x->_gm20bOk || (facts.compressed >> c & 1) ||
            e->_formats[c] != x->_raw.format || x->_raw.planarStandIn ||
            (x->_raw.memoryless && a.load != MTLLoadActionClear))
            continue;
        const gm20b::XrForm form = gm20b::xrShadowForm(x->_raw.format, e->_xrPlanes);
        const char *tile = gm20b::xrTileState(form);
        if (form == gm20b::XrForm::None || (a.load != MTLLoadActionClear && !gRM->raw->ownPipeline(tile)))
            continue;
        o.pass.eligible |= uint8_t(1u << c);
        const double clear[4] = {a.clear.red, a.clear.green, a.clear.blue, a.clear.alpha};
        if (a.load == MTLLoadActionClear && !gm20b::xrClearHeldByCodes(form, clear))
            o.pass.clearedOff |= uint8_t(1u << c);
    }
    o.open = o.pass.eligible != 0;
    o.passAt = e->_passAt;
    o.renderAt = e->_renderAt;
}

static uint8_t readsOf(const gm20b::Pipeline &pl)
{
    return pl.storageBits | pl.fetchBits | pl.readBits;
}

static uint8_t writesOf(const gm20b::Pipeline &pl)
{
    return pl.writeBits | pl.storageWriteBits;
}

// Each draw, in order (checkDraw's every call, before its cache of the state's checks)
void gm20bXrShadowDrew(SwitchMetalCommandBuffer *cb, const gm20b::Pipeline &base)
{
    if (cb->_xrShadowOpen.open)
        cb->_xrShadowOpen.pass.draw(readsOf(base), writesOf(base));
}

// A state and variant the pass's draws take (drawnRecord, at each change of them)
void gm20bXrShadowDraw(SwitchMetalCommandBuffer *cb, SwitchMetalPipeline *pipe, const gm20b::Pipeline &base,
                       gm20b::Variant<gm20b::Pipeline> *variant, const gm20b::CubicBinds &binds, uint8_t tests,
                       bool planes, uint16_t xrSlots, const std::vector<gm20b::WrittenTexture> &written)
{
    SwitchMetalXrShadowOpen &o = cb->_xrShadowOpen;
    if (!o.open)
        return;
    const uint8_t touched = (readsOf(base) | writesOf(base)) & o.pass.eligible;
    if (!touched)
        return;
    for (const SwitchMetalXrShadowDraw &d : o.draws)
        if (d.pipe == pipe && d.base == variant)
            return;
    o.draws.push_back({pipe, variant, binds, tests, planes, xrSlots, written, touched});
}

void gm20bXrShadowBlock(SwitchMetalCommandBuffer *cb)
{
    cb->_xrShadowOpen.pass.blocked = true;
}

// A shadowed variant's refusal line, keyed by what it is made of
static uint64_t refusalKey(const SwitchMetalXrShadowDraw &d, uint8_t colours)
{
    uint64_t key = switch_refusal_key_with(SWITCH_REFUSAL_KEY("gm20b", "xr.shadow_twin"), d.pipe->_packKey);
    key = switch_refusal_key_with(key, uint64_t(colours) | uint64_t(d.tests) << 8 | uint64_t(d.planes) << 16 |
                                           uint64_t(d.xrSlots) << 32);
    for (const gm20b::CubicBinds::Sampler &s : d.binds.samplers)
        key = switch_refusal_key_with(key, uint64_t(s.slot) | uint64_t(s.s) << 8 | uint64_t(s.t) << 16 |
                                               uint64_t(s.r) << 24 | uint64_t(s.border) << 32 |
                                               uint64_t(s.normalized) << 40 | uint64_t(s.stage) << 48);
    for (const gm20b::CubicBinds::Texture &t : d.binds.textures)
        key = switch_refusal_key_with(key, uint64_t(t.slot) | uint64_t(t.format) << 8 | uint64_t(t.stage) << 40);
    for (const gm20b::WrittenTexture &w : d.written)
        key = switch_refusal_key_with(key, uint64_t(w.slot) | uint64_t(w.format) << 8);
    return key;
}

// The shadowed variant of each state that touches a held colour, asked now so the queue head finds it made
void gm20bXrShadowEnd(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalXrShadowOpen &o = cb->_xrShadowOpen;
    if (!o.open)
        return;
    o.open = false;
    const uint8_t held = o.pass.held();
    if (!held) {
        o.draws.clear();
        return;
    }
    SwitchMetalXrShadowTwins twins;
    twins.renderAt = o.renderAt;
    twins.passAt = o.passAt;
    for (const SwitchMetalXrShadowDraw &d : o.draws)
        if (const uint8_t mask = d.touched & held)
            twins.twins.push_back({d.base ? static_cast<const void *>(d.base) : (__bridge const void *)d.pipe, d.pipe,
                                   gm20bRenderVariant(d.pipe, d.binds, d.tests, d.planes, d.xrSlots, d.written, mask),
                                   refusalKey(d, mask)});
    o.draws.clear();
    cb->_passes.own(o.passAt).xrShadows = held;
    cb->_xrShadowPasses.push_back(std::move(twins));
    // the queue head may still drop these shadows (gm20bXrShadowReadiness): only an encode after the commit can follow
    gm20bPipelineVeto(cb, VetoShadows);
}

// Every state that draws into a held colour has a shadowed variant, so one the compiler refused or failed is a defect,
// its key named once per process. The line's own key gates the name's lookup: a pass drawn every frame costs a relaxed
// load
static void reportTwin(const SwitchMetalXrShadowTwin &twin, bool failed, uint8_t colours)
{
    if (!switch_refusal_first(twin.refusal))
        return;
    std::string key = twin.pipe->_key;
    {
        std::lock_guard<gm20b::Mutex> lock(twin.pipe->_variantLock);
        for (const auto &[name, variant] : twin.pipe->_variants)
            if (variant.get() == twin.shadowed) {
                key += name;
                break;
            }
    }
    SWITCH_REFUSE_DETAIL(
        "gm20b", "xr.shadow_twin", gm20b::Pack::fnv(key.data(), key.size()),
        "the shader compiler %s the shadowed variant %s: its pass's extended-range colours %#x are drawn "
        "from their codes, without their half shadow",
        failed ? "failed" : "refused", key.c_str(), unsigned(colours));
}

// A pass whose shadowed variants are not all made holds no shadow: its draws read the codes, as before
int gm20bXrShadowReadiness(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    using Variant = gm20b::Variant<gm20b::Pipeline>;
    bool pending = false;
    for (SwitchMetalXrShadowTwins &t : cb->_xrShadowPasses) {
        if (t.renderAt < from || t.renderAt >= to || t.twins.empty())
            continue;
        bool given = false;
        for (const SwitchMetalXrShadowTwin &twin : t.twins) {
            std::function<void()> observer;
            if (cb->_variantObserved.insert(twin.shadowed).second) {
                void *queue = (__bridge void *)cb->_queue;
                observer = [queue] {
                    wakeSubmit(queue);
                };
            }
            const Variant::State state = twin.shadowed->observe(std::move(observer));
            pending |= state == Variant::State::Pending;
            if (state == Variant::State::Refused || state == Variant::State::Failed) {
                reportTwin(twin, state == Variant::State::Failed, cb->_passes[t.passAt].xrShadows);
                given = true;
            }
        }
        if (given) {
            cb->_passes[t.passAt].xrShadows = 0;
            t.twins.clear();
        }
    }
    return pending ? 0 : 1;
}

const SwitchMetalXrShadowTwins *gm20bXrShadowTwinsOf(const SwitchMetalCommandBuffer *cb, uint32_t passAt)
{
    if (passAt >= cb->_passes.size() || !cb->_passes[passAt].xrShadows)
        return nullptr;
    for (const SwitchMetalXrShadowTwins &t : cb->_xrShadowPasses)
        if (t.passAt == passAt)
            return &t;
    return nullptr;
}

// Each held colour's shadow, set before the pass: a Clear's halves, else the tile of the attachment's codes as the pass
// begins (the pack's tile state). The codes' header it reads goes in `codes`, which lives as long as `rc`: rc caches a
// texture's reads by its address
bool gm20bXrShadowFill(gm20b::CommandBuffer *rc, const SwitchMetalPass &pass, gm20b::PassDesc &p,
                       std::deque<gm20b::Texture> &codes, const char **why)
{
    gm20b::Device &d = *gRM->raw;
    for (uint8_t held = pass.xrShadows; held; held &= uint8_t(held - 1)) {
        const uint32_t c = uint32_t(__builtin_ctz(held));
        const gm20b::Texture *target = p.color[c].texture;
        __unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)pass.color[c].texture;
        const gm20b::XrForm form = gm20b::xrShadowForm(t->_raw.format, false);
        gm20b::Texture *shadow =
            target ? rc->turnScratch(gm20b::kXrStandInFormat, target->width, target->height, gm20b::kXrShadowTag + c)
                   : nullptr;
        if (!shadow) {
            *why = "no half shadow for an extended-range colour";
            return false;
        }
        gm20b::PassDesc q;
        q.color[0].texture = shadow;
        q.color[0].store = true;
        q.count = 1;
        const bool cleared = pass.color[c].load == MTLLoadActionClear;
        if (cleared) {
            const MTLClearColor k = pass.color[c].clear;
            const double color[4] = {k.red, k.green, k.blue, k.alpha};
            q.color[0].load = gm20b::ColorAttachment::Clear;
            gm20b::xrClearTile(form, color, q.color[0].clear);
        } else {
            q.color[0].load = gm20b::ColorAttachment::DontCare;
        }
        const gm20b::Texture *tile = nullptr;
        if (!cleared) {
            gm20b::Texture &own = codes.emplace_back(*target);
            own.standIn = nullptr;
            if (own.sampled && rc->readTexture(&t->_raw) == &t->_raw) {
                gm20b::BlitEncoder *b = rc->blit();
                b->copyTexels(own, 0, 0, 0, *own.sampled, 0, 0, 0, own.width, own.height);
                b->end();
            }
            tile = &own;
        }
        gm20b::RenderEncoder *e = rc->render(q);
        if (!e) {
            *why = "an extended-range colour's half shadow the driver cannot render";
            return false;
        }
        if (tile) {
            e->setPipeline(d.ownPipeline(gm20b::xrTileState(form)));
            e->setTexture(0, tile);
            e->setViewport(0, 0, shadow->width, shadow->height, 0, 1);
            e->setScissor(0, 0, shadow->width, shadow->height);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
        }
        e->end();
        p.xrShadow[c] = shadow;
    }
    return true;
}

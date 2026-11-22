// SPDX-License-Identifier: MIT
// Multisampled passes' forms on gm20b::Device: a one-sample colour beside a four-sample depth (target-independent
// rasterisation), and a multisampled clear's reach.
#include "device_forms.h"

namespace {

// A one-sample colour beside a four-sample depth: the depth cleared under the zeta's own layout (SET_ANTI_ALIAS 2X2), then
// target-independent rasterisation on (the colour's 1X1, raster 2X2, no modulation), and off again at the pass's end
TEST_F(DevicePassForms, AOneSampleColourBesideAFourSampleDepthRasterisesAtTheDepthsSamples)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    EXPECT_EQ(addressOf(ms, NVB197_SET_ZT_A), zeta.va);
    EXPECT_EQ(addressOf(ms, NVB197_SET_COLOR_TARGET_A(0)), colour.va);
    uint32_t antiAlias = ~0u, raster = ~0u, modulation = ~0u;
    bool zetaCleared = false, on = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ANTI_ALIAS)
            antiAlias = x.value;
        else if (x.method == NVB197_SET_ANTI_ALIAS_RASTER)
            raster = x.value;
        else if (x.method == NVB197_SET_TIR_MODULATION)
            modulation = x.value;
        else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 1) && !on) {
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_2X2)) << "the zeta's own layout";
            zetaCleared = true;
        } else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M && !on) {
            on = true;
            EXPECT_TRUE(zetaCleared) << "the depth is cleared before the mode is on";
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
            EXPECT_EQ(raster, uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_2X2));
            EXPECT_EQ(modulation, uint32_t(NVB197_SET_TIR_MODULATION_COMPONENT_SELECT_NO_MODULATION));
        }
    }
    EXPECT_TRUE(on);
    EXPECT_EQ(lastOf(ms, NVB197_SET_TIR), uint32_t(NVB197_SET_TIR_MODE_DISABLED));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS_RASTER), uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_1X1));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS), uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
}

// Each state's colour rule in a mixed pass (mixed_samples.h): a state of one sample merges the pixel's raster samples, a
// state of four keeps sample 0's; the depth is written at every covered sample under both (SET_SAMPLE_MASK all)
TEST_F(DevicePassForms, AMixedPassSetsEachStatesCoverageRule)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(
        p,
        [&](gm20b::RenderEncoder &e, const char **refused) {
            e.setPipeline(four);
            if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
                return false;
            e.setPipeline(one);
            return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        },
        &why);
    ASSERT_FALSE(ms.empty()) << why;
    const std::vector<uint32_t> controls = values(ms, NVB197_SET_TIR_CONTROL);
    ASSERT_GE(controls.size(), 3u);
    EXPECT_EQ(controls[controls.size() - 2], gm20b::tirControl(4));
    EXPECT_EQ(controls.back(), gm20b::tirControl(1));
    for (const uint32_t m : values(ms, NVB197_SET_SAMPLE_MASK_X0_Y0))
        EXPECT_EQ(m, 0xffffu);
}

TEST_F(DevicePassForms, OtherMixedSampleCountsAreRefused)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 2, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    EXPECT_TRUE(pass(p, &why).empty());
    EXPECT_NE(why.find("attachments of different sample counts"), std::string::npos) << why;
}

// A multisampled colour cleared over a render area smaller than it, neither stored nor memory of its own: the clear reaches
// the area rounded up to whole 32-pixel tiles, which its resolve reads (an M4 Max resolves a 64x64 Clear of a 40x24 area
// over [0, 64) x [0, 32), the clear's past the area: hwtests-r4 gpu.gm20b.resolve_codes_parts)
TEST_F(DevicePassForms, AMultisampledClearReachesTheAreasWholeTiles)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMemoryless(64, 64, kRGBA8, t, 4));
    gm20b::PassDesc p;
    p.color[0] = kept(t, gm20b::ColorAttachment::Clear);
    p.color[0].store = false;
    p.count = 1;
    p.width = 40;
    p.height = 24;
    p.drawn = true;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const auto h = values(ms, NVB197_SET_CLEAR_RECT_HORIZONTAL), v = values(ms, NVB197_SET_CLEAR_RECT_VERTICAL);
    EXPECT_NE(std::find(h.begin(), h.end(), 64u << 16), h.end());
    EXPECT_NE(std::find(v.begin(), v.end(), 32u << 16), v.end());
}

}  // namespace

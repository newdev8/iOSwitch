// SPDX-License-Identifier: MIT
// A " | xrsh=" variant's draw (xr_shadow.h): its record's EntryXrShadow binds the half shadow the pass holds of that
// colour (PassDesc::xrShadow, which TegraGM20BMetalXrShadow.mm fills before the pass), and a pass that holds none refuses the draw.
// Read off the set-0 word the draw uploaded.
#include "device_harness.h"
#include "../record.h"
#include <vector>

namespace {

constexpr uint32_t kBGR10XR = 554, kRGBA16Float = 115, kTriangle = 3;

class DeviceXrShadow : public harness::DeviceTest {
protected:
    gm20b::Texture target, shadow;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(32, 32, kBGR10XR, target));
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA16Float, shadow));
    }

    gm20b::Pipeline *record(uint64_t hash)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=554 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        gm20b::PackPipeline &pp = r.pipeline;
        pp.keyHash = hash;
        pp.flags = gm20b::PipelinePushMetadata;
        pp.setBytes = 16;
        gm20b::setEntryCount(pp, 1);
        pp.entry[0] = {0, gm20b::EntryXrShadow, 2, 0, 0, 0, 0};
        r.blob = {0x1a00, 0x1111};
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One draw of `pl` in a pass holding `held` as colour 0's shadow: "" drawn, else why not; `header` the set-0 word
    std::string draw(gm20b::Pipeline *pl, const gm20b::Texture *held, uint32_t &header)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.xrShadow[0] = held;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        const char *why = nullptr;
        if (!e->draw(kTriangle, 0, 3, 1, 0, &why)) {
            cb->abandon();
            return why ? why : "refused";
        }
        const uint64_t set = gm20b::DeviceHarness::preparedSet(*e);
        const uint8_t *words = gm20b::DeviceHarness::upload(dev, set, 4);
        header = words ? (words[0] | words[1] << 8 | (words[2] & 0xf) << 16) : 0;
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return "not committed";
        }
        return "";
    }
};

TEST_F(DeviceXrShadow, AShadowedVariantBindsThePassShadowOfItsColour)
{
    gm20b::Pipeline *pl = record(0x5ead0);
    ASSERT_NE(pl, nullptr);
    uint32_t header = 0;
    ASSERT_EQ(draw(pl, &shadow, header), "");
    EXPECT_EQ(header, shadow.tic);
}

TEST_F(DeviceXrShadow, APassThatHoldsNoShadowRefusesTheShadowedVariant)
{
    gm20b::Pipeline *pl = record(0x5ead1);
    ASSERT_NE(pl, nullptr);
    uint32_t header = 0;
    EXPECT_EQ(draw(pl, nullptr, header),
              "an extended-range shadow's variant drawn in a pass that holds no shadow of the colour");
}

}  // namespace

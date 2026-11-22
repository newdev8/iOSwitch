// SPDX-License-Identifier: MIT
// A record's colour targets: emit writes every target's per-target blend switch, target 0's too; a two-plane state's alpha
// targets are its 550 colours' alpha indices (xr_pass.h), emit lays them out, and assembleRecord keeps their
// registers as it keeps those of the colours the key names, a target neither names left out (record_parts.cpp foreign)
#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>
#include "../emit.h"
#include "../record_parts.h"
#include "../xr_pass.h"

namespace {

constexpr uint32_t kCtWrite = 0x1a00, kBlendPerTarget = 0x1e00;
constexpr char kTwoPlaneKey[] = "samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0";

uint32_t registerOf(const std::vector<gm20b::EmitRegister> &regs, uint32_t method)
{
    for (const gm20b::EmitRegister &r : regs)
        if (r.method == method)
            return r.value;
    return 0xffffffffu;
}

TEST(RecordTargets, TheAlphaTargetsAreTheAlphaIndices)
{
    const uint32_t renderBox[8] = {550, 105, 550};
    EXPECT_EQ(gm20b::xrAlphaTargets(renderBox), 1u << 3 | 1u << 4);
    const uint32_t hole[8] = {550, 0, 80};
    EXPECT_EQ(gm20b::xrAlphaTargets(hole), 1u << 1);
    const uint32_t after[8] = {0, 550};
    EXPECT_EQ(gm20b::xrAlphaTargets(after), 1u << 0);
    const uint32_t full[8] = {550, 80, 80, 80, 80, 80, 80, 80};
    EXPECT_EQ(gm20b::xrAlphaTargets(full), 0u);
}

TEST(RecordTargets, EmitStateLaysOutTheAlphaTargets)
{
    const std::array<uint8_t, 80> header{};
    gm20b::EmitProgram vertex, fragment;
    vertex.header = header.data();
    fragment.header = header.data();
    std::vector<gm20b::EmitRegister> regs;
    std::string why;
    uint32_t alpha = ~0u;
    ASSERT_TRUE(gm20b::emitState(kTwoPlaneKey, vertex, fragment, regs, why, nullptr, true, &alpha)) << why;
    EXPECT_EQ(alpha, 1u << 1);
    EXPECT_NE(registerOf(regs, kCtWrite + 4), 0xffffffffu);
    ASSERT_TRUE(gm20b::emitState(kTwoPlaneKey, vertex, fragment, regs, why, nullptr, false, &alpha)) << why;
    EXPECT_EQ(alpha, 0u);
}

TEST(RecordTargets, EveryTargetBlendsAlphaByItsOwnFactors)
{
    const std::array<uint8_t, 80> header{};
    gm20b::EmitProgram vertex, fragment;
    vertex.header = header.data();
    fragment.header = header.data();
    std::vector<gm20b::EmitRegister> regs;
    std::string why;
    ASSERT_TRUE(gm20b::emitState("samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:15:0:1:0:0:1:0:0", vertex,
                                 fragment, regs, why))
        << why;
    EXPECT_EQ(registerOf(regs, kBlendPerTarget), 1u);
    EXPECT_EQ(registerOf(regs, kBlendPerTarget + 0x20), 1u);
    EXPECT_NE(gm20b::emittableMethods().count(kBlendPerTarget), 0u);
}

gm20b::CapturedProgram program(const char *id)
{
    gm20b::CapturedProgram p;
    p.id = id;
    p.image.assign(96, 0);
    p.cbufTypes = {gm20b::CbufRoot};
    return p;
}

// The blob of a state of one 550 colour at target 0 whose registers name targets 0, 1 and 2
std::vector<uint32_t> blobOf(uint32_t alphaTargets)
{
    static const gm20b::CapturedProgram vs = program("vs"), fs = program("fs");
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.regs = {{kCtWrite, 0x1111}, {kCtWrite + 4, 0x1000}, {kCtWrite + 8, 0x1111}, {kBlendPerTarget + 0x20, 1}};
    parts.alphaTargets = alphaTargets;
    gm20b::PackRules rules;
    rules.varying = {kCtWrite, kCtWrite + 4, kCtWrite + 8, kBlendPerTarget + 0x20};
    gm20b::RuntimeRecord out;
    std::string why;
    EXPECT_TRUE(gm20b::assembleRecord(parts, std::string(kTwoPlaneKey) + " | v vs | f fs", rules, out, why)) << why;
    EXPECT_TRUE(out.extras.empty());
    return out.blob;
}

TEST(RecordTargets, ARecordKeepsItsAlphaTargetsRegisters)
{
    EXPECT_EQ(blobOf(1u << 1),
              (std::vector<uint32_t>{kCtWrite, 0x1111, kCtWrite + 4, 0x1000, kBlendPerTarget + 0x20, 1}));
    EXPECT_EQ(blobOf(0), (std::vector<uint32_t>{kCtWrite, 0x1111}));
}

}  // namespace

// SPDX-License-Identifier: MIT
#include "facade_variant_compiler.h"
#include "../fragment_tests.h"
#include "bazel/testing/fresh_process.h"
#import <IOSurface/IOSurface.h>

#include <unistd.h>

namespace {

using namespace harness;

void preparedCubic(uint32_t stage, bool pipelineFirst)
{
    const std::string directory = dir() + "/cubic-" + std::to_string(stage) +
                                  (pipelineFirst ? "-pipeline-" : "-bindings-") + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    RenderCompiler compiler;
    ASSERT_TRUE(compiler.serve(directory));
    const std::vector<gm20b::PackEntry> entries = {{0, gm20b::EntryTexture, 1, 0, gm20b::SamplerSlot, 0, 0},
                                                   {16, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0}};
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, entries);
    NSString *why = nil;
    id<MTLDevice> device = metal_device_open(pack.c_str(), 0, 0, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    id<MTLTexture> firstTexture = image(device, MTLTextureUsageShaderRead);
    id<MTLTexture> sameFormat = image(device, MTLTextureUsageShaderRead);
    ASSERT_TRUE(firstTexture != nil && sameFormat != nil);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = static_cast<MTLSamplerMinMagFilter>(gm20b::kFilterCubic);
    id<MTLSamplerState> cubic = [device newSamplerStateWithDescriptor:sd];
    id<MTLSamplerState> sameSampler = [device newSamplerStateWithDescriptor:sd];
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> unusedSampler = [device newSamplerStateWithDescriptor:sd];
    ASSERT_TRUE(cubic != nil && sameSampler != nil && unusedSampler != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = image(device, MTLTextureUsageRenderTarget);
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    const auto begin = [&] {
        commands = [queue commandBuffer];
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (pipelineFirst)
            [encoder setRenderPipelineState:state];
        texture(encoder, stage, firstTexture);
        sampler(encoder, stage, cubic);
        if (!pipelineFirst)
            [encoder setRenderPipelineState:state];
    };
    begin();
    ASSERT_TRUE(compiler.wait(1)) << "cubic preparation must ask before the first draw";
    const auto requests = compiler.captured();
    ASSERT_EQ(requests.size(), 1u);
    const auto &binds = requests[0].variant.cubic;
    ASSERT_EQ(binds.samplers.size(), 1u);
    ASSERT_EQ(binds.textures.size(), 1u);
    EXPECT_EQ(binds.samplers[0].stage, stage);
    EXPECT_EQ(binds.textures[0].stage, stage);
    EXPECT_EQ(binds.textures[0].format, MTLPixelFormatBGRA8Unorm);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    const auto found = pipeline->_variants.find(requests[0].variant.name());
    ASSERT_NE(found, pipeline->_variants.end());
    ASSERT_TRUE(ready(*found->second));
    gm20b::Pipeline *selected = found->second->value();
    ASSERT_NE(selected, nullptr);
    const uint32_t expected = program(selected->methods, stage);
    ASSERT_NE(expected, UINT32_MAX);
    const auto draws = [&] {
        for (unsigned i = 0; i < 16; i++) {
            texture(encoder, stage, i & 1 ? sameFormat : firstTexture);
            sampler(encoder, stage, i & 2 ? sameSampler : cubic);
            sampler(encoder, stage, i & 1 ? unusedSampler : cubic, 1);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    };
    for (unsigned round = 0; round < 8; round++) {
        @autoreleasepool {
            if (round)
                begin();
            draws();
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            ASSERT_EQ(commands.status, MTLCommandBufferStatusCompleted);
            encoder = nil;
            commands = nil;
        }
    }
    const size_t firstJob = kernel_model::model.jobs.size();
    begin();
    const uint64_t placed = gRM->raw->stats.programsPlaced;
    record_count work = {};
    record_count_start();
    draws();
    record_count_stop(&work);
    EXPECT_EQ(work.locks, 0u);
    EXPECT_EQ(work.mallocs, 0u);
    EXPECT_EQ(work.dispatches, 0u);
    EXPECT_EQ(compiler.captured().size(), 1u);
    EXPECT_EQ(gRM->raw->stats.programsPlaced, placed);
    std::vector<uint32_t> expectedDraws(16, expected);
    sampler(encoder, stage, nil);
    texture(encoder, stage, nil);
    texture(encoder, stage, firstTexture);
    sampler(encoder, stage, cubic);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(expected);
    EXPECT_EQ(compiler.captured().size(), 1u);

    id<MTLTexture> otherFormat = image(device, MTLTextureUsageShaderRead, MTLPixelFormatRGBA16Float);
    ASSERT_TRUE(otherFormat != nil);
    texture(encoder, stage, otherFormat);
    ASSERT_TRUE(compiler.wait(2));
    const auto rebound = compiler.captured();
    ASSERT_EQ(rebound.size(), 2u);
    ASSERT_EQ(rebound.back().variant.cubic.textures.size(), 1u);
    EXPECT_EQ(rebound.back().variant.cubic.textures[0].stage, stage);
    EXPECT_EQ(rebound.back().variant.cubic.textures[0].format, MTLPixelFormatRGBA16Float);
    const auto changed = pipeline->_variants.find(rebound.back().variant.name());
    ASSERT_NE(changed, pipeline->_variants.end());
    ASSERT_TRUE(ready(*changed->second));
    const uint32_t changedProgram = program(changed->second->value()->methods, stage);
    ASSERT_NE(changedProgram, UINT32_MAX);
    EXPECT_NE(changedProgram, expected);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(changedProgram);
    texture(encoder, stage, firstTexture);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(expected);
    EXPECT_EQ(compiler.captured().size(), 2u);
    const uint32_t otherStage = stage == 1 ? 2 : 1;
    texture(encoder, otherStage, sameFormat);
    sampler(encoder, otherStage, sameSampler);
    ASSERT_TRUE(compiler.wait(3));
    const auto both = compiler.captured();
    ASSERT_EQ(both.size(), 3u);
    ASSERT_EQ(both.back().variant.cubic.samplers.size(), 2u);
    EXPECT_EQ(both.back().variant.cubic.samplers[0].stage, gm20b::kCubicFragment);
    EXPECT_EQ(both.back().variant.cubic.samplers[1].stage, gm20b::kCubicVertex);
    const auto joined = pipeline->_variants.find(both.back().variant.name());
    ASSERT_NE(joined, pipeline->_variants.end());
    ASSERT_TRUE(ready(*joined->second));
    const uint32_t otherProgram = program(joined->second->value()->methods, otherStage);
    ASSERT_NE(otherProgram, UINT32_MAX);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(program(joined->second->value()->methods, stage));
    [encoder endEncoding];
    state = nil;
    library = nil;
    firstTexture = sameFormat = otherFormat = nil;
    cubic = sameSampler = unusedSampler = nil;
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    const auto programs = drawnPrograms(firstJob, stage);
    EXPECT_EQ(programs, expectedDraws);
    const auto otherPrograms = drawnPrograms(firstJob, otherStage);
    ASSERT_FALSE(otherPrograms.empty());
    EXPECT_EQ(otherPrograms.back(), otherProgram);
    EXPECT_EQ(compiler.captured().size(), 3u);
}

void composedForms()
{
    constexpr MTLPixelFormat colour = MTLPixelFormat(550);
    const std::string directory = dir() + "/cubic-composed-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    RenderCompiler compiler;
    ASSERT_TRUE(compiler.serve(directory));
    MTLRenderPipelineDescriptor *description = descriptor(nil, colour);
    description.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    const gm20b::StageKey vertex{"v", ""}, fragment{"f", ""};
    const std::string key = gm20b::pipelineKey(description, &vertex, &fragment, "", "QuartzCore");
    const std::vector<gm20b::PackEntry> entries = {{0, gm20b::EntryTexture, 1, 0, gm20b::SamplerSlot, 0, 0},
                                                   {16, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0},
                                                   {32, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0}};
    const std::string pack = writePack(key, {}, {}, true, "", {}, entries);
    NSString *why = nil;
    id<MTLDevice> device = metal_device_open(pack.c_str(), 0, 0, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    description.vertexFunction = [library newFunctionWithName:@"v"];
    description.fragmentFunction = [library newFunctionWithName:@"f"];
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:description error:nil];
    ASSERT_TRUE(state != nil);
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(64 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 64 * 64 * 4),
            (id)kIOSurfacePlaneSize : @(64 * 64 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(64 * 64 * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_NE(surface, nullptr);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:colour width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td iosurface:surface plane:0];
    CFRelease(surface);
    ASSERT_TRUE(target != nil);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = image(device, MTLTextureUsageRenderTarget, MTLPixelFormatDepth32Float);
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    texture(encoder, 1, image(device, MTLTextureUsageShaderRead));
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = static_cast<MTLSamplerMinMagFilter>(gm20b::kFilterCubic);
    sampler(encoder, 1, [device newSamplerStateWithDescriptor:sd]);
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
    ASSERT_TRUE(compiler.wait(3));
    const auto requests = compiler.captured();
    ASSERT_EQ(requests.size(), 3u);
    const auto composed = std::find_if(requests.begin(), requests.end(), [](const auto &request) {
        return request.variant.fragmentTests == uint8_t(gm20b::FragmentTests::DepthWrite) &&
               !request.variant.cubic.empty() && request.variant.twoPlane;
    });
    ASSERT_NE(composed, requests.end());
    const auto &forms = composed->variant;
    ASSERT_EQ(forms.cubic.samplers.size(), 1u);
    EXPECT_EQ(forms.cubic.samplers[0].stage, gm20b::kCubicVertex);
    EXPECT_TRUE(forms.twoPlane);
    EXPECT_EQ(forms.fragmentTests, uint8_t(gm20b::FragmentTests::DepthWrite));
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    for (const auto &request : requests) {
        const auto found = pipeline->_variants.find(request.variant.name());
        ASSERT_NE(found, pipeline->_variants.end());
        ASSERT_TRUE(ready(*found->second));
    }
    gm20b::Pipeline *selected = pipeline->_variants.at(forms.name())->value();
    ASSERT_NE(selected, nullptr);
    const uint32_t expected = program(selected->methods, 1);
    ASSERT_NE(expected, UINT32_MAX);
    const size_t firstJob = kernel_model::model.jobs.size();
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    state = nil;
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 1), (std::vector<uint32_t>{expected}));
    EXPECT_EQ(compiler.captured().size(), 3u);
}

// A BGRA10_XR colour written unblended by one state and then blended into by another in the same pass: the pass holds the
// colour's half shadow (xr_shadow.h) and asks the shader compiler for both states' " | xrsh=1" variants. Made, both
// draws take them; with `refused` the compiler refuses both, both draws take their states' own records, and each refused
// variant's key has a refusal line of its own.
void shadowedPass(bool refused)
{
    constexpr MTLPixelFormat colour = MTLPixelFormat(552);
    const std::string directory = dir() + "/xr-shadow-" + (refused ? "refused-" : "made-") + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    RenderCompiler compiler;
    if (refused)
        compiler.refuses = [](const gm20b::CompileRequest &request) {
            return request.variant.xrShadows != 0;
        };
    ASSERT_TRUE(compiler.serve(directory));
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    NSString *why = nil;
    id<MTLDevice> device = metal_device_open(pack.c_str(), 0, 0, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *blending = descriptor(library, colour);
    MTLRenderPipelineColorAttachmentDescriptor *over = blending.colorAttachments[0];
    over.blendingEnabled = YES;
    over.sourceRGBBlendFactor = over.sourceAlphaBlendFactor = MTLBlendFactorOne;
    over.destinationRGBBlendFactor = over.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> writer =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, colour) error:nil];
    id<MTLRenderPipelineState> blender = [device newRenderPipelineStateWithDescriptor:blending error:nil];
    ASSERT_TRUE(writer != nil && blender != nil);
    __unsafe_unretained SwitchMetalPipeline *states[2] = {(SwitchMetalPipeline *)writer,
                                                          (SwitchMetalPipeline *)blender};
    ASSERT_TRUE(states[0]->_raw && states[1]->_raw) << "the shader compiler makes both states at their creation";
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:colour width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLTexture> sampled = image(device, MTLTextureUsageShaderRead);
    id<MTLSamplerState> nearest = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    ASSERT_TRUE(target != nil && sampled != nil && nearest != nil);
    const size_t firstJob = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    for (uint32_t stage : {1u, 2u}) {
        texture(encoder, stage, sampled);
        sampler(encoder, stage, nearest);
    }
    for (id<MTLRenderPipelineState> state in @[ writer, blender ]) {
        [encoder setRenderPipelineState:state];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    ASSERT_TRUE(compiler.wait(4)) << "each state, then each state's shadowed variant at the pass's end";
    size_t asked = 0;
    for (const auto &request : compiler.captured())
        asked += request.variant.xrShadows == 1;
    EXPECT_EQ(asked, 2u);
    gm20b::RenderVariant shadowed;
    shadowed.xrShadows = 1;
    std::vector<uint32_t> expected;
    for (__unsafe_unretained SwitchMetalPipeline *state : states) {
        const auto twin = state->_variants.find(shadowed.name());
        ASSERT_NE(twin, state->_variants.end()) << state->_key;
        ASSERT_EQ(ready(*twin->second), !refused) << state->_key;
        const uint32_t own = program(state->_raw->methods, 2);
        if (!refused)
            ASSERT_NE(program(twin->second->value()->methods, 2), own) << state->_key;
        expected.push_back(refused ? own : program(twin->second->value()->methods, 2));
    }
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 2), expected);
    for (__unsafe_unretained SwitchMetalPipeline *state : states) {
        const std::string key = state->_key + shadowed.name();
        const uint64_t line = switch_refusal_key_with(SWITCH_REFUSAL_KEY("gm20b", "xr.shadow_twin"),
                                                      gm20b::Pack::fnv(key.data(), key.size()));
        EXPECT_EQ(switch_refusal_first(line), !refused)
            << (refused ? "no refusal line for " : "a refusal line for ") << key;
    }
}

}  // namespace

TEST(GM20BFacadeCubic, FragmentPreparationPrecedesDrawAndSameFormatRebindsReuseIt)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(2, true);
    });
}

TEST(GM20BFacadeCubic, VertexPreparationPrecedesDrawAndSameFormatRebindsReuseIt)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(1, true);
    });
}

TEST(GM20BFacadeCubic, VertexBindingsBeforePipelineSelectThePreparedProgram)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(1, false);
    });
}

TEST(GM20BFacadeCubic, FragmentBindingsBeforePipelineSelectThePreparedProgram)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(2, false);
    });
}

TEST(GM20BFacadeCubic, VertexCubicComposesWithDepthTestsAndTwoPlaneTargetsAtSetup)
{
    switch_ios::test::ExpectPassesInFreshProcess(composedForms);
}

TEST(GM20BFacadeCubic, APassWithAnUnblendedWriterAndABlendKeepsItsHalfShadow)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        shadowedPass(false);
    });
}

TEST(GM20BFacadeCubic, EachRefusedShadowedVariantHasItsOwnRefusalLine)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        shadowedPass(true);
    });
}

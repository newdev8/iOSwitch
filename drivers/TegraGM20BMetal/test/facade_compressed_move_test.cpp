// SPDX-License-Identifier: MIT
// A private target made plain for want of compressed memory moves onto it on the submit worker (private_comp.h
// compressedFromPlain) while another thread records passes into it, binds it for sampling and copies out of it, through
// the facade over the kernel model. The worker alone writes the texture's memory fields; the recording reads none of
// them, which the thread-sanitized build (facade_compressed_move_tsan_test) holds to: any such read with no order to
// the move is its failure.
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <mutex>
#include <thread>

namespace {

using scale_frames::opened;

constexpr int kRounds = 16;

uint64_t counted(id<MTLDevice> device, const char *selector, NSString *key)
{
    NSDictionary *d = ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)device, sel_registerName(selector));
    return [d[key] unsignedLongLongValue];
}

NSString *facts(id<MTLDevice> device, id<MTLTexture> texture)
{
    return ((NSString * (*)(id, SEL, id)) objc_msgSend)((id)device, sel_registerName("switchMetalProbeFacts:"),
                                                        texture);
}

id<MTLTexture> sharedTexture(id<MTLDevice> device, const scale_frames::Shape &shape)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:shape.width
                                                                                 height:shape.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:td];
}

TEST(GM20BFacadeCompressedMove, ARecordingThreadUsesATargetWhileTheWorkerMovesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const scale_frames::Shape shape{4, 24, 1, 704, 392};
    const uint64_t made = counted(device, "switchMetalCompression", @"made");
    const auto target = scale_frames::targets(device, shape);
    ASSERT_TRUE([facts(device, target[0]) containsString:@"comptags=0"]) << "none of its size made: asked for";
    ASSERT_EQ(harness::compressionSettled(device), 1) << "the compression thread did not settle the target's ask once";
    ASSERT_GT(counted(device, "switchMetalCompression", @"made"), made) << "no compressed memory was made";
    const uint64_t moves = counted(device, "switchMetalCounters", @"private_compressed_moves");
    const std::vector<id<MTLTexture>> beside = {sharedTexture(device, shape)};
    id<MTLTexture> copy = sharedTexture(device, shape);
    id<MTLTexture> moved = target[0];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLRenderPipelineState> state = opened().state;
    id<MTLCommandBuffer> last = nil;
    // each command buffer committed with no wait, so the next one records while the worker submits it and moves the
    // target: nothing orders this thread's reads of the texture after the move but what the facade itself does
    std::thread recorder([&] {
        for (int round = 0; round < kRounds; round++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            scale_frames::pass(cb, state, target, shape, 0, uint32_t(round));
            scale_frames::pass(cb, state, beside, shape, 0, uint32_t(round), nil, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentTexture:moved atIndex:0];
            });
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:moved
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(shape.width, shape.height, 1)
                        toTexture:copy
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cb commit];
            last = cb;
        }
    });
    recorder.join();
    [last waitUntilCompleted];
    EXPECT_EQ(last.status, MTLCommandBufferStatusCompleted) << last.error.description.UTF8String;
    EXPECT_TRUE([facts(device, moved) containsString:@"comptags=1"]) << facts(device, moved).UTF8String;
    EXPECT_EQ(counted(device, "switchMetalCounters", @"private_compressed_moves"), moves + 1);
}

// A private Depth32Float texture made plain for want of ZF32_2CZ memory moves onto it on the submit worker while
// another thread records passes that clear, load and store it as their depth attachment: the move writes its memory
// fields under the device lock and the recording reads none of them (the thread-sanitized build's check)
TEST(GM20BFacadeCompressedMove, ARecordingThreadAttachesADepthTextureWhileTheWorkerMovesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const uint64_t made = counted(device, "switchMetalCompression", @"made");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:200
                                                                                 height:120
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [device newTextureWithDescriptor:td];
    ASSERT_TRUE([facts(device, depth) containsString:@"comptags=0"]) << "none of its size made: asked for";
    ASSERT_EQ(harness::compressionSettled(device), 1);
    ASSERT_GT(counted(device, "switchMetalCompression", @"made"), made) << "no compressed memory was made";
    const auto target = scale_frames::targets(device, scale_frames::Shape{1, 4, 1, 200, 120});
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> last = nil;
    std::thread recorder([&] {
        for (int round = 0; round < kRounds; round++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target[0];
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = depth;
            rp.depthAttachment.loadAction = round ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 1.0;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit];
            last = cb;
        }
    });
    recorder.join();
    [last waitUntilCompleted];
    EXPECT_EQ(last.status, MTLCommandBufferStatusCompleted) << last.error.description.UTF8String;
    EXPECT_TRUE([facts(device, depth) containsString:@"comptags=1"]) << facts(device, depth).UTF8String;
}

// Nothing asked of the compression thread: the block runs at once, on the caller's thread, before the call returns
TEST(GM20BFacadeCompressedMove, NothingAskedSettlesAtOnceOnTheCallersThread)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    ASSERT_EQ(harness::compressionSettled(device), 1);
    __block bool ran = false;
    NSString *refused = ((NSString * (*)(id, SEL, id))
                             objc_msgSend)((id)device, sel_registerName("switchMetalProbeCompressionSettled:"), ^{
        ran = true;
    });
    EXPECT_EQ(refused, nil);
    EXPECT_TRUE(ran) << "nothing was asked, yet the block did not run before the call returned";
}

}  // namespace

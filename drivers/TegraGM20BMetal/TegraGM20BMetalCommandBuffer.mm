// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPassCache.h"
#include "object_pool.h"
#include "parallel.h"
#include "compute.h"
#include "scheduled_wait.h"
#include "take_pool.h"
#import "TegraGM20BMetalPipeline.h"
#import "TegraGM20BMetalCounters.h"
#include "counter_kernel.h"

// ---- the device's submission machinery ----

// A finished command buffer's recording storage, kept for the next one (its capacity a frame's): Core Animation's steady frames
// allocate none of it. As many are kept as were live at once (an app that commits a batch before it waits records each into
// kept storage), up to kKeptTotal bytes and kKeptMost recordings; gRawRelease gives back all but kKeptIdle. TakePool
// transfers exclusive ownership: a command buffer takes one where it is made and returns it after completion, or at
// dealloc if it was never submitted. Public completion results stay with the command buffer.
struct SwitchMetalRecording {
    SwitchMetalCommandPlan plan;
    gm20b::GrowList<SwitchMetalPass> passes;
    SwitchMetalBytes bytes;
    std::deque<gm20b::RetirementWrites> retirements;
    gm20b::GrowList<uint8_t> shadow;
    gm20b::GrowList<uint64_t> extra;
    std::vector<id> owners;
    gm20b::PointerMap<bool> ownerKeys;
    SwitchMetalPassCachePtr passCache;
    size_t kept = 0;
};

static constexpr size_t kKeptTotal = 16 << 20, kKeptIdle = 4;
static constexpr uint32_t kKeptMost = 64;
static gm20b::TakePool<SwitchMetalRecording, kKeptMost> &gRecordings =
    *new gm20b::TakePool<SwitchMetalRecording, kKeptMost>;
static std::atomic<size_t> gRecordingBytes{0};

static void swapRecording(SwitchMetalCommandBuffer *b, SwitchMetalRecording &r)
{
    std::swap(b->_plan, r.plan);
    b->_passes.swap(r.passes);
    std::swap(b->_bytes, r.bytes);
    b->_retirements.swap(r.retirements);
    b->_shadow.swap(r.shadow);
    b->_extra.swap(r.extra);
    b->_owners.swap(r.owners);
    std::swap(b->_ownerKeys, r.ownerKeys);
    b->_passCache.swap(r.passCache);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static std::atomic<uint64_t> gRecordingMisses{0}, gRecordingDropsLarge{0}, gRecordingDropsTotal{0},
    gRecordingDropsSlots{0};
#define SWITCH_METAL_RECORDING_COUNT(counter) (counter).fetch_add(1, std::memory_order_relaxed)

SwitchMetalRecordingCounts gm20bRecordingCounts()
{
    return {gRecordingMisses.load(std::memory_order_relaxed), gRecordingDropsLarge.load(std::memory_order_relaxed),
            gRecordingDropsTotal.load(std::memory_order_relaxed), gRecordingDropsSlots.load(std::memory_order_relaxed)};
}
#else
#define SWITCH_METAL_RECORDING_COUNT(counter)
#endif

void gm20bTakeRecording(SwitchMetalCommandBuffer *b)
{
    uint32_t node;
    SwitchMetalRecording *r = gRecordings.take(node);
    if (!r) {
        SWITCH_METAL_RECORDING_COUNT(gRecordingMisses);
        b->_owners.reserve(256);
        b->_passCache = gm20bNewPassCache();
        return;
    }
    swapRecording(b, *r);
    gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
    r->kept = 0;
    gRecordings.giveBack(node);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// facadecost's: setBytes chunks made for a command buffer the pool had none for, and made ahead, and the bytes of every
// setBytes chunk alive (kept, in a recording or in flight)
static std::atomic<uint64_t> gChunkMisses{0}, gChunksMadeAhead{0}, gChunkBytesLive{0};
#define SWITCH_METAL_CHUNK_COUNT(counter, value) counter.fetch_add(uint64_t(value), std::memory_order_relaxed)
#else
#define SWITCH_METAL_CHUNK_COUNT(counter, value)
#endif

static void dropBytes(SwitchMetalBytes &s)
{
    for (gm20b::Buffer &k : s.chunks)
        if (k.va) {
            SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, -k.length);
            gRM->raw->destroyBuffer(k);
        } else
            free(k.cpu);
    s = {};
}

// A recording that outgrew its first chunk keeps one chunk of their total: the next command buffer of its shape names one
// chunk to its job, not one for each 64 KB of its setBytes
static void coalesceBytes(SwitchMetalBytes &s)
{
    if (s.chunks.size() < 2)
        return;
    uint64_t length = 0;
    for (const gm20b::Buffer &k : s.chunks) {
        if (!k.va)
            return;
        length += k.length;
    }
    gm20b::Buffer one;
    if (!gRM->raw->createControlBuffer(length, one))
        return;
    SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, one.length);
    dropBytes(s);
    s.chunks.push_back(one);
}

// Finished command buffers' setBytes chunks for the next to fill, given back at completion rather than with the recording
// at dealloc: command buffers an app keeps after they complete hold none of the GPU pool's control memory. As many are kept
// as were in flight at once, up to kChunksKept and kChunkBytesKept bytes, each up to kChunkMost (a command buffer of more
// setBytes frees its own, as its recording); gRawRelease gives back all but kChunksIdle.
static constexpr uint32_t kChunksKept = 64, kChunksIdle = 4;
static constexpr uint64_t kChunkMost = 1 << 20, kChunkBytesKept = 16 << 20;
static gm20b::TakePool<gm20b::Buffer, kChunksKept> &gBytesChunks = *new gm20b::TakePool<gm20b::Buffer, kChunksKept>;
static std::atomic<uint64_t> gBytesChunkBytes{0};

static std::atomic<bool> gBytesGrowing{false};

// The length of the chunk taken last, up to kChunkMost: what a chunk made for the pool, or for a command buffer it had none
// for, is made at. A recording that coalesced past 64 KB (coalesceBytes) given a 64 KB one outgrows it and coalesces
// again at its completion, making and freeing control buffers on the completion thread.
static std::atomic<uint64_t> gBytesChunkLength{kRMBytesChunk};

static void keepBytesChunk(const gm20b::Buffer &chunk)
{
    uint32_t node;
    const bool fits =
        chunk.va && chunk.length <= kChunkMost &&
        gBytesChunkBytes.fetch_add(chunk.length, std::memory_order_relaxed) + chunk.length <= kChunkBytesKept;
    gm20b::Buffer *slot = fits ? gBytesChunks.room(node) : nullptr;
    if (!slot) {
        if (chunk.va && chunk.length <= kChunkMost)
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
        SwitchMetalBytes one;
        one.chunks.push_back(chunk);
        dropBytes(one);
        return;
    }
    *slot = chunk;
    gBytesChunks.keep(node);
}

static void growBytesChunks(void *)
{
    gm20b::Buffer chunk;
    if (gRM->raw->createControlBuffer(gBytesChunkLength.load(std::memory_order_relaxed), chunk)) {
        SWITCH_METAL_CHUNK_COUNT(gChunksMadeAhead, 1);
        SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, chunk.length);
        keepBytesChunk(chunk);
    }
    gBytesGrowing.store(false, std::memory_order_release);
}

// The pool is grown by a chunk, on the utility queue, when a completion that gave chunks back leaves it at its last chunk
// or none: the completion side decides, so a recording thread only ever takes a chunk (or, finding none, makes its own: a
// miss)
static void growBytesChunksAhead()
{
    if (gBytesChunks.kept() <= 1 && !gBytesGrowing.exchange(true, std::memory_order_acq_rel))
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), nullptr, growBytesChunks);
}

void gm20bBytesDone(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalBytes &s = cb->_bytes;
    const bool gaveBack = !s.chunks.empty();
    coalesceBytes(s);
    for (const gm20b::Buffer &k : s.chunks)
        keepBytesChunk(k);
    s.chunks.clear();  // its capacity goes on with the recording to the next command buffer
    s.at = 0;
    s.used = 0;
    if (gaveBack)
        growBytesChunksAhead();
}

bool gm20bTakeBytesChunk(uint64_t length, gm20b::Buffer &chunk)
{
    uint32_t node;
    if (gm20b::Buffer *kept = gBytesChunks.take(node)) {
        if (kept->length >= length) {
            chunk = *kept;
            *kept = {};
            gBytesChunks.giveBack(node);
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
            gBytesChunkLength.store(std::min(chunk.length, kChunkMost), std::memory_order_relaxed);
            return true;
        }
        gBytesChunks.keep(node);
    }
    if (!gRM->raw->createControlBuffer(std::max(length, gBytesChunkLength.load(std::memory_order_relaxed)), chunk))
        return false;
    SWITCH_METAL_CHUNK_COUNT(gChunkMisses, 1);
    SWITCH_METAL_CHUNK_COUNT(gChunkBytesLive, chunk.length);
    return true;
}

void gm20bRecordingDone(SwitchMetalCommandBuffer *b)
{
    if (b->_recordingReturned)
        return;
    b->_recordingReturned = true;
    gm20bPassCacheUnbind(b->_passCache.get());
    constexpr size_t kKeptBytes = 1 << 20;  // (a command buffer of a few megabytes of setBytes: its own, freed)
    uint64_t bytes = 0;
    for (const gm20b::Buffer &k : b->_bytes.chunks)
        bytes += k.va ? k.length : kKeptBytes;  // (heap chunks are not kept)
    bytes += b->_plan.storageBytes() + b->_extra.capacity() * 8 + b->_shadow.capacity() +
             gm20bPassCacheBytes(b->_passCache.get());
    if (bytes > kKeptBytes) {
        SWITCH_METAL_RECORDING_COUNT(gRecordingDropsLarge);
        SwitchMetalRecording dropped;
        swapRecording(b, dropped);
        dropBytes(dropped.bytes);
        return;
    }
    uint32_t node;
    const bool fits = gRecordingBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes <= kKeptTotal;
    SwitchMetalRecording *r = fits ? gRecordings.room(node) : nullptr;
    if (!r) {
        SWITCH_METAL_RECORDING_COUNT(*(fits ? &gRecordingDropsSlots : &gRecordingDropsTotal));
        gRecordingBytes.fetch_sub(bytes, std::memory_order_relaxed);
        SwitchMetalRecording dropped;
        swapRecording(b, dropped);
        dropBytes(dropped.bytes);
        return;
    }
    swapRecording(b, *r);
    r->kept = bytes;
    r->plan.clear();
    r->passes.clear();
    r->bytes.at = 0;
    r->bytes.used = 0;
    r->shadow.clear();
    r->extra.clear();
    r->owners.clear();
    r->ownerKeys.clear();
    coalesceBytes(r->bytes);
    gRecordings.keep(node);
}

// `count` of each kept recording's pass bank, owned while it is read and kept again in the order it had: the next command
// buffer takes the recording returned last
template <typename Count> static SwitchMetalPassTrim sumKeptBanks(Count &&count)
{
    SwitchMetalPassTrim total{};
    uint32_t nodes[kKeptMost], taken = 0;
    for (; taken < kKeptMost; taken++) {
        SwitchMetalRecording *r = gRecordings.take(nodes[taken]);
        if (!r)
            break;
        const SwitchMetalPassTrim part = count(r->passCache.get());
        total.passes += part.passes;
        total.textures += part.textures;
    }
    while (taken)
        gRecordings.keep(nodes[--taken]);
    return total;
}

SwitchMetalPassTrim gm20bPassTrim()
{
    return sumKeptBanks(gm20bPassCacheTrim);
}

SwitchMetalPassTrim gm20bPassHeld()
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return sumKeptBanks(gm20bPassCacheHeld);
#else
    return {};
#endif
}

void releaseRecordings()
{
    while (gBytesChunks.kept() > kChunksIdle) {
        uint32_t node;
        gm20b::Buffer *kept = gBytesChunks.take(node);
        if (!kept)
            break;
        gBytesChunkBytes.fetch_sub(kept->length, std::memory_order_relaxed);
        SwitchMetalBytes one;
        one.chunks.push_back(*kept);
        dropBytes(one);
        *kept = {};
        gBytesChunks.giveBack(node);
    }
    while (gRecordings.kept() > kKeptIdle) {
        uint32_t node;
        SwitchMetalRecording *r = gRecordings.take(node);
        if (!r)
            break;
        gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
        dropBytes(r->bytes);
        *r = SwitchMetalRecording();
        gRecordings.giveBack(node);
    }
}

void gm20bChanged(SwitchMetalCommandBuffer *cb)
{
    cb->_changes.changed();
}

NSArray *gm20bTakeHandlers(SwitchMetalCommandBuffer *cb, NSMutableArray *__strong &list, std::atomic<bool> &added)
{
    if (!added.load())
        return nil;
    std::lock_guard<gm20b::Mutex> l(cb->_lock);
    NSArray *taken = list;
    list = nil;
    return taken;
}

// Decided where its scheduled handlers are sent, not where they run: one the GPU fails after its submission still
// presents (the kernel ends that present), one that failed before presents nothing (TegraGM20BMetalFinish.mm withholdPresents)
static bool presents(id<MTLCommandBuffer> cb)
{
    return ((SwitchMetalCommandBuffer *)cb)->_presents;
}

template <typename Done> static void waitFor(SwitchMetalCommandBuffer *cb, Done &&done)
{
    cb->_changes.waitFor(done);
}

// On the thread that let go of the last reference, which may not be the one that completed it. The completion's last
// write is its turn's mark (completeBuffer: its recording given back before), and its reference went after it; read
// with acquire before anything here, so this dealloc and whoever takes the block from the pool next come after that
// write through the command buffer's own atomic, not only through libobjc's reference count
static void commandBufferDropped(void *object)
{
    SwitchMetalCommandBuffer *__unsafe_unretained b = (__bridge SwitchMetalCommandBuffer *)object;
    const bool completed = b->_turn.ran();
    if (SwitchMetalEncoder *__unsafe_unretained e = (SwitchMetalEncoder *)b->_active)
        e->_cb = nil;
    b->_active = nil;
    if (!completed)
        gm20bRecordingDone(b);
}

// Command-buffer objects use the same instance pool as encoders (object_pool.h). Dealloc returns an unsubmitted
// recording before pooling its object. Sub-encoders (SwitchMetalSubRecorder) use NSObject allocation and return their recordings
// there too.
void gm20bPoolCommandBuffers()
{
    if (!gm20bPoolInstances([SwitchMetalCommandBuffer class], commandBufferDropped))
        os_log_error(gm20bLog(), "SwitchMetal: command buffers are not pooled");
}

@implementation SwitchMetalCommandBuffer

- (void)retain:(__unsafe_unretained id)object
{
    if (!object || !_ownerKeys.insert((__bridge void *)object, true).second)
        return;
    _owners.push_back(object);
    SwitchMetalResourceFacts facts;
    if ([object isKindOfClass:[SwitchMetalTexture class]]) {
        facts.kind = SwitchMetalResourceKind::Texture;
        __unsafe_unretained SwitchMetalTexture *texture = (SwitchMetalTexture *)object;
        facts.roles = uint8_t(texture->_raw.converted() ? SwitchMetalResourceFacts::Converted : 0);
        __unsafe_unretained SwitchMetalTexture *root = texture->_levelsOwner ?: texture;
        facts.surfaceWrites = root->_surfaceWrites;
        facts.cpuBuffer = (__bridge void *)root->_buffer;
    } else if ([object isKindOfClass:[SwitchMetalBuffer class]]) {
        facts.kind = SwitchMetalResourceKind::Buffer;
        facts.cpuBuffer = (__bridge void *)object;
    } else if ([object isKindOfClass:[SwitchMetalSampler class]])
        facts.kind = SwitchMetalResourceKind::Sampler;
    else if ([object isKindOfClass:[SwitchMetalHeap class]])
        facts.kind = SwitchMetalResourceKind::Heap;
    _plan.registerResource((__bridge void *)object, facts);
}

// A binding's state variant still being made (OpPipeline's obj2): an encode ahead would draw its draws with no state, as
// RenderEncoder::draw drops a draw without one; the commit's submission waits for it (TegraGM20BMetalSchedule.mm variantReadiness)
static bool bindsPendingVariant(const Cmd &binding)
{
    const auto *variant = static_cast<const gm20b::Variant<gm20b::Pipeline> *>(binding.obj2);
    return variant && variant->state() == gm20b::Variant<gm20b::Pipeline>::State::Pending;
}

// The raw state a draw of this raster state runs
static const gm20b::Pipeline *recordedPipeline(const gm20b::RasterState &raster)
{
    if (const gm20b::Pipeline *native = static_cast<const gm20b::Pipeline *>(raster.nativePipeline))
        return native;
    __unsafe_unretained SwitchMetalPipeline *pipeline = (__bridge SwitchMetalPipeline *)raster.pipeline;
    return [(id)pipeline isKindOfClass:[SwitchMetalPipeline class]] ? pipeline->_raw : nullptr;
}

- (void)record:(const Cmd &)cmd
{
    if (_commandLog) {
        SwitchMetalSubCommand s{cmd, {}};
        if (const size_t words = gm20b::extraWords(cmd))
            s.extra.assign(_extra.ownData() + cmd.x, _extra.ownData() + cmd.x + words);
        _commandLog->push_back(s);
    }
    if (cmd.op == OpPipeline && bindsPendingVariant(cmd))
        gm20bPipelineVeto(self, VetoVariant);
    if (cmd.op == OpCompute)
        _predicates.computeEncoder(cmd.a);
    else if (cmd.op == OpRender || cmd.op == OpBlit || cmd.op == OpSignalEvent || cmd.op == OpWaitEvent)
        _predicates.otherWork();
    const bool draws = isPlanDraw(cmd.op), captures = draws && _plan.capturesRender();
    const gm20b::Pipeline *native = captures ? recordedPipeline(_plan.raster.current()) : nullptr;
    if (_plan.record(cmd, _extra.ownData(), _extra.size(), native && native->rec)) {
        std::vector<gm20b::ResourceUse> &uses = _plan.recordUses();
        if (draws) {
            const uint32_t index = _plan.ownDraw(_plan.size() - 1).state;
            const auto &state = _plan.renderStates.own(index);
            if (!state.usesFinal) {
                if (!captures)
                    native = recordedPipeline(_plan.raster.own(state.raster));
                if (native && native->rec) {
                    _plan.resolveRenderUses(index, native, [&](std::vector<gm20b::ResourceUse> &bound) {
                        const auto &record = *native->rec;
                        const gm20b::SetEntries &entries = native->entries;
                        for (uint32_t i = 0; i < entries.size(); ++i) {
                            const auto &entry = entries[i];
                            if (entry.kind == gm20b::EntryUbo && entry.stage >= 1 && entry.stage <= 2 &&
                                entry.slot < 31) {
                                const auto &binding =
                                    _plan.buffers[entry.stage - 1][state.buffers[entry.stage - 1]][entry.slot];
                                if (binding.source == gm20b::BufferBinding::Source::Buffer)
                                    bound.push_back(
                                        {binding.object,
                                         uint8_t(gm20b::ResourceUse::Read |
                                                 (gm20b::bufferStorage(entry) ? gm20b::ResourceUse::Write : 0))});
                            } else if ((entry.kind == gm20b::EntryTexture || entry.kind == gm20b::EntryTexture3d) &&
                                       entry.slot < gm20b::kStageTextures) {
                                const uint32_t at = gm20b::handleStage(entry);
                                const bool written = entry.stage == 2 && entry.slot < gm20b::kWrittenSlots &&
                                                     (native->writtenSlots >> entry.slot & 1);
                                bound.push_back({_plan.textures[at][state.textures[at]].objects[entry.slot],
                                                 uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample |
                                                         (written ? gm20b::ResourceUse::Write : 0))});
                            }
                            if ((entry.kind == gm20b::EntryTexture || entry.kind == gm20b::EntryTexture3d ||
                                 entry.kind == gm20b::EntrySamplerOnly) &&
                                entry.samplerKind == gm20b::SamplerSlot && entry.sampler < gm20b::kStageSamplers) {
                                const uint32_t at = gm20b::handleStage(entry);
                                bound.push_back({_plan.samplers[at][state.samplers[at]].objects[entry.sampler],
                                                 gm20b::ResourceUse::Read});
                            }
                        }
                        if (record.vertexSlot < 31 && record.vertexStride) {
                            const auto &binding = _plan.buffers[0][state.buffers[0]][record.vertexSlot];
                            if (binding.source == gm20b::BufferBinding::Source::Buffer)
                                bound.push_back({binding.object, gm20b::ResourceUse::Read});
                        }
                        // the buffer slots a stage reaches through its root table, which may be written there too
                        for (uint32_t stage = 0; stage < 2; ++stage)
                            for (uint32_t slots = record.rootSlots[stage]; slots; slots &= slots - 1) {
                                const auto &binding = _plan.buffers[stage][state.buffers[stage]][__builtin_ctz(slots)];
                                if (binding.source == gm20b::BufferBinding::Source::Buffer)
                                    bound.push_back({binding.object,
                                                     uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Write)});
                            }
                    });
                }
            }
        } else if (cmd.op == OpDispatch && cmd.obj2) {
            admitComputeDispatch(self, _plan.size() - 1,
                                 static_cast<const gm20b::ComputeKernelSet<gm20b::ComputeState> *>(cmd.obj2), nullptr);
        } else if (cmd.op == OpRender) {
            _recordedPass = uint32_t(cmd.x);
            const SwitchMetalPass &pass = _passes.own(cmd.x);
            for (const SwitchMetalPass::Color &a : pass.color)
                if (a.texture)
                    uses.push_back({(__bridge void *)a.texture,
                                    uint8_t(gm20b::ResourceUse::Write |
                                            (a.load == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
            if (pass.depth)
                uses.push_back({(__bridge void *)pass.depth,
                                uint8_t(gm20b::ResourceUse::Write |
                                        (pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad
                                             ? gm20b::ResourceUse::Read
                                             : 0))});
            if (id stencil = pass.zeta[1].texture; stencil && stencil != pass.depth)
                uses.push_back({(__bridge void *)stencil,
                                uint8_t(gm20b::ResourceUse::Write |
                                        (pass.stencilLoad == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
            for (const SwitchMetalPass::Color &a : pass.color)
                if (a.resolve)
                    uses.push_back({(__bridge void *)a.resolve, gm20b::ResourceUse::Write});
            if (pass.depthResolve)
                uses.push_back({(__bridge void *)pass.depthResolve, gm20b::ResourceUse::Write});
            if (pass.stencilResolve)
                uses.push_back({(__bridge void *)pass.stencilResolve, gm20b::ResourceUse::Write});
        } else if (cmd.op == OpVisibility && cmd.obj) {
            uses.push_back({cmd.obj, gm20b::ResourceUse::Write});
        } else if (cmd.op == OpPredicate && cmd.obj) {
            uses.push_back({cmd.obj, gm20b::ResourceUse::Read});
        }
        if (!uses.empty())
            _plan.addUses(_plan.size() - 1, uses);
        if (cmd.op == OpEnd && _encOpen) {
            _plan.prepareRange(_encAt, _plan.size());
            [self closeEncoder];
            gm20bPublishEncoder(self);
        }
    }
    const size_t consumed = gm20b::extraWords(cmd);
    if (consumed && cmd.x + consumed == _extra.size())
        _extra.resize(cmd.x);
}

- (void)recordBinding:(const Cmd &)cmd
{
    if (_commandLog)
        _commandLog->push_back({cmd, {}});
    _plan.bind(cmd, _extra.ownData());
}

// The encoder recorded last is done: an OpEnd recorded after this publishes nothing
- (void)closeEncoder
{
    _encOpen = false;
}

- (NSMutableDictionary *)userDictionary
{
    std::lock_guard<gm20b::Mutex> l(_lock);
    if (!_userDictionary)
        _userDictionary = [NSMutableDictionary dictionary];
    return _userDictionary;
}

- (uint64_t)globalTraceObjectID
{
    std::lock_guard<gm20b::Mutex> l(_lock);
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

// Every allocation is mapped in the one GPU VM and stays resident: a residency set asks nothing of the driver
- (void)useResidencySet:(id)set
{
    (void)set;
}

// (QuartzCore 27, per command buffer: the kernel charges GPU time to the task that submits it, this one)
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    (void)sets;
    (void)count;
}

- (MTLCommandBufferStatus)status
{
    return _status.load();
}

- (NSError *)error
{
    return _error;
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}

- (id<MTLLogContainer>)logs
{
    return nil;
}

- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}

- (CFTimeInterval)kernelStartTime
{
    return _kernelStart;
}

- (CFTimeInterval)kernelEndTime
{
    return _kernelEnd;
}

// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}

- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

// Before its commit a command buffer's handler lists are its recording thread's alone (Metal takes handlers before the
// commit), published to the submit worker by the commit's hand-off with no lock; one added after the commit takes the lock
// its consumer takes them under
- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    if (!_committed.load(std::memory_order_relaxed)) {
        if (!_scheduledHandlers)
            _scheduledHandlers = [NSMutableArray array];
        [_scheduledHandlers addObject:[block copy]];
        _hasScheduledHandlers.store(true, std::memory_order_relaxed);
        return;
    }
    _hasScheduledHandlers.store(true);
    {
        std::lock_guard<gm20b::Mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        if (!_scheduledSent) {
            if (!_scheduledHandlers)
                _scheduledHandlers = [NSMutableArray array];
            [_scheduledHandlers addObject:[block copy]];
        } else {
            _handlersQueued = true;
            dispatch_async(_queue->_handlers, ^{
                block((id<MTLCommandBuffer>)self);
            });
        }
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    if (!_committed.load(std::memory_order_relaxed)) {
        if (!_completedHandlers)
            _completedHandlers = [NSMutableArray array];
        [_completedHandlers addObject:[block copy]];
        _hasCompletedHandlers.store(true, std::memory_order_relaxed);
        return;
    }
    bool now;
    _hasCompletedHandlers.store(true);
    {
        std::lock_guard<gm20b::Mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now) {
            if (!_completedHandlers)
                _completedHandlers = [NSMutableArray array];
            [_completedHandlers addObject:[block copy]];
        }
    }
    if (now)
        block((id<MTLCommandBuffer>)self);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
        if (presents(cb))
            [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
        if (presents(cb))
            [drawable presentAtTime:t];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
        if (presents(cb))
            [drawable presentAfterMinimumDuration:d];
    }];
}

- (void)enqueue
{
    enqueueBuffer(self);
}

- (void)commit
{
    [self enqueue];
    if (_committed.exchange(true))
        return;
    _status = MTLCommandBufferStatusCommitted;
    _kernelStart = hostSeconds();
#if SWITCH_METAL_DIAGNOSTICS
    _committedTicks = mach_absolute_time();
#endif
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const gm20b::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(SwitchMetalEncoder *)_active endEncoding];
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    glassProbeCommitted(self);
#endif
    [(SwitchMetalDevice *)_device gm20bCommit:self];
#if SWITCH_FRAME_TIMELINE
    _frame.mark(switch_frame::Back);
#endif
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}

- (void)waitUntilScheduled
{
    // A failed one returns once finish() withdrew the writes it will never make (TegraGM20BMetalSchedule.mm cancelPromises), so
    // a present its caller makes next of a surface it was to write is refused, never shown with the frame before
    auto returns = [&] {
        return gm20b::scheduledWaitReturns(_scheduledDone,
                                           _segError.set() && _promised.load(std::memory_order_acquire));
    };
    if (_committed && !returns()) {
#if SWITCH_METAL_DIAGNOSTICS
        const BlockedWait blocked(gRM->stats.scheduledWaits, gRM->stats.scheduledWaitUs);
#endif
        waitFor(self, returns);
    }
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t none = 0;
    if (_held && !_scheduledDone && _heldReturnUs.compare_exchange_strong(none, nowUs()))
        gRM->stats.heldReturns++;
#endif
}

- (void)waitUntilCompleted
{
    if (!_committed || _completedDone)
        return;
#if SWITCH_METAL_DIAGNOSTICS
    const BlockedWait blocked(gRM->stats.completedWaits, gRM->stats.completedWaitUs);
#endif
    // a range of it in flight may have ended quiet: the completion thread waits for the newest one
    gRM->completionWaiters.fetch_add(1, std::memory_order_seq_cst);
    gRM->inflight.wake();
    waitFor(self, [&] {
        return _completedDone.load();
    });
    gRM->completionWaiters.fetch_sub(1, std::memory_order_relaxed);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
- (NSNumber *)switchMetalWholePrograms
{
    std::lock_guard<gm20b::Mutex> lock(_lock);
    if (_committed)
        return @NO;
    gm20bPipelineVeto(self, VetoProbes);
    _forceWholePrograms = true;
    return @YES;
}

// hw_test_app's gpu.path.state_segments: this command buffer binds every state as an inline copy (gpu.h inlineStates)
- (NSNumber *)switchMetalInlineStates
{
    std::lock_guard<gm20b::Mutex> lock(_lock);
    if (_committed)
        return @NO;
    gm20bPipelineVeto(self, VetoProbes);
    _inlineStates = true;
    return @YES;
}

// hw_test_app's clock of GPUStartTime/GPUEndTime, final once the command buffer completed: "ns" (its submission's GPU
// stamps) or "host" (the host's window from submission to completion, which stands in where no stamp was read)
- (NSString *)switchGpuClock
{
    if (!_completedSent)
        return @"pending";
    return _gpuStamped ? @"ns" : @"host";
}

// hw_test_app's attribution, final once the command buffer completed (its completed handlers may ask): "gm20b", or for
// one that completed with an error "error:<predicate>", the predicate its failure named, "error:none" where it named
// none (the failed.* key switchMetalCounters counted it under). With no lock: what it reads is set before the completed
// mark this acquires.
- (NSString *)switchMetalPath
{
    if (!_completedSent)
        return @"pending";
    if (!_error)
        return @"gm20b";
    return [NSString stringWithFormat:@"error:%s", failedName(SwitchMetalPred(_failedBy))];
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// Host tests' wait for the submit worker, before its commit: returns once the worker encoded ahead everything it
// published so far, or gave it back (answering the reasons given back, 0 where none). The worker reaches either once it
// was handed the command buffer and no veto stands; a recorded veto it reads only at its next step.
- (NSNumber *)switchMetalWaitAhead
{
    const auto done = [&] {
        return _pipelineDeclined.load(std::memory_order_acquire) ||
               _aheadAt.load(std::memory_order_acquire) >= size_t(_published.load(std::memory_order_acquire) >> 32);
    };
    waitFor(self, done);
    return @(_pipelineDeclined.load(std::memory_order_acquire) ? _pipelineGivenBack : 0u);
}
#endif

// hw_test_app's gpu.path.commit_latency, final once it completed: of its raw ranges' encoders, those the worker encoded
// ahead of its commit; the encodes ahead of it the worker gave back and why (given_back), the reasons the recording gave
// (veto, both SwitchMetalPipelineVeto); and, while its stamps' slot still holds them (kStampSlots command buffers on), the time
// from its commit to its submission's start on the GPU (its start stamp's PTIMER on the CPU's clock through the
// kernel's anchors)
- (NSDictionary<NSString *, NSNumber *> *)switchMetalPipelined
{
    if (!_completedSent)
        return nil;
    NSMutableDictionary *out = [@{
        @"encoders" : @(_pipelineEncoders),
        @"pipelined" : @(_pipelinedEncoders),
        @"abandons" : @(_pipelineAbandons),
        @"veto" : @(_pipelineVeto.load(std::memory_order_relaxed)),
        @"given_back" : @(_pipelineGivenBack),
    } mutableCopy];
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t units = 0;
    const TegraGM20BAbiClockAnchors anchors = gm20bClockAnchorsNow();
    if (gRM->raw->gpuStartUnits(_stamp, units))
        if (const gm20b::counters::Anchor a = gm20b::counters::anchorOf(anchors, units); a.valid) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            const double committedNs = double(_committedTicks) * tb.numer / tb.denom;
            out[@"commit_to_gpu_start_us"] = @((double(gm20b::counters::nanoseconds(a, units)) - committedNs) / 1000.0);
        }
#endif
    return out;
}

// facadecost's held-chunk check: the GPU addresses of its setBytes chunks, read while its job is held in flight (they go
// back at its completion)
- (NSArray<NSNumber *> *)switchMetalBytesChunks
{
    NSMutableArray<NSNumber *> *vas = [NSMutableArray array];
    for (const gm20b::Buffer &k : _bytes.chunks)
        [vas addObject:@(k.va)];
    return vas;
}

// job_words.h's: the lengths the recording filled those chunks to before taking the next
- (NSArray<NSNumber *> *)switchMetalBytesChunkLengths
{
    NSMutableArray<NSNumber *> *lengths = [NSMutableArray array];
    for (const gm20b::Buffer &k : _bytes.chunks)
        [lengths addObject:@(k.length)];
    return lengths;
}

// facadecost's: the setBytes chunks' misses and those made ahead since the process began, and their live bytes now
+ (NSDictionary<NSString *, NSNumber *> *)switchMetalBytesChunkStats
{
    return @{
        @"misses" : @(gChunkMisses.load(std::memory_order_relaxed)),
        @"made_ahead" : @(gChunksMadeAhead.load(std::memory_order_relaxed)),
        @"live_bytes" : @(gChunkBytesLive.load(std::memory_order_relaxed)),
    };
}
#endif

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

// Every MTLCommandBuffer selector is a method of the command buffer: one that reaches here is no Metal selector, answered zero
// with an error line
- (void)forwardInvocation:(NSInvocation *)inv
{
    SWITCH_REFUSE_DETAIL("gm20b", "call.cbinvocation", uintptr_t(inv.selector),
                         "-[MTLCommandBuffer %s]: no method of this device's command buffer: answered zero",
                         sel_getName(inv.selector));
    answerZero(self, inv);
}

@end

@implementation SwitchMetalQueue

- (instancetype)init
{
    if ((self = [super init])) {
        queuePriorityAdd(self);
        _handlers = followingSerialQueue("com.brunostjohn.switch.gm20b.command.handlers");
    }
    return self;
}

- (void)dealloc
{
    queuePriorityRemove(self);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    SwitchMetalCommandBuffer *b = [SwitchMetalCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_status = MTLCommandBufferStatusNotEnqueued;
    gm20bTakeRecording(b);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

// (as the command buffer's: everything is resident)
- (void)addResidencySet:(id)set
{
    (void)set;
}

- (void)addResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    (void)sets;
    (void)count;
}

- (void)removeResidencySet:(id)set
{
    (void)set;
}

- (void)removeResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    (void)sets;
    (void)count;
}

- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}

- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}

@end

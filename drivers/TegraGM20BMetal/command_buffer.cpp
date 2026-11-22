// SPDX-License-Identifier: MIT
// See gpu.h (the channel's fences, kills and reopen; CommandBuffer begin to commit).
#include "lock.h"
#include "event_release.h"
#include "gpu.h"
#include "TegraGM20BJobABI.h"
#include "slm.h"
#include "stamps.h"
#include "native_surface.h"
#include "tiled_cache.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// ---- command buffer ----

// A wait the kernel refuses with no kill (the call failed, or the submit thread's reopen destroyed the channel meanwhile) is
// no completion: the channel's status decides. A healthy channel is waited on again; one that keeps refusing is taken as
// killed, with everything it has not completed, so nothing it may still run is released or written over as done.
bool Device::waitFence(uint32_t seqno, uint64_t generation)
{
    constexpr unsigned kRefusals = 3;
    const uint32_t ch = channel_, opened = k_.opened();
    if (done(seqno) && semanticFence(seqno).status == SemanticFenceStatus::Superseded) {
        int result = KERN_SUCCESS;
        if (k_.wait(ch, seqno, ~0ull, &result))
            return !lost(seqno);
        if (result == kIOReturnAborted) {
            keepFailure(seqno);
            return false;
        }
    }
    for (unsigned refused = 0; !done(seqno);) {
        if (channelGeneration() != generation)
            return false;
        if (k_.wait(ch, seqno))
            break;
        if (channelGeneration() != generation)
            return false;
        TegraGM20BAbiStatus st;
        if (k_.lost(ch) || !k_.status(ch, st)) {  // killed, or destroyed by a reopen that kept its kill record
            break;
        }
        if (st.error) {
            k_.markLost(ch, opened);
            break;
        }
        if (done(seqno))
            return !lost(seqno);
        if (++refused == kRefusals) {
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                         "SwitchMetal: the kernel refused %u waits on channel %u's seqno %u (%s): taken as killed",
                         refused, ch, seqno, k_.error().c_str());
            {
                std::lock_guard<gm20b::Mutex> l(killedLock_);
                killedKept_[killedNext_++ % killedKept_.size()] = {st.completed + 1, lastSubmitted(), kWaitRefused};
                keptThrough_.store(lastSubmitted(), std::memory_order_relaxed);
                keptKills_.store(true, std::memory_order_release);
            }
            k_.markLost(ch, opened);  // begin() makes it again
            return false;
        }
    }
    return !lost(seqno);
}

// A job the kernel says its own event wait failed, which the fence record no longer names
void Device::keepFailure(uint32_t seqno)
{
    std::lock_guard<gm20b::Mutex> l(killedLock_);
    killedKept_[killedNext_++ % killedKept_.size()] = {seqno, seqno, kDependencyFailed};
    const uint32_t through = keptThrough_.load(std::memory_order_relaxed);
    if (!keptKills_.load(std::memory_order_relaxed) || int32_t(seqno - through) > 0)
        keptThrough_.store(seqno, std::memory_order_relaxed);
    keptKills_.store(true, std::memory_order_release);
}

// What the live channel failed, for its retirement: the kept records (its kill record among them) and the newest job its
// fence record names failed
std::vector<KillRange> Device::failedSeqnos() const
{
    std::vector<KillRange> failed;
    {
        std::lock_guard<gm20b::Mutex> l(killedLock_);
        for (const Killed &kept : killedKept_)
            if (kept.error)
                failed.push_back(kept);
    }
    Killed live = {};
    if (!k_.killRecords())  // nothing tells the seqnos the channel finished from those it took with it
        failed.push_back({1, lastSubmitted(), kWaitRefused});
    else if (k_.lost(channel_) && !liveKill(live))  // the kernel publishes the error before the record and the fence
        failed.push_back({__atomic_load_n(fence_, __ATOMIC_ACQUIRE) + 1, lastSubmitted(),
                          k_.lostError() ? k_.lostError() : kWaitRefused});
    const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_);
    if (const uint32_t seqno =
            newestSemanticFailure(reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(record + 1)))
        failed.push_back({seqno, seqno, kDependencyFailed});
    return failed;
}

// A kept record names the seqnos of the slot it was written in, which a channel made in another slot gives out again
void Device::forgetKept()
{
    std::lock_guard<gm20b::Mutex> l(killedLock_);
    killedKept_ = {};
    keptThrough_.store(0, std::memory_order_relaxed);
    keptKills_.store(false, std::memory_order_release);
}

// The barriers order the record after the caller's read of the fence word, and its range after its error: the kernel
// writes the range, the error, then the fence word.
bool Device::liveKill(Killed &k) const
{
    const volatile uint32_t *fence = fence_;
    if (fence == &kNoFence)
        return false;
    if (k_.killRecords()) {
        const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        std::atomic_thread_fence(std::memory_order_acquire);
        k.error = record->error;
        if (k.error) {
            std::atomic_thread_fence(std::memory_order_acquire);
            k.first = record->killed_first;
            k.last = record->killed_last;
            return true;
        }
    }
    return false;
}

bool Device::killed(uint32_t seqno) const
{
    const auto covers = [seqno](const Killed &k) {
        return k.error && int32_t(seqno - k.first) >= 0 && int32_t(k.last - seqno) >= 0;
    };
    Killed live = {};
    if (liveKill(live) && covers(live)) {
        killedError_.store(live.error, std::memory_order_relaxed);
        return true;
    }
    if (semanticFence(seqno).status == SemanticFenceStatus::Failed) {
        killedError_.store(kDependencyFailed, std::memory_order_relaxed);
        return true;
    }
    if (!keptKills_.load(std::memory_order_acquire) ||
        int32_t(seqno - keptThrough_.load(std::memory_order_relaxed)) > 0)
        return false;
    std::lock_guard<gm20b::Mutex> l(killedLock_);
    for (const Killed &kept : killedKept_)
        if (covers(kept)) {
            killedError_.store(kept.error, std::memory_order_relaxed);
            return true;
        }
    return false;
}

// The kernel killed the channel (a GPU fault or hang, its own or one it was stuck behind; as a GPU restart on iOS): what it
// had in flight failed, and a new channel, named by ChannelCreate's reply, gets the context the old one took with it.
// Between command buffers: no span of either ring is open, and every one the old channel had is done (the kernel released
// its fence), whatever word the new channel's fence is.
std::string Device::vaOwner(uint64_t va)
{
    char line[96];
    auto in = [&](const Bo &bo, uint64_t bytes, const char *what) {
        const uint64_t base = bo.vaBase ? bo.vaBase : bo.va;
        if (!base || va < base || va - base >= bytes)
            return false;
        snprintf(line, sizeof line, "%s +0x%llx", what, (unsigned long long)(va - base));
        return true;
    };
    auto chunk = [&](const std::vector<Chunk> &chunks, const char *what) {
        for (size_t i = 0; i < chunks.size(); i++) {
            if (chunks[i].bo.handle && in(chunks[i].bo, chunks[i].bo.bytes, what))
                return true;
            if (const uint64_t view = chunks[i].pitchVa; view && va >= view && va - view < chunks[i].bytes) {
                snprintf(line, sizeof line, "%s's kind-0 view +0x%llx", what, (unsigned long long)(va - view));
                return true;
            }
        }
        return false;
    };
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (in(heap_, heapBound_, "the shader heap") || in(heap_, heap_.bytes, "the shader heap's unbound reservation") ||
        in(push_.bo, push_.bo.bytes, "the push ring") || in(upload_.bo, upload_.bo.bytes, "the upload ring") ||
        in(root_, root_.bytes, "the root table") || in(tic_, tic_.bytes, "the texture headers") ||
        in(tsc_, tsc_.bytes, "the sampler headers") || in(zero_, zero_.bytes, "the zero page") ||
        in(tickets_, tickets_.bytes, "the interlock tickets") ||
        in(colorTable_, colorTable_.bytes, "the colour table") ||
        in(srgbEncode_, srgbEncode_.bytes, "the sRGB encode table") ||
        in(stamps_, stamps_.bytes, "the GPU time stamps") ||
        in(handleTables_, handleTables_.bytes, "the argument buffers' handle tables") ||
        in(argumentRoots_, argumentRoots_.bytes, "the argument buffers' root table") ||
        std::any_of(heapSets_.begin(), heapSets_.end(),
                    [&](const Bo &bo) {
                        return in(bo, bo.bytes, "an argument buffers' heap set");
                    }) ||
        chunk(chunks_, "a pool chunk") || chunk(blChunks_, "a block-linear chunk") ||
        std::any_of(std::begin(zChunks_), std::end(zChunks_),
                    [&](const std::vector<Chunk> &z) {
                        return chunk(z, "a zeta chunk");
                    }) ||
        chunk(wcChunks_, "a write-combined chunk"))
        return line;
    for (const auto &[key, i] : surfaces_)
        if (in(i->bo, i->bo.bytes, i->kind == TGM_LAYOUT_KIND ? "a block-linear IOSurface" : "a pitch IOSurface"))
            return std::string(line) + " (IOSurface " + std::to_string(i->id) + ")";
    return "none of the device's memory";
}

bool Device::reopen()
{
#if SWITCH_METAL_DIAGNOSTICS
    const uint64_t began = mach_absolute_time();
#endif
    if (channel_) {  // none after a reopen the kernel refused: that channel was retired then
        keptSlot_ = fence_;
        Killed live = {};
        if (liveKill(live)) {
            k_.markLost(channel_, k_.opened());
            std::lock_guard<gm20b::Mutex> l(killedLock_);
            killedKept_[killedNext_++ % killedKept_.size()] = live;
            keptThrough_.store(live.last, std::memory_order_relaxed);
            keptKills_.store(true, std::memory_order_release);
        }
        contextless_ = true;
        const uint64_t retiring = generations_.retire(failedSeqnos());
        // Closed once: the connection is NVK's, whose next channel may take the slot number
        k_.closeChannel(channel_);
        generations_.closed(retiring);
    }
    if (!channelRefusal_.mayAsk(k_))
        return false;
    bool gaveBack = false;
    if (!makeChannel(k_, kEngines, channel_, fence_, lastSeqno_, &kNoFence,
                     [this, &gaveBack](uint32_t channel, uint32_t &seqno) {
                         gaveBack = !newContext(channel, seqno);
                         return !gaveBack;
                     })) {
        channelRefusal_.refused(gaveBack);
        std::lock_guard<gm20b::Mutex> l(channelLock_);
        channelError_ = k_.error();
        return false;
    }
    channelRefusal_.made();
    if (fence_ != keptSlot_)
        forgetKept();
    contextless_ = false;
    SWITCH_METAL_DIAGNOSTIC(stats.reopenTicks = mach_absolute_time() - began);
    return true;
}

// A channel made again: the context the dead one took with it, before anything else runs on it
bool Device::newContext(uint32_t channel, uint32_t &seq)
{
    stats.reopens++;
    push_.inflight.clear();
    upload_.inflight.clear();
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        for (Backing *b : backings_)
            b->submitted = false;
        heapInvalidated_ = ~0ull;
    }
    slmChannel_ = 0;
    memset(rootPush_, 0, sizeof rootPush_);
    restart_ = 0;
    rtLayer_ = ~0u;
    alphaHeld_ = kAlphaControlUnknown;
    sampleMaskHeld_ = kSampleMaskUnknown;
    const uint32_t words = contextWords() + uint32_t(fx::twodInitWords());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, words * 4, 256, va));
    Push p{start, start + words};
    emitContext(p);
    // The interlock's tickets start at zero with the new context's dispensers (a fresh context: its dispenser values are
    // zero already). Warps of the dead channel may have held tickets, and their lines may sit dirty in the GPU's L2, so the
    // copy engine zeroes the buffer in the new channel's own order, done before the 3D engine runs again.
    p.inc(CE_OFFSET_IN_UPPER,
          {uint32_t(zero_.va >> 32), uint32_t(zero_.va), uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)},
          SUBC_COPY);
    p.inc(CE_LINE_LENGTH_IN, {uint32_t(tickets_.bytes), 1}, SUBC_COPY);
    p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
    p.m(WAIT_FOR_IDLE, 0);
    fx::emitTwodInit(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    SurfaceList resources;
    if (!member(zero_))
        resources.add(zero_.handle, false);
    if (!member(tickets_))
        resources.add(tickets_.handle, true);
    if (!job(setView_, channel, &seg, 1, resources, JobVisibility::External, seq))
        return false;
    retire(push_, seq);
    return true;
}

// Between command buffers: a ring that made an allocation wait for the GPU doubles (to `most`), the old one freed once the work
// that used it completed, so a client's rings follow what its command buffers use.
void Device::growRing(Ring &r, uint64_t most)
{
    const bool grow = r.waited && r.bo.bytes < most;
    r.waited = false;
    Bo larger;
    if (!grow || !k_.allocControl(std::min(r.bo.bytes * 2, most), larger, ControlUse::Ring))
        return;
    set_.add(k_, larger, &r == &upload_ ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    grownOut_.push_back({r.bo, lastSubmitted(), channelGeneration()});
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
}

void Device::reapAtBegin()
{
    if (!trimmedPending_.load(std::memory_order_acquire) && !nativeStale_.load(std::memory_order_acquire))
        return;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    reapNativeSurfaces();
    nativeStale_.store(std::any_of(nativeSurfaces_.begin(), nativeSurfaces_.end(),
                                   [](const std::unique_ptr<NativeSurface> &s) {
                                       return !s->cached;
                                   }),
                       std::memory_order_relaxed);
    for (Backing *b : trimmedBackings_)
        delete b;
    trimmedBackings_.clear();
    trimmedPending_.store(false, std::memory_order_relaxed);
}

CommandBuffer *Device::begin()
{
    if (!ensureChannel())
        return nullptr;
    learnCompleted();
    Killed live = {};
    if ((contextless_ || k_.lost(channel_) || liveKill(live)) && !reopen())
        return nullptr;
    for (auto it = grownOut_.begin(); it != grownOut_.end();)
        if (it->unused(channelGeneration(), [this](uint32_t seqno) {
                return done(seqno);
            })) {
            k_.free(it->bo);
            it = grownOut_.erase(it);
        } else {
            ++it;
        }
    if (!movedPlain_.empty())
        freeMovedPlain();
    growRing(push_, kSizes[0].push);
    growRing(upload_, kSizes[0].upload);
#if SWITCH_METAL_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed) || glassMemLive_ || glassMemBusyCount_)
        glassMemFrame();
    cb_.glassMemSlot_ = -1;
#endif
    CommandBuffer &cb = cb_;
    cb.dev_ = this;
    cb.segments_.clear();
    cb.batchWork_ = {};
    cb.dirty_.clear();
    cb.backings_.clear();
    cb.heaps_.clear();
    cb.surfaces_.clear();
    cb.zcullPlanes_.clear();
    set_.view(setView_);
    cb.memberUses_.clear();
    cb.memberRemovals_ = setView_.removals;
    cb.memberHostWritten_ = false;
    cb.named_.clear();
    cb.sharedWaits_.clear();
    cb.sharedSignals_.clear();
    cb.sharedReleases_.clear();
    cb.predication_.endUnit();
    cb.loops_ = {};
    cb.visibility_ = JobVisibility::Legacy;
    cb.externalWrites_ = false;
    cb.publication_ = {};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forceWholePrograms_ = cb.inlineStates_ = false;
    cb.splitProbe_ = SplitProbe::Twin;
    cb.splitForced_ = false;
    cb.splitCapturing_ = splitCaptureLeft.load(std::memory_order_relaxed) != 0;
    cb.splitCaptures_.clear();
    cb.foldCounts();
#endif
    cb.draws = cb.passes = cb.blits = cb.bands = 0;
    cb.refusal = {};
    cb.skipped = {};
    cb.skips = 0;
    cb.partial = false;
    cb.admissionBlocked = false;
    cb.quietEnd = false;
    cb.promises.clear();
    cb.promisesKept = false;
    cb.nativeError = 0;
    cb.kernelResult = 0;
    cb.notSubmitted = nullptr;
    cb.setRefused.clear();
    cb.nativeReads_.clear();
    cb.nativeSurfaces_.clear();
    cb.pitchImports_.clear();
    overflowing_ = false;
    overflow_.clear();
    cb.pushStart_ = push_.head;
    cb.uploadStart_ = upload_.head;
    memcpy(cb.rootPush_, rootPush_, sizeof rootPush_);
    cb.restart_ = restart_;
    cb.rtLayer_ = rtLayer_;
    cb.alphaHeld_ = alphaHeld_;
    cb.sampleMaskHeld_ = sampleMaskHeld_;
    cb.ds_ = ds_;
    cb.extrasLive_ = extrasLive_;
    cb.heldSegment_ = heldSegment_;
    cb.segmentBinds_ = 0;
    cb.drawn_ = false;
    reapAtBegin();
    // a trim's lock once in 64 command buffers, never waited for: the compressed-memory thread holding it trims too
    if (commits() % 64 == 0) {
        std::unique_lock<gm20b::Mutex> l(lock_, std::try_to_lock);
        if (l.owns_lock()) {
            SWITCH_METAL_ENCODE_COUNT(Locks);
            trimBackings();
        }
    }
    cb.stamp = stampOf(commits());
    cb.serial = ++begun_;
    SWITCH_METAL_DIAGNOSTIC(spans_.begin(cb.stamp));
    cb.timings.clear();
    cb.loopsLaid = false;
    // submission prologue, as NVK opens each submission: header/sampler caches (headers written since), constant, shader data and
    // texture caches, and the instruction caches where commit finds the heap grew
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, 64 * 4, 256, va));
    Push p{start, start + 64};
    emitStartStamp(p, stamps_.va, cb.stamp);
    cb.lastEngine_ = BarrierEngine::None;
    cb.chain_.reset();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forms_ = nextForms;
    nextForms = {};
    cb.taken_ = {};
    cb.pixelOpenings_ = 0;
#endif
    if (const EntryForm entry = entryFormFor(cb.barrierForms().entry); relaxed(entry)) {
        cb.shaderInvalidate_ = entryOpening(p, entry);
        SWITCH_METAL_DIAGNOSTIC(cb.taken_.entries++);
    } else {
        p.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
        p.inc(INVALIDATE_SHADER_CACHES_NO_WFI, {kInvalidateConstant});
        cb.shaderInvalidate_ = p.at - 1;
        p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_SHADER_CACHES_NO_WFI, kInvalidateData);
    }
    SWITCH_METAL_DIAGNOSTIC(beginDrawTimes(cb, p));
    checkRoom(p, "a submission prologue");
    cb.segment(start, p.at);
    cb.prologueDwords_ = uint32_t(p.at - start);
    cb.splits_.clear();
    cb.splitRecords_.clear();
    cb.splitReuse_.clear();
    cb.bufferWrites_.clear();
    cb.geometryBudget_ = {};
    cb.geometryPool_.clear();
    return &cb;
}

LoopStream CommandBuffer::loopStream(Push &p, uint32_t *&start)
{
    return {p, start, segments_, dev_->push_.bo.cpu, dev_->push_.bo.va};
}

void CommandBuffer::segment(uint32_t *from, uint32_t *to)
{
    if (to == from)
        return;
    const uint64_t va = dev_->push_.bo.va + (reinterpret_cast<uint8_t *>(from) - dev_->push_.bo.cpu);
    segments_.append({va, uint32_t(to - from), 0});
}

void CommandBuffer::timestamps(const counters::Stamp *stamps, size_t count)
{
    if (!count)
        return;
    const uint32_t words = uint32_t(count) * counters::kTimestampWords;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, words * 4, 256, va));
    Push p{start, start + words};
    for (size_t i = 0; i < count; i++) {
        useAddress(stamps[i].va, counters::kRecordBytes, true);
        counters::emit(p, stamps[i]);
    }
    checkRoom(p, "counter samples");
    segment(start, p.at);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - dev_->push_.bo.cpu);
}

void CommandBuffer::releaseEvent(uint64_t va, uint32_t payload)
{
    uint64_t pushVa;
    auto *words = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, kEventReleaseWords * 4, 4, pushVa));
    Push p{words, words + kEventReleaseWords};
    // a kernel without addressed jobs publishes DRAM at every job end, so its mid-job releases do as well
    const bool publish = publication_.take() || !dev_->k_.exactJobs();
    emitEventRelease(p, va, payload, publish);
    segment(words, p.at);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!publish)
        dev_->unflushed_.fetch_add(1, std::memory_order_relaxed);
#endif
}

uint64_t CommandBuffer::transient(uint32_t bytes)
{
    uint64_t va = 0;
    dev_->alloc(dev_->upload_, std::max(bytes, 16u), 256, va);
    return va;
}

std::shared_ptr<ResourceReadyCompletion> CommandBuffer::resourceReadiness(std::function<void()> ready, int &result)
{
    auto events = dev_->k_.sharedEvents();
    result = kIOReturnUnsupported;
    return events ? events->resources(surfaces_.uses(), std::move(ready), result) : nullptr;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void CommandBuffer::foldCounts()
{
    Device::Stats &stats = dev_->stats;
    stats.uploadBytes += counts_.uploadBytes;
    stats.segmentBinds += counts_.segmentBinds;
    stats.segmentWords += counts_.segmentWords;
    stats.segmentSkips += counts_.segmentSkips;
    stats.segmentCopies += counts_.segmentCopies;
    stats.blendKillBinds += counts_.blendKillBinds;
    stats.interlockDraws += counts_.interlockDraws;
    stats.rasterOrderedDraws += counts_.rasterOrderedDraws;
    stats.fetch.interlocked += counts_.interlocked;
    stats.fetch.interlockedBlends += counts_.interlockedBlends;
    stats.fetch.twinRule += counts_.twinRule;
    stats.splitTwins += counts_.splitTwins;
    for (size_t k = 0; k < size_t(SplitWhole::Count); k++)
        stats.splitWhole[k] += counts_.splitWhole[k];
    counts_ = {};
}
#endif

void CommandBuffer::describeRefusal(char *line, size_t size) const
{
    if (!setRefused.empty())
        snprintf(line, size, "%s", setRefused.c_str());
    else if (kernelResult)
        snprintf(line, size, "the kernel refused its job (0x%x)", unsigned(kernelResult));
    else
        snprintf(line, size, "%s", notSubmitted ? notSubmitted : "nothing named why");
}

// One error line a commit that submits nothing more for a reason its caller does not take up again (the CPU holding a
// use, a native surface changed under it): the first 16, then every 256th
static void logNotSubmitted(const CommandBuffer &cb)
{
    if (cb.kernelResult == kIOReturnCannotLock || (cb.nativeError && cb.kernelResult == cb.nativeError))
        return;
    static std::atomic<uint32_t> lines{0};
    const uint32_t n = lines.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16 && n % 256)
        return;
    char why[192];
    cb.describeRefusal(why, sizeof why);
    os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                 "SwitchMetal: a command buffer not submitted: %{public}s (#%u)", why, n);
}

bool CommandBuffer::commit(uint32_t &seqno)
{
    struct NotSubmittedLine {
        const CommandBuffer &cb;
        bool submitted = false;

        ~NotSubmittedLine()
        {
            if (!submitted && (cb.kernelResult || cb.notSubmitted || !cb.setRefused.empty()))
                logNotSubmitted(cb);
        }
    } notSubmittedLine{*this};

    SWITCH_METAL_ENCODE_SPAN(Commit);
    SWITCH_METAL_ENCODE_COUNT(GeometryVertices, GeometryBudget().vertices - geometryBudget_.vertices);
    if (nativeError)
        return false;

    Device &d = *dev_;
    SWITCH_METAL_DIAGNOSTIC(foldCounts());
    // a commit that fails after some of its jobs ran leaves the channel holding a segment the device cannot name
    d.heldSegment_.lose();
    launchPrologues();
    if (d.overflowing_) {
        notSubmitted = "a command buffer larger than its ring, with no memory for a larger one";
        return false;
    }
    bool invalidates = false, published = false;
    uint64_t invalidatedBefore = 0;
    bool tablesInvalidate = false;
    uint64_t tablesBefore = 0;

    struct RollbackInvalidation {
        Device &device;
        bool &invalidates, &published;
        uint64_t &before;
        bool &tables;
        uint64_t &tablesBefore;

        ~RollbackInvalidation()
        {
            if (invalidates && !published)
                device.heapInvalidated_ = before;
            if (tables && !published)
                device.tablesInvalidated_ = tablesBefore;
        }
    } rollback{d, invalidates, published, invalidatedBefore, tablesInvalidate, tablesBefore};

    uint64_t slmGiven = 0, slmVa = 0;
    const uint64_t placed = d.heapEpoch_.load(std::memory_order_acquire);
    if (shaderInvalidate_ && placed != d.heapInvalidated_) {
        *shaderInvalidate_ |= kInvalidateInstruction;
        invalidates = true;
        invalidatedBefore = d.heapInvalidated_;
        d.heapInvalidated_ = placed;
    }
    if (const uint64_t perWarp = d.slmPerWarp_.load(std::memory_order_acquire); perWarp != d.slmChannel_) {
        slmGiven = perWarp;
        slmVa = d.slmVa_.load(std::memory_order_relaxed);
    }
    // ahead of everything in it: a program placed after begin() may ask for more than the channel has
    if (slmGiven && !segments_.empty()) {
        uint64_t va;
        auto *head = reinterpret_cast<uint32_t *>(d.alloc(d.push_, kSlmWords * 4, 4, va));
        Push p{head, head + kSlmWords};
        emitSlm(p, slmVa, slmArea(slmGiven, d.k_.info().tpc_count));
        segments_.insert(0, {va, uint32_t(p.at - head), 0});
    }
#if SWITCH_METAL_DIAGNOSTICS
    if (!segments_.empty() && d.glassMemLive_)
        glassMemTail();
#endif
    if (!segments_.empty()) {
        uint64_t va;
        auto *tail = reinterpret_cast<uint32_t *>(d.alloc(d.push_, (kTiledCacheFlushWords + kStampWords) * 4, 4, va));
        Push p{tail, tail + kTiledCacheFlushWords + kStampWords};
        if (tiledCache(d.k_.info()))
            p.at = tiledCacheFlush(p.at);
        emitEndStamp(p, d.stamps_.va, stamp);
        segment(tail, p.at);
    }
    for (const auto &range : dirty_.merged()) {
#if defined(__aarch64__)
        if (range.invalidate) {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc civac, %0" ::"r"(address) : "memory");
        } else {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc cvac, %0" ::"r"(address) : "memory");
        }
#endif
        SWITCH_METAL_DIAGNOSTIC(d.stats.cleanBytes += range.end - range.begin);
    }
#if defined(__aarch64__)
    // The cleans and WC ring writes complete before the kernel rings GP_PUT.
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    // A loop's copies end with the GPU's writes into the push ring (loop_copies.h): the ring is a member the set
    // otherwise names read-only
    if (loopsLaid) {
        useAddress(d.push_.bo.va, d.push_.bo.bytes, true);
        for (const Bo &bo : d.push_.outgrown)
            useAddress(bo.va, bo.bytes, true);
    }
    if (d.k_.exactJobs()) {
        // the handle tables and heap sets the CPU wrote since the last submission that invalidated the L2 for them, set
        // members the job's flag names host-written
        const uint64_t written = d.tablesWritten_.load(std::memory_order_acquire);
        if (written != d.tablesInvalidated_) {
            tablesInvalidate = true;
            tablesBefore = d.tablesInvalidated_;
            d.tablesInvalidated_ = written;
        }
        if (!heaps_.empty()) {
            SWITCH_METAL_ENCODE_LOCK(lock, d.lock_);
            nameHeaps();
        }
    }
    if (!batchWork_.operations)
        segments_.includeTail();
    const uint32_t maxPushes = d.k_.maxPushes(), maxFences = d.k_.maxFences();
    if (!segments_.valid(maxPushes)) {
        notSubmitted = "a continuation group past the pushes one job takes";
        return false;  // validate all indivisible continuation groups before publishing any prefix
    }
    // A command buffer of several jobs holds its shared uses' CPU-lock admission from its first job to its last
    const bool multiple = (!segments_.empty() && segments_.submission(0, maxPushes, maxFences) < segments_.size()) ||
                          sharedWaits_.size() + sharedSignals_.size() > TegraGM20BMaxJobEvents;
    const uint64_t admission = multiple ? d.k_.nextAdmission() : 0;
    bool admissionHeld = false;
    uint32_t seq = 0;
    auto submit = [&](const TegraGM20BAbiPush *pushes, uint32_t count, SurfaceList &resources, JobVisibility visibility,
                      const JobEvents &events, bool last) {
        int result = KERN_SUCCESS;
        SurfaceList &uses = admission ? surfaces_ : resources;
        const uint32_t admissionFlags =
            admission ? (published ? uint32_t(TegraGM20BJobAdmissionContinue) : uint32_t(TegraGM20BJobAdmissionBegin)) |
                            (last ? uint32_t(TegraGM20BJobAdmissionEnd) : 0u)
                      : 0u;
        SWITCH_METAL_DIAGNOSTIC(noteExecUses(uses.uses().size()));
        SWITCH_METAL_DIAGNOSTIC(noteExecPushes(count));
        SWITCH_METAL_ENCODE_BEGIN(execSpan, Exec);
        const bool executed =
            d.job(d.setView_, d.channel_, pushes, count, uses, visibility, seq, &result, events, &memberUses_,
                  memberRemovals_, invalidates || memberHostWritten_ || tablesInvalidate, admission, admissionFlags,
                  !last || quietEnd, last && !promises.empty() ? &promises : nullptr);
        SWITCH_METAL_ENCODE_END(execSpan);
        if (executed && last && quietEnd && gm20b::jobKeptLoud())
            quietEnd = false;
        if (!executed) {
            if (!uses.valid())
                notSubmitted = "a job's uses past what one job names, or a use of no backing";
            else
                kernelResult = result;
            admissionBlocked = !published && result == kIOReturnCannotLock;
            setRefused = d.setView_.refused;
            if (admissionHeld) {
                SurfaceList none;
                uint32_t cancelled = 0;
                (void)d.job(d.setView_, d.channel_, nullptr, 0, none, JobVisibility::Internal, cancelled, nullptr, {},
                            nullptr, 0, false, admission, TegraGM20BJobAdmissionCancel);
                admissionHeld = false;
            }
            if ((!nativeSurfaces_.empty() || !sharedWaits_.empty() || !sharedSignals_.empty()) &&
                (result == kIOReturnNotReady || result == kIOReturnAborted))
                nativeError = result;
            if (published) {
                partial = true;
                d.wait(d.lastSubmitted());
                d.contextless_ = true;
            }
            return false;
        }
        published = true;
        promisesKept |= last && !promises.empty();
        admissionHeld = admission && !last;
        d.submittedThrough(seq);
        return true;
    };
    SurfaceList eventResources;
    size_t waited = 0, signaled = 0;
    // Capacity is an ABI bound, not permission to truncate dependencies. Prefix joins use the same channel's order.
    while (sharedWaits_.size() - waited + std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents) >
           TegraGM20BMaxJobEvents) {
        const uint32_t count = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const JobEvents events{sharedWaits_.data() + waited, nullptr, count, 0};
        if (!submit(nullptr, 0, eventResources, JobVisibility::Internal, events, false))
            return false;
        waited += count;
    }
    const bool pushed = segments_.submit(maxPushes, maxFences, [&](size_t at, size_t n) {
        const bool closing = at + n == segments_.size();
        const JobVisibility visibility = visibility_ == JobVisibility::Legacy ? JobVisibility::Legacy
                                         : externalWrites_ && closing         ? JobVisibility::External
                                                                              : JobVisibility::Internal;
        const uint32_t waitCount = uint32_t(sharedWaits_.size() - waited);
        const uint32_t signalCount =
            closing ? uint32_t(std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents - waitCount)) : 0;
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() : nullptr};
        const bool last =
            closing && waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(segments_.data() + at, uint32_t(n), surfaces_, visibility, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
        for (NativeSurface *surface : nativeSurfaces_) {
            surface->lastUse = seq;
            surface->channelGeneration = d.channelGeneration();
        }
        return true;
    });
    if (!pushed)
        return false;
    while (waited < sharedWaits_.size() || signaled < sharedSignals_.size()) {
        const uint32_t waitCount = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const uint32_t signalCount =
            uint32_t(std::min<size_t>(sharedSignals_.size() - signaled, TegraGM20BMaxJobEvents - waitCount));
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() + signaled : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() + signaled : nullptr};
        const bool last = waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(nullptr, 0, eventResources, JobVisibility::External, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
    }
    seqno = d.lastSubmitted();
#if SWITCH_METAL_DIAGNOSTICS
    if (glassMemSlot_ >= 0) {
        d.glassMemCbs_[glassMemSlot_].seqno = seqno;
        d.glassMemBusy_[glassMemSlot_] = true;
        d.glassMemBusyCount_++;
        glassMemSlot_ = -1;
    }
#endif
    if (slmGiven)
        d.slmChannel_ = slmGiven;
    d.retire(d.push_, seq);
    d.retire(d.upload_, seq);
    memcpy(d.rootPush_, rootPush_, sizeof rootPush_);
    d.restart_ = restart_;
    d.rtLayer_ = rtLayer_;
    d.alphaHeld_ = alphaHeld_;
    d.sampleMaskHeld_ = sampleMaskHeld_;
    d.ds_ = ds_;
    d.extrasLive_ = extrasLive_;
    d.heldSegment_ = heldSegment_;
    const uint64_t now = compClock();
    for (Texture *b : backings_) {
        Device::Backing &k = Device::backingOf(*b);
        k.lastUse.store(seq, std::memory_order_relaxed);
        k.submitted.store(true, std::memory_order_relaxed);
        k.usedNs.store(now, std::memory_order_relaxed);
    }
    d.commitCount_.store(d.commitCount_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    d.closedCount_.store(d.closedCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    d.noteCommittedTimings(timings);
    SWITCH_METAL_DIAGNOSTIC(if (drawTimes_) d.committedDrawTimes(*this, seq));
    releaseNativeReads();
    SWITCH_METAL_DIAGNOSTIC(d.stats.commits++);
    SWITCH_METAL_DIAGNOSTIC(d.stats.passPixelOpenings.fetch_add(pixelOpenings_, std::memory_order_relaxed));
    notSubmittedLine.submitted = true;
    return true;
}

uint64_t Device::loopRecordVa(uint32_t stamp) const
{
    return stamps_.va + loopRecordOffset(stamp);
}

LoopContinue Device::loopContinue(uint32_t stamp) const
{
    if (!stamp || !stamps_.cpu)
        return {};
    const auto *record = reinterpret_cast<const volatile uint32_t *>(stamps_.cpu + loopRecordOffset(stamp));
    return gm20b::loopContinue(stamp, record[0], record[1]);
}

bool Device::gpuNanoseconds(uint32_t stamp, uint64_t &ns) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    return stampedNanoseconds(*slot, stamp, ns);
}

#if SWITCH_METAL_DIAGNOSTICS
bool Device::gpuStartUnits(uint32_t stamp, uint64_t &units) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    if (slot->start.payload != stamp || slot->end.payload != stamp)
        return false;
    units = slot->start.ptimer;
    return true;
}

std::vector<StateSpan> Device::longSpans(uint32_t stamp)
{
    if (!spans_.on() || !stamp)
        return {};
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    if (slot->start.payload != stamp || slot->end.payload != stamp)
        return {};
    std::vector<StateSpan> spans = spans_.take(stamp, slot->start.ptimer, slot->end.ptimer);
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const StateSpan &s) {
                                   return s.ns < kLongSpanNs;
                               }),
                spans.end());
    return spans;
}

void RenderEncoder::markSpan(Push &p, uint32_t index, SpanKind kind)
{
    const Texture *t = pass_.color[0].texture;
    const uint32_t kpx = t ? uint32_t(uint64_t(t->width) * t->height * t->samples / 1000) : 0;
    SpanLabel label;
    if (kind == SpanKind::Draws && emitted_) {
        label = drawSpanLabel(*emitted_, emitted_->rec ? emitted_->rec->keyHash : 0, kpx, samples_);
        const Pipeline &pl = *emitted_;
        label.form = pl.splitOf                      ? SpanForm::Twin
                     : pl.freeForm.rung != FetchNone ? SpanForm::FetchFree
                     : pl.storageBits                ? SpanForm::Interlock
                     : pipeline_ && &pl != pipeline_ ? SpanForm::Variant
                                                     : SpanForm::Whole;
        label.pixels = uint32_t(std::min<uint64_t>(drawPixels_, UINT32_MAX));
        if (t) {
            label.format = t->format;
            label.width = uint16_t(std::min<uint32_t>(t->width, 0xffff));
            label.height = uint16_t(std::min<uint32_t>(t->height, 0xffff));
            label.blockLinear = t->blockLinear;
            label.compressed = t->comptags;
        }
    }
    label.kind = kind;
    label.kpx = uint16_t(std::min<uint32_t>(kpx, 0xffff));
    label.samples = samples_;
    dev_->spans_.markAt(p, index, label);
}
#endif

void CommandBuffer::abandon()
{
    releaseNativeReads();
    Device &d = *dev_;
    d.closedCount_.store(d.closedCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    d.overflowing_ = false;
    d.overflow_.clear();
    for (Device::Ring *r : {&d.push_, &d.upload_}) {
        // drop the open command buffer's spans and give back what it took (nothing else was allocated since: one encodes)
        for (auto it = r->inflight.begin(); it != r->inflight.end();)
            it = it->submitted ? std::next(it) : r->inflight.erase(it);
        r->head = r->since = (r == &d.push_ ? pushStart_ : uploadStart_);
        if (!r->outgrown.empty()) {  // it began in a ring it grew out of: the new one is empty
            for (const Bo &b : r->outgrown)
                d.grownOut_.push_back({b, d.lastSubmitted(), d.channelGeneration()});
            r->outgrown.clear();
            r->head = r->since = 0;
        }
    }
    segments_.clear();
    dirty_.clear();
    surfaces_.clear();
    named_.clear();
}

}  // namespace gm20b

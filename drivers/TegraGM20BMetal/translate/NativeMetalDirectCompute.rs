// SPDX-License-Identifier: MIT
//! The driver's direct compile of a compute state at one dispatch shape (NativeMetalDirect.h): what m2v's
//! translator makes of the kernel for the three dispatches the pack's kernel probe ran
//! (drivers/TegraGM20BMetal/pack/kernels.py), with no pipeline state, command buffer or capture. Only the
//! translator's public API is used. A runtime device lends its compile device and its Vulkan device, on which the
//! variant's layout and samplers are made too; the offline compile (the pack generator's, and the compile context's:
//! NativeMetalCompile.rs) runs on the compile profile's native device with the layout as data alone.

use crate::address_maps;
use crate::descriptors::{
    DescriptorFunction, NmbComputePipelineDescriptorOf, NmbSamplerDescriptor, compute_descriptor, e, get, guard, raw,
    sampler_descriptor,
};
use crate::direct::{
    HEAP_ARRAYS, Layout, NMB_DIRECT_IMMUTABLE, NmbDirectBinding, NmbDirectSamplerState, NmbDirectStage, NmbDirectWrite,
    UNNAMED_RESIDUAL, compile_error, residual_words, sampler_state, spill_table, write_of,
};
use crate::kernel_textures::{TWO_PLANES, kernel_texture};
use crate::offline::{Devices, NmbOfflineComputePipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, Dispatch, DispatchCommands, FieldLocation,
    ImageSource, NeutralResource, PushHole, SamplerSource, StageInRegion, StageTable, TextureFacts, UseTarget,
    WriteSource, heap_class_code,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::metal::sampler::AddressModeMap;
use metal2vulkan::metal::{PixelFormat, SamplerAddressMode, TextureSwizzleChannels, TextureType, TextureUsage};
use metal2vulkan::pipeline::{
    BindingSource, ComputePipeline, ComputePipelineDescriptor, NativePipeline, NativeProgram, NativeRecord,
    PipelineLayout, PushField, SamplerRef, SlotClass, SlotInterface, Stage, StageSource, TextureAccess,
};
use std::ffi::{CString, c_void};
use std::sync::Arc;

const SLOTS: u32 = 31;
const TAG_BYTES: u32 = 4096;
const MOVED_OFFSET: u64 = 256;
const GRID: [u32; 3] = [3, 5, 2];
// A format the translator drops a write-only slot's writes of (metal2vulkan format.rs `writes_dropped`: a 4:2:0 one)
const WRITES_DROPPED: PixelFormat = PixelFormat::YCBCR84202P;

#[repr(C)]
pub struct NmbDirectBoundTexture {
    pub slot: u32,
    pub planes: u32,
    pub pixel_format: u64,
    pub texture_type: u64,
    pub usage: u64,
    pub levels: u64,
    pub array_length: u64,
    pub samples: u64,
}

#[repr(C)]
pub struct NmbDirectBoundSampler {
    pub slot: u32,
    pub descriptor: NmbSamplerDescriptor,
}

#[repr(C)]
pub struct NmbDirectDispatch {
    pub threads: [u32; 3],
    pub memory: [u32; 31],
    pub textures: *const NmbDirectBoundTexture,
    pub texture_count: u32,
    pub samplers: *const NmbDirectBoundSampler,
    pub sampler_count: u32,
    pub region_mode: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectKernelWrite {
    pub write: NmbDirectWrite,
    pub view_format: u32,
    pub components: [u32; 4],
    pub plane: u32,
    pub sampler: u64,
    pub sampler_state: NmbDirectSamplerState,
    pub has_sampler: bool,
}

#[repr(C)]
pub struct NmbDirectKernel {
    pub device: u64,
    pub layout: u64,
    pub layout_data: *const crate::direct::NmbDirectLayout,
    pub set0: u64,
    pub set0_bindings: *const NmbDirectBinding,
    pub set0_binding_count: u32,
    pub stage: NmbDirectStage,
    pub writes: *const NmbDirectKernelWrite,
    pub write_count: u32,
    pub push_mask: u32,
    pub push: [u32; 32],
    pub moved_mask: u32,
    pub moved: [u32; 32],
    pub bound_mask: u32,
    pub bound: [u32; 32],
    pub moved_same: bool,
    pub region_word: i32,
    pub regions: u32,
    pub push_holes: u32,
    pub variant: *const std::ffi::c_char,
    pub buffer_reads: u32,
    pub buffer_writes: u32,
    pub texture_reads: u32,
    pub texture_writes: u32,
    pub root_table_word: u8,
    pub status_word: u8,
    pub other_holes: u32,
    pub root_slots: u32,
    pub set1: u64,
    pub heap_arrays: u32,
    pub heap_class: [u32; HEAP_ARRAYS],
    pub spill: *const u32,
    pub moved_spill: *const u32,
    pub bound_spill: *const u32,
    pub spill_words: u32,
    pub moved_spill_words: u32,
    pub bound_spill_words: u32,
    pub residual_word: [u8; 32],
    pub stage_in_region_word: i32,
    pub stage_in_strides: [[u32; 2]; 4],
}

/// The kernel's reads through argument buffers the driver fills (NativeMetalDirect.rs holes_of and heap_of for a draw): its
/// root-table and ExecutionStatus address holes (push word + 1), the others counted, the buffer slots its root table holds
/// an entry for as the bound dispatch names them, and its set 1's heap arrays
fn arguments_of(
    commands: &DispatchCommands,
    bound: &DispatchCommands,
    layout: &PipelineLayout,
    sets: &Layout<'_>,
    kernel: &mut NmbDirectKernel,
) -> Result<(), String> {
    for hole in &commands.push_holes {
        let (word, offset) = match *hole {
            PushHole::RootTableAddress { offset, .. } => (&mut kernel.root_table_word, offset),
            PushHole::ExecutionStatusAddress { offset, .. } => (&mut kernel.status_word, offset),
            _ => {
                kernel.other_holes += 1;
                continue;
            }
        };
        if offset % 4 != 0 || offset > 128 - 8 {
            return Err("an address push hole outside the root push window".into());
        }
        let encoded = (offset / 4 + 1) as u8;
        if *word != 0 && *word != encoded {
            return Err("the kernel's address push hole at two places".into());
        }
        *word = encoded;
    }
    if let Some(table) = &bound.root_table {
        let field = |root: usize, at: usize| {
            table
                .bytes
                .get(32 * root + at..32 * root + at + 8)
                .and_then(|b| b.try_into().ok())
                .map_or(0, u64::from_le_bytes)
        };
        let first = (field(1, 8) as usize).min(table.bytes.len() / 32);
        for root in 2..first {
            if field(root, 0) == 0 {
                continue;
            }
            if root - 2 >= 31 {
                return Err("a root table entry past the buffer slots".into());
            }
            kernel.root_slots |= 1 << (root - 2);
        }
    }
    let Some((at, set)) = layout.sets.iter().enumerate().find(|(_, s)| s.set == 1) else {
        return Ok(());
    };
    kernel.set1 = sets.set_at(at);
    kernel.heap_arrays = set.bindings.len() as u32;
    for b in &set.bindings {
        let code = match &b.source {
            BindingSource::Heap(class) => heap_class_code(class).unwrap_or(0),
            _ => 0,
        };
        if let Some(slot) = kernel.heap_class.get_mut(b.binding as usize) {
            *slot = code;
        }
    }
    Ok(())
}

fn access_masks(plans: [&BindingPlan; 3]) -> [u32; 4] {
    let mut masks = [0u32; 4];
    for use_ in plans.into_iter().flat_map(|p| &p.uses) {
        let UseTarget::Slot(slot) = use_.resource else {
            if use_.access.write {
                masks[1] = u32::MAX;
                masks[3] = u32::MAX;
            }
            continue;
        };
        if slot.stage != Stage::Kernel || slot.index >= 32 {
            continue;
        }
        let base = match slot.class {
            SlotClass::Buffer => 0,
            SlotClass::Texture => 2,
            SlotClass::Sampler => continue,
        };
        masks[base] |= u32::from(use_.access.read) << slot.index;
        masks[base + 1] |= u32::from(use_.access.write) << slot.index;
    }
    masks
}

/// Which of the probe's dispatches a bind state is: 4096 inline bytes at every slot, the same bytes `MOVED_OFFSET` into a
/// buffer of each slot's own, or each of those bound `1 + slot % 15` bytes further (pack/probe_bind.h probeComputeCode)
#[derive(Clone, Copy, PartialEq, Eq)]
enum Run {
    Bytes,
    Moved,
    Bound,
}

fn counted<'a, T>(items: *const T, count: u32) -> &'a [T] {
    if items.is_null() || count == 0 {
        return &[];
    }
    // SAFETY: the caller's counted array, live for the call.
    unsafe { std::slice::from_raw_parts(items, count as usize) }
}

/// Map `map` (address_maps.rs) as the translator's
fn address_modes(map: u8) -> AddressModeMap {
    let edge = SamplerAddressMode::ClampToEdge;
    let mut out = AddressModeMap::IDENTITY;
    if map & address_maps::WRAPPED != 0 {
        for mode in [
            SamplerAddressMode::MirrorClampToEdge,
            SamplerAddressMode::Repeat,
            SamplerAddressMode::MirrorRepeat,
        ] {
            out = out.with(mode, edge);
        }
    }
    if map & address_maps::CLAMPS != 0 {
        for mode in [SamplerAddressMode::ClampToZero, SamplerAddressMode::ClampToBorderColor] {
            out = out.with(mode, edge);
        }
    }
    out
}

/// The map of the texture a dispatch binds at `slot`, 0 for none
fn bound_map(ask: &NmbDirectDispatch, slot: u32) -> u8 {
    counted(ask.textures, ask.texture_count)
        .iter()
        .find(|t| t.slot == slot)
        .and_then(|t| {
            let pf = raw::<PixelFormat>(t.pixel_format, "pixel format").ok()?;
            Some(kernel_texture(pf, t.planes).map)
        })
        .unwrap_or(0)
}

/// `state` with each runtime sampler the kernel samples a converted texture through mapped by that texture's map, as
/// m2v binds it (metal2vulkan-runtime address_map.rs `sampler_maps`): the pairs are the layout's. A sampler two textures
/// need under different modes fails the dispatch's command buffer ("fails: ", TegraGM20BMetalDirect.mm), as m2v's
/// `SharedSamplerSlot` does.
fn mapped_samplers(layout: &PipelineLayout, ask: &NmbDirectDispatch, state: &BindState) -> Result<BindState, String> {
    let mut pairs = Vec::new();
    for b in layout.sets.iter().flat_map(|s| &s.bindings) {
        if let BindingSource::CombinedTextureSampler { texture, sampler }
        | BindingSource::SeparateTexture { texture, sampler } = &b.source
            && texture.stage == Stage::Kernel
            && let SamplerRef::Slot(index) = &sampler.sampler
        {
            pairs.push((texture.index, *index));
        }
    }
    let samplers = counted(ask.samplers, ask.sampler_count);
    let modes_of = |slot: u32| {
        samplers
            .iter()
            .find(|s| s.slot == slot)
            .map(|s| [s.descriptor.s_address, s.descriptor.t_address, s.descriptor.r_address])
    };
    let maps = address_maps::sampler_maps(&pairs, |slot| bound_map(ask, slot), modes_of).map_err(|shared| {
        format!(
            "fails: kernel sampler slot {} samples texture slots {} and {}, whose address-mode maps give it two sets \
             of modes, which one sampler cannot hold (m2v's SharedSamplerSlot)",
            shared.sampler, shared.textures[0], shared.textures[1]
        )
    })?;
    let mut mapped = state.clone();
    if let Some(table) = mapped.stages.get_mut(&Stage::Kernel) {
        for (slot, map) in maps {
            if let Some(bound) = table.samplers.get_mut(&slot) {
                bound.desc = address_modes(map).sampler(bound.desc);
            }
        }
    }
    Ok(mapped)
}

fn facts_of(t: &NmbDirectBoundTexture) -> Result<TextureFacts, String> {
    let read = kernel_texture(raw::<PixelFormat>(t.pixel_format, "pixel format")?, t.planes);
    let mut facts = TextureFacts::new(raw::<TextureType>(t.texture_type, "texture type")?, read.format);
    facts.address_modes = address_modes(read.map);
    facts.usage = raw::<TextureUsage>(t.usage, "texture usage")?;
    facts.sample_count = u32::try_from(t.samples.max(1)).map_err(e("sample count"))?;
    facts.array_length = u32::try_from(t.array_length.max(1)).map_err(e("array length"))?;
    facts.mipmap_level_count = u32::try_from(t.levels.max(1)).map_err(e("levels"))?;
    facts.two_planes = t.planes & TWO_PLANES != 0;
    Ok(facts)
}

/// `state` with each texture Metal stores no kernel write into (kernel_textures.rs) bound, at a slot the kernel only
/// writes, as a format whose write-only slot the translator binds its neutral at, so the writes store nothing, as
/// Metal's do; `access` is the kernel's declared access of a texture slot. A slot that reads and writes such a texture
/// has no form: its reads are of the image raw converts, which its writes would change.
fn dropped_stores(
    access: impl Fn(u32) -> Option<TextureAccess>,
    ask: &NmbDirectDispatch,
    mut state: BindState,
) -> Result<BindState, String> {
    for t in counted(ask.textures, ask.texture_count) {
        let pf = raw::<PixelFormat>(t.pixel_format, "pixel format")?;
        let Some(a) = access(t.slot).filter(|a| a.write) else {
            continue;
        };
        if kernel_texture(pf, t.planes).stores {
            continue;
        }
        if a.read || a.sample {
            return Err(format!(
                "a kernel that reads and writes texture slot {} of {pf:?}, whose writes Metal stores nothing of",
                t.slot
            ));
        }
        if let Some(BoundTexture::Texture { facts, .. }) = state
            .stages
            .get_mut(&Stage::Kernel)
            .and_then(|table| table.textures.get_mut(&t.slot))
        {
            facts.pixel_format = WRITES_DROPPED;
        }
    }
    Ok(state)
}

fn bind_state(run: Run, dispatch: &NmbDirectDispatch, stage_in: bool) -> Result<BindState, String> {
    let mut table = StageTable::default();
    for slot in 0..SLOTS {
        let bound = if run == Run::Bytes {
            BoundBuffer::Bytes {
                len: TAG_BYTES,
                attribute_stride: None,
            }
        } else {
            let address = 0x1_0000_0000u64 + (u64::from(slot) << 16);
            let code = if run == Run::Bound { 1 + u64::from(slot % 15) } else { 0 };
            BoundBuffer::Buffer {
                length: MOVED_OFFSET + u64::from(TAG_BYTES),
                offset: MOVED_OFFSET + code,
                gpu_address: address,
                device_address: Some(address),
                attribute_stride: None,
            }
        };
        table.buffers.insert(slot, bound);
    }
    for t in counted(dispatch.textures, dispatch.texture_count) {
        let bound = if raw::<TextureType>(t.texture_type, "texture type")? == TextureType::TextureBuffer {
            // the texel buffer view raw binds is the texture's own header (Device::createTexelBuffer), not this range
            BoundTexture::Buffer {
                pixel_format: raw::<PixelFormat>(t.pixel_format, "pixel format")?,
                offset: 0,
                length: u64::from(TAG_BYTES),
                bytes_per_row: u64::from(TAG_BYTES),
                resource_id: None,
                swizzle: TextureSwizzleChannels::default(),
            }
        } else {
            BoundTexture::Texture {
                facts: facts_of(t)?,
                resource_id: None,
            }
        };
        table.textures.insert(t.slot, bound);
    }
    for s in counted(dispatch.samplers, dispatch.sampler_count) {
        table
            .samplers
            .insert(s.slot, BoundSampler::new(sampler_descriptor(&s.descriptor)?));
    }
    for (index, &length) in dispatch.memory.iter().enumerate() {
        if length != 0 {
            table.threadgroup_lengths.insert(index as u32, length);
        }
    }
    let mut state = BindState::default();
    state.stages.insert(Stage::Kernel, table);
    if stage_in {
        state.stage_in_region = Some(StageInRegion::new([0; 3], [1; 3]));
    }
    Ok(state)
}

/// The kernel compiled under variant `key`, as the runtime compiles a state's variant: the function under the key's lowering
/// inputs with the descriptor's stage input and call stack depth, then the pipeline of it
pub fn compiled(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    key: &VariantKey,
    budget: &Budget,
) -> Result<NativeComputePipeline, String> {
    let StageSource::Function(function) = &descriptor.kernel else {
        return Err("a kernel compiled already".into());
    };
    let mut inputs = key.lowering_inputs(Stage::Kernel);
    inputs.stage_input = descriptor.stage_input.clone();
    inputs.max_call_stack_depth = Some(descriptor.max_call_stack_depth).filter(|&k| k > 1);
    let function = compile
        .compile_native_function(function, &inputs, budget)
        .map_err(compile_error("compile kernel"))?;
    let pipeline = function
        .into_pipeline(compile, descriptor)
        .map_err(compile_error("link native kernel"))?;
    Ok(NativeComputePipeline {
        pipeline: Arc::new(pipeline),
    })
}

/// A kernel's compiled variants a caller already holds (the m2v runtime's compute artifact), taken in place of compiling
/// them again
pub trait KernelVariants {
    fn pipeline(&self, key: &VariantKey, budget: &Budget) -> Result<Arc<NativeComputePipeline>, String>;
}

#[derive(Clone)]
pub struct NativeComputePipeline {
    pub pipeline: Arc<NativePipeline>,
}

impl std::ops::Deref for NativeComputePipeline {
    type Target = ComputePipeline;
    fn deref(&self) -> &Self::Target {
        &self.pipeline.metadata
    }
}

struct Settled {
    pipeline: NativeComputePipeline,
    plan: BindingPlan,
    commands: DispatchCommands,
    state: BindState,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    (ask, state): (&NmbDirectDispatch, &BindState),
    dispatch: &Dispatch,
    budget: &Budget,
    artifact: Option<&dyn KernelVariants>,
) -> Result<Settled, String> {
    let pipeline = |key: &VariantKey| match artifact {
        Some(artifact) => artifact.pipeline(key, budget).map(|p| (*p).clone()),
        None => compiled(compile, descriptor, key, budget),
    };
    let base = pipeline(&VariantKey::new())?;
    let access = |slot: u32| match base
        .interface
        .slots
        .iter()
        .find(|(s, _)| s.stage == Stage::Kernel && s.class == SlotClass::Texture && s.index == slot)
    {
        Some((_, SlotInterface::Texture(t))) => Some(t.access),
        _ => None,
    };
    let state = dropped_stores(access, ask, mapped_samplers(&base.layout, ask, state)?)?;
    let shape = state.shape();
    let plan = base.plan(compile, &shape).map_err(e("plan"))?;
    let wanted = plan.dispatch_variant(dispatch).map_err(e("dispatch variant"))?;
    let (pipeline, plan) = if wanted == VariantKey::new() {
        (base, plan)
    } else {
        let pipeline = pipeline(&wanted)?;
        let plan = pipeline.plan(compile, &shape).map_err(e("plan variant"))?;
        if plan.dispatch_variant(dispatch).map_err(e("dispatch variant"))? != wanted {
            return Err("the plan of a compiled variant asks for another variant".into());
        }
        (pipeline, plan)
    };
    let commands = plan
        .instantiate_dispatch(&state, dispatch)
        .map_err(e("instantiate dispatch"))?;
    Ok(Settled {
        pipeline,
        plan,
        commands,
        state,
    })
}

fn push_words(commands: &DispatchCommands) -> (u32, [u32; 32]) {
    let (mut mask, mut words) = (0u32, [0u32; 32]);
    let Some(region) = commands.regions.first() else {
        return (mask, words);
    };
    for write in &region.push_constants {
        for (k, word) in write.bytes.chunks(4).enumerate() {
            let at = write.offset as usize / 4 + k;
            if at < 32 && word.len() == 4 {
                words[at] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                mask |= 1 << at;
            }
        }
    }
    (mask, words)
}

/// What a write is in the record: its binding, kind, source and slot (not the range, which the offsets move)
fn layout_of(plan: &BindingPlan) -> Vec<(u32, u32, u32, i32, u32)> {
    plan.descriptors
        .iter()
        .map(|w| {
            let source = match &w.source {
                WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => slot.index,
                WriteSource::Texture { slot, .. } | WriteSource::Sampler { slot, .. } => slot.index,
                _ => u32::MAX,
            };
            (
                w.set,
                w.binding,
                w.array_element,
                avk::DescriptorType::from(w.ty).as_raw(),
                source,
            )
        })
        .collect()
}

/// What a kernel's plan changes where the probe binds its buffers off 16 bytes (`bound`'s variant, specialization and
/// writes against those of the same buffers bound 16-aligned), None where nothing: a record holds one plan, whose
/// residual words then carry each buffer's offset, so a plan that changes has no record
fn offset_change(
    [aligned, bound]: [&VariantKey; 2],
    respecialized: bool,
    [aligned_layout, bound_layout]: &[Vec<(u32, u32, u32, i32, u32)>; 2],
) -> Option<String> {
    let mut slots: std::collections::BTreeSet<u32> = aligned
        .psb_slots
        .symmetric_difference(&bound.psb_slots)
        .chain(
            aligned
                .storage_view_slots
                .symmetric_difference(&bound.storage_view_slots),
        )
        .filter(|s| s.class == SlotClass::Buffer)
        .map(|s| s.index)
        .collect();
    let apart = |a: &[(u32, u32, u32, i32, u32)], b: &[(u32, u32, u32, i32, u32)]| {
        a.iter().filter(|w| !b.contains(w)).map(|w| w.4).collect::<Vec<_>>()
    };
    slots.extend(
        apart(aligned_layout, bound_layout)
            .into_iter()
            .chain(apart(bound_layout, aligned_layout)),
    );
    slots.remove(&u32::MAX);
    if !slots.is_empty() {
        let named: Vec<String> = slots.iter().map(u32::to_string).collect();
        return Some(format!("buffer slots {}", named.join(", ")));
    }
    if aligned != bound {
        return Some("its variant".into());
    }
    if aligned_layout != bound_layout {
        return Some("the order of its descriptor writes".into());
    }
    respecialized.then(|| "its specialization constants".into())
}

/// The record's residual words, the plain plan's `words` (NativeMetalDirect.rs residual_words), where they hold every
/// buffer's offset: raw binds a buffer entry 16-aligned and adds back only the remainder a residual word names
/// (compute.cpp kernelBufferDescriptor), so a plan the offsets change (offset_change of `variants`, `respecialized`
/// and `layouts`), or a residual placed where no record word names it, is refused
fn record_residuals(
    variants: [&VariantKey; 2],
    respecialized: bool,
    layouts: &[Vec<(u32, u32, u32, i32, u32)>; 2],
    words: [u8; 32],
) -> Result<[u8; 32], String> {
    if let Some(what) = offset_change(variants, respecialized, layouts) {
        return Err(format!(
            "a kernel whose plan changes with its buffers bound off 16 bytes ({what}): no residual word holds their \
             offsets"
        ));
    }
    match words.iter().position(|&w| w == UNNAMED_RESIDUAL) {
        Some(slot) => Err(format!(
            "buffer slot {slot}'s bind offset residual placed past word 30 of its argument table, which no record word \
             names"
        )),
        None => Ok(words),
    }
}

/// A stage-in kernel's push words the raw dispatch writes: its StageInRegion's and each dynamic layout's StageInStride
fn stage_in_words(plan: &BindingPlan) -> Result<(i32, [[u32; 2]; 4]), String> {
    let (mut region, mut strides, mut n) = (-1, [[0u32; 2]; 4], 0);
    for p in &plan.push.fields {
        let FieldLocation::PushWindow { offset } = p.location else {
            continue;
        };
        match p.field {
            PushField::StageInRegion => region = (offset / 4) as i32,
            PushField::StageInStride(index) => {
                if n == strides.len() {
                    return Err("more than four stage-in layouts of a dynamic stride".into());
                }
                strides[n] = [index, offset / 4 + 1];
                n += 1;
            }
            _ => {}
        }
    }
    Ok((region, strides))
}

fn region_word(plan: &BindingPlan) -> i32 {
    plan.push
        .fields
        .iter()
        .find_map(|p| match (p.field, p.location) {
            (PushField::DispatchRegion, FieldLocation::PushWindow { offset }) => Some((offset / 4) as i32),
            _ => None,
        })
        .unwrap_or(-1)
}

/// The samplers a dispatch's sampler writes take: their states, and the Vulkan samplers made of them where there is a
/// device, destroyed with it
struct Samplers<'d> {
    device: Option<&'d ash::Device>,
    made: Vec<avk::Sampler>,
}

impl Samplers<'_> {
    /// The sampler a write takes (its handle, 0 with no device, and its state); none where the dispatch binds none
    fn make(
        &mut self,
        compile: &metal2vulkan::device::Device,
        state: &BindState,
        source: &SamplerSource,
        image_format: Option<metal2vulkan::vk::Format>,
    ) -> Result<Option<(u64, NmbDirectSamplerState)>, String> {
        if let SamplerSource::Neutral(NeutralResource::Sampler { compare }) = source {
            let info = avk::SamplerCreateInfo::default()
                .mag_filter(avk::Filter::NEAREST)
                .min_filter(avk::Filter::NEAREST)
                .mipmap_mode(avk::SamplerMipmapMode::NEAREST)
                .address_mode_u(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_v(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_w(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .max_lod(avk::LOD_CLAMP_NONE)
                .compare_enable(*compare)
                .compare_op(avk::CompareOp::NEVER);
            let state = sampler_state(&info, avk::SamplerReductionMode::WEIGHTED_AVERAGE, None);
            return Ok(Some((self.create(&info)?, state)));
        }
        let SamplerSource::Slot {
            slot,
            compare,
            integer,
            opaque_black,
        } = source
        else {
            return Ok(None);
        };
        let Some(bound) = state
            .stages
            .get(&Stage::Kernel)
            .and_then(|table| table.samplers.get(&slot.index))
        else {
            return Ok(None);
        };
        let desc =
            metal2vulkan::bind::sampler_desc(compile, &bound.desc, *compare, *integer, *opaque_black, image_format)
                .map_err(e("sampler"))?;
        let mut reduction = avk::SamplerReductionModeCreateInfo::default();
        let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
        let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
        let made = sampler_state(
            &info,
            desc.reduction.into(),
            desc.border_components.map(avk::ComponentMapping::from),
        );
        Ok(Some((self.create(&info)?, made)))
    }

    fn create(&mut self, info: &avk::SamplerCreateInfo<'_>) -> Result<u64, String> {
        let Some(device) = self.device else {
            return Ok(0);
        };
        // SAFETY: `info` and its chain are live for the call.
        let sampler = unsafe { device.create_sampler(info, None) }.map_err(e("sampler"))?;
        self.made.push(sampler);
        Ok(avk::Handle::as_raw(sampler))
    }
}

impl Drop for Samplers<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        for &sampler in &self.made {
            // SAFETY: made here; NVK only read its header.
            unsafe { device.destroy_sampler(sampler, None) };
        }
    }
}

impl NmbDirectKernelWrite {
    fn take_sampler(&mut self, made: Option<(u64, NmbDirectSamplerState)>) {
        if let Some((sampler, state)) = made {
            (self.sampler, self.sampler_state, self.has_sampler) = (sampler, state, true);
        }
    }
}

fn view_parts(view: &metal2vulkan::bind::ImageViewDesc) -> (u32, [u32; 4]) {
    let components = if view.shader_swizzle {
        view.components
    } else {
        TextureSwizzleChannels::DEFAULT.compose(view.components)
    };
    let c = avk::ComponentMapping::from(components);
    (
        avk::Format::from(view.format).as_raw() as u32,
        [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
    )
}

/// Whether binding `(set, binding)` of `layout` is a two-plane texture's alpha plane
fn alpha_plane(layout: &PipelineLayout, set: u32, binding: u32) -> bool {
    layout
        .sets
        .iter()
        .filter(|s| s.set == set)
        .flat_map(|s| &s.bindings)
        .any(|b| b.binding == binding && matches!(b.source, BindingSource::TexturePlane(_)))
}

fn kernel_write(
    w: &metal2vulkan::bind::DescriptorWrite,
    (commands, layout): (&DispatchCommands, &PipelineLayout),
    compile: &metal2vulkan::device::Device,
    state: &BindState,
    samplers: &mut Samplers<'_>,
) -> Result<NmbDirectKernelWrite, String> {
    let mut out = NmbDirectKernelWrite {
        write: write_of(w, &commands.descriptor_ranges),
        view_format: 0,
        components: [0; 4],
        plane: u32::from(alpha_plane(layout, w.set, w.binding)),
        sampler: 0,
        sampler_state: NmbDirectSamplerState::default(),
        has_sampler: false,
    };
    match &w.source {
        WriteSource::Texture { view, .. } => (out.view_format, out.components) = view_parts(view),
        WriteSource::TexelBuffer { format, .. } => out.view_format = format.as_raw() as u32,
        WriteSource::Sampler {
            slot,
            compare,
            integer,
            opaque_black,
        } => {
            let source = SamplerSource::Slot {
                slot: *slot,
                compare: *compare,
                integer: *integer,
                opaque_black: *opaque_black,
            };
            out.take_sampler(samplers.make(compile, state, &source, None)?);
        }
        WriteSource::Neutral(neutral @ NeutralResource::Sampler { .. }) => {
            out.take_sampler(samplers.make(compile, state, &SamplerSource::Neutral(*neutral), None)?);
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            let format = match image {
                ImageSource::Texture { view, .. } => {
                    (out.view_format, out.components) = view_parts(view);
                    Some(view.format)
                }
                ImageSource::Neutral(_) => None,
            };
            if out.write.sampler != NMB_DIRECT_IMMUTABLE {
                out.take_sampler(samplers.make(compile, state, sampler, format)?);
            }
        }
        _ => {}
    }
    Ok(out)
}

/// nmb_compute_pipeline_direct's answer for a kernel of offline functions, made on the compile profile's device with no
/// Vulkan object: every handle the kernel hands over is 0.
///
/// # Safety
/// As nmb_compute_pipeline_direct's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_offline(
    descriptor: *const NmbOfflineComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        compute_offline(offline_devices()?, descriptor, dispatch, each, ctx)
    })
}

/// nmb_compute_pipeline_offline's compile on `devices`, of a descriptor of either function handle: the pack generator's
/// offline functions, or the compile context's (NativeMetalCompile.rs)
pub fn compute_offline<F: DescriptorFunction>(
    devices: &Devices,
    descriptor: *const NmbComputePipelineDescriptorOf<F>,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let descriptor = compute_descriptor(descriptor)?;
    direct(None, &devices.native, &descriptor, None, get(dispatch)?, each, ctx)
}

/// The answers the runtime's compute prepare gives a state of offline functions (nmb_compute_artifact_new's metal_info,
/// on the compile profile's native device), into `facts`: maxTotalThreadsPerThreadgroup, threadExecutionWidth,
/// staticThreadgroupMemoryLength
///
/// # Safety
/// The descriptor and its function handles are live for this call; `facts` holds three words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_compute_pipeline_facts(
    descriptor: *const NmbOfflineComputePipelineDescriptor,
    facts: *mut u32,
) -> bool {
    guard(false, || {
        if facts.is_null() {
            return Err("null compute facts".into());
        }
        let descriptor = compute_descriptor(descriptor)?;
        let made = compiled(
            &offline_devices()?.native,
            &descriptor,
            &VariantKey::new(),
            &crate::compile_budget(),
        )?;
        let info = &made.metal_info;
        // SAFETY: the caller's three words.
        unsafe { std::slice::from_raw_parts_mut(facts, 3) }.copy_from_slice(&[
            info.max_total_threads_per_threadgroup,
            info.thread_execution_width,
            info.static_threadgroup_memory_length,
        ]);
        Ok(true)
    })
}

pub fn direct(
    vulkan: Option<&ash::Device>,
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    artifact: Option<&dyn KernelVariants>,
    ask: &NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let budget = crate::compile_budget();
    let call = if ask.region_mode != 0 {
        Dispatch::ThreadgroupRange {
            groups: GRID,
            first: [0; 3],
            count: GRID,
            threads_per_group: ask.threads,
        }
    } else {
        Dispatch::Threadgroups {
            groups: GRID,
            threads_per_group: ask.threads,
        }
    };
    let stage_in = descriptor.stage_input.is_some();
    let plain = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Bytes, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    let moved = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Moved, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    let bound = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Bound, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    // InlineBytes have no buffer hazard entries; the moved and bound plans expose those accesses.
    let [buffer_reads, buffer_writes, texture_reads, texture_writes] =
        access_masks([&plain.plan, &moved.plan, &bound.plan]);
    let region = |s: &Settled| s.commands.regions.first().map(|r| r.specialization.clone());
    let same = |s: &Settled| {
        s.plan.variant == plain.plan.variant
            && region(s) == region(&plain)
            && layout_of(&s.plan) == layout_of(&plain.plan)
    };
    let residual_word = record_residuals(
        [&moved.plan.variant, &bound.plan.variant],
        region(&bound) != region(&moved),
        &[layout_of(&moved.plan), layout_of(&bound.plan)],
        residual_words(&plain.plan, Stage::Kernel),
    )?;
    let (push_mask, push) = push_words(&plain.commands);
    let (stage_in_region_word, stage_in_strides) = stage_in_words(&plain.plan)?;
    let (moved_mask, moved_push) = push_words(&moved.commands);
    let (bound_mask, bound_push) = push_words(&bound.commands);
    let ranges = |s: &Settled| -> Vec<(u32, u32, u64)> {
        s.commands
            .descriptor_ranges
            .iter()
            .map(|r| (r.set, r.binding, r.range.range))
            .collect()
    };
    let moved_same = same(&moved) && ranges(&moved) == ranges(&plain);
    let spill = |s: &Settled| spill_table(&s.plan, &s.commands.descriptor_bytes, Stage::Kernel);
    let (plain_spill, moved_spill, bound_spill) = (spill(&plain), spill(&moved), spill(&bound));

    let layout = Layout::new(vulkan, &plain.pipeline.layout)?;
    let mut samplers = Samplers {
        device: vulkan,
        made: Vec::new(),
    };
    let writes = plain
        .plan
        .descriptors
        .iter()
        .map(|w| {
            let parts = (&plain.commands, &plain.pipeline.layout);
            kernel_write(w, parts, compile, &plain.state, &mut samplers)
        })
        .collect::<Result<Vec<_>, String>>()?;
    let first = plain.commands.regions.first();
    let specialization = first.map(|r| r.specialization.clone()).unwrap_or_default();
    let map: Vec<avk::SpecializationMapEntry> = specialization.entries.iter().map(|&m| m.into()).collect();
    let info = avk::SpecializationInfo::default()
        .map_entries(&map)
        .data(&specialization.data);
    let stage = &plain.pipeline.stage;
    let entry = CString::new(stage.entry_point.as_str()).unwrap_or_default();
    let variant = CString::new(format!("{:?}", plain.plan.variant)).unwrap_or_default();
    let semantic = plain.pipeline.pipeline.program.as_ref().map(SemanticView::of);
    let mut kernel = NmbDirectKernel {
        device: vulkan.map_or(0, |d| avk::Handle::as_raw(d.handle())),
        layout: avk::Handle::as_raw(layout.pipeline),
        layout_data: &layout.data,
        set0: layout.set0(),
        set0_bindings: layout.set0_bindings.as_ptr(),
        set0_binding_count: layout.set0_bindings.len() as u32,
        stage: NmbDirectStage {
            spirv: stage.spirv.as_ptr(),
            spirv_bytes: stage.spirv.len() * 4,
            entry: entry.as_ptr(),
            stage: avk::ShaderStageFlags::from(stage.stage).as_raw(),
            flags: first.map_or(0, |r| avk::PipelineShaderStageCreateFlags::from(r.stage_flags).as_raw()),
            subgroup_size: first.and_then(|r| r.required_subgroup_size).unwrap_or(0),
            specialization: if map.is_empty() {
                std::ptr::null()
            } else {
                (&info as *const avk::SpecializationInfo<'_>).cast()
            },
            semantic: semantic
                .as_ref()
                .map_or(std::ptr::null(), |p| (p as *const SemanticView).cast()),
            portable_context: Arc::as_ptr(&plain.pipeline.pipeline).cast(),
            portable: Some(materialize_portable),
        },
        writes: writes.as_ptr(),
        write_count: writes.len() as u32,
        push_mask,
        push,
        moved_mask,
        moved: moved_push,
        bound_mask,
        bound: bound_push,
        moved_same,
        region_word: region_word(&plain.plan),
        regions: plain.commands.regions.len() as u32,
        push_holes: plain.commands.push_holes.len() as u32,
        variant: variant.as_ptr(),
        buffer_reads,
        buffer_writes,
        texture_reads,
        texture_writes,
        root_table_word: 0,
        status_word: 0,
        other_holes: 0,
        root_slots: 0,
        set1: 0,
        heap_arrays: 0,
        heap_class: [0; HEAP_ARRAYS],
        spill: plain_spill.as_ptr(),
        moved_spill: moved_spill.as_ptr(),
        bound_spill: bound_spill.as_ptr(),
        spill_words: plain_spill.len() as u32,
        moved_spill_words: moved_spill.len() as u32,
        bound_spill_words: bound_spill.len() as u32,
        residual_word,
        stage_in_region_word,
        stage_in_strides,
    };
    arguments_of(
        &plain.commands,
        &bound.commands,
        &plain.pipeline.layout,
        &layout,
        &mut kernel,
    )?;
    // SAFETY: the caller's contract; everything `kernel` points at lives until after the call.
    Ok(unsafe { each(&kernel, ctx) })
}

unsafe extern "C" fn materialize_portable(context: *const c_void, words: *mut *const u32, bytes: *mut usize) -> bool {
    guard(false, || {
        let pipeline = get(context.cast::<NativePipeline>())?;
        let portable = pipeline
            .portable_words()
            .map_err(compile_error("portable native fallback"))?;
        // SAFETY: NVK supplies both output fields, and the pipeline owns the returned words through the callback.
        unsafe {
            *words.as_mut().ok_or("null portable words")? = portable.as_ptr();
            *bytes.as_mut().ok_or("null portable size")? = portable.len() * 4;
        }
        Ok(true)
    })
}

#[repr(C)]
struct SemanticView {
    version: u32,
    types: u32,
    values: u32,
    blocks: u32,
    entry: u32,
    record_count: u32,
    operand_count: u32,
    records: *const NativeRecord,
    operands: *const u32,
}

impl SemanticView {
    fn of(program: &NativeProgram) -> Self {
        Self {
            version: NativeProgram::VERSION,
            types: program.types,
            values: program.values,
            blocks: program.blocks,
            entry: program.entry,
            record_count: program.records.len() as u32,
            operand_count: program.operands.len() as u32,
            records: program.records.as_ptr(),
            operands: program.operands.as_ptr(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::pipeline::SlotRef;

    fn bound(slot: u32, pixel_format: u64, planes: u32) -> NmbDirectBoundTexture {
        NmbDirectBoundTexture {
            slot,
            planes,
            pixel_format,
            texture_type: 2,
            usage: 1,
            levels: 1,
            array_length: 1,
            samples: 1,
        }
    }

    // C32's ci_srgb_to_linear_colormatrix over the planes of a 'p420' surface (588, 589): no format of the translator's
    #[test]
    fn a_packed_planes_slot_is_planned_as_the_image_raw_binds_there() {
        for format in [588, 589] {
            let facts = facts_of(&bound(1, format, 0)).unwrap();
            assert_eq!(facts.pixel_format, PixelFormat::RGB10A2Unorm, "format {format}");
            assert_eq!(facts.address_modes, AddressModeMap::IDENTITY);
        }
    }

    // A 4:2:0 converted texture over a P3_D65 surface: raw reads its codes image and samples it under the codes map (3)
    #[test]
    fn a_stated_map_decides_the_samplers_and_a_codes_read_the_format() {
        let facts = facts_of(&bound(0, 500, (1 << 15) | (3 << 8))).unwrap();
        assert_eq!(facts.pixel_format, PixelFormat::RGB84202P);
        assert_eq!(facts.address_modes, address_modes(3));
        assert!(!facts.two_planes, "bit 15 states a map, not two planes");
        let facts = facts_of(&bound(0, 500, 0)).unwrap();
        assert_eq!(
            (facts.pixel_format, facts.address_modes),
            (PixelFormat::YCBCR84202P, address_modes(1))
        );
        assert!(facts_of(&bound(0, 550, 1)).unwrap().two_planes);
    }

    fn buffer(index: u32) -> SlotRef {
        SlotRef {
            stage: Stage::Kernel,
            class: SlotClass::Buffer,
            index,
        }
    }

    // A record holds one plan, and each buffer's residual word carries its offset: a kernel whose plan the probe's
    // buffers bound off 16 bytes change is refused, naming the slots, and one whose plan they leave keeps its words
    #[test]
    fn a_plan_a_buffer_offset_changes_is_named_and_one_it_leaves_is_kept() {
        let plain = VariantKey::new();
        let layout = vec![(0, 0, 0, 7, 2), (0, 1, 0, 6, 5)];
        let mut words = [0u8; 32];
        words[2] = 4;
        words[5] = 40;
        let kept = record_residuals([&plain, &plain], false, &[layout.clone(), layout.clone()], words);
        assert_eq!(kept, Ok(words));
        let mut addressed = VariantKey::new();
        addressed.psb_slots.insert(buffer(5));
        let moved = vec![(0, 0, 0, 7, 2), (0, 1, 0, 7, 3)];
        let refused = record_residuals([&plain, &addressed], false, &[layout.clone(), moved.clone()], words);
        assert!(refused.unwrap_err().contains("(buffer slots 3, 5)"));
        let named = offset_change([&plain, &addressed], false, &[layout.clone(), moved]);
        assert_eq!(named.as_deref(), Some("buffer slots 3, 5"));
        let mut viewed = VariantKey::new();
        viewed.emulated_simdgroups = true;
        let changed = offset_change([&plain, &viewed], false, &[layout.clone(), layout.clone()]);
        assert_eq!(changed.as_deref(), Some("its variant"));
        let reordered: Vec<_> = layout.iter().rev().copied().collect();
        let order = offset_change([&plain, &plain], false, &[layout.clone(), reordered]);
        assert_eq!(order.as_deref(), Some("the order of its descriptor writes"));
        let specialized = offset_change([&plain, &plain], true, &[layout.clone(), layout]);
        assert_eq!(specialized.as_deref(), Some("its specialization constants"));
    }

    #[test]
    fn a_residual_no_record_word_names_is_refused_by_its_slot() {
        let plain = VariantKey::new();
        let layouts = [vec![(0, 0, 0, 7, 3)], vec![(0, 0, 0, 7, 3)]];
        let mut words = [0u8; 32];
        words[0] = 1;
        words[3] = 63;
        assert_eq!(record_residuals([&plain, &plain], false, &layouts, words), Ok(words));
        words[9] = UNNAMED_RESIDUAL;
        let refused = record_residuals([&plain, &plain], false, &layouts, words).unwrap_err();
        assert!(refused.starts_with("buffer slot 9's"), "{refused}");
    }

    fn kernel_state(textures: &[NmbDirectBoundTexture]) -> BindState {
        let mut table = StageTable::default();
        for t in textures {
            table.textures.insert(
                t.slot,
                BoundTexture::Texture {
                    facts: facts_of(t).unwrap(),
                    resource_id: None,
                },
            );
        }
        let mut state = BindState::default();
        state.stages.insert(Stage::Kernel, table);
        state
    }

    fn format_at(state: &BindState, slot: u32) -> PixelFormat {
        match &state.stages[&Stage::Kernel].textures[&slot] {
            BoundTexture::Texture { facts, .. } => facts.pixel_format,
            _ => panic!("no texture at {slot}"),
        }
    }

    // An M4 Max stores nothing of a kernel's write into an R10Unorm_PACKED or RG10Unorm_PACKED texture, nor into a
    // YCBCR8_422_1P one: a slot the kernel only writes is bound as a format whose writes the translator drops, one it
    // reads too is refused, and any other texture's writes are planned as they were
    #[test]
    fn a_kernels_writes_into_a_texture_metal_stores_none_of_store_nothing() {
        let textures = [bound(0, 70, 0), bound(3, 588, 0), bound(4, 501, 0)];
        let ask = NmbDirectDispatch {
            threads: [32, 8, 1],
            memory: [0; 31],
            textures: textures.as_ptr(),
            texture_count: 3,
            samplers: std::ptr::null(),
            sampler_count: 0,
            region_mode: 0,
        };
        let write = TextureAccess::new(false, false, true);
        let state = dropped_stores(|_| Some(write), &ask, kernel_state(&textures)).unwrap();
        assert_eq!(format_at(&state, 0), PixelFormat::RGBA8Unorm);
        assert_eq!(format_at(&state, 3), WRITES_DROPPED);
        assert_eq!(format_at(&state, 4), WRITES_DROPPED);
        let read = |slot| Some(TextureAccess::new(false, slot == 3, true));
        let why = dropped_stores(read, &ask, kernel_state(&textures)).unwrap_err();
        assert!(why.contains("texture slot 3 of R10UnormPACKED"), "{why}");
        let sampled = |slot| Some(TextureAccess::new(true, false, slot == 0));
        let state = dropped_stores(sampled, &ask, kernel_state(&textures)).unwrap();
        assert_eq!(
            format_at(&state, 3),
            PixelFormat::RGB10A2Unorm,
            "a slot only read keeps the image raw binds"
        );
    }
}

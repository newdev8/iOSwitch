#!/bin/bash
# raw GM20B path host build, trimmed to the reproduce harnesses: nvkframe (frontend + host bridge + NVK twin),
# facadeframe (RawMetal.mm over the frontend as its m2v fallback) and computeframe (one Metal kernel through the
# frontend + m2v + NVK and through the raw compute encoder). The full set is upstream's build.sh.
# usage: build.sh <host libnative_metal_bridge.a> <frontend dir> <tree> <outdir>
set -e -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RAW=$HERE/..
BRIDGE=$1
FRONT=$2
TREE=$3
OUT=$4
METAL_ABI=${METAL_ABI:-$TREE/bazel-bin/drivers/NativeMetal/abi}
COMPAT=()
if [ "$(xcrun --show-sdk-version | cut -d. -f1)" -lt 15 ]; then
    COMPAT=(-include "$HERE/../../hostsim/MetalHostCompat.h")
fi
mkdir -p "$OUT"
FLAGS=(-O2 -g -Wall -Wextra -std=c++17 -DSWITCH_RAW_HARNESS_MARKS -I "$TREE/bazel-bin" -I "$METAL_ABI" -I "$TREE/kits/SwitchGM20BEmit/mesa"
    -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers")
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$f.c" -o "$OUT/$f.o"; done
xcrun clang -O1 -Wall -c -DSWITCH_MOCK_JOB_INTERFACE -I "$TREE/drivers/TegraGM20B" "$RAW/pack/mock_iokit.c" -o "$OUT/mock_iokit.o"
xcrun clang++ "${FLAGS[@]}" -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/cap.cpp" -o "$OUT/cap.o"
xcrun clang++ "${FLAGS[@]}" -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" -c "$RAW/pack/nvdec.cpp" -o "$OUT/nvdec.o"
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" -c "$RAW/pack/nvdec_compute.cpp" -o "$OUT/nvdec_compute.o"
xcrun clang++ "${FLAGS[@]}" -c "$RAW/fx/test/twod_sim.cpp" -o "$OUT/twod_sim.o"
for f in SwitchGM20BEmit3D SwitchGM20BEmitBarrier SwitchGM20BEmitIterated mesa/nouveau_copy_rect; do
    xcrun clang -O2 -std=c11 -DNDEBUG -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" \
        -I "$TREE/third_party/mesa/src/nouveau/headers" -c "$TREE/kits/SwitchGM20BEmit/$f.c" -o "$OUT/kit_${f##*/}.o"
done
xcrun clang -O2 -std=c11 -DNDEBUG -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/third_party/mesa/src/nouveau/headers" -I "$TREE/third_party/mesa/src" -I "$TREE/third_party/mesa/include" -I "$TREE/third_party/mesa/src/nouveau/headers/nvidia/classes" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/kits/SwitchGM20BEmit/mesa" -c "$TREE/kits/SwitchGM20BEmit/SwitchGM20BEmitReport.c" -o "$OUT/emit_report.o"
xcrun clang++ -dynamiclib "$OUT/mock_iokit.o" "$OUT/cap.o" "$OUT/nvdec.o" "$OUT/nvdec_compute.o" "$OUT/twod_sim.o" "$OUT/mme_fermi.o" "$OUT/mme_fermi_sim.o" "$OUT"/kit_*.o -framework IOKit \
    -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
OBJC=(-O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_RAW_HARNESS_MARKS -I "$TREE/bazel-bin" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" -I "$FRONT" -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" -I "$TREE/bazel-bin/kits/SwitchGM20B" -I "$TREE/bazel-bin/drivers/TegraGM20BMetal/precompile/ca-selectors" "${COMPAT[@]}")
FACADE_RUNTIME=("$RAW/runtime/raw_semantic_plan.cpp" "$RAW/runtime/raw_semantic_resources.cpp" "$RAW/runtime/raw_semantic_setters.cpp"
    "$RAW/runtime/raw_xr_texture.cpp" "$RAW/runtime/raw_multisample.cpp" "$RAW/runtime/raw_object_pool.cpp"
    "$RAW/runtime/raw_visibility.cpp" "$RAW/runtime/raw_visibility_pass.cpp" "$RAW/runtime/raw_texture_shape.cpp")
COMPLETION=("$RAW/runtime/raw_record_parts.cpp" "$RAW/runtime/raw_direct.cpp" "$RAW/runtime/raw_direct_compute.cpp" "$RAW/runtime/raw_emit.cpp" "$RAW/runtime/raw_cache.cpp" "$RAW/runtime/raw_kernel_cache.cpp" "$RAW/runtime/raw_compiler.cpp" "$RAW/runtime/raw_metallib.cpp")
FACADE=("$RAW"/runtime/Raw*.mm "$RAW/runtime/SwitchSharedEvent.mm")
LIBS=(-L "$OUT" -lmock_iokit "-Wl,-rpath,$OUT" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation)
RAWGPU=("$RAW/runtime/raw_gpu.cpp" "$RAW/runtime/raw_tessellation.cpp" "$RAW/runtime/raw_amplification.cpp" "$RAW/runtime/raw_private_comp.cpp" "$RAW/runtime/raw_completed.cpp" "$RAW/runtime/raw_vertex_streams.cpp" "$RAW/runtime/raw_draw_bands.cpp" "$RAW/runtime/raw_turned_pass.cpp" "$RAW/runtime/raw_resolve_twod.cpp" "$RAW/runtime/raw_turn_copy.cpp" "$RAW/runtime/raw_compile_request.cpp" "$RAW/runtime/raw_job_uses.cpp" "$RAW/runtime/raw_transfer.cpp" "$RAW/runtime/raw_split.cpp" "$RAW/runtime/raw_variants.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_kernel_job.cpp" "$RAW/runtime/raw_resource_set.cpp" "$RAW/runtime/raw_state_segment.cpp" "$RAW/runtime/raw_encoder_state.cpp" "$RAW/runtime/raw_shared_events.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_arena.cpp")
xcrun clang++ "${OBJC[@]}" -x objective-c++ "$HERE/nvkframe.mm" "$FRONT/NativeBridgeMetalFrontend.mm" -x none "$BRIDGE" "$OUT/emit_report.o" "${LIBS[@]}" -o "$OUT/nvkframe" 2>&1 | awk '!/duplicate libraries/'
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -DRAW_FACADE -x objective-c++ "$HERE/nvkframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "${RAWGPU[@]}" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "$RAW/runtime/raw_compute.cpp" "$RAW/runtime/raw_predication.cpp" "$RAW/runtime/raw_icb_compute.cpp" "$RAW/runtime/raw_compute_loops.cpp" "$RAW/runtime/raw_loop_copies.cpp" "$RAW/runtime/raw_predicate_loops.cpp" "${COMPLETION[@]}" "${FACADE_RUNTIME[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "$OUT/emit_report.o" "${LIBS[@]}" \
    -o "$OUT/facadeframe" 2>&1 | awk '!/duplicate libraries/'
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/computeframe.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$HERE/computeframe_raw.cpp" "$RAW/runtime/raw_compute.cpp" "$RAW/runtime/raw_predication.cpp" "$RAW/runtime/raw_icb_compute.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "${RAWGPU[@]}" "$RAW/runtime/raw_compute_loops.cpp" "$RAW/runtime/raw_loop_copies.cpp" "$RAW/runtime/raw_predicate_loops.cpp" \
    "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" -x none "$BRIDGE" "$OUT/emit_report.o" "${LIBS[@]}" \
    -o "$OUT/computeframe" 2>&1 | awk '!/duplicate libraries/'
echo "built $OUT/nvkframe $OUT/facadeframe $OUT/computeframe"

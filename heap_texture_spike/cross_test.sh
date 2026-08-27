#!/bin/bash
# 三驱动 × 三变体交叉测试：descriptor heap texture/sampler 语义分离
# 用法: ./cross_test.sh
set -u
cd "$(dirname "$0")/build"
OUT=/tmp/heap_spike_cross_$(date +%H%M%S)
mkdir -p "$OUT"
echo "output: $OUT"

run_one() {
    local driver="$1" variant_env="$2" tag="$3"
    local envs=()
    case "$driver" in
        radv) envs+=(VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json RADV_EXPERIMENTAL=heap) ;;
        intel) envs+=(VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json) ;;
        nvidia) envs+=(VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json __NV_PRIME_RENDER_OFFLOAD=1 __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 __GLX_VENDOR_LIBRARY_NAME=zink __VK_LAYER_NV_optimus=NVIDIA_only) ;;
    esac
    echo "=== $driver / $tag ==="
    env "${envs[@]}" $variant_env ./heap_texture_spike > "$OUT/${driver}_${tag}.log" 2>&1
    local rc=$?
    grep -E '^(Selected:|Q[0-9]|OVERALL|  (top-left|center-left|center-right|bottom-right))' "$OUT/${driver}_${tag}.log" | sed 's/^/  /'
    # 非 09600 的 VVL 报错（09600 是已知布局跟踪伪影）
    local extra_vvl
    extra_vvl=$(grep -E 'Validation (Error|Warning)' "$OUT/${driver}_${tag}.log" | grep -v '09600' | grep -v 'vkGetDeviceProcAddr' | sort -u | head -5)
    [ -n "$extra_vvl" ] && { echo "  EXTRA VVL:"; echo "$extra_vvl" | sed 's/^/    /'; }
    echo "  exit=$rc"
}

for driver in radv intel nvidia; do
    run_one "$driver" 'SPIKE_SHADER=glsl' glsl_embedded
    run_one "$driver" 'SPIKE_SHADER=slang SPIKE_SAMPLER=heap' slang_heap
    run_one "$driver" 'SPIKE_SHADER=slang SPIKE_SAMPLER=heap SPIKE_SAMPLER_FIELD=samplerHeapOffset' slang_heap_oldfield
    run_one "$driver" 'SPIKE_SHADER=slang SPIKE_SAMPLER=embedded' slang_embedded
    run_one "$driver" 'SPIKE_SHADER=slang_ctrl' slang_ctrl
done

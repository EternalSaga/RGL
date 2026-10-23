#!/bin/bash
# 债-140 §22 复现器构建：glslangValidator → SPIR-V，clang++ 链系统 libvulkan。
set -e
cd "$(dirname "$0")"
mkdir -p shaders/spv
G=glslangValidator
T="--target-env spirv1.4"
$G -V $T -S task shaders/task_const.glsl  -o shaders/task_const.spv
$G -V $T -S task shaders/task_buffer.glsl -o shaders/task_buffer.spv
$G -V $T -S mesh shaders/mesh.glsl        -o shaders/mesh.spv
$G -V $T -DMESHDI=1 -S mesh shaders/mesh.glsl -o shaders/mesh_di.spv
$G -V $T -S mesh shaders/mesh_dim2.glsl   -o shaders/mesh_dim2.spv
$G -V $T -S vert shaders/vert.glsl        -o shaders/vert.spv
# §26 真取数链档：引擎形态 task（MDI/DD 两变体）+ 链 mesh（trivial 走 spec 常量 CHAIN=0）
$G -V $T -S task shaders/task_engine.glsl     -o shaders/task_engine.spv
$G -V $T -DDD=1 -S task shaders/task_engine.glsl -o shaders/task_engine_dd.spv
$G -V $T -S mesh shaders/mesh_chain.glsl      -o shaders/mesh_chain.spv
# 任务 16 grid-size 变体（恒 +0 语义中性，强制 user SGPR 槽非 0）
$G -V $T -DGRIDSIZE=1 -S mesh shaders/mesh_chain.glsl -o shaders/mesh_chain_gs.spv
$G -V $T -S frag shaders/frag.glsl        -o shaders/frag.spv
clang++ -std=c++20 -O1 -g -o repro repro.cpp -lvulkan
echo "BUILD OK"

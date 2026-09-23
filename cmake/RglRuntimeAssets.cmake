# ─── Runtime 资源拷贝（configs / models / skybox / env → build dir）──────────
# 由 rgl/CMakeLists.txt include。程序按 CWD 相对路径读取，构建期 copy_if_different
# 增量拷贝：改源后下次 just build 即生效，无需重链接。
include_guard()

# 调用：rgl_copy_files(<OUT_VAR> <DEST_SUB> <SRC>...)
# 每源文件 → ${CMAKE_CURRENT_BINARY_DIR}/DEST_SUB/<name>；OUT_VAR 收集输出列表
# （供 add_custom_target DEPENDS）。DEST_SUB 目录自动创建。
function(rgl_copy_files out_var dest_sub)
    set(dest_dir "${CMAKE_CURRENT_BINARY_DIR}/${dest_sub}")
    file(MAKE_DIRECTORY ${dest_dir})
    set(outputs "")
    foreach(src IN LISTS ARGN)
        get_filename_component(name ${src} NAME)
        set(out "${dest_dir}/${name}")
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} ${dest_dir}/
            DEPENDS ${src}
            COMMENT "Copying ${name} to ${dest_sub}/ ..."
            VERBATIM
        )
        list(APPEND outputs ${out})
    endforeach()
    set(${out_var} ${outputs} PARENT_SCOPE)
endfunction()

# ─── configs/ → build configs/ ────────────────────────────────────────────────
# 新增 config 文件需要 just config（file GLOB 在 configure 时求值）。
file(GLOB CONFIG_FILES "${PROJECT_SOURCE_DIR}/configs/*")
rgl_copy_files(COPIED_CONFIGS configs ${CONFIG_FILES})
add_custom_target(copy_configs ALL DEPENDS ${COPIED_CONFIGS})

# ─── assets/*.glb → build models/ ─────────────────────────────────────────────
# glb 在 assets/（git-lfs 管理）。新增模型需在此加一行 + just config。
set(MODEL_GLB_SOURCES
    "${PROJECT_SOURCE_DIR}/assets/transparent.glb"
    "${PROJECT_SOURCE_DIR}/assets/grass_variations.glb"
    "${PROJECT_SOURCE_DIR}/assets/realistic_wooden_cottage_garden_house_3d_model.glb"
    "${PROJECT_SOURCE_DIR}/assets/castle_of_loarre.glb"
    "${PROJECT_SOURCE_DIR}/assets/kyoto_fushimi_inari_taisha_temple.glb"
    "${PROJECT_SOURCE_DIR}/assets/bistro_exterior.glb")
rgl_copy_files(MODEL_GLB_OUTPUTS models ${MODEL_GLB_SOURCES})
add_custom_target(copy_models ALL DEPENDS ${MODEL_GLB_OUTPUTS})

# ─── assets/<dir>/*.png → build skybox/<dir>/（Phase 5.5）────────────────────
# 6 面 cubemap 素材（nightSky/rainbow，面顺序定案见 docs/archive/phase5_5_plan.md）。
# 新增素材目录需在此加 dir 名 + just config（GLOB 于 configure 时求值）。
set(SKYBOX_DIRS nightSky rainbow)
set(SKYBOX_OUTPUTS)
foreach(dir ${SKYBOX_DIRS})
    file(GLOB SKYBOX_DIR_FILES "${PROJECT_SOURCE_DIR}/assets/${dir}/*")
    rgl_copy_files(_skybox_outs skybox/${dir} ${SKYBOX_DIR_FILES})
    list(APPEND SKYBOX_OUTPUTS ${_skybox_outs})
endforeach()
add_custom_target(copy_skybox ALL DEPENDS ${SKYBOX_OUTPUTS})

# ─── Bistro_v5_2/*.hdr → build env/（Phase 8 批次 ②b）────────────────────────
# .hdr 属 gitignore 本地素材（Bistro_v5_2/ 1.7G 不入 git）：存在才拷，缺失时引擎按
# scene.lua environment.hdr 探测告警并回落 faces PNG 路径（干净 checkout 仍可运行）。
# 新增 .hdr 素材需在此加文件名 + just config。
set(ENV_HDR_FILES san_giuseppe_bridge_4k.hdr)
set(ENV_HDR_EXISTING)
foreach(hdr ${ENV_HDR_FILES})
    set(hdr_src "${PROJECT_SOURCE_DIR}/Bistro_v5_2/${hdr}")
    if(EXISTS ${hdr_src})
        list(APPEND ENV_HDR_EXISTING ${hdr_src})
    endif()
endforeach()
if(ENV_HDR_EXISTING)
    rgl_copy_files(ENV_HDR_OUTPUTS env ${ENV_HDR_EXISTING})
    add_custom_target(copy_env_hdr ALL DEPENDS ${ENV_HDR_OUTPUTS})
endif()

# 反射产物新鲜度机制（2026-09-02 编译提速；取代原 add_custom_command 自动管线）：
# 反射管线只在 `just reflect` 时跑，产物（*_reflection.json / generated/*.cppm）
# 纳入版本控制。本脚本双模式（-DMODE=write|check）：
#   write：为 PAIRS_FILE 里每条 "输出|输入1,输入2,..." 计算各输入文件的 SHA1，
#          写清单（reflection_manifest.txt，随产物提交）；
#   check：逐条比对当前输入哈希与清单记录——不一致/缺记录即 FATAL_ERROR。
# 内容寻址比 mtime 严谨：touch 无改动不误报，改动必报（忘跑 reflect = 构建拒绝，
# 不会静默使用过期生成代码）。
#
# 不用 file(TIMESTAMP)：本机 CMake 4.3.4 该子命令恒返回空串（Gentoo 构建异常），
# 已实测不可靠——哈希方案同时跨平台且更正确。

if(NOT DEFINED PAIRS_FILE)
    message(FATAL_ERROR "ReflectionManifest: PAIRS_FILE 未传入")
endif()
include("${PAIRS_FILE}")
string(REPLACE "\n" ";" PAIRS "${PAIRS}")

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ReflectionManifest: ROOT 未传入")
endif()
if(NOT DEFINED MANIFEST)
    message(FATAL_ERROR "ReflectionManifest: MANIFEST 未传入")
endif()

function(rel out path)
    file(RELATIVE_PATH r "${ROOT}" "${path}")
    set(${out} "${r}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "write")
    set(lines "")
    foreach(entry IN LISTS PAIRS)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 out)
        list(GET parts 1 ins)
        string(REPLACE "," ";" ins "${ins}")
        foreach(in ${ins})
            file(SHA1 "${in}" h)
            rel(r "${in}")
            string(APPEND lines "${h}  ${r}\n")
        endforeach()
    endforeach()
    file(WRITE "${MANIFEST}" "# reflection input hashes — regenerate with: just reflect\n${lines}")
    message(STATUS "reflection manifest written: ${MANIFEST}")
    return()
endif()

# check
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR
        "reflection manifest 缺失（${MANIFEST}）——运行: just reflect")
endif()
file(READ "${MANIFEST}" content)
string(REPLACE "\n" ";" rows "${content}")
set(records "")
foreach(row ${rows})
    if(row MATCHES "^([0-9a-f]+)[ \t]+(.+)$")
        list(APPEND records "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}")
    endif()
endforeach()

set(stale "")
foreach(entry IN LISTS PAIRS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 out)
    list(GET parts 1 ins)
    string(REPLACE "," ";" ins "${ins}")
    foreach(in ${ins})
        if(NOT EXISTS "${in}")
            string(APPEND stale "\n  缺失输入: ${in}")
            continue()
        endif()
        file(SHA1 "${in}" h)
        rel(r "${in}")
        set(found "")
        foreach(rec ${records})
            string(REPLACE "|" ";" rp "${rec}")
            list(GET rp 0 rh)
            list(GET rp 1 rr)
            if(rr STREQUAL r)
                set(found "${rh}")
            endif()
        endforeach()
        if(NOT found STREQUAL h)
            string(APPEND stale "\n  ${r} 已变更（清单哈希 ≠ 当前哈希）")
        endif()
    endforeach()
endforeach()

if(stale)
    message(FATAL_ERROR
        "反射输入已变更，生成产物过期：${stale}\n"
        "注：SHA1 按文件字节算，连注释/空格的改动都会触发（不代表语义变更）；"
        "纯注释改动 just reflect 后产物通常仅清单哈希更新、生成码位级不变。\n"
        "修复：just reflect（重跑 slangc 反射 + reflection_tool），并把 "
        "vulkan_practice/generated/ 下的产物与清单一起提交")
endif()

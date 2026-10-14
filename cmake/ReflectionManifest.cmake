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

# 输入项展开：`glob:<pattern>` → 该 glob 命中的全部文件；其余原样追加。
# 🔴 write 与 check **必须共用本函数**：两处各写一份展开逻辑 = 一致性 bug 的温床——只要
# 有一处忘了同步，清单记录的输入集合与守卫看到的输入集合就不是同一份，结果是「护栏永远绿
# 但产物已过期」，正是本机制存在的理由的反面。
# glob 串由 configure 期拼成绝对路径（脚本模式里没有 CMAKE_SOURCE_DIR），且不能含 `,` 或
# `|`——那两个字符分别是 PAIRS 的输入分隔符与产物/输入分隔符。
function(expand_inputs out specs)
    set(expanded "")
    foreach(spec ${specs})
        if(spec MATCHES "^glob:(.*)$")
            file(GLOB_RECURSE hits LIST_DIRECTORIES false "${CMAKE_MATCH_1}")
            # 🔴 glob 命中 0 个 = 模式漂了（目录改名 / 后缀变了），而 write 与 check 会**一致地**
            # 展开成空 ⟹ 清单与守卫永远对得上、护栏全绿，闭包却已经整块消失。这是本机制唯一
            # 一种「两处同步所以测不出来」的失效，只能在展开点当场夺非零码。
            list(LENGTH hits n_hits)
            if(n_hits EQUAL 0)
                message(FATAL_ERROR
                    "ReflectionManifest: glob 模式 '${CMAKE_MATCH_1}' 命中 0 个文件——"
                    "模式漂了（目录改名？后缀变了？）。继续跑会得到一份**看起来全绿**但闭包"
                    "已整块消失的清单，那比没有护栏更坏。")
            endif()
            list(APPEND expanded ${hits})
        else()
            list(APPEND expanded "${spec}")
        endif()
    endforeach()
    # 去重：显式列出的文件常常也在同一条 glob 里（compute-access 的 9 枚 comp shader 就是）。
    # 不去重不会算错（同一文件哈希相同），但清单会多出重复行——清单是已提交文件，
    # 多出来的行会让人怀疑「这两行为什么不一样」，而它们一模一样。
    if(expanded)
        list(REMOVE_DUPLICATES expanded)
    endif()
    set(${out} ${expanded} PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "write")
    set(lines "")
    foreach(entry IN LISTS PAIRS)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 out)
        list(GET parts 1 ins)
        string(REPLACE "," ";" ins "${ins}")
        expand_inputs(ins_expanded "${ins}")
        foreach(in ${ins_expanded})
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
    expand_inputs(ins_expanded "${ins}")
    foreach(in ${ins_expanded})
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
        "rgl/generated/ 下的产物与清单一起提交")
endif()

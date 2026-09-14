#!/usr/bin/env bash
# spike 构建：模块 API 编进宿主 exe（符号导出）+ 脚本 so 经 BMI 编译 + dlopen 解析
# 三步对应三个待验点：① 模块能否产 BMI 供跨单元 import ② 宿主 exe 能否导出模块符号
# ③ dlopen 时脚本的模块符号引用能否解析到宿主
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-clang++}
FLAGS="-std=c++23"

echo "== [1/3] 编译 API 模块（GMF → BMI + 目标文件）"
$CXX $FLAGS -fmodules -x c++-module -c spike_api.cppm -o spike_api.o

echo "== [2/3] 链接宿主 exe（--export-dynamic 导出全部符号进 .dynsym）"
$CXX $FLAGS -fmodule-file=spike_api=spike_api.pcm main.cpp spike_api.o -o host -Wl,--export-dynamic
nm -D host | grep -c "api_" | xargs echo "宿主 .dynsym 中 api_* 符号数:"

echo "== [3/3] 编译脚本 so（import BMI，不链接任何引擎库）"
$CXX $FLAGS -fmodule-file=spike_api=spike_api.pcm script.cpp -shared -o script.so
nm -D script.so | grep "U .*api_" | xargs -r echo "脚本未定义 api_* 引用（应 3 条）:"

echo "== 运行"
./host

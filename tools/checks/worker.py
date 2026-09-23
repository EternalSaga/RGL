"""worker 进程（每进程加载一次 libclang；ctypes 持 GIL → 线程并行无效）。

ProcessPoolExecutor 一进程一文件，默认 min(cpu, 4) 个 worker，每进程 ~550MB 内存。
本模块函数必须保持模块顶层且仅依赖可 pickle 的参数/返回值（Windows spawn 下子
进程按模块名重新 import worker，包内其余模块随 sys.path 一并可导入）。

缓存 key = sha256(CACHE_VERSION + sha256(源字节) + command)：增量只重解析改动
文件；CACHE_VERSION 递增自动失效旧缓存（指标结构变化时）。

产物竞态兜底（坑 4c，主防线 = CMake add_dependencies(source_checks rgl)）：
libclang 解析前 bmi_stable 双采样 + 失败退避重试（1/2/4s）；全部尝试耗尽按
"检查失败（跳过）"上报，不阻断构建。CXIndex 每 worker 建一次复用，**永不
disposeIndex**（modules + Indexer 场景 dispose 必段错误，坑 7 附赠）。
"""

import ctypes
import hashlib
import json
import os
import time
from concurrent.futures import ProcessPoolExecutor

from clangbind import bind_indexer, bind_libclang
from cmdclean import bmi_ready, bmi_stable, clean_cmd
from config import CACHE_VERSION
from metrics import analyze_file

_W = {}
_RETRY_WAITS = (0.0, 1.0, 2.0, 4.0)


def _worker_init(ctx):
    lib = ctypes.CDLL(ctx["libclang"])
    bind_libclang(lib)
    bind_indexer(lib)
    _W["lib"] = lib
    _W["cxindex"] = lib.clang_createIndex(0, 0)
    _W["ctx"] = ctx


def _worker_task(rel_entry):
    rel, entry = rel_entry
    ctx = _W["ctx"]
    full = os.path.join(ctx["root"], rel)
    try:
        src = open(full, "rb").read()
        cmd = clean_cmd(entry, full, ctx["scratch_dir"], ctx["resdir"])
    except Exception as ex:
        return rel, None, "error:" + str(ex).splitlines()[0]
    key = hashlib.sha256(
        CACHE_VERSION.encode()
        + hashlib.sha256(src).hexdigest().encode()
        + json.dumps(cmd, sort_keys=True).encode()
    ).hexdigest()[:24]
    cache = os.path.join(ctx["cache_dir"], key + ".json")
    if os.path.exists(cache):
        return rel, json.load(open(cache, encoding="utf-8")), "cache"
    if not bmi_ready(entry):
        return rel, None, "skip:构建产物未就绪（modmap/BMI 缺失），编译完成后重跑即生效"
    last_err = "unknown"
    for wait in _RETRY_WAITS:
        if wait:
            time.sleep(wait)
        if not bmi_stable(entry):
            last_err = "BMI/modmap 写入不稳定（疑似并发构建中）"
            continue
        try:
            m = analyze_file(_W["lib"], _W["cxindex"], entry, full,
                             ctx["scratch_dir"], ctx["resdir"])
        except Exception as ex:
            last_err = str(ex).splitlines()[0]
            continue
        os.makedirs(ctx["cache_dir"], exist_ok=True)
        with open(cache, "w", encoding="utf-8") as f:
            json.dump(m, f, ensure_ascii=False)
        return rel, m, "parsed"
    return rel, None, "error:" + last_err + f"（重试 {len(_RETRY_WAITS)} 次）"


def _pool_results(ctx, ready, workers):
    """ProcessPoolExecutor 一进程一文件跑 _worker_task，逐条 yield 结果（main 的
    try/except BrokenProcessPool 消费本生成器，worker 猝死不 traceback 阻断构建）。

    @param ctx _worker_init 上下文字典（libclang/root/scratch/cache/resdir）
    @param ready 预检就绪的 (rel, entry) 列表
    @param workers 并行进程数
    @return (rel, metrics, how) 三元组生成器（metrics=None 时 how 携 error:/skip: 描述）
    """
    with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init,
                             initargs=(ctx,)) as pool:
        yield from pool.map(_worker_task, ready)

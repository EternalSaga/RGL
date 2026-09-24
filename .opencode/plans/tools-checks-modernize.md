# tools/checks 现代化 + 粗粒度拆分

## 目标（用户已拍板）

1. **Python 地板 = 3.14**（本机 3.14.7；入口加 `sys.version_info` 守卫）
2. **Warning = `NamedTuple`**（`.rel` / `.line` / `.msg`），全链路改字段访问
3. **rules 粗粒度拆 3 文件**（metrics / text / doxygen），不搞细粒度子包
4. **顺手卫生**：`metrics.py:59` 恒 False、全部裸 `open` 改 with、pathlib 替换 `os.path` 高频点、f-string、`TOK_COMMENT` 魔数、`match` 状态机、`removeprefix`、全量类型注解

**硬约束**：CMake/Justfile 只认 `tools/checks/main.py`——入口路径不动；`worker` 不 import `rules`——拆分零影响 pickle 边界；`CACHE_VERSION` **不 bump**（metrics JSON schema 不变，仅 Python 侧类型化）。

---

## 回归基线（拆分前已采样，实施后必须逐行 diff 为空）

| 集合 | 基线文件 | 条数 |
|---|---|---|
| style 警告行 | `/tmp/opencode/style_baseline.txt`（若已删则重跑采样） | 12 |
| doc 警告行 | `/tmp/opencode/doc_baseline.txt` | 1 |

验收命令：
```bash
python3 tools/checks/main.py --only style | grep '^\[style-check\] rgl/' | sort
python3 tools/checks/main.py --only doc   | grep '^\[doc-check\]' | sort
# 与 baseline 逐行 diff 必须为空（消息文案允许因 Warning 字段化有重排，但 rel:line + 规则语义不变——若文案完全不变更佳）
```

---

## 目标布局

```
tools/checks/
  main.py           # CLI + 编排 + 报告（从 121 行压到 ~90；version guard）
  config.py         # 名单/阈值（保持；仅文档指针更新）
  types.py          # NEW：Warning / FileMetrics / FunctionMetrics / DeclEntry
  fs.py             # NEW：read_text、rgl_cppm_files、iter_docs_markdown、rel_path(pathlib)
  rules_metrics.py  # 从 rules.py 抽：rule_file_lines…rule_expected_throw + evaluate_metrics
  rules_text.py     # 从 rules.py 抽：parse_module_file、strip_comments、text_checks、
                    #   _module_structure_checks、modern_cpp_checks、docs_status_checks、
                    #   forbidden_vulkan_checks、is_internal_header、rgl 相关发现
  rules_doxygen.py  # 从 rules.py 抽：has_doxygen_above、rule_doxygen、interface_declared_names、
                    #   _unstar/_block_has_content/_is_fwd_line
  metrics.py        # 卫生修 + 注解 + TypedDict 字面量
  worker.py         # 卫生修（with open）+ 注解（逻辑不动）
  cmdclean.py       # removeprefix/suffix + with open + 注解
  clangbind.py      # pathlib find_libclang + 注解（绑定/坑逻辑不动）
  # 删除 rules.py（一次性 git mv 拆三）
```

粗粒度 3 拆后预估行数：
- `rules_text.py` ~380（最大，可接受）
- `rules_metrics.py` ~90
- `rules_doxygen.py` ~130

---

## 实施步骤

### Phase 1 — 类型与卫生（不改拆分，先跑通 baseline 对齐）

1. **`types.py`**
   ```python
   from typing import NamedTuple, TypedDict, NotRequired

   class Warning(NamedTuple):
       rel: str
       line: int
       msg: str

   class FunctionMetrics(TypedDict):
       name: str
       line: int
       code_lines: int
       cognitive: int
       returns_expected: bool
       throws: bool

   class FileMetrics(TypedDict):
       total_lines: int
       file_code_lines: int
       functions: list[FunctionMetrics]
       fields: list[dict[str, str | int]]  # 或 FieldMetrics
       decls: list[DeclEntry]

   type Pair = tuple[str, int]  # 仅在仍需 tuple 处
   ```
   `Thresholds` 从 `config.py` 迁到 `types.py`（或留 config——选 **迁 types**，config 保持零逻辑名单），`config` 保留阈值默认常量。

2. **全库 `Warning(...)` 替换裸三元组**（约 50 处 append/return；`main` 排序键 `key=lambda w: (w.rel, w.line)`，打印 `w.rel`/`w.line`/`w.msg`；doxygen 内 `for rel, line, msg` 改 `for w`）。

3. **卫生项**
   - `metrics.py:59` → `return False`（循环外），并用 `from config import TOK_COMMENT` 替换魔数 `4`
   - 裸 `open` 改 `with`：`main.py:59`、`metrics.py:100`、`worker.py:46,57`、`cmdclean.py:37,85`
   - `rules.py:526` `%` → f-string；`rules.py:347` 去掉无意义 f
   - `cmdclean`：`removeprefix("-fmodule-file=")` / `removesuffix` 替换手切片
   - `metrics`/`main`/`worker`/`cmdclean`/`clangbind` 公共 API 补类型注解（3.14 惰性求值，无 import 顺序风险）
   - `config.HERE/ROOT` 用 `pathlib.Path`（`ROOT: Path`；注意 worker 里 `os.path.join(ctx["root"], rel)` 若 root 变 Path 则 `str(root)`——**ctx 保持 str 或全程 Path 统一，选 `ctx["root"]` 仍为 str、ROOT 在 fs 边界转 Path**，避免 worker pickle 路径行为变化）

4. 跑 baseline 对账 → 必须 12+1 行不变。

### Phase 2 — `fs.py` 抽路径/遍历 + pathlib

- `read_text(path)`（迁自 rules，语义不变：OSError→None、errors=replace）
- `rgl_cppm_files() -> list[str]`（POSIX 相对路径；内部 `Path.rglob("*.cppm")` + 跳 `generated`）
- `iter_files(suffix, under: Path)` 供 docs_status / 扩展名扫描复用
- `is_internal_header` 留 `rules_text`（依赖 ROOT 语义），用 `Path.resolve()` + `is_relative_to`（3.9+）
- `text_checks` 里两遍扫 `gmf_incs` 合并为一趟
- `strip_comments` 状态机改 `match state:`（0–4 case），行为不变（注释/字符串规则保留）

### Phase 3 — 拆 `rules.py` → 三文件

| 目标文件 | 迁入 |
|---|---|
| `rules_metrics.py` | `rule_file_lines`…`rule_expected_throw`、`evaluate_metrics` |
| `rules_text.py` | `parse_module_file`、`is_internal_header`、`text_checks`、`_module_structure_checks`、`modern_cpp_checks`、`docs_status_checks`、`strip_comments`、`_build_forbidden_regexes`/`FORBIDDEN_REGEXES`/`forbidden_vulkan_checks`、相关 `_MODERN_REGEXES` 等 |
| `rules_doxygen.py` | `_unstar`、`_block_has_content`、`_is_fwd_line`、`has_doxygen_above`、`interface_declared_names`、`rule_doxygen` |

- `main.py` import 改为三模块（不建 `rules/` 包——用户选粗粒度，扁平更贴现状）
- `git mv`/复制后删除 `rules.py`
- 更新 `config.py` 注释里 `rules.xxx` 指向、`docs/风格检查规则.md` §「判定 = rules.py」、skill 里若提及

### Phase 4 — `main` 变薄 + 3.14 守卫

1. 文件顶：
   ```python
   if sys.version_info < (3, 14):
       raise SystemExit(f"error: tools/checks 需要 Python ≥3.14，当前 {sys.version}")
   ```
2. `run_style/run_doc = only in (...)` 布尔，消灭 8 次 `only in (...)`
3. 报告段可抽 `report.py` 的 `print_style_warnings` / `print_summary`——**若 main 已 ≤100 行则可不做 report.py**（粗粒度精神，避免过度文件化）
4. argparse 帮助文案顺带写明 3.14

### Phase 5 — 验证与文档

1. `just style-check` / `just doc-check` / `just checks`：警告集 == baseline
2. `python3 -m compileall tools/checks`
3. 故意制造 1 条临时违规（或改阈值 `--func-threshold 50`）确认 Warning 字段路径活着
4. `--strict` exit 1、非 strict exit 0
5. 文档：
   - `docs/技术概念.md §构建检查工具机制`：补三文件名 + `types.py` + Python 3.14
   - `docs/风格检查规则.md` 头部「判定 = rules.py」→ 三文件
   - `AGENTS.md` 构建节一句提及「工具要求 Python ≥3.14」
   - skill `rgl-new-module` 红线里若写 `tools/checks/rules.py` 则改指向

---

## 明确不做

- 不建 `rules/` 细子包、不建 `pyproject.toml`/安装化
- 不改规则语义、名单、阈值、输出文案（除非字段化触碰格式——保持文案字节级不变最稳）
- 不 bump `CACHE_VERSION`
- 不动 `clangbind` 布局与坑注释结构（仅注解/pathlib 邻近改动）
- 不引入第三方依赖（无 typing_extensions——3.14 标准库够用）
- `worker`/ProcessPool 接口不改（仍 yield `(rel, metrics, how)` 三元组；`how` 保持 str）

---

## 风险与对策

| 风险 | 对策 |
|---|---|
| Warning 字段化漏改导致 TypeError | `compileall` + 全量 `just checks` 跑通；grep 残留 `w[0]`/`(rel, 0,` |
| pathlib 与 str 路径混用 | `config.ROOT` 保持 `str`；仅 `fs` 内部临时 Path；ctx pickle 不变 |
| Windows `python` 非 3.14 | 入口 fail-fast 清晰报错（用户已确认可升） |
| 拆分误删私有辅助 | 拆前 `python -c "import rules"` 符号清单；拆后 grep 旧名无悬空 import |
| 既有 12+1 警告基线被顺手“修掉” | 本次 **不修** rgl/ 既有警告；只动工具自身 |

---

## 交付物清单

- [ ] `types.py`、`fs.py` 新建
- [ ] `rules.py` → `rules_metrics.py` / `rules_text.py` / `rules_doxygen.py`
- [ ] 全库 `Warning` NamedTuple 化 + 注解 + 卫生修（with/open、恒 False、TOK_COMMENT、f-string、removeprefix、match）
- [ ] `main.py` 3.14 guard + run_style/run_doc
- [ ] baseline 对账 12+1 逐行不变
- [ ] 文档 4 处指针更新（技术概念 / 风格检查规则 / AGENTS / skill）
- [ ] 不 commit 除非用户要求（本任务完成后按用户指示 commit）

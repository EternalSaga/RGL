#!/usr/bin/env python3
"""债-140 复现器三卡复测驱动（README「三卡复测口径」× 14 轮/格）。

用法：retest_cards.py [轮数=14] [卡1,卡2,...]（卡名 = CARDS 键；VK_ICD_FILENAMES 锁单 ICD）。
本机首跑读数 = 取证文档 §29（9 格 0 挂）。

口径 = tools/debt140_repro/README「三卡复测口径」三条命令 × 14 轮/格（对齐 §27 矩阵轮数）。
挂判据 = rc≠0（124=超时，139=段错误，-4=DEVICE_LOST）；每格跑完查 journalctl -k
（Xid / ring timeout / MODE2 / fault）。逐轮 jsonl 落 results/。
"""
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPRO = HERE
OUT = HERE / "results"

CARDS = {
	"nv5090": "/usr/share/vulkan/icd.d/nvidia_icd.json",
	"intel_b50": "/usr/share/vulkan/icd.d/intel_icd.x86_64.json",
	"amd_raphael": "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json",
}
CELLS = {
	"r3": ["--chain", "1", "--mode", "mdi", "--rows", "1500", "--dispatches", "3", "--frames", "400", "--rowvary"],
	"r1triv": ["--triv", "1", "--mode", "mdi", "--rows", "1500", "--dispatches", "3", "--frames", "400"],
	"r4direct": ["--chain", "1", "--mode", "direct", "--rows", "1500", "--dispatches", "3", "--frames", "400"],
}
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 14
KERNEL_PAT = "Xid|ring .*timeout|MODE2|GPU reset|fault detected|i915|xe:"


def jlines(since, pattern):
	r = subprocess.run(["journalctl", "-k", "--no-pager", "--since", since, "--grep", pattern],
	                   capture_output=True, text=True)
	return [l for l in r.stdout.splitlines() if l.strip() and not l.lstrip().startswith("--")]


def main():
	cards = sys.argv[2].split(",") if len(sys.argv) > 2 else list(CARDS)
	OUT.mkdir(exist_ok=True)
	summary = []
	for card in cards:
		env = {"VK_ICD_FILENAMES": CARDS[card], "PATH": "/usr/bin:/bin:/usr/lib/llvm/22/bin"}
		for cell, args in CELLS.items():
			label = f"retest_{card}_{cell}"
			fh = open(OUT / f"{label}.jsonl", "w")
			hangs = 0
			since = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
			t0 = time.time()
			for n in range(1, ROUNDS + 1):
				r0 = time.time()
				try:
					p = subprocess.run([str(REPRO / "repro")] + args, capture_output=True, text=True,
					                   timeout=150, env=env, cwd=REPRO)
					rc, so, se = p.returncode, p.stdout.strip(), p.stderr.strip()
				except subprocess.TimeoutExpired as e:
					rc, so, se = 124, "", (e.stderr or b"").decode()[-500:]
				rec = {"round": n, "rc": rc, "wall_s": round(time.time() - r0, 1), "stderr": se[-500:]}
				try:
					j = json.loads(so)
					rec.update(checksum=j["checksum"], lit=j["lit_pixels"], device=j["device"])
				except Exception:
					rec.update(checksum=None, lit=None, device=None)
				rec["hang"] = rc != 0
				hangs += rec["hang"]
				fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
				fh.flush()
				print(f"[{label}] r{n:02d} rc={rc} lit={rec['lit']} hang={rec['hang']}", flush=True)
				if rec["hang"]:
					time.sleep(5)  # 给驱动回收 / 内核日志落盘留窗口
			klog = jlines(since, KERNEL_PAT)
			fh.write(json.dumps({"kernel_lines": klog, "cell_wall_s": round(time.time() - t0, 1)}) + "\n")
			fh.close()
			summary.append((card, cell, hangs, len(klog)))
			print(f"== {label}: {hangs}/{ROUNDS} 挂, 内核行 {len(klog)}", flush=True)
	print("\n===== 汇总 =====")
	for card, cell, h, k in summary:
		print(f"{card:12s} {cell:9s} 挂 {h:2d}/{ROUNDS}  内核可疑行 {k}")


if __name__ == "__main__":
	main()

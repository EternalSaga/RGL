#!/usr/bin/env python3
"""§22 剂量扫描驱动：跑 repro N 轮，逐轮记 rc/JSON/内核复位/fault 地址（同族性）。

用法：run_scan.py <label> <rounds> <repro 参数...>
产物 = results/<label>.jsonl（逐轮一行）。挂判据 = rc≠0；挂后等 reset succeeded +15s。
"""
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).parent
OUT = HERE / "results"
REPRO = HERE / "repro"


def sh(cmd):
	r = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True)
	return r


def jcount(since, pattern):
	r = sh(["journalctl", "-k", "--no-pager", "--since", since, "--grep", pattern])
	if r.returncode not in (0, 1):
		return None
	return sum(1 for l in r.stdout.splitlines() if l.strip() and not l.lstrip().startswith("--"))


def jfind(since, pattern):
	r = sh(["journalctl", "-k", "--no-pager", "--since", since, "--grep", pattern])
	if r.returncode not in (0, 1):
		return []
	return [l for l in r.stdout.splitlines() if "fault detected at address" in l]


def wait_reset(since, grace=15, limit=180):
	t0 = time.time()
	while time.time() - t0 < limit:
		if jcount(since, "reset succeeded"):
			break
		time.sleep(4)
	time.sleep(grace)


def main():
	label, rounds = sys.argv[1], int(sys.argv[2])
	args = sys.argv[3:]
	OUT.mkdir(exist_ok=True)
	(OUT.parent / "os_live.txt").unlink(missing_ok=True)  # 上一场的实时计数别串进本场显示（2026-10-24 坑）
	path = OUT / f"{label}.jsonl"
	fh = open(path, "a")
	for n in range(1, rounds + 1):
		if sh(["pgrep", "-cx", "rgl"]).stdout.strip() != "0":
			print("  🔴 rgl 残留，停", flush=True)
			sys.exit(2)
		since = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
		t0 = time.time()
		try:
			p = subprocess.run([str(REPRO)] + args, capture_output=True, text=True, timeout=150)
			rc, so, se = p.returncode, p.stdout, p.stderr
		except subprocess.TimeoutExpired:
			rc, so, se = 124, "", "timeout 150s"
		wall = round(time.time() - t0, 1)
		try:
			out = json.loads(so)
		except Exception:
			out = {}
		faults = jfind(since, "GPUVM fault")
		rec = {"round": n, "rc": rc, "hang": rc != 0, "wall_s": wall,
			"checksum": out.get("checksum"), "lit": out.get("lit_pixels"),
			"resets": jcount(since, "reset succeeded"),
			"mode2": jcount(since, "MODE2"),
			"fault_lines": [l.split("address")[-1].strip()[:20] for l in faults][:2],
			"overshoot_count": out.get("overshoot_count"), "overshoot_cap8": out.get("overshoot_cap8"),
			"overshoot_max": out.get("overshoot_max"), "overshoot_slots": out.get("overshoot_slots"),
			"overshoot_mgcs": out.get("overshoot_mgcs"),
			"os_tail": ((OUT.parent / "os_live.txt").read_text().strip().splitlines() or [""])[-1]
			if (OUT.parent / "os_live.txt").exists() else "",
			"stderr": (se or "")[-200:]}
		fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
		fh.flush()
		print(f"  r{n}: {'❌挂' if rec['hang'] else '✅净'} rc={rc} {wall}s "
			f"lit={rec['lit']} reset={rec['resets']} MODE2={rec['mode2']} "
			f"fault={rec['fault_lines']} os={rec['overshoot_count'] or rec['os_tail'] or '-'}", flush=True)
		if rec["hang"]:
			wait_reset(since)
	h = sum(1 for l in open(path) if json.loads(l)["hang"])
	print(f"\n=== {label}: {h}/{rounds} 挂 ===", flush=True)


if __name__ == "__main__":
	main()

#!/usr/bin/env python3
"""envBrdfFactor (scene.slang) 系数复现脚本：split-sum 第二项 BRDF 积分的蒙特卡洛数值积分 + 二维三次拟合。

积分定义：I(F0) = ∫ f_GGX(V,L)·F_schlick·(N·L)dω = F0·A + B
  - f_GGX：GGX D × height-correlated Smith G2（Heitz 闭式 0.5/(NoL·√(NoV²+α²(1−NoV²)) + NoV·√(NoL²+α²(1−NoL²)))）
  - F_schlick = F0 + (1−F0)·(1−VoH)^5 → A = (1−s) 权重积分、B = s 权重积分（s=(1−VoH)^5）
  - 蒙特卡洛：H 按 GGX pdf=D·NoH 重要性采样，Jacobian dωL=4·VoH·dωH → 权重 G2·VoH/(NoV·NoH)
    （G2 用 Karis 形式 0.5/(sv+sl)，其本身 = G2_真实/(4·NoV·NoL)，合并为 2·NoL·VoH/((sv+sl)·NoH)）
  - 镜像极限校验：(NoV=1, rough→0) → A=1, B=0

拟合：29×29 网格（x=NoV、y=rough ∈ [0.05,1]，52 万样本/格）→ 张量积三次（16 系数）最小二乘。
用法：python3 tools/brdf_fit.py（输出 shader 可直接粘贴的 envBrdfA/envBrdfB 多项式）
"""
import numpy as np

N = 524288
rng = np.random.default_rng(777)
u1 = rng.random(N)
u2 = rng.random(N)

def integrate(NoV, rough):
    """返回 (A, B)：GGX height-correlated split-sum 第二项蒙特卡洛积分。"""
    a2 = (rough * rough) ** 2
    cosH = np.sqrt((1 - u1) / ((a2 - 1) * u1 + 1))
    sinH = np.sqrt(np.clip(1 - cosH * cosH, 0, 1))
    phi = 2 * np.pi * u2
    NoV = max(NoV, 1e-4)
    sinV = np.sqrt(1 - NoV * NoV)
    VoH = np.clip(NoV * cosH + sinV * sinH * np.cos(phi), 1e-6, None)
    NoL = 2 * VoH * cosH - NoV
    m = NoL > 0
    NoLm = np.where(m, NoL, 1.0)
    sv = NoLm * np.sqrt(NoV * NoV + a2 * (1 - NoV * NoV))
    sl = NoV * np.sqrt(NoLm * NoLm + a2 * (1 - NoLm * NoLm))
    s = (1 - VoH) ** 5
    w = np.where(m, 2 * NoL * VoH / ((sv + sl) * cosH), 0.0)
    return np.mean(w * (1 - s)), np.mean(w * s)

def main():
    for nv, rgh in [(1.0, 0.02), (0.5, 0.02), (1.0, 0.5), (1.0, 0.74), (1.0, 1.0), (0.1, 1.0), (0.05, 0.3)]:
        A, B = integrate(nv, rgh)
        print(f"# NoV={nv:4.2f} rough={rgh:4.2f}: A={A:.3f} B={B:.3f} coeff(F0=0.04)={0.04*A+B:.4f}")

    n = 29
    xs = np.linspace(0.05, 1.0, n)
    ys = np.linspace(0.05, 1.0, n)
    Ag = np.empty((n, n))
    Bg = np.empty((n, n))
    for i, r in enumerate(ys):
        for j, v in enumerate(xs):
            Ag[i, j], Bg[i, j] = integrate(v, r)

    Vx = np.vander(xs, 4, increasing=True)
    Vy = np.vander(ys, 4, increasing=True)
    M = (Vy[:, None, :, None] * Vx[None, :, None, :]).reshape(n * n, 16)

    def fit(Z, name):
        c, *_ = np.linalg.lstsq(M, Z.reshape(-1), rcond=None)
        err = np.abs((M @ c).reshape(n, n) - Z).max()
        rows = []
        for row in range(4):
            terms = " + ".join(f"{c[row*4+k]:.6f}f" + (f" * x{' * x' * (k-1)}" if k else "") for k in range(4))
            rows.append(f"        ({terms})" + (" * y" * row if row else ""))
        print(f"// {name} 最大误差 {err:.4f}")
        print("    float " + name + " =\n" + " +\n".join(rows) + ";")
        return c

    fit(Ag, "A")
    fit(Bg, "B")

if __name__ == "__main__":
    main()

// selftest_ao_noise.cpp —— **ao-noise 口径层**（`ao_noise_metric.cpp`）的自检（拆文件的缝
// 说明全文见 `selftest.h`）。本 TU 收此前**零覆盖**的算子（债-90 / 债-91 的教训：口径层
// 从 `ao_noise.cpp` 拆出时是「一字未改照搬」，而搬的两条里恰有两条是错的——没有自检的照搬
// 等于把 bug 也照搬进新缝）：
//   · `testAoNoiseMetric` 的 diffFrames 段   —— 帧差三量（changed / mean / mx）的闭式与区域纪律
//   · `testAoNoiseMetric` 的 printAoScanline 段 —— 扫描线的「无定义不出数」纪律与阳性锚点
//   · `testAoNoiseMetric` 的 oidOf / burstDiffFiles 段 —— 输入校验（债-96：认不出的 piece 名
//     返回 0 = 背景、burst 不足 2 帧除零），全部**反向用例** = 喂坏输入必须抛/早退出声
// 全部合成小图直测，不读 PNG、不启引擎（同 `selftest_block.cpp` 那条缝；burstDiffFiles 的
// 阳性锚点写两张 2×2 PNG 到 /tmp，是「不读引擎产物」而非「不碰盘」）。
//
// 每条断言都带**反向用例**（算子在应该无响应的地方必须无响应）与**变异红线**（把实现改坏成
// 哪种具体形态时本用例会红）。
#include "selftest.h"

#include "ao_noise_metric.h"
#include "imgqa.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {

/// 2×2 的 8UC3 合成图，每像素三通道相同。三通道相同 ⟹ BGR2GRAY 是恒等映射
/// （OpenCV 定点式 (v·4899 + v·9617 + v·1868 + 8192) >> 14 = (v·16384 + 8192) >> 14 = v），
/// 灰度均值可以按输入字节**闭式**写死，不需要再抄一遍 OpenCV 的取整口径。
cv::Mat grayQuad(uchar a, uchar b, uchar d, uchar e)
{
    const cv::Mat g = (cv::Mat_<uchar>(2, 2) << a, b, d, e);
    cv::Mat o;
    cv::cvtColor(g, o, cv::COLOR_GRAY2BGR);
    return o;
}

}  // namespace

void testAoNoiseMetric(Checker& c)
{
    // ── diffFrames（债-90：meanStdDev 把 mean 与 stddev 写进**同一个 Mat**，
    //    第二次写覆盖第一次 ⟹ 拿到的「mean」其实是 stddev）──────────────────
    // 合成图 [0 10; 0 30] vs 全零：闭式灰度均值 = (0+10+0+30)/4 = 10.0；
    // 改前实得 = 总体标准差 √150 ≈ 12.247（正是本断言的区分力来源）。
    {
        const cv::Mat a = grayQuad(0, 10, 0, 30);
        const cv::Mat b = cv::Mat::zeros(2, 2, CV_8UC3);
        const cv::Mat all = cv::Mat(2, 2, CV_8UC1, cv::Scalar(255));
        const FrameDiff d = diffFrames(a, b, all);
        c.chk("diffFrames·mask 全覆盖均值 = 闭式 10.0（债-90 改前实得 ≈12.247 = stddev）",
            std::abs(d.mean - 10.0) < 1e-9, d3(d.mean));
        c.chk("diffFrames·changed = 闭式 2（灰度非零像素数：10 与 30，两个 0 不算）",
            d.changed == 2, std::to_string(d.changed));
        c.chk("diffFrames·mx = 闭式 30", d.mx == 30.0, d3(d.mx));
        // 含零填充的**重归一化**口径：mask 只盖 (0,10,0) 三像素 ⟹ mask 内均值 = 10/3。
        // 变异红线：×total ÷countNonZero(mask) 两个操作数删任一个 → 本断言红。
        const cv::Mat m3 = (cv::Mat_<uchar>(2, 2) << 255, 255, 255, 0);
        const FrameDiff p = diffFrames(a, b, m3);
        c.chk("diffFrames·部分 mask 均值 = 闭式 10/3（钉住含零填充的重归一化式）",
            std::abs(p.mean - 10.0 / 3.0) < 1e-9, d3(p.mean));
        c.chk("diffFrames·部分 mask 的 changed = 闭式 1（mask 外再大也不算）",
            p.changed == 1, std::to_string(p.changed));
        // 两分支同口径：mask 空分支（cv::mean 直取）与全覆盖 mask 在全图上必须同值
        // （变异红线：若只修一个分支、两分支口径分家 → 本断言红）。
        const FrameDiff f = diffFrames(a, b);
        c.chk("diffFrames·mask 空分支与全覆盖分支同口径（全图均值同为 10.0）",
            std::abs(f.mean - d.mean) < 1e-12 && std::abs(f.mean - 10.0) < 1e-9, d3(f.mean));
        // 反向用例：同帧之差必须**全零**（不是「接近零」）——算子在无响应处不许动。
        const FrameDiff z = diffFrames(a, a, all);
        c.chk("diffFrames·同帧差全零（changed/mean/mx 三量都是）",
            z.changed == 0 && z.mean == 0.0 && z.mx == 0.0, d3(z.mean));
    }

    // ── printAoScanline（债-91：拉普拉斯能量恒 0 时 bestRow 带着 −1 去 ao.ptr(−1) 越界读，
    //    读出来的鬼数照常打印）──────────────────────────────────────────────
    {
        // 全平输入（AO 场恒 255 = 反向用例的真实形态）：扫描线**无定义** ⟹ 不出数
        // （输出里没有那行数值）且必须给提示（静默 = 坏量具与「没数据」不可区分）。
        // 变异红线：越界读改「出个 0」或「出 NaN」而非「不出数」→ 第一条红。
        const cv::Mat flat = cv::Mat(24, 32, CV_8UC1, cv::Scalar(255));
        const std::string out = captureStdout([&] { printAoScanline(flat); });
        c.chk("扫描线·全平输入不出数值行（债-91 改前带 bestRow=−1 越界读印鬼数）",
            out.find("扫描线 y=") == std::string::npos, "[" + out + "]");
        c.chk("扫描线·全平输入必须给「AO 场全平」提示（静默 = 判据失效不出声）",
            out.find("AO 场全平") != std::string::npos, "[" + out + "]");
        // 阳性锚点：单行阶跃（row 8 = 0，其余 255）⟹ 扫描行闭式 = y=8（采样网格 y+=4 上
        // 只有它能量非零），正常路径改后**仍要出数**——堵「把越界读改成一律不出数」。
        cv::Mat edge = cv::Mat(24, 32, CV_8UC1, cv::Scalar(255));
        edge.row(8).setTo(0);
        const std::string on = captureStdout([&] { printAoScanline(edge); });
        c.chk("扫描线·阳性锚点：row8 阶跃 → 扫描线 y=8（闭式最大能量行，正常路径仍出数）",
            on.find("扫描线 y=8") != std::string::npos, "[" + on + "]");
    }

    // ── oidOf（债-96：认不出 piece 名返回 0 = 背景 ⟹ `--glb` 换靶后锚点在「背景 vs 背景」
    //    上求值，判据静默失效、不报错也不告警）────────────────────────────────
    {
        Collected col;
        col.oidByName = {{"floor", 1}, {"box_tall", 2}};
        c.chk("oidOf·已知 piece 返回自己的 oid（阳性锚点，堵「改成抛后连认出的也抛」）",
            oidOf(col, "floor") == 1 && oidOf(col, "box_tall") == 2);
        bool threw = false;
        int32_t got = -1;
        std::string msg;
        try {
            got = oidOf(col, "sphere");
        } catch (const std::exception& e) {
            threw = true;
            msg = e.what();
        }
        // 改前的红输出 extra = 「returned oid=0」——静默返回 0（背景）本身就是缺陷形态。
        c.chk("oidOf·未知 piece 名必须抛（改前静默返回 0 = 背景）", threw,
            "returned oid=" + std::to_string(got));
        c.chk("oidOf·异常信息带认出的 piece 名清单（换靶的人一眼看到该写什么）",
            threw && msg.find("floor") != std::string::npos
                && msg.find("box_tall") != std::string::npos,
            msg);
    }

    // ── burstDiffFiles（债-96：假定 /shotburst 至少 2 帧 ⟹ 部分失败时 files[1] 越界 +
    //    files.size() − 1 下溢成 0 = 整数除零。正常路径走不到，坏在「部分失败」那天）───
    {
        // 帧数不足的两档（0 / 1）：不许崩、返回值全零（「0 对帧」不许伪装成「测到了 0 噪声」）、
        // 且必须**出声**（stdout 里要有带帧数的那行——静默早退与「测到了没噪声」不可区分）。
        const auto runShort = [](size_t n, bool& threwOut,
                                   std::pair<std::array<FrameDiff, 3>, int64_t>& resOut) {
            std::vector<std::string> files;
            for (size_t i = 0; i < n; ++i)
                files.push_back("/tmp/imgqa_selftest_burst_不存在_" + std::to_string(i) + ".png");
            return captureStdout([&] {
                try {
                    resOut = burstDiffFiles(files, cv::Mat(), cv::Mat(), "selftest_short", "/tmp");
                } catch (...) {
                    threwOut = true;
                }
            });
        };
        const auto allZero = [](const std::pair<std::array<FrameDiff, 3>, int64_t>& r) {
            for (const FrameDiff& d : r.first)
                if (d.changed != 0 || d.mean != 0.0 || d.mx != 0.0)
                    return false;
            return r.second == 0;
        };
        bool threw0 = false;
        std::pair<std::array<FrameDiff, 3>, int64_t> res0;
        const std::string out0 = runShort(0, threw0, res0);
        c.chk("burstDiffFiles·0 帧不抛且全零返回（改前 files[0] 越界读 + 除零）",
            !threw0 && allZero(res0), "[" + out0 + "]");
        c.chk("burstDiffFiles·0 帧早退必须出声（说清拿到几帧）", out0.find("帧") != std::string::npos,
            "[" + out0 + "]");
        bool threw1 = false;
        std::pair<std::array<FrameDiff, 3>, int64_t> res1;
        const std::string out1 = runShort(1, threw1, res1);
        c.chk("burstDiffFiles·1 帧不抛且全零返回（改前 files.size()−1 下溢 = 整数除零崩进程）",
            !threw1 && allZero(res1), "[" + out1 + "]");
        c.chk("burstDiffFiles·1 帧早退必须出声（静默 = 坏量具被读成「没噪声」）",
            out1.find("帧") != std::string::npos, "[" + out1 + "]");

        // 阳性锚点：两张 2×2 PNG（[0 10;0 30] vs 全零）的闭式——changed=2 / mean=10 / mx=30，
        // 契约区（全覆盖）同全图，接触环（空 mask）恒零。堵「加早退守卫后正常路径也不出数」。
        namespace fs = std::filesystem;
        const std::string dir = "/tmp/imgqa_selftest_burst";
        std::error_code ec;
        fs::create_directories(dir, ec);
        cv::imwrite(dir + "/f0.png", grayQuad(0, 10, 0, 30));
        cv::imwrite(dir + "/f1.png", cv::Mat::zeros(2, 2, CV_8UC3));
        const auto r2 = burstDiffFiles({dir + "/f0.png", dir + "/f1.png"},
            cv::Mat(2, 2, CV_8UC1, cv::Scalar(255)), cv::Mat::zeros(2, 2, CV_8UC1),
            "selftest_ok", dir);
        c.chk("burstDiffFiles·阳性锚点：2 帧 1 对差的闭式（changed=2 / mean=10 / 空环恒零 / 全图合计 2）",
            r2.first[0].changed == 2 && std::abs(r2.first[0].mean - 10.0) < 1e-9
                && r2.first[0].mx == 30.0 && r2.first[1].changed == 2
                && r2.first[2].changed == 0 && r2.first[2].mean == 0.0 && r2.second == 2,
            "changed=" + std::to_string(r2.first[0].changed) + " mean=" + d3(r2.first[0].mean));
    }
}

}  // namespace RGL::imgqa

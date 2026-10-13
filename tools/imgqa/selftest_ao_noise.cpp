// selftest_ao_noise.cpp —— **ao-noise 口径层**（`ao_noise_metric.cpp`）的自检（拆文件的缝
// 说明全文见 `selftest.h`）。本 TU 收两台此前**零覆盖**的算子（债-90 / 债-91 的教训：口径层
// 从 `ao_noise.cpp` 拆出时是「一字未改照搬」，而搬的两条里恰有两条是错的——没有自检的照搬
// 等于把 bug 也照搬进新缝）：
//   · `testAoNoiseMetric` 的 diffFrames 段   —— 帧差三量（changed / mean / mx）的闭式与区域纪律
//   · `testAoNoiseMetric` 的 printAoScanline 段 —— 扫描线的「无定义不出数」纪律与阳性锚点
// 全部合成小图直测，不读 PNG、不启引擎（同 `selftest_block.cpp` 那条缝）。
//
// 每条断言都带**反向用例**（算子在应该无响应的地方必须无响应）与**变异红线**（把实现改坏成
// 哪种具体形态时本用例会红）。
#include "selftest.h"

#include "ao_noise_metric.h"
#include "imgqa.h"

#include <cmath>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>
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
}

}  // namespace RGL::imgqa

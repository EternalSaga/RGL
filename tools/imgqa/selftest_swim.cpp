// selftest_swim.cpp —— **taa-swim 仪器层**（`temporal_swim.cpp`）的输出面自检（拆文件的缝
// 说明全文见 `selftest.h`）。本 TU 收读数表打印面（债-94）：
//   · `testSwimLagTableSurface` —— lagTable 的**输出行数稳定性**与逐 lag 行集合。
//
// 为什么这条判据只能长在 stdout 上：lagTable 的产物**就是那张表**（打印顺序与逐字文案 =
// 契约，见 `temporal_swim.h` 头注释），台账与逐行/逐字节下游判据读的都是它——返回值只有
// 汇总两量，从返回值断言测不到「行数是彩票」这个缺陷本体。合成帧直测，不启引擎。
//
// 每条断言都带**变异红线**（把实现改坏成哪种具体形态时本用例会红）。
#include "selftest.h"

#include "temporal_swim.h"
#include "imgqa.h"

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

// 抖动周期取引擎真值（与 temporal_swim.cpp 同源；手抄 16 = 第二份真相）。
import taa_jitter;

namespace RGL::imgqa {

namespace {

/// 合成连拍：n 张 32×32 8UC3 渐变底图。`perturb` = 每帧给**互不相同**的一个像素 +1 ⟹
/// 任意两帧之差非零但极小（meas 从「恰为 0.0」变成「微小正数」，正是债-94 的噪声形态）。
std::vector<cv::Mat> synthFrames(int n, bool perturb)
{
    std::vector<cv::Mat> frames;
    frames.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        cv::Mat f(32, 32, CV_8UC3);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                f.at<cv::Vec3b>(y, x) = cv::Vec3b(
                    static_cast<uchar>(8 * x), static_cast<uchar>(8 * y),
                    static_cast<uchar>(4 * (x + y)));
        if (perturb) {
            auto& px = f.at<cv::Vec3b>(i % 32, 0);
            px[0] = static_cast<uchar>(px[0] + 1);
        }
        frames.push_back(f);
    }
    return frames;
}

/// 从 lagTable 输出里抽出**数据行的 lag 值序列**。数据行 = `  <数字>` 开头；组名行与
/// 「→ 汇总」行第 3 个字符不是数字，天然被排除（行前缀是打印契约的一部分）。
std::vector<int> printedLags(const std::string& out)
{
    std::vector<int> lags;
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t eol = out.find('\n', pos);
        const std::string line = out.substr(pos, eol == std::string::npos ? eol : eol - pos);
        if (line.size() > 3 && line[0] == ' ' && line[1] == ' '
            && static_cast<unsigned char>(line[2]) >= '0'
            && static_cast<unsigned char>(line[2]) <= '9')
            lags.push_back(std::stoi(line.substr(2)));
        if (eol == std::string::npos)
            break;
        pos = eol + 1;
    }
    return lags;
}

int lineCount(const std::string& out)
{
    int n = 0;
    for (char ch : out)
        n += ch == '\n';
    return n;
}

}  // namespace

void testSwimLagTableSurface(Checker& c)
{
    // 债-94：跳过条件写成「实测浮点精确相等」（`s.meas == 0.0 && s.pred == 0.0 && k > 1`）
    // ⟹ k = 周期那行的去留随噪声变（台账实测同一份代码两轮分别印 15 行与 16 行）。
    // 修法 = 按语义跳（k == 抖动周期）。锚点形态 = 同一段表喂「逐帧相同」与「微小扰动」
    // 两组 frames：改前 meas(k=16) 一组恰 0、一组微小正 ⟹ 行数 17 vs 18 红；改后恒 17。
    const int period = static_cast<int>(kTaaJitterPeriod);
    const auto still = synthFrames(period + 1, false);
    const auto noisy = synthFrames(period + 1, true);
    const std::string a = captureStdout([&] { lagTable("合成·逐帧相同", still, 1.0, 0u); });
    const std::string b = captureStdout([&] { lagTable("合成·微小扰动", noisy, 1.0, 0u); });

    c.chk("lagTable·输出行数不随实测噪声变（债-94 改前 17 vs 18 行彩票）",
        lineCount(a) == lineCount(b),
        std::to_string(lineCount(a)) + " vs " + std::to_string(lineCount(b)));
    // 阳性锚点：数据行**恰为 lag 1..15**（k=16 = 免费标定行，按语义恒不进表——与 meas
    // 取值无关）。变异红线：跳过条件若写成 k>=period+1（永不跳）→ 两组都 16 行、本断言红；
    // 写成 k>1 全跳 → 数据行塌空、也红。
    std::vector<int> want;
    for (int k = 1; k < period; ++k)
        want.push_back(k);
    c.chk("lagTable·数据行恰为 lag 1..15（k=16 按语义跳，不按实测值跳）",
        printedLags(a) == want && printedLags(b) == want,
        "A=" + std::to_string(printedLags(a).size()) + " 行 B="
            + std::to_string(printedLags(b).size()) + " 行");
    // 汇总行必须在场（行数契约 = 组名 1 + 数据 15 + 汇总 1；汇总行丢了上面两条会一起漂）。
    c.chk("lagTable·汇总行在场且只有一行（两组都是）",
        a.find("→ 汇总") != std::string::npos && b.find("→ 汇总") != std::string::npos
            && a.find("→ 汇总") == a.rfind("→ 汇总") && b.find("→ 汇总") == b.rfind("→ 汇总"));
}

}  // namespace RGL::imgqa

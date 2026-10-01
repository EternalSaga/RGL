# 参考资料 — HFTS / FTIZB / IZB（批 11.3）

> 状态：现行（2026-10-01 建）。批 11.3 实施期间的离线参考文献，配套权威计划 =
> [`../hfts_izb_plan.md`](../hfts_izb_plan.md)。阅读顺序按下方编号（族谱时间序）。
> 全部为公开渠道下载；商用再分发限制未核验，**仅仓库内部学习用**。

## 索引（按推荐阅读顺序）

| # | 文件 | 文献 | 定位与看点 |
|---|---|---|---|
| 1 | `IZB-2004-TR0409-Johnson-Mark-Burns-shadowmapping.pdf` | Johnson/Mark/Burns TR-04-09 (UT Austin, 2004) | IZB 算法本源（期刊版 TOG 24(4) 2005 的前身，内容同源）。链表 A-buffer 数据结构定义 |
| 2 | `FTIZB-2015-I3D-cameraReady.pdf` | Wyman/Hoetzlein/Lefohn, I3D 2015（Best Paper） | **最重要**——六 pass 流水线、伪代码、优化收益表（计划 §1.2/§4.2 的出处）。1c–1e 实现按它 |
| 3 | `FTIZB-2016-TVCG-extended.pdf` | 同系统期刊扩展版 (TVCG 2016) | 数据布局细节：节点预分配、地址即像素索引（izb 节点内存布局照这版） |
| 4 | `HFTS-2016-SIGGRAPH-talk.pdf` | Story & Wyman, SIGGRAPH 2016 Talks | The Division 实装：IZB 硬影↔PCSS 远场混合、per-pixel occluder distance 过渡（阶段 3 HFTS 混合的依据） |
| 5 | `Tokuyoshi-2018-Conservative-Z-Prepass.pdf` | Tokuyoshi & Mizokuchi, SIGGRAPH 2018 Posters (Square Enix) | 性能最大单项：保守 shadow map 预剔除全影节点（12.4→2.12 ms，约 6×）；「texel 全覆盖」判定 = 四角重心坐标（阶段 2 排期第一） |
| 6 | `Tokuyoshi-2018-poster.pdf` + `Tokuyoshi-2018-supplemental.pdf` | 同上（poster + 补充实验） | supplemental 有额外数据点，正文放不下才看 |
| 7 | `AliasFreeHardShadows-Hybrid-EG09.pdf` | Wyman & Steed, Eurographics 2009 | HFTS/FTIZB 的思想前身（混合光栅管线出无走样硬影）；软件保守光栅的早期路线——**fallback 参考** |

## 未下载项（原因备案）

- **IZB 2005 期刊版**（ACM TOG 24(4), doi:10.1145/1095878.1095889）：ACM DL 收费墙；
  内容与 #1 技术报告同源（报告版还更详细），不必追。
- **Hasselgren/Akenine-Möller 保守光栅原文**：原始形态是 GPU Gems 2 第 18 章
  （gpugems.com 免费在线全文）；软件保守光栅 fallback 真触发时再存文本。
- **NVIDIA ShadowWorks**：闭源无 PDF；结论已在计划 §2（不可抄）。
- **izb-on / Godot proposal**：弃坑/关闭的 GitHub 仓库，无正文价值，git 历史考古可查。

## 重新下载命令（2026-10-01 实测有效；cwyman.org 需浏览器 UA）

```bash
UA="Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0"
curl -sL -A "$UA" -o FTIZB-2015-I3D-cameraReady.pdf \
  "https://research.nvidia.com/sites/default/files/pubs/2015-02_Frustum-Traced-Raster-Shadows/ftizb_cameraReady.pdf"
curl -sL -A "$UA" -o FTIZB-2016-TVCG-extended.pdf https://cwyman.org/papers/tvcg16_ftizbExtended.pdf
curl -sL -A "$UA" -o HFTS-2016-SIGGRAPH-talk.pdf  https://cwyman.org/abstracts/sig2016_hfts.pdf
curl -sL -A "$UA" -o IZB-2004-TR0409-Johnson-Mark-Burns-shadowmapping.pdf \
  "https://www.cs.utexas.edu/ftp/techreports/tr04-09.pdf"   # 注意小写 tr04-09
curl -sL -A "$UA" -o Tokuyoshi-2018-Conservative-Z-Prepass.pdf \
  "https://www.jp.square-enix.com/tech/library/pdf/Conservative%20Z-Prepass%20for%20Frustum-Traced%20Irregular%20Z-Buffers.pdf"
# 同目录还有 "…(poster).pdf" 与 "…(supplemental).pdf" 两变体
curl -sL -A "$UA" -o AliasFreeHardShadows-Hybrid-EG09.pdf \
  "https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/A_Hybrid_GPU_Rendering_Pipeline/eg_area09_shadows.pdf"
```

md5（2026-10-01 抓取件）：

```
a0bd84d8074c896242480ef81daf0e62  IZB-2004-TR0409-Johnson-Mark-Burns-shadowmapping.pdf
fe72e3b65cb180c3e9e372dbcf2487d9  FTIZB-2015-I3D-cameraReady.pdf
5fc9a4e75a549f194a8e0b8b4b4ff74c  FTIZB-2016-TVCG-extended.pdf
db8e282af48fea7c1d1cc18f7be0d777  HFTS-2016-SIGGRAPH-talk.pdf
3d068c537e63576d5519c5d287674021  Tokuyoshi-2018-Conservative-Z-Prepass.pdf
2d6e611f7f7feac47e75a1fe92d24ed5  Tokuyoshi-2018-poster.pdf
27fbb4c368797c43d9f577d59b4cdbc9  Tokuyoshi-2018-supplemental.pdf
5dcc06f3b183909661e90b933bcc707d  AliasFreeHardShadows-Hybrid-EG09.pdf
```

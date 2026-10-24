// NO SIGNAL 遮罩的绘制。
//
// 【为什么会有这个文件】
//
// 以前只有导出端会画 NO SIGNAL（Exporter.cpp 的 FillNoSignal，NV12 画布）：
// 录制端在没有画面时**干脆不喂帧**，于是时间轴停在"最后一帧"。后果是一串
// 用户能直接看到的问题：
//
//   * 关掉被录的窗口 → 录制器被 Stop 掉、时间轴不动 → 导出的回放窗口是
//     「最后内容往前 N 秒」，整段都是关窗前的画面，永远没有 NO SIGNAL；
//   * 录制器没了 → ExportNow 直接 return false → 「没画面就不让导出」。
//
// 现在改成录制端在没画面时**继续按帧率喂一张 NO SIGNAL**，时间轴一直走到
// 现在。预览、分片、导出三口吃的都是同一批像素，不会再出现"预览是 NO SIGNAL、
// 导出却有画面"这种自相矛盾。
//
// 所以这份绘制要在采集线程里跑（BGRA，每拍 memcpy 一次），必须便宜：
// 整块生成一次之后缓存复用，只有尺寸变了才重画（见 Session 的 noSignalBuf_）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shadow {

// ---------------------------------------------------------------------------
// 5x7 点阵字
//
// 【为什么不引字体】只要 8 个字母。走 GDI 要链 gdi32、还得处理 DPI/ClearType
// 彩色边缘（贴到视频上会带彩边），代码量比这张常量表大十倍。
// 每个字符 7 行，每行低 5 位 = 5 个像素，最高位是最左边那个。
// ---------------------------------------------------------------------------
struct NoSignalGlyph {
    char    ch;
    uint8_t rows[7];
};

inline const NoSignalGlyph kNoSignalGlyphs[] = {
    {'N', {0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
};

inline const uint8_t* NoSignalGlyphOf(char c) {
    for (const NoSignalGlyph& g : kNoSignalGlyphs) {
        if (g.ch == c) return g.rows;
    }
    return nullptr; // 空格等没有字形的字符
}

/**
 * 把一块 BGRA 画布涂成黑底 + 居中的 "NO SIGNAL"。
 *
 * 采集链路的像素格式就是 BGRA（和共享环、编码器输入一致），所以这里不转
 * 任何格式，直接把每个像素写成 0xFF000000（不透明黑）或白字。
 *
 * stride 是一行的字节数（= width * 4，但显式传，别假设）。
 */
inline void DrawNoSignalBgra(uint8_t* buf, uint32_t width, uint32_t height, size_t stride) {
    if (!buf || width == 0 || height == 0 || stride < static_cast<size_t>(width) * 4u) return;

    // 黑底：一次按行 memset（每像素 4 字节，全 0 等于不透明黑）。
    std::memset(buf, 0, stride * static_cast<size_t>(height));

    constexpr char kText[] = "NO SIGNAL";
    constexpr uint32_t kChars = sizeof(kText) - 1;
    constexpr uint32_t kAdvance = 6; // 5 像素字形 + 1 像素字间距

    // 字号跟着画布高度走，和导出端保持同一套比例（1080p 约 12 倍）。
    uint32_t scale = height / 90u;
    if (scale < 2) scale = 2;

    const uint32_t textW = (kChars * kAdvance - 1) * scale;
    const uint32_t textH = 7u * scale;
    if (textW >= width || textH >= height) return; // 画布小到放不下字，留黑底就好

    const uint32_t originX = (width - textW) / 2u;
    const uint32_t originY = (height - textH) / 2u;
    // **白色**（#F0F0F0 + 不透明）。用户明确要求过：「no signal 不应该是灰色的，
    // 应该是白色的」。这里原来写的是 0x3A 暗灰，录进分片里就是一条发暗的字，
    // 和预览那层 DOM 遮罩（浅蓝白 #d6e6fb）对不上，也和导出端补的那张
    // （Exporter.cpp 的 FillNoSignal）不一个颜色 —— 同一支视频里 "没录到"
    // 的那几秒会出现两种深浅不同的 NO SIGNAL，看着像两个功能。
    //
    // BGRA 三通道同值 = 中性灰阶，0xF0 解码出来接近纯白。
    constexpr uint8_t kTone = 0xF0;

    for (uint32_t ci = 0; ci < kChars; ++ci) {
        const uint8_t* rows = NoSignalGlyphOf(kText[ci]);
        if (!rows) continue;
        const uint32_t cellX = originX + ci * kAdvance * scale;
        for (uint32_t gy = 0; gy < 7; ++gy) {
            const uint8_t bits = rows[gy];
            for (uint32_t gx = 0; gx < 5; ++gx) {
                if ((bits & (0x10u >> gx)) == 0) continue;
                const uint32_t px0 = cellX + gx * scale;
                const uint32_t py0 = originY + gy * scale;
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    uint8_t* row = buf + static_cast<size_t>(py0 + sy) * stride +
                                   static_cast<size_t>(px0) * 4u;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        row[0] = kTone;      // B
                        row[1] = kTone;      // G
                        row[2] = kTone;      // R
                        row[3] = 0xFF;       // A
                        row += 4;
                    }
                }
            }
        }
    }
}

} // namespace shadow

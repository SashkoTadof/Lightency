#include "icon_gen.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <memory>
#include <type_traits>

namespace Lightency {

struct GdiObjectDeleter {
    void operator()(HGDIOBJ obj) const { if (obj) DeleteObject(obj); }
};
using UniqueHBITMAP = std::unique_ptr<std::remove_pointer_t<HBITMAP>, GdiObjectDeleter>;

HICON IconGenerator::CreateMinimalistIcon(int size) {
    if (size <= 0) return nullptr;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdcScreen = GetDC(nullptr);
    uint32_t* pPixels = nullptr;
    HBITMAP rawColorBitmap = CreateDIBSection(
        hdcScreen, reinterpret_cast<BITMAPINFO*>(&bi),
        DIB_RGB_COLORS, reinterpret_cast<void**>(&pPixels), nullptr, 0
    );
    ReleaseDC(nullptr, hdcScreen);
    UniqueHBITMAP hColorBitmap(rawColorBitmap);

    if (!hColorBitmap || !pPixels) return nullptr;

    std::fill_n(pPixels, size * size, 0);

    const float cx = (size - 1) / 2.0f;
    const float cy = (size - 1) / 2.0f;
    const float scale = size * 0.46f;
    const float inv_scale = 1.0f / scale;

    constexpr int samples = 3;
    constexpr float step = 1.0f / static_cast<float>(samples);
    constexpr float num_samples = static_cast<float>(samples * samples);

    constexpr float p = 0.54f;
    constexpr float inv_p = 1.0f / p;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float total_r = 0.0f, total_g = 0.0f, total_b = 0.0f, total_a = 0.0f;

            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    float px = x + (sx + 0.5f) * step;
                    float py = y + (sy + 0.5f) * step;

                    float nx = (px - cx) * inv_scale;
                    float ny = (py - cy) * inv_scale;

                    float ax = std::abs(nx);
                    float ay = std::abs(ny);

                    float dist_star = (ax > 0.0f || ay > 0.0f) ? std::pow(std::pow(ax, p) + std::pow(ay, p), inv_p) : 0.0f;

                    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;

                    if (dist_star < 1.0f) {
                        if (dist_star < 0.28f) {
                            r = 255.0f; g = 255.0f; b = 255.0f;
                            a = 1.0f;
                        } else {
                            float t = (dist_star - 0.28f) / 0.72f;
                            r = 255.0f * (1.0f - t * 0.95f);
                            g = 255.0f * (1.0f - t * 0.22f);
                            b = 255.0f;
                            a = (1.0f - t) * 0.95f + 0.05f;
                        }
                    }

                    float rDist = std::hypot(nx, ny);
                    if (rDist < 0.70f) {
                        float glow_a = (0.70f - rDist) / 0.70f * 0.45f;
                        if (glow_a > a) {
                            r = 0.0f; g = 190.0f; b = 255.0f;
                            a = glow_a;
                        }
                    }

                    total_r += r * a;
                    total_g += g * a;
                    total_b += b * a;
                    total_a += a;
                }
            }

            float avg_a = total_a / num_samples;
            if (avg_a > 0.001f) {
                float avg_r = total_r / total_a;
                float avg_g = total_g / total_a;
                float avg_b = total_b / total_a;

                uint8_t aByte = static_cast<uint8_t>(std::clamp(avg_a * 255.0f, 0.0f, 255.0f));
                uint32_t pr = static_cast<uint32_t>(std::clamp((avg_r * aByte) / 255.0f, 0.0f, 255.0f));
                uint32_t pg = static_cast<uint32_t>(std::clamp((avg_g * aByte) / 255.0f, 0.0f, 255.0f));
                uint32_t pb = static_cast<uint32_t>(std::clamp((avg_b * aByte) / 255.0f, 0.0f, 255.0f));

                pPixels[y * size + x] = (aByte << 24) | (pr << 16) | (pg << 8) | pb;
            }
        }
    }

    UniqueHBITMAP hMaskBitmap(CreateBitmap(size, size, 1, 1, nullptr));
    if (!hMaskBitmap) return nullptr;

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hColorBitmap.get();
    ii.hbmMask = hMaskBitmap.get();

    return CreateIconIndirect(&ii);
}

}
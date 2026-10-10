#include "TextRender.h"

#include <windows.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cstring>
#include <cwchar>
using std::min;
using std::max;
#include <gdiplus.h>

namespace {
std::once_flag g_gdiOnce;
ULONG_PTR g_gdiToken = 0;
std::mutex g_gdiMutex;

void EnsureGdi()
{
    std::call_once(g_gdiOnce, [] {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&g_gdiToken, &in, nullptr);
    });
}
} // namespace

namespace render {

const std::vector<std::wstring>& InstalledFonts()
{
    static std::vector<std::wstring> fonts;
    static std::once_flag once;
    std::call_once(once, [] {
        EnsureGdi();
        std::lock_guard<std::mutex> lk(g_gdiMutex);
        Gdiplus::InstalledFontCollection col;
        int count = col.GetFamilyCount();
        if (count > 0) {
            std::unique_ptr<Gdiplus::FontFamily[]> fam(new Gdiplus::FontFamily[count]);
            int found = 0;
            col.GetFamilies(count, fam.get(), &found);
            for (int i = 0; i < found; i++) {
                WCHAR name[LF_FACESIZE] = {0};
                fam[i].GetFamilyName(name);
                if (name[0]) fonts.push_back(name);
            }
        }
        std::sort(fonts.begin(), fonts.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        fonts.erase(std::unique(fonts.begin(), fonts.end(), [](const std::wstring& a, const std::wstring& b) {
                        return _wcsicmp(a.c_str(), b.c_str()) == 0;
                    }),
                    fonts.end());
        if (fonts.empty()) fonts.push_back(L"Arial");
    });
    return fonts;
}

bool RenderText(const std::wstring& text, const Style& st, int w, int h,
                std::vector<unsigned char>& out)
{
    using namespace Gdiplus;
    if (w <= 0 || h <= 0) return false;
    EnsureGdi();
    std::lock_guard<std::mutex> lk(g_gdiMutex);

    out.assign((size_t)w * (size_t)h * 4, 0);
    if (text.empty()) return true;

    // Separar en lineas
    std::vector<std::wstring> lines;
    {
        size_t s = 0;
        for (;;) {
            size_t p = text.find(L'\n', s);
            std::wstring ln = (p == std::wstring::npos) ? text.substr(s) : text.substr(s, p - s);
            while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
            lines.push_back(ln);
            if (p == std::wstring::npos) break;
            s = p + 1;
        }
    }

    Bitmap bmp(w, h, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    std::unique_ptr<FontFamily> fam(new FontFamily(st.font.c_str()));
    if (fam->GetLastStatus() != Ok) fam.reset(new FontFamily(L"Arial"));

    int wanted = FontStyleRegular;
    if (st.bold) wanted |= FontStyleBold;
    if (st.italic) wanted |= FontStyleItalic;
    int candidates[5] = {wanted, FontStyleRegular, FontStyleBold, FontStyleItalic,
                         FontStyleBold | FontStyleItalic};
    int style = FontStyleRegular;
    for (int c : candidates) {
        if (fam->IsStyleAvailable(c)) { style = c; break; }
    }

    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces |
                       StringFormatFlagsNoWrap);

    const float margin = (float)w * 0.04f;
    const float avail = (float)w - 2.0f * margin;

    auto measure = [&](const Font& f, const std::wstring& s) -> float {
        if (s.empty()) return 0.0f;
        RectF layout(0, 0, 100000.0f, 100000.0f), box;
        g.MeasureString(s.c_str(), (INT)s.size(), &f, layout, &fmt, &box);
        return box.Width;
    };

    float size = st.sizePx > 4.0f ? st.sizePx : 4.0f;
    std::unique_ptr<Font> font(new Font(fam.get(), size, style, UnitPixel));
    float maxW = 0.0f;
    for (auto& l : lines) { float lw = measure(*font, l); if (lw > maxW) maxW = lw; }
    if (maxW > avail && maxW > 0.0f) {
        size = size * avail / maxW; // auto-ajuste para que quepa
        font.reset(new Font(fam.get(), size, style, UnitPixel));
    }

    const float lineH = font->GetHeight(&g);
    const float step = lineH * st.lineSpacing;
    const float blockH = step * (float)(lines.size() - 1) + lineH;
    float top;
    float room = (float)h - blockH - 2.0f * margin;
    if (room < 0.0f) top = ((float)h - blockH) * 0.5f;
    else top = margin + room * (1.0f - st.vertical);

    SolidBrush brush(Color(255, 255, 255, 255));
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        float lw = measure(*font, lines[i]);
        float x;
        if (st.align == 0) x = margin;
        else if (st.align == 2) x = (float)w - margin - lw;
        else x = ((float)w - lw) * 0.5f;
        g.DrawString(lines[i].c_str(), (INT)lines[i].size(), font.get(),
                     PointF(x, top + step * (float)i), &fmt, &brush);
    }

    BitmapData bd;
    Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    for (int y = 0; y < h; y++) {
        std::memcpy(&out[(size_t)y * (size_t)w * 4], (BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride,
                    (size_t)w * 4);
    }
    bmp.UnlockBits(&bd);
    return true;
}

} // namespace render

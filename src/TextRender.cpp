#include "TextRender.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
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

BYTE B8(float v)
{
    int x = (int)(v * 255.0f + 0.5f);
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return (BYTE)x;
}

// Desenfoque de caja horizontal (fuera de la imagen = 0)
void BoxBlurH(const float* src, float* dst, int w, int h, int r)
{
    const float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++) {
        const float* s = src + (size_t)y * w;
        float* d = dst + (size_t)y * w;
        float acc = 0.0f;
        for (int i = 0; i <= r && i < w; i++) acc += s[i];
        for (int x = 0; x < w; x++) {
            d[x] = acc * inv;
            int addI = x + r + 1, subI = x - r;
            if (addI < w) acc += s[addI];
            if (subI >= 0) acc -= s[subI];
        }
    }
}

// Desenfoque de caja vertical
void BoxBlurV(const float* src, float* dst, int w, int h, int r)
{
    const float inv = 1.0f / (float)(2 * r + 1);
    std::vector<float> acc((size_t)w, 0.0f);
    for (int i = 0; i <= r && i < h; i++) {
        const float* s = src + (size_t)i * w;
        for (int x = 0; x < w; x++) acc[(size_t)x] += s[x];
    }
    for (int y = 0; y < h; y++) {
        float* d = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) d[x] = acc[(size_t)x] * inv;
        int addY = y + r + 1, subY = y - r;
        if (addY < h) {
            const float* s = src + (size_t)addY * w;
            for (int x = 0; x < w; x++) acc[(size_t)x] += s[x];
        }
        if (subY >= 0) {
            const float* s = src + (size_t)subY * w;
            for (int x = 0; x < w; x++) acc[(size_t)x] -= s[x];
        }
    }
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
                std::vector<unsigned char>& out, Result* res)
{
    using namespace Gdiplus;
    if (res) *res = Result();
    if (w <= 0 || h <= 0) return false;
    EnsureGdi();
    std::lock_guard<std::mutex> lk(g_gdiMutex);

    const size_t npx = (size_t)w * (size_t)h;
    out.assign(npx * 4, 0);
    if (text.empty()) return true;

    // Parrafos (saltos de linea explicitos)
    std::vector<std::wstring> paras;
    {
        size_t s0 = 0;
        for (;;) {
            size_t p = text.find(L'\n', s0);
            std::wstring ln = (p == std::wstring::npos) ? text.substr(s0) : text.substr(s0, p - s0);
            while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
            paras.push_back(ln);
            if (p == std::wstring::npos) break;
            s0 = p + 1;
        }
    }

    Bitmap bmp(w, h, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

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

    // Zona de texto: ancho configurable (el texto se parte en lineas, el tamano NO cambia)
    const float marginY = (float)h * 0.04f;
    float wf = st.wrapWidth;
    if (wf < 0.2f) wf = 0.2f;
    if (wf > 1.0f) wf = 1.0f;
    const float availW = (float)w * wf;
    const float availH = (float)h - 2.0f * marginY;
    const float blockLeft = ((float)w - availW) * 0.5f;
    const float trackEm = st.tracking;

    float size = st.sizePx > 4.0f ? st.sizePx : 4.0f;
    std::unique_ptr<Font> font;
    std::vector<std::wstring> lines;
    float lineH = 0.0f, step = 0.0f, blockH = 0.0f, maxW = 0.0f;

    auto measure = [&](const std::wstring& s) -> float {
        if (s.empty()) return 0.0f;
        RectF layout(0, 0, 100000.0f, 100000.0f), box;
        g.MeasureString(s.c_str(), (INT)s.size(), font.get(), layout, &fmt, &box);
        return box.Width;
    };
    auto lineWidth = [&](const std::wstring& s) -> float {
        float wd = measure(s);
        if (s.size() > 1) wd += (float)(s.size() - 1) * trackEm * size;
        return wd;
    };

    // Reparte las palabras en lineas que quepan en availW con el tamano actual
    auto layoutLines = [&]() {
        font.reset(new Font(fam.get(), size, style, UnitPixel));
        lines.clear();
        maxW = 0.0f;
        for (const std::wstring& para : paras) {
            if (para.empty()) { lines.push_back(L""); continue; }
            std::wstring cur;
            size_t pos = 0;
            while (pos <= para.size()) {
                size_t sp = para.find(L' ', pos);
                std::wstring word = (sp == std::wstring::npos) ? para.substr(pos) : para.substr(pos, sp - pos);
                pos = (sp == std::wstring::npos) ? para.size() + 1 : sp + 1;
                if (word.empty()) continue;
                if (cur.empty()) {
                    cur = word;
                } else {
                    std::wstring cand = cur + L" " + word;
                    if (lineWidth(cand) <= availW) cur = cand;
                    else { lines.push_back(cur); cur = word; }
                }
            }
            lines.push_back(cur);
        }
        for (const std::wstring& l : lines) {
            float lw = lineWidth(l);
            if (lw > maxW) maxW = lw;
        }
        lineH = font->GetHeight(&g);
        step = lineH * st.lineSpacing;
        blockH = step * (float)(lines.size() - 1) + lineH;
    };

    layoutLines();
    // Solo se reduce el tamano si es imprescindible: una palabra mas ancha que la zona,
    // o un bloque mas alto que la pantalla.
    for (int it = 0; it < 8; it++) {
        float sW = maxW > availW + 0.5f ? availW / maxW : 1.0f;
        float sH = blockH > availH + 0.5f ? availH / blockH : 1.0f;
        float sc = sW < sH ? sW : sH;
        if (sc >= 0.999f) break;
        size *= sc * 0.995f;
        if (size < 4.0f) { size = 4.0f; layoutLines(); break; }
        layoutLines();
    }

    float top;
    float room = (float)h - blockH - 2.0f * marginY;
    if (room < 0.0f) top = ((float)h - blockH) * 0.5f;
    else top = marginY + room * (1.0f - st.vertical);

    std::vector<LineInfo> infos;
    // Todo el texto como un unico trazado (sirve para relleno, contorno, sombra y glow)
    GraphicsPath path(FillModeWinding);
    const float trackPx = trackEm * size;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::wstring& ln = lines[i];
        const float lw = lineWidth(ln);
        float x;
        if (st.align == 0) x = blockLeft;
        else if (st.align == 2) x = blockLeft + availW - lw;
        else x = blockLeft + (availW - lw) * 0.5f;
        const float y = top + step * (float)i;

        LineInfo li;
        li.topPx = y;
        li.heightPx = lineH;
        if (!ln.empty()) {
            li.charX.push_back(x);
            if (std::fabs(trackEm) < 0.001f) {
                path.AddString(ln.c_str(), (INT)ln.size(), fam.get(), style, size, PointF(x, y), &fmt);
                for (size_t k = 1; k <= ln.size(); k++) li.charX.push_back(x + measure(ln.substr(0, k)));
            } else {
                // Con espaciado entre letras cada caracter se coloca por separado
                for (size_t k = 0; k < ln.size(); k++) {
                    float px = x + (k == 0 ? 0.0f : measure(ln.substr(0, k))) + trackPx * (float)k;
                    if (ln[k] != L' ')
                        path.AddString(&ln[k], 1, fam.get(), style, size, PointF(px, y), &fmt);
                }
                for (size_t k = 1; k <= ln.size(); k++)
                    li.charX.push_back(x + measure(ln.substr(0, k)) + trackPx * (float)(k - 1));
            }
        }
        infos.push_back(li);
    }

    g.Clear(Color(0, 0, 0, 0));
    if (st.outlineWidthPx > 0.5f) {
        Pen pen(Color(255, B8(st.outline[0]), B8(st.outline[1]), B8(st.outline[2])),
                st.outlineWidthPx * 2.0f);
        pen.SetLineJoin(LineJoinRound);
        g.DrawPath(&pen, &path);
    }
    SolidBrush brush(Color(255, B8(st.fill[0]), B8(st.fill[1]), B8(st.fill[2])));
    g.FillPath(&brush, &path);

    BitmapData bd;
    Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;

    // Zona que ocupa el texto (para no procesar toda la imagen)
    int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
    for (int y = 0; y < h; y++) {
        const BYTE* s = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        for (int x = 0; x < w; x++) {
            if (s[x * 4 + 3]) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    }
    if (bx1 < 0) { bmp.UnlockBits(&bd); return true; }

    const bool shadowOn = st.shadowOpacity > 0.001f;
    const bool glowOn = st.glowIntensity > 0.001f;
    const int radS = shadowOn ? (int)(st.shadowBlurPx * 0.5f) : 0;
    const int radG = glowOn ? (int)(st.glowBlurPx * 0.5f) : 0;
    int dx = 0, dy = 0;
    if (shadowOn) {
        const float ang = st.shadowAngleDeg * 3.14159265f / 180.0f;
        dx = (int)std::lround(std::cos(ang) * st.shadowDistPx);
        dy = (int)std::lround(-std::sin(ang) * st.shadowDistPx); // y de pantalla va hacia abajo
    }
    int pad = 2;
    if (shadowOn) {
        int p2 = 3 * radS + (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + 2;
        if (p2 > pad) pad = p2;
    }
    if (glowOn) {
        int p2 = 3 * radG + 2;
        if (p2 > pad) pad = p2;
    }
    const int rx0 = bx0 - pad < 0 ? 0 : bx0 - pad;
    const int ry0 = by0 - pad < 0 ? 0 : by0 - pad;
    const int rx1 = bx1 + 1 + pad > w ? w : bx1 + 1 + pad;
    const int ry1 = by1 + 1 + pad > h ? h : by1 + 1 + pad;
    const int rw = rx1 - rx0, rh = ry1 - ry0;

    // Silueta desenfocada (desenfoque gaussiano aproximado con 3 pasadas de caja)
    std::vector<float> maskS, maskG, tmp;
    auto buildMask = [&](int rad, std::vector<float>& m) {
        m.assign((size_t)rw * (size_t)rh, 0.0f);
        for (int y = 0; y < rh; y++) {
            const BYTE* s = (const BYTE*)bd.Scan0 + (ptrdiff_t)(y + ry0) * bd.Stride;
            float* mm = &m[(size_t)y * rw];
            for (int x = 0; x < rw; x++) mm[x] = (float)s[(x + rx0) * 4 + 3] * (1.0f / 255.0f);
        }
        if (rad >= 1) {
            tmp.assign((size_t)rw * (size_t)rh, 0.0f);
            for (int pass = 0; pass < 3; pass++) {
                BoxBlurH(m.data(), tmp.data(), rw, rh, rad);
                BoxBlurV(tmp.data(), m.data(), rw, rh, rad);
            }
        }
    };
    if (shadowOn) buildMask(radS, maskS);
    if (glowOn) buildMask(radG, maskG);

    // Composicion (alfa premultiplicado): sombra, luego glow, luego texto
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = ry0; y < ry1; y++) {
        const BYTE* s = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        BYTE* d = &out[(size_t)y * w * 4];
        for (int x = rx0; x < rx1; x++) {
            const float am = (float)s[x * 4 + 3] * (1.0f / 255.0f);
            const float cb = (float)s[x * 4 + 0] * (1.0f / 255.0f);
            const float cg = (float)s[x * 4 + 1] * (1.0f / 255.0f);
            const float cr = (float)s[x * 4 + 2] * (1.0f / 255.0f);
            float Pr = 0.0f, Pg = 0.0f, Pb = 0.0f, A = 0.0f;
            if (shadowOn) {
                int sx = x - dx, sy = y - dy;
                if (sx >= rx0 && sx < rx1 && sy >= ry0 && sy < ry1) {
                    float as = maskS[(size_t)(sy - ry0) * rw + (size_t)(sx - rx0)] * st.shadowOpacity;
                    A = as;
                    Pr = st.shadow[0] * as; Pg = st.shadow[1] * as; Pb = st.shadow[2] * as;
                }
            }
            if (glowOn) {
                float ag = maskG[(size_t)(y - ry0) * rw + (size_t)(x - rx0)] * st.glowIntensity;
                if (ag > 1.0f) ag = 1.0f;
                const float k = 1.0f - ag;
                Pr = st.glow[0] * ag + Pr * k;
                Pg = st.glow[1] * ag + Pg * k;
                Pb = st.glow[2] * ag + Pb * k;
                A = ag + A * k;
            }
            const float k2 = 1.0f - am;
            Pr = cr * am + Pr * k2;
            Pg = cg * am + Pg * k2;
            Pb = cb * am + Pb * k2;
            A = am + A * k2;
            if (A <= 0.0f) continue;
            d[x * 4 + 0] = B8(Pb);
            d[x * 4 + 1] = B8(Pg);
            d[x * 4 + 2] = B8(Pr);
            d[x * 4 + 3] = B8(A);
            if (A > 0.01f) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    bmp.UnlockBits(&bd);

    if (res && maxX >= 0) {
        res->any = true;
        res->minX = minX; res->maxX = maxX;
        res->minY = minY; res->maxY = maxY;
        res->lines = infos;
    }
    return true;
}

} // namespace render

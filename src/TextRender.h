#pragma once
#include <string>
#include <vector>

namespace render {

struct Style {
    std::wstring font = L"Arial";
    bool  bold = false;
    bool  italic = false;
    float sizePx = 60.0f;
    int   align = 1;          // 0 izquierda, 1 centro, 2 derecha
    float lineSpacing = 1.0f; // multiplicador
    float vertical = 0.0f;    // 0 abajo, 0.5 centro, 1 arriba
};

// Lista de familias de fuentes instaladas (ordenada)
const std::vector<std::wstring>& InstalledFonts();

// Dibuja el texto (blanco, alfa = cobertura) en un bitmap BGRA w*h*4
bool RenderText(const std::wstring& text, const Style& st, int w, int h,
                std::vector<unsigned char>& bgra);

} // namespace render

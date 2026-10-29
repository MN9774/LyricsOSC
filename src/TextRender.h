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
    float vertical = 0.5f;    // 0 abajo, 0.5 centro, 1 arriba
    float tracking = 0.0f;    // espaciado entre letras en "em" (0 = normal, negativo = mas junto)
    float wrapWidth = 0.8f;   // ancho maximo del texto (fraccion del ancho de pantalla); se parte en lineas

    float fill[3] = {1.0f, 1.0f, 1.0f};   // color del texto (RGB 0..1)

    float outlineWidthPx = 0.0f;           // 0 = sin contorno
    float outline[3] = {0.0f, 0.0f, 0.0f};

    float shadowOpacity = 0.0f;            // 0 = sin sombra
    float shadowDistPx = 0.0f;
    float shadowAngleDeg = 315.0f;         // 0 = derecha, 90 = arriba
    float shadowBlurPx = 0.0f;
    float shadow[3] = {0.0f, 0.0f, 0.0f};

    float glowIntensity = 0.0f;            // 0 = sin glow
    float glowBlurPx = 0.0f;
    float glow[3] = {0.0f, 0.8f, 1.0f};
};

// Posicion de cada linea ya maquetada (para el efecto maquina de escribir)
struct LineInfo {
    float topPx = 0.0f, heightPx = 0.0f;
    std::vector<float> charX; // x (px) del borde tras cada caracter (n+1 valores)
};

struct Result {
    bool any = false;
    int minX = 0, minY = 0, maxX = 0, maxY = 0; // caja que ocupa lo dibujado (px)
    std::vector<LineInfo> lines;
};

// Lista de familias de fuentes instaladas (ordenada)
const std::vector<std::wstring>& InstalledFonts();

// Dibuja texto + contorno + sombra. Salida: bitmap BGRA con alfa PREMULTIPLICADO (w*h*4).
bool RenderText(const std::wstring& text, const Style& st, int w, int h,
                std::vector<unsigned char>& bgraPremult, Result* res = nullptr);

} // namespace render

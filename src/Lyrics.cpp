// Plugin FFGL "Lyrics OSC" para Resolume Arena
// Letras linea por linea, tipografias, color, contorno, sombra, glow, blur,
// animaciones, copiar/pegar ajustes, presets y control por OSC.
#include <FFGLSDK.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "LyricsCore.h"
#include "TextRender.h"

using namespace ffglex;

// El orden de este enum es el orden en que aparecen los parametros en Resolume.
enum ParamID : FFUInt32 {
    // --- Grupo "Lyrics"
    P_TEXT = 0,   // cuadro de texto con la letra (una linea = un paso)
    P_FILE,       // archivo .txt (arrastrar y soltar)
    P_RESTART,    // boton: vuelve a "nada en pantalla"
    P_PREV,       // boton: linea anterior
    P_NEXT,       // boton: linea siguiente
    P_HIDESTATUS, // oculta el texto de diagnostico

    // --- Grupo "Text"
    P_FONT,       // slider: la rueda del raton recorre las tipografias
    P_FONT_PREV,
    P_FONT_NEXT,
    P_SIZE,
    P_COLOR_R,    // Color del texto (selector de color: R, G, B consecutivos)
    P_COLOR_G,
    P_COLOR_B,
    P_ALIGN,
    P_POS,
    P_SPACING,
    P_TRACK,      // espaciado entre letras
    P_WRAP,       // ancho maximo del texto (se parte en lineas, el tamano no cambia)
    P_BOLD,
    P_ITALIC,

    // --- Grupo "Outline"
    P_OL_COPY, P_OL_PASTE,
    P_OUT_W, P_OUT_R, P_OUT_G, P_OUT_B,

    // --- Grupo "Shadow"
    P_SH_COPY, P_SH_PASTE,
    P_SH_OP, P_SH_DIST, P_SH_ANGLE, P_SH_BLUR, P_SH_R, P_SH_G, P_SH_B,

    // --- Grupo "Glow"
    P_GL_COPY, P_GL_PASTE,
    P_GL_INT, P_GL_BLUR, P_GL_R, P_GL_G, P_GL_B,

    // --- Grupo "Blur"
    P_BL_COPY, P_BL_PASTE,
    P_BLUR_AMT,

    // --- Grupo "Animation"
    P_AN_COPY, P_AN_PASTE,
    P_ANIM_IN, P_IN_TIME, P_ANIM_OUT, P_OUT_TIME, P_ANIM_DIST, P_EASE,

    // --- Grupo "Presets"
    P_COPY_ALL, P_PASTE_ALL, P_SAVE_PRESET, P_LOAD_PRESET, P_PRESET_FILE,

    P_COUNT
};

enum AnimKind {
    A_NONE = 0, A_FADE, A_SLIDE_UP, A_SLIDE_DOWN, A_SLIDE_LEFT, A_SLIDE_RIGHT,
    A_ZOOM_IN, A_ZOOM_OUT, A_WIPE_RIGHT, A_WIPE_LEFT, A_BLUR, A_TYPE, A_COUNT
};

static const char* kAnimNames[A_COUNT] = {
    "None", "Fade", "Slide Up", "Slide Down", "Slide Left", "Slide Right",
    "Zoom In", "Zoom Out", "Wipe Right", "Wipe Left", "Blur", "Typewriter"};

static const char* kEaseNames[4] = {"Linear", "Ease Out", "Ease In Out", "Back"};

// ---------------------------------------------------------------- grupos copiar/pegar
enum Group { G_OUTLINE = 0, G_SHADOW, G_GLOW, G_BLUR, G_ANIM, G_ALL, G_COUNT };

static std::vector<int> GroupParams(int g)
{
    switch (g) {
    case G_OUTLINE: return {(int)P_OUT_W, (int)P_OUT_R, (int)P_OUT_G, (int)P_OUT_B};
    case G_SHADOW:
        return {(int)P_SH_OP, (int)P_SH_DIST, (int)P_SH_ANGLE, (int)P_SH_BLUR,
                (int)P_SH_R, (int)P_SH_G, (int)P_SH_B};
    case G_GLOW:
        return {(int)P_GL_INT, (int)P_GL_BLUR, (int)P_GL_R, (int)P_GL_G, (int)P_GL_B};
    case G_BLUR: return {(int)P_BLUR_AMT};
    case G_ANIM:
        return {(int)P_ANIM_IN, (int)P_IN_TIME, (int)P_ANIM_OUT, (int)P_OUT_TIME,
                (int)P_ANIM_DIST, (int)P_EASE};
    case G_ALL: {
        // Todo salvo la tipografia y el archivo de la letra
        std::vector<int> all = {(int)P_SIZE, (int)P_COLOR_R, (int)P_COLOR_G, (int)P_COLOR_B,
                                (int)P_ALIGN, (int)P_POS, (int)P_SPACING, (int)P_TRACK, (int)P_WRAP,
                                (int)P_BOLD, (int)P_ITALIC};
        for (int k = 0; k < G_ALL; k++) {
            std::vector<int> sub = GroupParams(k);
            all.insert(all.end(), sub.begin(), sub.end());
        }
        return all;
    }
    default: return {};
    }
}

// Portapapeles compartido por todas las capas/instancias del plugin
static std::mutex gClipMu;
static std::map<int, std::vector<std::pair<int, float>>> gClip;

// ---------------------------------------------------------------- presets (archivo)
struct PresetKey { int id; const char* key; };
static const PresetKey kPresetKeys[] = {
    {P_SIZE, "size"}, {P_COLOR_R, "colorR"}, {P_COLOR_G, "colorG"}, {P_COLOR_B, "colorB"},
    {P_ALIGN, "align"}, {P_POS, "position"}, {P_SPACING, "lineSpacing"}, {P_BOLD, "bold"},
    {P_ITALIC, "italic"}, {P_TRACK, "letterSpacing"}, {P_WRAP, "wrapWidth"},
    {P_OUT_W, "outlineWidth"}, {P_OUT_R, "outlineR"}, {P_OUT_G, "outlineG"}, {P_OUT_B, "outlineB"},
    {P_SH_OP, "shadowOpacity"}, {P_SH_DIST, "shadowDistance"}, {P_SH_ANGLE, "shadowAngle"},
    {P_SH_BLUR, "shadowBlur"}, {P_SH_R, "shadowR"}, {P_SH_G, "shadowG"}, {P_SH_B, "shadowB"},
    {P_GL_INT, "glowIntensity"}, {P_GL_BLUR, "glowBlur"}, {P_GL_R, "glowR"}, {P_GL_G, "glowG"},
    {P_GL_B, "glowB"},
    {P_BLUR_AMT, "blurAmount"},
    {P_ANIM_IN, "animIn"}, {P_IN_TIME, "animInTime"}, {P_ANIM_OUT, "animOut"},
    {P_OUT_TIME, "animOutTime"}, {P_ANIM_DIST, "animDistance"}, {P_EASE, "animEasing"},
};

struct Async {
    std::atomic<bool> busy{false};
    std::atomic<bool> hasLoad{false};
    std::mutex m;
    std::wstring loadPath;
};

static std::mutex gPresetDirMu;
static std::wstring gPresetDir;

static std::wstring PresetStartDir()
{
    std::lock_guard<std::mutex> lk(gPresetDirMu);
    if (gPresetDir.empty()) {
        gPresetDir = lyr::LyricsDir() + L"\\Presets";
        CreateDirectoryW(gPresetDir.c_str(), nullptr);
    }
    return gPresetDir;
}

static void RememberPresetDir(const std::wstring& filePath)
{
    size_t sl = filePath.find_last_of(L"\\/");
    if (sl == std::wstring::npos) return;
    std::lock_guard<std::mutex> lk(gPresetDirMu);
    gPresetDir = filePath.substr(0, sl);
}

// Los cuadros de dialogo de Windows se abren en un hilo aparte para no bloquear a Resolume.
static void SaveDialogThread(std::shared_ptr<Async> a, std::string content, void* owner)
{
    std::wstring path;
    if (lyr::PickSavePath(owner, PresetStartDir(), L"LyricsStyle.lyrpreset", path)) {
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f << content;
            f.close();
            RememberPresetDir(path);
            lyr::Log("Preset guardado: " + lyr::WToUtf8(path));
        } else {
            lyr::Log("ERROR: no se pudo escribir el preset: " + lyr::WToUtf8(path));
        }
    }
    a->busy.store(false);
}

static void OpenDialogThread(std::shared_ptr<Async> a, void* owner)
{
    std::wstring path;
    if (lyr::PickOpenPath(owner, PresetStartDir(), path)) {
        RememberPresetDir(path);
        std::lock_guard<std::mutex> lk(a->m);
        a->loadPath = path;
        a->hasLoad.store(true);
    }
    a->busy.store(false);
}

// ---------------------------------------------------------------- shaders
static const char* kVertex = R"GLSL(#version 410 core
out vec2 uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// uA = (desplazamientoX, desplazamientoY, escala)
// uB = (pivoteX, pivoteY, opacidad)
// uC = (barridoMin, barridoMax, suavizado)
// uD = (radio de desenfoque en uv, ancho/alto, 0)
// uE = (maquina de escribir activa, limite inferior de la linea actual, limite superior)
// uG = (limite en x de la linea actual, 0, 0)
static const char* kFragment = R"GLSL(#version 410 core
uniform sampler2D tex;
uniform vec3 uA;
uniform vec3 uB;
uniform vec3 uC;
uniform vec3 uD;
uniform vec3 uE;
uniform vec3 uG;
in vec2 uv;
out vec4 fragColor;

vec4 fetch(vec2 q)
{
    return texture(tex, vec2(q.x, 1.0 - q.y)); // textura con alfa premultiplicado
}

void main()
{
    vec2 pivot = uB.xy;
    vec2 p = pivot + (uv - pivot - uA.xy) / uA.z;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
        fragColor = vec4(0.0);
        return;
    }
    float soft = max(uC.z, 0.0001);
    float m = smoothstep(uC.x - soft, uC.x, p.x) * (1.0 - smoothstep(uC.y, uC.y + soft, p.x));
    if (uE.x > 0.5) {
        // lineas de arriba: visibles; linea actual: hasta el limite en x; lineas de abajo: ocultas
        float vis = 0.0;
        if (p.y > uE.z) vis = 1.0;
        else if (p.y >= uE.y) vis = 1.0 - step(uG.x, p.x);
        m *= vis;
    }

    vec4 t;
    float br = uD.x;
    if (br > 0.00005) {
        // desenfoque: 32 muestras en espiral con rotacion aleatoria por pixel
        float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float rot = n * 6.2831853;
        vec4 acc = vec4(0.0);
        for (int i = 0; i < 32; i++) {
            float r = sqrt((float(i) + 0.5) / 32.0);
            float a = float(i) * 2.39996323 + rot;
            vec2 o = vec2(cos(a), sin(a)) * (r * br);
            o.y *= uD.y;
            acc += fetch(p + o);
        }
        t = acc / 32.0;
    } else {
        t = fetch(p);
    }
    fragColor = t * (uB.z * m);
}
)GLSL";

static double NowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float Ease(int mode, float t)
{
    t = Clamp01(t);
    switch (mode) {
    case 1: { float u = 1.0f - t; return 1.0f - u * u * u; }
    case 2: {
        if (t < 0.5f) return 4.0f * t * t * t;
        float u = -2.0f * t + 2.0f;
        return 1.0f - u * u * u * 0.5f;
    }
    case 3: {
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    default: return t;
    }
}

struct RenderKey {
    std::wstring text, font;
    std::vector<float> nums;
    bool valid = false;
    bool operator==(const RenderKey& o) const
    {
        return valid && o.valid && text == o.text && font == o.font && nums == o.nums;
    }
};

struct Layer {
    GLuint tex = 0;
    RenderKey key;
    bool has = false;
    float bx0 = 0.0f, bx1 = 1.0f; // caja del texto en uv (x)
    float cx = 0.5f, cy = 0.5f;   // centro del texto en uv (y hacia arriba)
    struct LineBand {             // una linea de texto ya maquetada (maquina de escribir)
        float yHi = 0.0f, yLo = 0.0f; // limites verticales en uv (y hacia arriba)
        std::vector<float> charU;     // borde tras cada caracter en uv
    };
    std::vector<LineBand> bands;
};

// Guarda y restaura el estado de OpenGL que tocamos
struct GlStateGuard {
    GLint prog = 0, vao = 0, active = 0, tex = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
    GLboolean blend = 0, depth = 0;
    GlStateGuard()
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
    }
    ~GlStateGuard()
    {
        glBindVertexArray((GLuint)vao);
        glUseProgram((GLuint)prog);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
        glActiveTexture((GLenum)active);
        glBlendFuncSeparate((GLenum)srcRGB, (GLenum)dstRGB, (GLenum)srcA, (GLenum)dstA);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
    }
};

class LyricsPlugin : public CFFGLPlugin
{
public:
    LyricsPlugin();
    ~LyricsPlugin() override = default;

    FFResult InitGL(const FFGLViewportStruct* vp) override;
    FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
    FFResult DeInitGL() override;
    FFResult SetFloatParameter(unsigned int dwIndex, float value) override;
    float GetFloatParameter(unsigned int index) override;
    FFResult SetTextParameter(unsigned int index, const char* value) override;
    char* GetTextParameter(unsigned int index) override;
    char* GetParameterDisplay(unsigned int index) override;

private:
    void RefreshLyricsLocked(); // requiere fileMu_
    void CheckFileChanged();
    void MakeTexture(GLuint& t);
    void StartTransition(const float* v);
    void DrawLayer(const Layer& L, int kind, bool isIn, float e);

    // --- tipografia, copiar/pegar y presets
    int FontIndexFromValue(float v) const;
    float FontValueFromIndex(int i) const;
    void PushValue(unsigned int id, float v); // cambia un valor y avisa a Resolume
    void OnTrigger(unsigned int idx);
    void CopyGroup(int g);
    void PasteGroup(int g);
    std::string BuildPresetText();
    bool LoadPresetFile(const std::wstring& path);
    void FlushAsync();

    lyr::LyricsState state_;
    std::vector<std::wstring> fonts_;

    std::mutex mu_;
    float values_[P_COUNT];
    ULONGLONG lastFire_[P_COUNT];
    std::atomic<int> clicks_{0};
    ULONGLONG createdTick_ = 0;

    std::mutex fileMu_;
    std::string textUtf8_;
    std::string fileUtf8_;
    std::string presetFileUtf8_;
    std::string emptyText_;
    std::wstring filePath_;
    ULONGLONG fileStamp_ = 0;
    ULONGLONG lastFileCheck_ = 0;
    bool fileOk_ = false;

    std::shared_ptr<Async> async_ = std::make_shared<Async>();
    char dispBuf_[200];

    FFGLShader shader_;
    GLuint vao_ = 0;
    int w_ = 0, h_ = 0;
    Layer cur_, prev_;
    std::vector<unsigned char> pixels_;

    // Transicion entre lineas (parametros fijados al empezar cada transicion)
    unsigned long long lastSerial_ = 0;
    double transStart_ = 0.0;
    bool outActive_ = false;
    int tIn_ = 0, tOut_ = 0, tEase_ = 1;
    float tInDur_ = 0.0f, tOutDur_ = 0.0f, tDist_ = 0.0f;
    float staticBlurU_ = 0.0f;

    int glErrLogs_ = 0;
    bool loggedFirstFrame_ = false;
    bool loggedFirstDraw_ = false;
    std::wstring lastLoggedText_;
};

static CFFGLPluginInfo PluginInfo(
    PluginFactory<LyricsPlugin>,
    "LYRC",
    "Lyrics OSC",
    2, 1,
    1, 0,
    FF_SOURCE,
    "Letras linea por linea con tipografias, efectos, animaciones y OSC",
    "Lyrics OSC");

static bool IsTriggerParam(unsigned int id)
{
    switch (id) {
    case P_RESTART: case P_PREV: case P_NEXT:
    case P_FONT_PREV: case P_FONT_NEXT:
    case P_OL_COPY: case P_OL_PASTE: case P_SH_COPY: case P_SH_PASTE:
    case P_GL_COPY: case P_GL_PASTE: case P_BL_COPY: case P_BL_PASTE:
    case P_AN_COPY: case P_AN_PASTE:
    case P_COPY_ALL: case P_PASTE_ALL: case P_SAVE_PRESET: case P_LOAD_PRESET:
        return true;
    default: return false;
    }
}

LyricsPlugin::LyricsPlugin()
{
    SetMinInputs(0);
    SetMaxInputs(0);

    createdTick_ = GetTickCount64();
    dispBuf_[0] = 0;
    for (int i = 0; i < P_COUNT; i++) { values_[i] = 0.0f; lastFire_[i] = 0; }

    // Ayudantes para declarar parametros
    auto button = [&](unsigned int id, const char* uniqueName, const char* shownName) {
        SetParamInfof(id, uniqueName, FF_TYPE_EVENT);
        if (shownName) SetParamDisplayName(id, std::string(shownName), false);
    };
    auto group = [&](std::initializer_list<unsigned int> ids, const char* name) {
        for (unsigned int id : ids) SetParamGroup(id, std::string(name));
    };

    // =============== Grupo "Lyrics"
    SetParamInfo(P_TEXT, "Lyrics", FF_TYPE_TEXT, (const char*)"");
    SetFileParamInfo(P_FILE, "Lyrics File", std::vector<std::string>{"txt", ".txt"}, "");
    button(P_RESTART, "Restart", nullptr);
    button(P_PREV, "Prev", nullptr);
    button(P_NEXT, "Next", nullptr);
    SetParamInfof(P_HIDESTATUS, "Hide Status", FF_TYPE_BOOLEAN);
    group({P_TEXT, P_FILE, P_RESTART, P_PREV, P_NEXT, P_HIDESTATUS}, "Lyrics");

    // =============== Grupo "Text"
    fonts_ = render::InstalledFonts();
    if (fonts_.size() > 1000) fonts_.resize(1000);
    int defFont = 0;
    for (size_t i = 0; i < fonts_.size(); i++)
        if (_wcsicmp(fonts_[i].c_str(), L"Arial") == 0) { defFont = (int)i; break; }
    values_[P_FONT] = FontValueFromIndex(defFont);
    state_.SetFont(fonts_[(size_t)defFont]);
    // Slider normalizado: arrastra o usa la rueda del raton; la barra muestra el nombre de la fuente.
    SetParamInfof(P_FONT, "Font Select", FF_TYPE_STANDARD);
    SetParamDisplayName(P_FONT, std::string("Font"), false);
    button(P_FONT_PREV, "Font Prev", "Font -");
    button(P_FONT_NEXT, "Font Next", "Font +");

    values_[P_SIZE] = 0.5f;
    SetParamInfo(P_SIZE, "Size", FF_TYPE_STANDARD, 0.5f);

    values_[P_COLOR_R] = values_[P_COLOR_G] = values_[P_COLOR_B] = 1.0f;
    SetParamInfo(P_COLOR_R, "Text Color", FF_TYPE_RED, 1.0f);
    SetParamInfo(P_COLOR_G, "Text Color G", FF_TYPE_GREEN, 1.0f);
    SetParamInfo(P_COLOR_B, "Text Color B", FF_TYPE_BLUE, 1.0f);

    values_[P_ALIGN] = 1.0f;
    SetOptionParamInfo(P_ALIGN, "Align", 3, 1.0f);
    SetParamElementInfo(P_ALIGN, 0, "Left", 0.0f);
    SetParamElementInfo(P_ALIGN, 1, "Center", 1.0f);
    SetParamElementInfo(P_ALIGN, 2, "Right", 2.0f);

    values_[P_POS] = 0.5f;
    SetParamInfo(P_POS, "Position", FF_TYPE_STANDARD, 0.5f);
    SetParamInfo(P_SPACING, "Line Spacing", FF_TYPE_STANDARD, 0.0f);
    values_[P_TRACK] = 0.25f; // 0.25 = espaciado normal
    values_[P_WRAP] = 0.8f;
    SetParamInfo(P_TRACK, "Letter Spacing", FF_TYPE_STANDARD, 0.25f);
    SetParamInfo(P_WRAP, "Wrap Width", FF_TYPE_STANDARD, 0.8f);
    SetParamInfof(P_BOLD, "Bold", FF_TYPE_BOOLEAN);
    SetParamInfof(P_ITALIC, "Italic", FF_TYPE_BOOLEAN);
    group({P_FONT, P_FONT_PREV, P_FONT_NEXT, P_SIZE, P_COLOR_R, P_COLOR_G, P_COLOR_B, P_ALIGN,
           P_POS, P_SPACING, P_TRACK, P_WRAP, P_BOLD, P_ITALIC}, "Text");

    // =============== Grupo "Outline"
    button(P_OL_COPY, "Outline Copy", "Copy");
    button(P_OL_PASTE, "Outline Paste", "Paste");
    SetParamInfo(P_OUT_W, "Outline Width", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_OUT_R, "Outline Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_OUT_G, "Outline Color G", FF_TYPE_GREEN, 0.0f);
    SetParamInfo(P_OUT_B, "Outline Color B", FF_TYPE_BLUE, 0.0f);
    group({P_OL_COPY, P_OL_PASTE, P_OUT_W, P_OUT_R, P_OUT_G, P_OUT_B}, "Outline");

    // =============== Grupo "Shadow"
    values_[P_SH_DIST] = 0.3f;
    values_[P_SH_ANGLE] = 0.875f; // 315 grados: abajo a la derecha
    values_[P_SH_BLUR] = 0.3f;
    button(P_SH_COPY, "Shadow Copy", "Copy");
    button(P_SH_PASTE, "Shadow Paste", "Paste");
    SetParamInfo(P_SH_OP, "Shadow Opacity", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_SH_DIST, "Shadow Distance", FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(P_SH_ANGLE, "Shadow Angle", FF_TYPE_STANDARD, 0.875f);
    SetParamInfo(P_SH_BLUR, "Shadow Blur", FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(P_SH_R, "Shadow Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_SH_G, "Shadow Color G", FF_TYPE_GREEN, 0.0f);
    SetParamInfo(P_SH_B, "Shadow Color B", FF_TYPE_BLUE, 0.0f);
    group({P_SH_COPY, P_SH_PASTE, P_SH_OP, P_SH_DIST, P_SH_ANGLE, P_SH_BLUR, P_SH_R, P_SH_G, P_SH_B},
          "Shadow");

    // =============== Grupo "Glow"
    values_[P_GL_BLUR] = 0.4f;
    values_[P_GL_G] = 0.8f;
    values_[P_GL_B] = 1.0f;
    button(P_GL_COPY, "Glow Copy", "Copy");
    button(P_GL_PASTE, "Glow Paste", "Paste");
    SetParamInfo(P_GL_INT, "Glow Intensity", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_GL_BLUR, "Glow Blur", FF_TYPE_STANDARD, 0.4f);
    SetParamInfo(P_GL_R, "Glow Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_GL_G, "Glow Color G", FF_TYPE_GREEN, 0.8f);
    SetParamInfo(P_GL_B, "Glow Color B", FF_TYPE_BLUE, 1.0f);
    group({P_GL_COPY, P_GL_PASTE, P_GL_INT, P_GL_BLUR, P_GL_R, P_GL_G, P_GL_B}, "Glow");

    // =============== Grupo "Blur" (desenfoque fijo de todo el texto)
    button(P_BL_COPY, "Blur Copy", "Copy");
    button(P_BL_PASTE, "Blur Paste", "Paste");
    SetParamInfo(P_BLUR_AMT, "Blur Amount", FF_TYPE_STANDARD, 0.0f);
    group({P_BL_COPY, P_BL_PASTE, P_BLUR_AMT}, "Blur");

    // =============== Grupo "Animation" (se repite en cada cambio de linea)
    values_[P_IN_TIME] = 0.15f;
    values_[P_OUT_TIME] = 0.15f;
    values_[P_ANIM_DIST] = 0.4f;
    values_[P_EASE] = 1.0f;
    button(P_AN_COPY, "Animation Copy", "Copy");
    button(P_AN_PASTE, "Animation Paste", "Paste");
    SetOptionParamInfo(P_ANIM_IN, "Animation In", (unsigned int)A_COUNT, 0.0f);
    for (int i = 0; i < A_COUNT; i++)
        SetParamElementInfo(P_ANIM_IN, (unsigned int)i, kAnimNames[i], (float)i);
    SetParamInfo(P_IN_TIME, "Anim In Time", FF_TYPE_STANDARD, 0.15f);
    SetOptionParamInfo(P_ANIM_OUT, "Animation Out", (unsigned int)A_COUNT, 0.0f);
    for (int i = 0; i < A_COUNT; i++)
        SetParamElementInfo(P_ANIM_OUT, (unsigned int)i, kAnimNames[i], (float)i);
    SetParamInfo(P_OUT_TIME, "Anim Out Time", FF_TYPE_STANDARD, 0.15f);
    SetParamInfo(P_ANIM_DIST, "Anim Distance", FF_TYPE_STANDARD, 0.4f);
    SetOptionParamInfo(P_EASE, "Anim Easing", 4, 1.0f);
    for (int i = 0; i < 4; i++) SetParamElementInfo(P_EASE, (unsigned int)i, kEaseNames[i], (float)i);
    group({P_AN_COPY, P_AN_PASTE, P_ANIM_IN, P_IN_TIME, P_ANIM_OUT, P_OUT_TIME, P_ANIM_DIST, P_EASE},
          "Animation");

    // =============== Grupo "Presets"
    button(P_COPY_ALL, "Copy All", nullptr);
    button(P_PASTE_ALL, "Paste All", nullptr);
    button(P_SAVE_PRESET, "Save Preset", nullptr);
    button(P_LOAD_PRESET, "Load Preset", nullptr);
    SetFileParamInfo(P_PRESET_FILE, "Preset File", std::vector<std::string>{"lyrpreset", ".lyrpreset"}, "");
    group({P_COPY_ALL, P_PASTE_ALL, P_SAVE_PRESET, P_LOAD_PRESET, P_PRESET_FILE}, "Presets");

    lyr::Log("Plugin creado. Fuentes: " + std::to_string(fonts_.size()));
}

int LyricsPlugin::FontIndexFromValue(float v) const
{
    const int n = (int)fonts_.size();
    if (n <= 1) return 0;
    int i = (int)std::lround(Clamp01(v) * (float)(n - 1));
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    return i;
}

float LyricsPlugin::FontValueFromIndex(int i) const
{
    const int n = (int)fonts_.size();
    if (n <= 1) return 0.0f;
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    return (float)i / (float)(n - 1);
}

void LyricsPlugin::MakeTexture(GLuint& t)
{
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

FFResult LyricsPlugin::InitGL(const FFGLViewportStruct* vp)
{
    if (!shader_.Compile(kVertex, kFragment)) {
        lyr::Log("ERROR: no compila el shader (InitGL falla)");
        DeInitGL();
        return FF_FAIL;
    }
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGenVertexArrays(1, &vao_);
    MakeTexture(cur_.tex);
    MakeTexture(prev_.tex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    w_ = (int)vp->width;
    h_ = (int)vp->height;
    cur_.key = RenderKey(); cur_.has = false;
    prev_.key = RenderKey(); prev_.has = false;
    outActive_ = false;
    lyr::Log("InitGL ok. Viewport " + std::to_string(w_) + "x" + std::to_string(h_));
    return CFFGLPlugin::InitGL(vp);
}

FFResult LyricsPlugin::DeInitGL()
{
    shader_.FreeGLResources();
    if (cur_.tex) { glDeleteTextures(1, &cur_.tex); cur_.tex = 0; }
    if (prev_.tex) { glDeleteTextures(1, &prev_.tex); prev_.tex = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    cur_.key = RenderKey(); prev_.key = RenderKey();
    cur_.has = prev_.has = false;
    outActive_ = false;
    return FF_SUCCESS;
}

// ---------------------------------------------------------------- valores, copiar/pegar, presets
// Cambia un valor desde el propio plugin y avisa a Resolume para que mueva el control en pantalla.
void LyricsPlugin::PushValue(unsigned int id, float v)
{
    if (id >= P_COUNT) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        values_[id] = v;
    }
    if (id == P_FONT) {
        int i = FontIndexFromValue(v);
        if (i >= 0 && i < (int)fonts_.size()) state_.SetFont(fonts_[(size_t)i]);
    }
    RaiseParamEvent(id, FF_EVENT_FLAG_VALUE);
}

void LyricsPlugin::CopyGroup(int g)
{
    std::vector<int> ids = GroupParams(g);
    std::vector<std::pair<int, float>> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int id : ids) snap.push_back({id, values_[id]});
    }
    {
        std::lock_guard<std::mutex> lk(gClipMu);
        gClip[g] = snap;
    }
    lyr::Log("Copiado grupo " + std::to_string(g) + " (" + std::to_string(snap.size()) + " valores)");
}

void LyricsPlugin::PasteGroup(int g)
{
    std::vector<std::pair<int, float>> snap;
    {
        std::lock_guard<std::mutex> lk(gClipMu);
        auto it = gClip.find(g);
        if (it != gClip.end()) snap = it->second;
    }
    if (snap.empty()) {
        lyr::Log("Pegar grupo " + std::to_string(g) + ": no hay nada copiado");
        return;
    }
    for (auto& kv : snap) PushValue((unsigned int)kv.first, kv.second);
    lyr::Log("Pegado grupo " + std::to_string(g));
}

std::string LyricsPlugin::BuildPresetText()
{
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "# Lyrics OSC preset\nversion=1\n";
    int fi = 0;
    float vals[P_COUNT];
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < P_COUNT; i++) vals[i] = values_[i];
    }
    fi = FontIndexFromValue(vals[P_FONT]);
    if (fi >= 0 && fi < (int)fonts_.size()) os << "font=" << lyr::WToUtf8(fonts_[(size_t)fi]) << "\n";
    for (const PresetKey& k : kPresetKeys) os << k.key << "=" << vals[k.id] << "\n";
    return os.str();
}

bool LyricsPlugin::LoadPresetFile(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lyr::Log("No se pudo leer el preset: " + lyr::WToUtf8(path));
        return false;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string text = buf.str();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    int applied = 0;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "font") {
            std::wstring want = lyr::Utf8ToW(val);
            for (size_t i = 0; i < fonts_.size(); i++) {
                if (_wcsicmp(fonts_[i].c_str(), want.c_str()) == 0) {
                    PushValue(P_FONT, FontValueFromIndex((int)i));
                    applied++;
                    break;
                }
            }
            continue;
        }
        for (const PresetKey& k : kPresetKeys) {
            if (key == k.key) {
                std::istringstream vs(val);
                vs.imbue(std::locale::classic());
                float fv = 0.0f;
                if (vs >> fv) { PushValue((unsigned int)k.id, fv); applied++; }
                break;
            }
        }
    }
    lyr::Log("Preset cargado (" + std::to_string(applied) + " valores): " + lyr::WToUtf8(path));
    return applied > 0;
}

// Resultado de un cuadro de dialogo (que corre en otro hilo): se aplica desde un hilo de Resolume.
void LyricsPlugin::FlushAsync()
{
    if (!async_->hasLoad.load()) return;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(async_->m);
        if (!async_->hasLoad.exchange(false)) return;
        path = async_->loadPath;
    }
    LoadPresetFile(path);
}

void LyricsPlugin::OnTrigger(unsigned int idx)
{
    switch (idx) {
    case P_RESTART: state_.Restart(); break;
    case P_PREV: state_.Prev(); break;
    case P_NEXT: state_.Next(); break;
    case P_FONT_PREV:
    case P_FONT_NEXT: {
        float cur;
        { std::lock_guard<std::mutex> lk(mu_); cur = values_[P_FONT]; }
        int i = FontIndexFromValue(cur) + (idx == P_FONT_NEXT ? 1 : -1);
        if (i < 0) i = 0;
        if (i > (int)fonts_.size() - 1) i = (int)fonts_.size() - 1;
        PushValue(P_FONT, FontValueFromIndex(i));
        break;
    }
    case P_OL_COPY: CopyGroup(G_OUTLINE); break;
    case P_OL_PASTE: PasteGroup(G_OUTLINE); break;
    case P_SH_COPY: CopyGroup(G_SHADOW); break;
    case P_SH_PASTE: PasteGroup(G_SHADOW); break;
    case P_GL_COPY: CopyGroup(G_GLOW); break;
    case P_GL_PASTE: PasteGroup(G_GLOW); break;
    case P_BL_COPY: CopyGroup(G_BLUR); break;
    case P_BL_PASTE: PasteGroup(G_BLUR); break;
    case P_AN_COPY: CopyGroup(G_ANIM); break;
    case P_AN_PASTE: PasteGroup(G_ANIM); break;
    case P_COPY_ALL: CopyGroup(G_ALL); break;
    case P_PASTE_ALL: PasteGroup(G_ALL); break;
    case P_SAVE_PRESET:
        if (!async_->busy.exchange(true)) {
            std::thread(SaveDialogThread, async_, BuildPresetText(), lyr::ForegroundWindowHandle()).detach();
        }
        break;
    case P_LOAD_PRESET:
        if (!async_->busy.exchange(true)) {
            std::thread(OpenDialogThread, async_, lyr::ForegroundWindowHandle()).detach();
        }
        break;
    default: break;
    }
}

FFResult LyricsPlugin::SetFloatParameter(unsigned int idx, float value)
{
    FlushAsync();
    if (idx >= P_COUNT) return FF_FAIL;
    if (idx == P_TEXT || idx == P_FILE || idx == P_PRESET_FILE) return FF_SUCCESS;

    // Botones trigger: Resolume envia 1 al pulsar. Actuamos una vez y devolvemos 0
    if (IsTriggerParam(idx)) {
        if (value > 0.5f) {
            ULONGLONG now = GetTickCount64();
            if (now - lastFire_[idx] > 80) { // evita dobles disparos en el mismo clic
                lastFire_[idx] = now;
                clicks_++;
                OnTrigger(idx);
                if (idx == P_RESTART || idx == P_PREV || idx == P_NEXT) {
                    int n = 0, i = 0;
                    state_.GetStatus(n, i);
                    lyr::Log(std::string("Boton ") + (idx == P_RESTART ? "Restart" : idx == P_PREV ? "Prev" : "Next") +
                             " -> paso " + std::to_string(i) + " de " + std::to_string(n));
                }
            }
        }
        return FF_SUCCESS;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        values_[idx] = value;
    }
    if (idx == P_FONT) {
        int i = FontIndexFromValue(value);
        if (i >= 0 && i < (int)fonts_.size()) state_.SetFont(fonts_[(size_t)i]);
    }
    return FF_SUCCESS;
}

float LyricsPlugin::GetFloatParameter(unsigned int index)
{
    if (index >= P_COUNT) return 0.0f;
    std::lock_guard<std::mutex> lk(mu_);
    return values_[index]; // los triggers siempre valen 0
}

// Texto que Resolume muestra dentro de las barras de los parametros
char* LyricsPlugin::GetParameterDisplay(unsigned int index)
{
    FlushAsync();
    if (index == P_FONT) {
        float v;
        { std::lock_guard<std::mutex> lk(mu_); v = values_[P_FONT]; }
        int i = FontIndexFromValue(v);
        std::string name = (i >= 0 && i < (int)fonts_.size()) ? lyr::WToUtf8(fonts_[(size_t)i]) : "-";
        snprintf(dispBuf_, sizeof(dispBuf_), "%s", name.c_str());
        return dispBuf_;
    }
    if (index == P_SH_ANGLE) {
        float v;
        { std::lock_guard<std::mutex> lk(mu_); v = values_[P_SH_ANGLE]; }
        snprintf(dispBuf_, sizeof(dispBuf_), "%d deg", (int)std::lround(v * 360.0f));
        return dispBuf_;
    }
    if (index == P_TRACK) {
        float v;
        { std::lock_guard<std::mutex> lk(mu_); v = values_[P_TRACK]; }
        snprintf(dispBuf_, sizeof(dispBuf_), "%.2f em", (v - 0.25f) * 0.4f);
        return dispBuf_;
    }
    if (index == P_WRAP) {
        float v;
        { std::lock_guard<std::mutex> lk(mu_); v = values_[P_WRAP]; }
        snprintf(dispBuf_, sizeof(dispBuf_), "%d %%", (int)std::lround((0.3f + 0.7f * v) * 100.0f));
        return dispBuf_;
    }
    if (index == P_IN_TIME || index == P_OUT_TIME) {
        float v;
        { std::lock_guard<std::mutex> lk(mu_); v = values_[index]; }
        snprintf(dispBuf_, sizeof(dispBuf_), "%.2f s", v * 2.0f);
        return dispBuf_;
    }
    return CFFGLPlugin::GetParameterDisplay(index);
}

// La letra viene del archivo (si hay y existe) o, si no, del cuadro de texto.
void LyricsPlugin::RefreshLyricsLocked()
{
    fileOk_ = false;
    fileStamp_ = 0;
    if (!fileUtf8_.empty()) {
        filePath_ = lyr::ResolveLyricsPath(fileUtf8_);
        std::wstring content;
        if (!filePath_.empty() && lyr::ReadTextFile(filePath_, content)) {
            fileOk_ = true;
            fileStamp_ = lyr::FileStamp(filePath_);
            state_.SetLyrics(content);
            lyr::Log("Letra cargada del archivo: " + lyr::WToUtf8(filePath_));
            return;
        }
        lyr::Log("No se pudo leer el archivo: " + lyr::WToUtf8(filePath_));
    }
    state_.SetLyrics(lyr::Utf8ToW(textUtf8_));
}

void LyricsPlugin::CheckFileChanged()
{
    ULONGLONG now = GetTickCount64();
    if (now - lastFileCheck_ < 1000) return;
    lastFileCheck_ = now;
    std::lock_guard<std::mutex> lk(fileMu_);
    if (fileUtf8_.empty()) return;
    ULONGLONG st = filePath_.empty() ? 0 : lyr::FileStamp(filePath_);
    if (st != fileStamp_ || (!fileOk_ && st != 0)) RefreshLyricsLocked();
}

// Resolume envia los textos en CADA fotograma: solo reaccionamos si cambiaron.
FFResult LyricsPlugin::SetTextParameter(unsigned int index, const char* value)
{
    FlushAsync();
    if (index != P_TEXT && index != P_FILE && index != P_PRESET_FILE) return FF_FAIL;
    std::string v = value ? value : "";

    if (index == P_PRESET_FILE) {
        bool load = false;
        {
            std::lock_guard<std::mutex> lk(fileMu_);
            if (presetFileUtf8_ == v) return FF_SUCCESS;
            presetFileUtf8_ = v;
            // Los primeros segundos son la restauracion del proyecto: no re-aplicar el preset
            load = !v.empty() && (GetTickCount64() - createdTick_ > 2500);
        }
        if (load) LoadPresetFile(lyr::Utf8ToW(v));
        return FF_SUCCESS;
    }

    std::lock_guard<std::mutex> lk(fileMu_);
    std::string& target = (index == P_TEXT) ? textUtf8_ : fileUtf8_;
    if (target == v) return FF_SUCCESS;
    target = v;
    lyr::Log(std::string(index == P_TEXT ? "Texto de la caja recibido: " : "Archivo recibido: ") +
             std::to_string(v.size()) + " bytes");
    RefreshLyricsLocked();
    int n = 0, i = 0;
    state_.GetStatus(n, i);
    lyr::Log("Lineas detectadas: " + std::to_string(n));
    return FF_SUCCESS;
}

char* LyricsPlugin::GetTextParameter(unsigned int index)
{
    if (index == P_TEXT) return const_cast<char*>(textUtf8_.c_str());
    if (index == P_FILE) return const_cast<char*>(fileUtf8_.c_str());
    if (index == P_PRESET_FILE) return const_cast<char*>(presetFileUtf8_.c_str());
    return const_cast<char*>(emptyText_.c_str());
}

// Al cambiar de linea: la imagen actual pasa a "saliente" y la nueva entra.
void LyricsPlugin::StartTransition(const float* v)
{
    std::swap(cur_, prev_);
    cur_.key = RenderKey();
    cur_.has = false;
    transStart_ = NowSec();
    tIn_ = (int)std::lround(v[P_ANIM_IN]);
    tOut_ = (int)std::lround(v[P_ANIM_OUT]);
    tInDur_ = v[P_IN_TIME] * 2.0f;
    tOutDur_ = v[P_OUT_TIME] * 2.0f;
    tDist_ = v[P_ANIM_DIST];
    tEase_ = (int)std::lround(v[P_EASE]);
    if (tIn_ < 0 || tIn_ >= A_COUNT) tIn_ = 0;
    if (tOut_ < 0 || tOut_ >= A_COUNT) tOut_ = 0;
    outActive_ = prev_.has && tOut_ != A_NONE && tOutDur_ > 0.001f;
}

// e: 0 = oculto, 1 = visible del todo
void LyricsPlugin::DrawLayer(const Layer& L, int kind, bool isIn, float e)
{
    float ox = 0.0f, oy = 0.0f, sc = 1.0f, op = 1.0f, lo = -1.0f, hi = 2.0f, soft = 0.004f;
    float blur = 0.0f;
    float typeOn = 0.0f, typeLo = 0.0f, typeHi = 1.0f, typeX = 2.0f;
    const float hid = 1.0f - e;
    const float slide = tDist_ * 0.5f;
    const float zoom = tDist_ * 0.8f;
    const float bw = L.bx1 - L.bx0;
    const float sgn = isIn ? -1.0f : 1.0f;

    switch (kind) {
    case A_FADE: op = e; break;
    case A_SLIDE_UP: oy = sgn * slide * hid; op = e; break;
    case A_SLIDE_DOWN: oy = -sgn * slide * hid; op = e; break;
    case A_SLIDE_LEFT: ox = -sgn * slide * hid; op = e; break;
    case A_SLIDE_RIGHT: ox = sgn * slide * hid; op = e; break;
    case A_ZOOM_IN: sc = isIn ? 1.0f - zoom * hid : 1.0f + zoom * hid; op = e; break;
    case A_ZOOM_OUT: sc = isIn ? 1.0f + zoom * hid : 1.0f - zoom * hid; op = e; break;
    case A_WIPE_RIGHT:
        if (isIn) { lo = L.bx0; hi = L.bx0 + e * bw; }
        else { lo = L.bx0 + hid * bw; hi = L.bx1; }
        break;
    case A_WIPE_LEFT:
        if (isIn) { lo = L.bx1 - e * bw; hi = L.bx1; }
        else { lo = L.bx0; hi = L.bx1 - hid * bw; }
        break;
    case A_BLUR:
        blur = hid * tDist_ * 0.06f; // radio en uv (x); Anim Distance = cantidad
        op = e;
        break;
    case A_TYPE: {
        // Maquina de escribir: aparecen letra a letra, linea por linea (al salir se borra desde el final)
        int total = 0;
        for (const Layer::LineBand& bd : L.bands) total += bd.charU.size() > 1 ? (int)bd.charU.size() - 1 : 0;
        if (total == 0) {
            lo = L.bx0; hi = L.bx0 + e * bw; // sin datos de caracteres: barrido simple
        } else {
            int c = (int)std::floor(e * (float)total + 0.0001f);
            if (c < 0) c = 0;
            if (c > total) c = total;
            if (c == 0) {
                op = 0.0f;
            } else if (c < total) {
                int rem = c;
                for (const Layer::LineBand& bd : L.bands) {
                    const int nk = bd.charU.size() > 1 ? (int)bd.charU.size() - 1 : 0;
                    if (nk == 0) continue;
                    if (rem <= nk) {
                        typeOn = 1.0f;
                        typeLo = bd.yLo;
                        typeHi = bd.yHi;
                        typeX = bd.charU[(size_t)rem];
                        break;
                    }
                    rem -= nk;
                }
            }
        }
        break;
    }
    default: break;
    }
    op = Clamp01(op);
    if (sc < 0.05f) sc = 0.05f;
    blur += staticBlurU_; // desenfoque fijo del grupo "Blur"

    glBindTexture(GL_TEXTURE_2D, L.tex);
    shader_.Set("uA", ox, oy, sc);
    shader_.Set("uB", L.cx, L.cy, op);
    shader_.Set("uC", lo, hi, soft);
    shader_.Set("uD", blur, (float)w_ / (float)(h_ > 0 ? h_ : 1), 0.0f);
    shader_.Set("uE", typeOn, typeLo, typeHi);
    shader_.Set("uG", typeX, 0.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

FFResult LyricsPlugin::ProcessOpenGL(ProcessOpenGLStruct* /*pGL*/)
{
    FlushAsync();
    float v[P_COUNT];
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < P_COUNT; i++) v[i] = values_[i];
    }
    state_.Touch();
    CheckFileChanged();
    lyr::LyricsState::View view = state_.GetView();

    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        lyr::Log("Primer fotograma. Viewport " + std::to_string(w_) + "x" + std::to_string(h_) +
                 ", textura=" + std::to_string(cur_.tex));
    }

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (w_ <= 0 || h_ <= 0 || !cur_.tex || !prev_.tex) return FF_SUCCESS;

    // Cambio de linea -> empieza una transicion (animacion de entrada/salida)
    if (view.serial != lastSerial_) {
        lastSerial_ = view.serial;
        StartTransition(v);
    }
    staticBlurU_ = v[P_BLUR_AMT] * 0.02f;

    // Texto de diagnostico SOLO cuando no hay ninguna letra cargada ("Hide Status" lo oculta)
    if (!view.blank && view.text.empty() && v[P_HIDESTATUS] < 0.5f) {
        int n = 0, i = 0;
        state_.GetStatus(n, i);
        if (n == 0) {
            std::wstring fileState = fileUtf8_.empty() ? L"-" : (fileOk_ ? L"OK" : L"NO ENCONTRADO");
            view.text = L"[Lyrics OSC] sin letra cargada\nclics=" + std::to_wstring(clicks_.load()) +
                        L"  caja=" + std::to_wstring(textUtf8_.size()) + L" bytes  archivo=" + fileState;
        }
    }

    const bool want = !view.blank && !view.text.empty();
    GlStateGuard guard;

    // --- Imagen de la linea actual (se vuelve a dibujar solo si algo cambio)
    if (want) {
        RenderKey key;
        key.valid = true;
        key.text = view.text;
        key.font = view.font;
        const float h = (float)h_;
        render::Style st;
        st.font = key.font;
        st.bold = v[P_BOLD] > 0.5f;
        st.italic = v[P_ITALIC] > 0.5f;
        st.sizePx = h * (0.04f + 0.30f * v[P_SIZE]); // Size 0.5 = 19 % de la altura, 1.0 = 34 %
        st.align = (int)std::lround(v[P_ALIGN]);
        st.lineSpacing = 1.0f + v[P_SPACING];
        st.vertical = v[P_POS];
        st.tracking = (v[P_TRACK] - 0.25f) * 0.4f;
        st.wrapWidth = 0.3f + 0.7f * v[P_WRAP];
        st.fill[0] = v[P_COLOR_R]; st.fill[1] = v[P_COLOR_G]; st.fill[2] = v[P_COLOR_B];
        st.outlineWidthPx = v[P_OUT_W] * 0.02f * h;
        st.outline[0] = v[P_OUT_R]; st.outline[1] = v[P_OUT_G]; st.outline[2] = v[P_OUT_B];
        st.shadowOpacity = v[P_SH_OP];
        st.shadowDistPx = v[P_SH_DIST] * 0.05f * h;
        st.shadowAngleDeg = v[P_SH_ANGLE] * 360.0f;
        st.shadowBlurPx = v[P_SH_BLUR] * 0.04f * h;
        st.shadow[0] = v[P_SH_R]; st.shadow[1] = v[P_SH_G]; st.shadow[2] = v[P_SH_B];
        st.glowIntensity = v[P_GL_INT] * 4.0f;
        st.glowBlurPx = v[P_GL_BLUR] * 0.06f * h;
        st.glow[0] = v[P_GL_R]; st.glow[1] = v[P_GL_G]; st.glow[2] = v[P_GL_B];

        key.nums = {st.sizePx, (float)st.align, st.lineSpacing, st.vertical, st.bold ? 1.0f : 0.0f,
                    st.italic ? 1.0f : 0.0f, st.fill[0], st.fill[1], st.fill[2], st.outlineWidthPx,
                    st.outline[0], st.outline[1], st.outline[2], st.shadowOpacity, st.shadowDistPx,
                    st.shadowAngleDeg, st.shadowBlurPx, st.shadow[0], st.shadow[1], st.shadow[2],
                    st.glowIntensity, st.glowBlurPx, st.glow[0], st.glow[1], st.glow[2],
                    st.tracking, st.wrapWidth, (float)w_, (float)h_};

        if (!(key == cur_.key)) {
            render::Result rr;
            bool ok = render::RenderText(key.text, st, w_, h_, pixels_, &rr);
            if (ok) {
                glBindTexture(GL_TEXTURE_2D, cur_.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                             pixels_.data());
                cur_.key = key;
                if (rr.any) {
                    cur_.bx0 = (float)rr.minX / (float)w_ - 0.01f;
                    cur_.bx1 = (float)(rr.maxX + 1) / (float)w_ + 0.01f;
                    cur_.cx = 0.5f * (cur_.bx0 + cur_.bx1);
                    cur_.cy = 1.0f - 0.5f * (float)(rr.minY + rr.maxY + 1) / (float)h_;
                    cur_.bands.clear();
                    for (const render::LineInfo& li : rr.lines) {
                        Layer::LineBand b;
                        b.yHi = 1.0f - li.topPx / (float)h_;
                        b.yLo = 1.0f - (li.topPx + li.heightPx) / (float)h_;
                        for (float px : li.charX) b.charU.push_back(px / (float)w_);
                        cur_.bands.push_back(b);
                    }
                } else {
                    cur_.bands.clear();
                    cur_.bx0 = 0.0f; cur_.bx1 = 1.0f; cur_.cx = 0.5f; cur_.cy = 0.5f;
                }
            }
            if (key.text != lastLoggedText_) {
                lastLoggedText_ = key.text;
                lyr::Log(std::string("Texto dibujado: '") + lyr::WToUtf8(key.text.substr(0, 40)) +
                         "' fuente=" + lyr::WToUtf8(key.font) + " ok=" + (ok ? "si" : "NO"));
            }
        }
        cur_.has = cur_.key.valid;
    } else {
        cur_.has = false;
        cur_.key = RenderKey();
    }

    // --- Progreso de las animaciones
    const double now = NowSec();
    bool drawPrev = false;
    float ePrev = 0.0f;
    if (outActive_) {
        float t = (float)((now - transStart_) / (double)tOutDur_);
        if (t >= 1.0f) outActive_ = false;
        else { drawPrev = true; ePrev = 1.0f - Ease(tEase_, t); }
    }
    const bool drawCur = cur_.has;
    float eCur = 1.0f;
    if (drawCur && tIn_ != A_NONE && tInDur_ > 0.001f) {
        float t = (float)((now - transStart_) / (double)tInDur_);
        eCur = Ease(tEase_, t);
    }
    if (!drawPrev && !drawCur) return FF_SUCCESS;

    // --- Dibujo (mezcla con alfa premultiplicado)
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(shader_.GetGLID());
    shader_.Set("tex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);

    if (drawPrev) DrawLayer(prev_, tOut_, false, ePrev);
    if (drawCur) DrawLayer(cur_, tIn_, true, eCur);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR && glErrLogs_ < 5) {
        glErrLogs_++;
        char b2[16];
        snprintf(b2, sizeof(b2), "%X", (unsigned)err);
        lyr::Log(std::string("Error OpenGL tras dibujar: 0x") + b2);
    } else if (err == GL_NO_ERROR && !loggedFirstDraw_) {
        loggedFirstDraw_ = true;
        lyr::Log("Primer dibujo sin errores de OpenGL");
    }
    return FF_SUCCESS;
}

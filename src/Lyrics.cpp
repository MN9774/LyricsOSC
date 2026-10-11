// Plugin FFGL "Lyrics OSC" para Resolume Arena
// Letras linea por linea, tipografias, color, contorno, sombra, animaciones y OSC.
#include <FFGLSDK.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "LyricsCore.h"
#include "TextRender.h"

using namespace ffglex;

enum ParamID : FFUInt32 {
    P_TEXT = 0,   // cuadro de texto con la letra (una linea = un paso)
    P_FILE,       // archivo .txt (arrastrar y soltar)
    P_RESTART,    // boton: vuelve a "nada en pantalla"
    P_PREV,       // boton: linea anterior
    P_NEXT,       // boton: linea siguiente

    P_FONT,
    P_SIZE,
    P_COLOR_R,    // Color del texto (selector de color: R, G, B consecutivos)
    P_COLOR_G,
    P_COLOR_B,
    P_ALIGN,
    P_POS,
    P_SPACING,
    P_BOLD,
    P_ITALIC,

    P_OUT_W,      // Contorno: grosor (0 = sin contorno)
    P_OUT_R,      // Contorno: color
    P_OUT_G,
    P_OUT_B,

    P_SH_OP,      // Sombra: opacidad (0 = sin sombra)
    P_SH_DIST,
    P_SH_ANGLE,
    P_SH_BLUR,
    P_SH_R,       // Sombra: color
    P_SH_G,
    P_SH_B,

    P_GL_INT,     // Glow: intensidad (0 = sin glow)
    P_GL_BLUR,    // Glow: desenfoque gaussiano
    P_GL_R,       // Glow: color
    P_GL_G,
    P_GL_B,

    P_ANIM_IN,    // Animacion de entrada de cada linea
    P_IN_TIME,
    P_ANIM_OUT,   // Animacion de salida de la linea anterior
    P_OUT_TIME,
    P_ANIM_DIST,
    P_EASE,

    P_HIDESTATUS, // oculta el texto de diagnostico
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
static const char* kFragment = R"GLSL(#version 410 core
uniform sampler2D tex;
uniform vec3 uA;
uniform vec3 uB;
uniform vec3 uC;
uniform vec3 uD;
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
    std::vector<float> charU;     // borde tras cada caracter en uv (maquina de escribir)
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

private:
    void RefreshLyricsLocked(); // requiere fileMu_
    void CheckFileChanged();
    void MakeTexture(GLuint& t);
    void StartTransition(const float* v);
    void DrawLayer(const Layer& L, int kind, bool isIn, float e);

    lyr::LyricsState state_;
    std::vector<std::wstring> fonts_;

    std::mutex mu_;
    float values_[P_COUNT];
    ULONGLONG lastFire_[P_COUNT];
    std::atomic<int> clicks_{0};

    std::mutex fileMu_;
    std::string textUtf8_;
    std::string fileUtf8_;
    std::string emptyText_;
    std::wstring filePath_;
    ULONGLONG fileStamp_ = 0;
    ULONGLONG lastFileCheck_ = 0;
    bool fileOk_ = false;

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
    "Letras linea por linea con tipografias, contorno, sombra, animaciones y OSC",
    "Lyrics OSC");

LyricsPlugin::LyricsPlugin()
{
    SetMinInputs(0);
    SetMaxInputs(0);

    for (int i = 0; i < P_COUNT; i++) { values_[i] = 0.0f; lastFire_[i] = 0; }

    // --- Letra
    SetParamInfo(P_TEXT, "Lyrics", FF_TYPE_TEXT, (const char*)"");
    SetFileParamInfo(P_FILE, "Lyrics File", std::vector<std::string>{"txt", ".txt"}, "");

    // --- Botones de pulsar y soltar (trigger)
    SetParamInfof(P_RESTART, "Restart", FF_TYPE_EVENT);
    SetParamInfof(P_PREV, "Prev", FF_TYPE_EVENT);
    SetParamInfof(P_NEXT, "Next", FF_TYPE_EVENT);

    // --- Tipografia: todas las fuentes instaladas en Windows
    fonts_ = render::InstalledFonts();
    if (fonts_.size() > 500) fonts_.resize(500);
    int defFont = 0;
    for (size_t i = 0; i < fonts_.size(); i++)
        if (_wcsicmp(fonts_[i].c_str(), L"Arial") == 0) { defFont = (int)i; break; }
    values_[P_FONT] = (float)defFont;
    state_.SetFont(fonts_[(size_t)defFont]);
    SetOptionParamInfo(P_FONT, "Font", (unsigned int)fonts_.size(), (float)defFont);
    for (size_t i = 0; i < fonts_.size(); i++)
        SetParamElementInfo(P_FONT, (unsigned int)i, lyr::WToUtf8(fonts_[i]).c_str(), (float)i);

    values_[P_SIZE] = 0.3f;
    SetParamInfo(P_SIZE, "Size", FF_TYPE_STANDARD, 0.3f);

    // --- Color del texto (selector de color)
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
    SetParamInfof(P_BOLD, "Bold", FF_TYPE_BOOLEAN);
    SetParamInfof(P_ITALIC, "Italic", FF_TYPE_BOOLEAN);

    // --- Contorno
    SetParamInfo(P_OUT_W, "Outline Width", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_OUT_R, "Outline Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_OUT_G, "Outline Color G", FF_TYPE_GREEN, 0.0f);
    SetParamInfo(P_OUT_B, "Outline Color B", FF_TYPE_BLUE, 0.0f);

    // --- Sombra
    values_[P_SH_DIST] = 0.3f;
    values_[P_SH_ANGLE] = 0.875f; // 315 grados: abajo a la derecha
    values_[P_SH_BLUR] = 0.3f;
    SetParamInfo(P_SH_OP, "Shadow Opacity", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_SH_DIST, "Shadow Distance", FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(P_SH_ANGLE, "Shadow Angle", FF_TYPE_STANDARD, 0.875f);
    SetParamInfo(P_SH_BLUR, "Shadow Blur", FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(P_SH_R, "Shadow Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_SH_G, "Shadow Color G", FF_TYPE_GREEN, 0.0f);
    SetParamInfo(P_SH_B, "Shadow Color B", FF_TYPE_BLUE, 0.0f);

    // --- Glow (resplandor)
    values_[P_GL_BLUR] = 0.4f;
    values_[P_GL_G] = 0.8f;
    values_[P_GL_B] = 1.0f;
    SetParamInfo(P_GL_INT, "Glow Intensity", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_GL_BLUR, "Glow Blur", FF_TYPE_STANDARD, 0.4f);
    SetParamInfo(P_GL_R, "Glow Color", FF_TYPE_RED, 0.0f);
    SetParamInfo(P_GL_G, "Glow Color G", FF_TYPE_GREEN, 0.8f);
    SetParamInfo(P_GL_B, "Glow Color B", FF_TYPE_BLUE, 1.0f);

    // --- Animaciones (se repiten en cada cambio de linea)
    SetOptionParamInfo(P_ANIM_IN, "Animation In", (unsigned int)A_COUNT, 0.0f);
    SetOptionParamInfo(P_ANIM_OUT, "Animation Out", (unsigned int)A_COUNT, 0.0f);
    for (int i = 0; i < A_COUNT; i++) {
        SetParamElementInfo(P_ANIM_IN, (unsigned int)i, kAnimNames[i], (float)i);
        SetParamElementInfo(P_ANIM_OUT, (unsigned int)i, kAnimNames[i], (float)i);
    }
    values_[P_IN_TIME] = 0.15f;
    values_[P_OUT_TIME] = 0.15f;
    values_[P_ANIM_DIST] = 0.4f;
    values_[P_EASE] = 1.0f;
    SetParamInfo(P_IN_TIME, "Anim In Time", FF_TYPE_STANDARD, 0.15f);
    SetParamInfo(P_OUT_TIME, "Anim Out Time", FF_TYPE_STANDARD, 0.15f);
    SetParamInfo(P_ANIM_DIST, "Anim Distance", FF_TYPE_STANDARD, 0.4f);
    SetOptionParamInfo(P_EASE, "Anim Easing", 4, 1.0f);
    for (int i = 0; i < 4; i++) SetParamElementInfo(P_EASE, (unsigned int)i, kEaseNames[i], (float)i);

    SetParamInfof(P_HIDESTATUS, "Hide Status", FF_TYPE_BOOLEAN);

    lyr::Log("Plugin creado. Fuentes: " + std::to_string(fonts_.size()));
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

FFResult LyricsPlugin::SetFloatParameter(unsigned int idx, float value)
{
    if (idx >= P_COUNT) return FF_FAIL;
    if (idx == P_TEXT || idx == P_FILE) return FF_SUCCESS;

    // Botones trigger: Resolume envia 1 al pulsar. Actuamos una vez y devolvemos 0
    if (idx == P_RESTART || idx == P_PREV || idx == P_NEXT) {
        if (value > 0.5f) {
            ULONGLONG now = GetTickCount64();
            if (now - lastFire_[idx] > 80) { // evita dobles disparos en el mismo clic
                lastFire_[idx] = now;
                clicks_++;
                if (idx == P_RESTART) state_.Restart();
                else if (idx == P_PREV) state_.Prev();
                else state_.Next();
                int n = 0, i = 0;
                state_.GetStatus(n, i);
                lyr::Log(std::string("Boton ") + (idx == P_RESTART ? "Restart" : idx == P_PREV ? "Prev" : "Next") +
                         " -> paso " + std::to_string(i) + " de " + std::to_string(n));
            }
        }
        return FF_SUCCESS;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        values_[idx] = value;
    }
    if (idx == P_FONT) {
        int i = (int)std::lround(value);
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
    if (index != P_TEXT && index != P_FILE) return FF_FAIL;
    std::string v = value ? value : "";
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
        // Maquina de escribir: aparecen letra a letra (al salir se borran desde el final)
        const int n = L.charU.size() > 1 ? (int)L.charU.size() - 1 : 0;
        if (n == 0) {
            // texto de varias lineas: barrido simple
            if (isIn) { lo = L.bx0; hi = L.bx0 + e * bw; }
            else { lo = L.bx0; hi = L.bx0 + e * bw; }
        } else {
            int c = (int)std::floor(e * (float)n + 0.0001f);
            if (c < 0) c = 0;
            if (c > n) c = n;
            if (c == 0) op = 0.0f;
            else if (c < n) { hi = L.charU[(size_t)c]; soft = 0.0002f; }
        }
        break;
    }
    default: break;
    }
    op = Clamp01(op);
    if (sc < 0.05f) sc = 0.05f;

    glBindTexture(GL_TEXTURE_2D, L.tex);
    shader_.Set("uA", ox, oy, sc);
    shader_.Set("uB", L.cx, L.cy, op);
    shader_.Set("uC", lo, hi, soft);
    shader_.Set("uD", blur, (float)w_ / (float)(h_ > 0 ? h_ : 1), 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

FFResult LyricsPlugin::ProcessOpenGL(ProcessOpenGLStruct* /*pGL*/)
{
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
        st.sizePx = h * (0.05f + 0.25f * v[P_SIZE]);
        st.align = (int)std::lround(v[P_ALIGN]);
        st.lineSpacing = 1.0f + v[P_SPACING];
        st.vertical = v[P_POS];
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
                    (float)w_, (float)h_};

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
                    cur_.charU.clear();
                    for (float px : rr.charX) cur_.charU.push_back(px / (float)w_);
                } else {
                    cur_.charU.clear();
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

// Plugin FFGL "Lyrics OSC" para Resolume Arena
// Letras linea por linea, varias tipografias y control por OSC.
#include <FFGLSDK.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
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
    P_FILE,       // opcional: nombre de un .txt (se edita en el Bloc de notas)
    P_RESTART,    // boton: vuelve a "nada en pantalla"
    P_PREV,       // boton: linea anterior
    P_NEXT,       // boton: linea siguiente
    P_FONT,
    P_SIZE,
    P_HUE,
    P_SAT,
    P_DARK,
    P_ALIGN,
    P_POS,
    P_SPACING,
    P_BOLD,
    P_ITALIC,
    P_HIDESTATUS, // oculta el texto de diagnostico
    P_COUNT
};

static const char* kVertex = R"GLSL(#version 410 core
out vec2 uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char* kFragment = R"GLSL(#version 410 core
uniform sampler2D tex;
uniform vec3 color;
in vec2 uv;
out vec4 fragColor;
void main()
{
    float a = texture(tex, vec2(uv.x, 1.0 - uv.y)).a;
    fragColor = vec4(color * a, a); // alfa premultiplicado
}
)GLSL";

static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    h = h - std::floor(h);
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    int seg = (int)(h * 6.0f);
    switch (seg) {
    case 0: rr = c; gg = x; break;
    case 1: rr = x; gg = c; break;
    case 2: gg = c; bb = x; break;
    case 3: gg = x; bb = c; break;
    case 4: rr = x; bb = c; break;
    default: rr = c; bb = x; break;
    }
    r = rr + m; g = gg + m; b = bb + m;
}

struct RenderKey {
    std::wstring text, font;
    bool bold = false, italic = false;
    float size = 0, spacing = 0, vertical = 0;
    int align = 0, w = 0, h = 0;
    bool valid = false;
    bool operator==(const RenderKey& o) const
    {
        return valid && o.valid && text == o.text && font == o.font && bold == o.bold &&
               italic == o.italic && size == o.size && spacing == o.spacing &&
               vertical == o.vertical && align == o.align && w == o.w && h == o.h;
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
    GLuint tex_ = 0;
    GLuint vao_ = 0;
    int w_ = 0, h_ = 0;
    RenderKey lastKey_;
    std::vector<unsigned char> pixels_;
    int glErrLogs_ = 0;
    bool loggedFirstFrame_ = false;
    bool loggedFirstDraw_ = false;
};

static CFFGLPluginInfo PluginInfo(
    PluginFactory<LyricsPlugin>,
    "LYRC",
    "Lyrics OSC",
    2, 1,
    1, 0,
    FF_SOURCE,
    "Letras linea por linea con varias tipografias y control por OSC",
    "Lyrics OSC");

LyricsPlugin::LyricsPlugin()
{
    SetMinInputs(0);
    SetMaxInputs(0);

    for (int i = 0; i < P_COUNT; i++) { values_[i] = 0.0f; lastFire_[i] = 0; }

    // Cuadro de texto: aqui se pega la letra de la cancion (una linea = un paso)
    SetParamInfo(P_TEXT, "Lyrics", FF_TYPE_TEXT, (const char*)"");
    // Alternativa: arrastra un .txt aqui (o haz clic para buscarlo). Se edita en el Bloc de notas.
    SetFileParamInfo(P_FILE, "Lyrics File", std::vector<std::string>{"txt", ".txt"}, "");

    // Botones de pulsar y soltar (trigger)
    SetParamInfof(P_RESTART, "Restart", FF_TYPE_EVENT);
    SetParamInfof(P_PREV, "Prev", FF_TYPE_EVENT);
    SetParamInfof(P_NEXT, "Next", FF_TYPE_EVENT);

    // Tipografia: todas las fuentes instaladas en Windows
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
    SetParamInfo(P_HUE, "Hue", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_SAT, "Saturation", FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(P_DARK, "Darken", FF_TYPE_STANDARD, 0.0f);

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
    SetParamInfof(P_HIDESTATUS, "Hide Status", FF_TYPE_BOOLEAN);

    lyr::Log("Plugin creado. Fuentes: " + std::to_string(fonts_.size()));
}

FFResult LyricsPlugin::InitGL(const FFGLViewportStruct* vp)
{
    if (!shader_.Compile(kVertex, kFragment)) {
        lyr::Log("ERROR: no compila el shader (InitGL falla)");
        DeInitGL();
        return FF_FAIL;
    }
    glGenVertexArrays(1, &vao_);
    glGenTextures(1, &tex_);
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    w_ = (int)vp->width;
    h_ = (int)vp->height;
    lastKey_ = RenderKey();
    lyr::Log("InitGL ok. Viewport " + std::to_string(w_) + "x" + std::to_string(h_));
    return CFFGLPlugin::InitGL(vp);
}

FFResult LyricsPlugin::DeInitGL()
{
    shader_.FreeGLResources();
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    lastKey_ = RenderKey();
    return FF_SUCCESS;
}

FFResult LyricsPlugin::SetFloatParameter(unsigned int idx, float value)
{
    if (idx >= P_COUNT) return FF_FAIL;
    if (idx == P_TEXT || idx == P_FILE) return FF_SUCCESS;

    // Botones trigger: Resolume envia 1 al pulsar. Actuamos una vez y devolvemos 0
    // (como hace el SDK oficial al "consumir" el trigger).
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

// La letra viene del archivo (si hay nombre y existe) o, si no, del cuadro de texto.
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
    lyr::Log(std::string(index == P_TEXT ? "Texto de la caja recibido: " : "Nombre de archivo recibido: ") +
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
                 ", textura=" + std::to_string(tex_));
    }

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Texto de diagnostico SOLO cuando no hay ninguna letra cargada (se oculta con "Hide Status").
    // Las lineas en blanco y el paso 0 se quedan realmente vacios.
    if (!view.blank && view.text.empty() && v[P_HIDESTATUS] < 0.5f) {
        int n = 0, i = 0;
        state_.GetStatus(n, i);
        if (n == 0) {
            std::wstring fileState = fileUtf8_.empty() ? L"-" : (fileOk_ ? L"OK" : L"NO ENCONTRADO");
            view.text = L"[Lyrics OSC] sin letra cargada\nclics=" + std::to_wstring(clicks_.load()) +
                        L"  caja=" + std::to_wstring(textUtf8_.size()) + L" bytes  archivo=" + fileState;
        }
    }

    if (view.blank || view.text.empty() || w_ <= 0 || h_ <= 0 || !tex_) return FF_SUCCESS;

    RenderKey key;
    key.valid = true;
    key.text = view.text;
    key.font = view.font;
    key.bold = v[P_BOLD] > 0.5f;
    key.italic = v[P_ITALIC] > 0.5f;
    key.size = (float)h_ * (0.05f + 0.25f * v[P_SIZE]);
    key.align = (int)std::lround(v[P_ALIGN]);
    key.spacing = 1.0f + v[P_SPACING];
    key.vertical = v[P_POS];
    key.w = w_;
    key.h = h_;

    // Guardar estado de GL que vamos a tocar
    GLint prevProg = 0, prevVao = 0, prevActive = 0, prevTex = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);

    glBindTexture(GL_TEXTURE_2D, tex_);
    if (!(key == lastKey_)) {
        render::Style st;
        st.font = key.font;
        st.bold = key.bold;
        st.italic = key.italic;
        st.sizePx = key.size;
        st.align = key.align;
        st.lineSpacing = key.spacing;
        st.vertical = key.vertical;
        bool ok = render::RenderText(key.text, st, w_, h_, pixels_);
        if (ok) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                         pixels_.data());
            lastKey_ = key;
        }
        size_t opaque = 0;
        if (ok) for (size_t p = 3; p < pixels_.size(); p += 4) if (pixels_[p]) opaque++;
        lyr::Log(std::string("Texto dibujado: '") + lyr::WToUtf8(key.text.substr(0, 40)) + "' fuente=" +
                 lyr::WToUtf8(key.font) + " ok=" + (ok ? "si" : "NO") + " pixeles_con_texto=" +
                 std::to_string(opaque));
    }

    float r, g, b;
    HsvToRgb(v[P_HUE], v[P_SAT], 1.0f - v[P_DARK], r, g, b);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(shader_.GetGLID());
    shader_.Set("tex", 0);
    shader_.Set("color", r, g, b);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR && glErrLogs_ < 5) {
        glErrLogs_++;
        lyr::Log("Error OpenGL tras dibujar: 0x" + [&] { char b2[16]; snprintf(b2, sizeof(b2), "%X", (unsigned)err); return std::string(b2); }());
    } else if (err == GL_NO_ERROR && !loggedFirstDraw_) {
        loggedFirstDraw_ = true;
        lyr::Log("Primer dibujo sin errores de OpenGL");
    }

    // Restaurar
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    return FF_SUCCESS;
}

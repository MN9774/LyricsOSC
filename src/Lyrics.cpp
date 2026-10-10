// Plugin FFGL "Lyrics OSC" para Resolume Arena
// Fuente de texto para letras: varias tipografias, lista de estrofas y control por OSC.
#include <FFGLSDK.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "LyricsCore.h"
#include "TextRender.h"

using namespace ffglex;

enum ParamID : FFUInt32 {
    P_FONT = 0,
    P_SIZE,
    P_HUE,
    P_SAT,
    P_DARK,
    P_ALIGN,
    P_POS,
    P_SPACING,
    P_BOLD,
    P_ITALIC,
    P_SONG,
    P_STANZA,
    P_NEXT,
    P_PREV,
    P_BLANK,
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

private:
    lyr::LyricsState state_;
    std::vector<std::wstring> fonts_;
    std::vector<std::wstring> songs_;

    std::mutex mu_;
    float values_[P_COUNT];
    bool prevNext_ = false, prevPrev_ = false;

    FFGLShader shader_;
    GLuint tex_ = 0;
    GLuint vao_ = 0;
    int w_ = 0, h_ = 0;
    RenderKey lastKey_;
    std::vector<unsigned char> pixels_;
};

static CFFGLPluginInfo PluginInfo(
    PluginFactory<LyricsPlugin>,
    "LYRC",
    "Lyrics OSC",
    2, 1,
    1, 0,
    FF_SOURCE,
    "Letras con varias tipografias y control por OSC",
    "Lyrics OSC");

LyricsPlugin::LyricsPlugin()
{
    SetMinInputs(0);
    SetMaxInputs(0);

    for (int i = 0; i < P_COUNT; i++) values_[i] = 0.0f;

    fonts_ = render::InstalledFonts();
    if (fonts_.size() > 500) fonts_.resize(500);
    int defFont = 0;
    for (size_t i = 0; i < fonts_.size(); i++)
        if (_wcsicmp(fonts_[i].c_str(), L"Arial") == 0) { defFont = (int)i; break; }
    values_[P_FONT] = (float)defFont;
    state_.SetFont(fonts_[(size_t)defFont]);

    songs_ = lyr::ListSongFiles();

    // Fuente (lista de todas las tipografias instaladas)
    SetOptionParamInfo(P_FONT, "Font", (unsigned int)fonts_.size(), (float)defFont);
    for (size_t i = 0; i < fonts_.size(); i++)
        SetParamElementInfo(P_FONT, (unsigned int)i, lyr::WToUtf8(fonts_[i]).c_str(), (float)i);

    SetParamInfof(P_SIZE, "Size", FF_TYPE_STANDARD);
    SetParamInfof(P_HUE, "Hue", FF_TYPE_STANDARD);
    SetParamInfof(P_SAT, "Saturation", FF_TYPE_STANDARD);
    SetParamInfof(P_DARK, "Darken", FF_TYPE_STANDARD);

    values_[P_ALIGN] = 1.0f;
    SetOptionParamInfo(P_ALIGN, "Align", 3, 1.0f);
    SetParamElementInfo(P_ALIGN, 0, "Left", 0.0f);
    SetParamElementInfo(P_ALIGN, 1, "Center", 1.0f);
    SetParamElementInfo(P_ALIGN, 2, "Right", 2.0f);

    SetParamInfof(P_POS, "Position", FF_TYPE_STANDARD);
    SetParamInfof(P_SPACING, "Line Spacing", FF_TYPE_STANDARD);
    SetParamInfof(P_BOLD, "Bold", FF_TYPE_BOOLEAN);
    SetParamInfof(P_ITALIC, "Italic", FF_TYPE_BOOLEAN);

    // Canciones: "(ninguna)" + los .txt de Documentos\Resolume Arena\Lyrics
    unsigned int nSongs = (unsigned int)std::min<size_t>(songs_.size(), 200);
    SetOptionParamInfo(P_SONG, "Song", nSongs + 1, 0.0f);
    SetParamElementInfo(P_SONG, 0, "(none)", 0.0f);
    for (unsigned int i = 0; i < nSongs; i++) {
        std::wstring name = songs_[i];
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos) name = name.substr(sl + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        SetParamElementInfo(P_SONG, i + 1, lyr::WToUtf8(name).c_str(), (float)(i + 1));
    }

    SetParamInfof(P_STANZA, "Stanza", FF_TYPE_STANDARD);
    SetParamInfof(P_NEXT, "Next", FF_TYPE_BOOLEAN);
    SetParamInfof(P_PREV, "Previous", FF_TYPE_BOOLEAN);
    SetParamInfof(P_BLANK, "Blank", FF_TYPE_BOOLEAN);
}

FFResult LyricsPlugin::InitGL(const FFGLViewportStruct* vp)
{
    if (!shader_.Compile(kVertex, kFragment)) {
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        values_[idx] = value;
    }
    switch (idx) {
    case P_FONT: {
        int i = (int)std::lround(value);
        if (i >= 0 && i < (int)fonts_.size()) state_.SetFont(fonts_[(size_t)i]);
        break;
    }
    case P_SONG: {
        int i = (int)std::lround(value);
        if (i <= 0) state_.Clear();
        else if (i - 1 < (int)songs_.size()) state_.LoadFile(songs_[(size_t)i - 1]);
        break;
    }
    case P_STANZA: state_.GotoNormalized(value); break;
    case P_NEXT: {
        bool high = value > 0.5f;
        if (high && !prevNext_) state_.Next();
        prevNext_ = high;
        break;
    }
    case P_PREV: {
        bool high = value > 0.5f;
        if (high && !prevPrev_) state_.Prev();
        prevPrev_ = high;
        break;
    }
    case P_BLANK: state_.SetBlank(value > 0.5f); break;
    default: break;
    }
    return FF_SUCCESS;
}

float LyricsPlugin::GetFloatParameter(unsigned int index)
{
    if (index >= P_COUNT) return 0.0f;
    std::lock_guard<std::mutex> lk(mu_);
    return values_[index];
}

FFResult LyricsPlugin::ProcessOpenGL(ProcessOpenGLStruct* /*pGL*/)
{
    float v[P_COUNT];
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < P_COUNT; i++) v[i] = values_[i];
    }
    lyr::LyricsState::View view = state_.GetView();

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

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
        if (render::RenderText(key.text, st, w_, h_, pixels_)) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                         pixels_.data());
            lastKey_ = key;
        }
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

    // Restaurar
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    return FF_SUCCESS;
}

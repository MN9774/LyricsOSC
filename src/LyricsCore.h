#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace lyr {

std::wstring Utf8ToW(const std::string& s);
std::string WToUtf8(const std::wstring& s);

// Documentos\Resolume Arena\Lyrics  (se crea si no existe)
std::wstring LyricsDir();
// Rutas completas de los .txt de esa carpeta, ordenadas por nombre
std::vector<std::wstring> ListSongFiles();

struct Stanza {
    std::wstring text;
    std::wstring font; // vacio = fuente por defecto
};

// Estado de una instancia del plugin. Se controla desde Resolume (parametros)
// y desde OSC (puerto UDP, por defecto 8000). Thread-safe.
class LyricsState {
public:
    LyricsState();
    ~LyricsState();

    void Next();
    void Prev();
    void Goto(int index0);
    void GotoNormalized(float v);
    void SetBlank(bool b);
    void SetDirectText(const std::wstring& t);
    void SetFont(const std::wstring& f);
    bool LoadFile(const std::wstring& path);
    void Clear();

    struct View {
        std::wstring text;
        std::wstring font;
        bool blank = false;
    };
    View GetView();

private:
    std::mutex m_;
    std::vector<Stanza> st_;
    int idx_ = -1;
    bool blank_ = false;
    bool direct_ = false;
    std::wstring directText_;
    std::wstring font_ = L"Arial";
};

} // namespace lyr

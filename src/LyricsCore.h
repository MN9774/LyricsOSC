#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace lyr {

std::wstring Utf8ToW(const std::string& s);
std::string WToUtf8(const std::wstring& s);

// Documentos\Resolume Arena\Lyrics  (solo para el archivo opcional osc_port.txt)
std::wstring LyricsDir();

// Registro de diagnostico: Documentos\Resolume Arena\Lyrics\lyrics_log.txt
void Log(const std::string& msg);

// Archivos de letra (.txt, UTF-8 o ANSI)
bool ReadTextFile(const std::wstring& path, std::wstring& out);
std::wstring ResolveLyricsPath(const std::string& nameUtf8); // nombre o ruta completa
unsigned long long FileStamp(const std::wstring& path);      // 0 si no existe

// Estado de una instancia (un clip) del plugin.
// La letra se pasa como texto; cada linea no vacia es un "paso".
//   indice -1 = no se muestra nada ("linea 0")
//   Next    -> avanza a la siguiente linea
//   Prev    -> retrocede una linea (desde la linea 1 vuelve a "nada")
//   Restart -> vuelve a "nada"
class LyricsState {
public:
    LyricsState();
    ~LyricsState();

    void SetLyrics(const std::wstring& raw);
    void Next();
    void Prev();
    void Restart();
    void Goto(int n1); // 1 = primera linea, 0 = nada
    void SetBlank(bool b);
    void SetFont(const std::wstring& f);

    // El plugin llama a Touch() en cada fotograma. Los mensajes OSC globales
    // solo llegan a clips que se estan reproduciendo (activos).
    void Touch();
    bool IsActive() const;
    void GetStatus(int& nLines, int& idx);

    struct View {
        std::wstring text;
        std::wstring font;
        bool blank = false;
        unsigned long long serial = 0; // sube cada vez que cambia de linea (para animar)
    };
    View GetView();

private:
    std::mutex m_;
    std::vector<std::wstring> lines_;
    int idx_ = -1;
    bool blank_ = false;
    std::wstring font_ = L"Arial";
    std::atomic<unsigned long long> lastTouch_{0};
    unsigned long long serial_ = 0;
};

} // namespace lyr

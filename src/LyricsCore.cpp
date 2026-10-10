#include "LyricsCore.h"
#include "TextRender.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <thread>

namespace lyr {

// ---------------------------------------------------------------- utilidades
static bool TryUtf8(const std::string& s, std::wstring& out)
{
    if (s.empty()) { out.clear(); return true; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n);
    return true;
}

std::wstring Utf8ToW(const std::string& s)
{
    std::wstring w;
    if (TryUtf8(s, w)) return w;
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    w.assign((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring LyricsDir()
{
    wchar_t docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
        return L".";
    std::wstring dir = std::wstring(docs) + L"\\Resolume Arena\\Lyrics";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::vector<std::wstring> ListSongFiles()
{
    std::vector<std::wstring> files;
    std::wstring dir = LyricsDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return files;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::vector<Stanza> ParseStanzas(const std::wstring& raw)
{
    std::vector<Stanza> out;
    std::vector<std::wstring> block;

    auto flush = [&]() {
        if (block.empty()) return;
        Stanza st;
        size_t first = 0;
        std::wstring head = Trim(block[0]);
        if (head.size() > 6 && _wcsnicmp(head.c_str(), L"#font=", 6) == 0) {
            st.font = Trim(head.substr(6));
            first = 1;
        }
        std::wstring text;
        for (size_t i = first; i < block.size(); i++) {
            if (!text.empty()) text += L'\n';
            text += block[i];
        }
        st.text = Trim(text);
        if (!st.text.empty()) out.push_back(st);
        block.clear();
    };

    std::wstringstream ss(raw);
    std::wstring line;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (Trim(line).empty()) flush();
        else block.push_back(line);
    }
    flush();
    return out;
}

// ---------------------------------------------------------------- estado
namespace {
std::mutex gLifecycle;
std::mutex gMtx;
std::vector<LyricsState*> gInst;
std::thread gThr;
std::atomic<bool> gRun{false};

int ReadPort()
{
    int port = 8000;
    std::ifstream f(LyricsDir() + L"\\osc_port.txt");
    int p = 0;
    if (f && (f >> p) && p > 0 && p < 65536) port = p;
    return port;
}

// ------------------------------ OSC
struct OscArg {
    char t = 0;
    int i = 0;
    float f = 0;
    std::string s;
};

int32_t BE32(const char* p)
{
    uint32_t v = ((uint32_t)(uint8_t)p[0] << 24) | ((uint32_t)(uint8_t)p[1] << 16) |
                 ((uint32_t)(uint8_t)p[2] << 8) | (uint32_t)(uint8_t)p[3];
    return (int32_t)v;
}

// lee string OSC en buf+pos; avanza pos (relleno a 4). false si desborda
bool ReadStr(const char* buf, int len, int& pos, std::string& out)
{
    if (pos >= len) return false;
    int n = 0;
    while (pos + n < len && buf[pos + n] != '\0') n++;
    if (pos + n >= len) return false;
    out.assign(buf + pos, (size_t)n);
    pos += ((n + 4) / 4) * 4;
    return true;
}

bool Pressed(const std::vector<OscArg>& a)
{
    if (a.empty()) return true;
    if (a[0].t == 'i') return a[0].i != 0;
    if (a[0].t == 'f') return a[0].f != 0.0f;
    if (a[0].t == 'T') return true;
    if (a[0].t == 'F') return false;
    return true;
}

int AsInt(const std::vector<OscArg>& a, int def = 0)
{
    if (a.empty()) return def;
    if (a[0].t == 'i') return a[0].i;
    if (a[0].t == 'f') return (int)std::lround(a[0].f);
    if (a[0].t == 's') return atoi(a[0].s.c_str());
    return def;
}

std::wstring ReplaceNewlines(std::wstring s)
{
    size_t p = 0;
    while ((p = s.find(L"\\n", p)) != std::wstring::npos) {
        s.replace(p, 2, L"\n");
        p += 1;
    }
    return s;
}

void Apply(LyricsState* s, const std::string& addr, const std::vector<OscArg>& a)
{
    if (addr == "/lyrics/next") { if (Pressed(a)) s->Next(); }
    else if (addr == "/lyrics/prev") { if (Pressed(a)) s->Prev(); }
    else if (addr == "/lyrics/clear") { if (Pressed(a)) s->SetBlank(true); }
    else if (addr == "/lyrics/show") { if (Pressed(a)) s->SetBlank(false); }
    else if (addr == "/lyrics/blank") { s->SetBlank(AsInt(a, 1) != 0); }
    else if (addr == "/lyrics/goto") { s->Goto(AsInt(a, 1) - 1); }
    else if (addr == "/lyrics/text") {
        if (!a.empty() && a[0].t == 's') s->SetDirectText(ReplaceNewlines(Utf8ToW(a[0].s)));
    }
    else if (addr == "/lyrics/font") {
        if (a.empty()) return;
        if (a[0].t == 's') {
            std::wstring want = Utf8ToW(a[0].s);
            // buscar coincidencia exacta (sin distinguir mayusculas) en las instaladas
            for (auto& f : render::InstalledFonts())
                if (_wcsicmp(f.c_str(), want.c_str()) == 0) { want = f; break; }
            s->SetFont(want);
        } else {
            int n = AsInt(a, 1) - 1;
            auto& fonts = render::InstalledFonts();
            if (n >= 0 && n < (int)fonts.size()) s->SetFont(fonts[(size_t)n]);
        }
    }
    else if (addr == "/lyrics/song") {
        int n = AsInt(a, 0);
        if (n <= 0) { s->Clear(); return; }
        auto files = ListSongFiles();
        if (n <= (int)files.size()) s->LoadFile(files[(size_t)n - 1]);
    }
    else if (addr == "/lyrics/load") {
        if (!a.empty() && a[0].t == 's') {
            std::wstring p = Utf8ToW(a[0].s);
            if (p.find(L':') == std::wstring::npos && p.find(L"\\\\") != 0)
                p = LyricsDir() + L"\\" + p; // ruta relativa -> carpeta Lyrics
            s->LoadFile(p);
        }
    }
}

void Dispatch(const std::string& addr, const std::vector<OscArg>& args)
{
    std::lock_guard<std::mutex> lk(gMtx);
    for (auto* inst : gInst) Apply(inst, addr, args);
}

void HandlePacket(const char* buf, int len)
{
    int pos = 0;
    std::string addr;
    if (!ReadStr(buf, len, pos, addr)) return;

    if (addr == "#bundle") {
        pos = 16; // "#bundle\0" + timetag
        while (pos + 4 <= len) {
            int sz = BE32(buf + pos);
            pos += 4;
            if (sz <= 0 || pos + sz > len) break;
            HandlePacket(buf + pos, sz);
            pos += sz;
        }
        return;
    }

    std::string tags;
    if (!ReadStr(buf, len, pos, tags) || tags.empty() || tags[0] != ',') {
        if (tags.empty() && pos >= len) { Dispatch(addr, {}); }
        return;
    }
    std::vector<OscArg> args;
    for (size_t k = 1; k < tags.size(); k++) {
        OscArg a;
        a.t = tags[k];
        switch (a.t) {
        case 'i':
            if (pos + 4 > len) return;
            a.i = BE32(buf + pos); pos += 4; break;
        case 'f': {
            if (pos + 4 > len) return;
            int32_t v = BE32(buf + pos); pos += 4;
            std::memcpy(&a.f, &v, 4); break;
        }
        case 's':
            if (!ReadStr(buf, len, pos, a.s)) return;
            break;
        case 'h': case 'd': case 't':
            pos += 8; break;
        default: break; // T, F, N, I: sin datos
        }
        args.push_back(a);
    }
    Dispatch(addr, args);
}

void ListenLoop(int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s); WSACleanup(); return; // puerto ocupado: sin OSC propio
    }
    DWORD tmo = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    std::vector<char> buf(16384);
    while (gRun.load()) {
        int n = recvfrom(s, buf.data(), (int)buf.size(), 0, nullptr, nullptr);
        if (n > 0) HandlePacket(buf.data(), n);
    }
    closesocket(s);
    WSACleanup();
}

void Register(LyricsState* s)
{
    std::lock_guard<std::mutex> life(gLifecycle);
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(gMtx);
        gInst.push_back(s);
        start = (gInst.size() == 1);
    }
    if (start && !gRun.load()) {
        gRun.store(true);
        gThr = std::thread(ListenLoop, ReadPort());
    }
}

void Unregister(LyricsState* s)
{
    std::lock_guard<std::mutex> life(gLifecycle);
    bool stop = false;
    {
        std::lock_guard<std::mutex> lk(gMtx);
        gInst.erase(std::remove(gInst.begin(), gInst.end(), s), gInst.end());
        stop = gInst.empty();
    }
    if (stop) {
        gRun.store(false);
        if (gThr.joinable()) gThr.join();
    }
}
} // namespace

LyricsState::LyricsState() { Register(this); }
LyricsState::~LyricsState() { Unregister(this); }

void LyricsState::Next()
{
    std::lock_guard<std::mutex> lk(m_);
    direct_ = false;
    if (idx_ + 1 < (int)st_.size()) idx_++;
}

void LyricsState::Prev()
{
    std::lock_guard<std::mutex> lk(m_);
    direct_ = false;
    if (idx_ > 0) idx_--;
}

void LyricsState::Goto(int i)
{
    std::lock_guard<std::mutex> lk(m_);
    if (st_.empty()) return;
    direct_ = false;
    if (i < 0) i = 0;
    if (i >= (int)st_.size()) i = (int)st_.size() - 1;
    idx_ = i;
}

void LyricsState::GotoNormalized(float v)
{
    int n;
    { std::lock_guard<std::mutex> lk(m_); n = (int)st_.size(); }
    if (n <= 0) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    Goto((int)std::lround(v * (float)(n - 1)));
}

void LyricsState::SetBlank(bool b)
{
    std::lock_guard<std::mutex> lk(m_);
    blank_ = b;
}

void LyricsState::SetDirectText(const std::wstring& t)
{
    std::lock_guard<std::mutex> lk(m_);
    directText_ = t;
    direct_ = true;
    blank_ = false;
}

void LyricsState::SetFont(const std::wstring& f)
{
    std::lock_guard<std::mutex> lk(m_);
    if (!f.empty()) font_ = f;
}

bool LyricsState::LoadFile(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    std::string bytes = buf.str();
    if (bytes.size() >= 3 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB &&
        (uint8_t)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    std::vector<Stanza> parsed = ParseStanzas(Utf8ToW(bytes));
    std::lock_guard<std::mutex> lk(m_);
    st_ = std::move(parsed);
    idx_ = -1; // no muestra nada hasta el primer "Next"
    direct_ = false;
    return true;
}

void LyricsState::Clear()
{
    std::lock_guard<std::mutex> lk(m_);
    st_.clear();
    idx_ = -1;
    direct_ = false;
}

LyricsState::View LyricsState::GetView()
{
    std::lock_guard<std::mutex> lk(m_);
    View v;
    v.blank = blank_;
    if (direct_) {
        v.text = directText_;
    } else if (idx_ >= 0 && idx_ < (int)st_.size()) {
        v.text = st_[(size_t)idx_].text;
        v.font = st_[(size_t)idx_].font;
    }
    if (v.font.empty()) v.font = font_;
    return v;
}

} // namespace lyr

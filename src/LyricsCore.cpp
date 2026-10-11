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

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Divide la letra en lineas: salto de linea, "|" o el texto literal \n.
// Las lineas en blanco SE CONSERVAN: son un paso en el que no se ve texto.
// Solo se descartan las lineas vacias del final (un salto de linea de mas al pegar).
static std::vector<std::wstring> SplitLines(std::wstring raw)
{
    size_t p = 0;
    while ((p = raw.find(L"\\n", p)) != std::wstring::npos) {
        raw.replace(p, 2, L"\n");
        p += 1;
    }
    p = 0;
    while ((p = raw.find(L"\r\n", p)) != std::wstring::npos) raw.replace(p, 2, L"\n");
    for (auto& ch : raw)
        if (ch == L'\r' || ch == 0x2028 || ch == 0x2029) ch = L'\n';

    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : raw) {
        if (ch == L'\n' || ch == L'|') {
            out.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(Trim(cur));
    while (!out.empty() && out.back().empty()) out.pop_back();
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
    else if (addr == "/lyrics/restart") { if (Pressed(a)) s->Restart(); }
    else if (addr == "/lyrics/clear") { if (Pressed(a)) s->SetBlank(true); }
    else if (addr == "/lyrics/show") { if (Pressed(a)) s->SetBlank(false); }
    else if (addr == "/lyrics/blank") { s->SetBlank(AsInt(a, 1) != 0); }
    else if (addr == "/lyrics/goto") { s->Goto(AsInt(a, 0)); }
    else if (addr == "/lyrics/text") {
        if (!a.empty() && a[0].t == 's') s->SetLyrics(Utf8ToW(a[0].s));
    }
    else if (addr == "/lyrics/font") {
        if (a.empty()) return;
        if (a[0].t == 's') {
            std::wstring want = Utf8ToW(a[0].s);
            for (auto& f : render::InstalledFonts())
                if (_wcsicmp(f.c_str(), want.c_str()) == 0) { want = f; break; }
            s->SetFont(want);
        } else {
            int n = AsInt(a, 1) - 1;
            auto& fonts = render::InstalledFonts();
            if (n >= 0 && n < (int)fonts.size()) s->SetFont(fonts[(size_t)n]);
        }
    }
}

void Dispatch(const std::string& addr, const std::vector<OscArg>& args)
{
    std::lock_guard<std::mutex> lk(gMtx);
    for (auto* inst : gInst)
        if (inst->IsActive()) Apply(inst, addr, args);
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

void LyricsState::SetLyrics(const std::wstring& raw)
{
    std::vector<std::wstring> lines = SplitLines(raw);
    std::lock_guard<std::mutex> lk(m_);
    if (lines == lines_) return;
    lines_ = std::move(lines);
    if (idx_ >= (int)lines_.size()) idx_ = (int)lines_.size() - 1;
}

void LyricsState::Next()
{
    std::lock_guard<std::mutex> lk(m_);
    if (idx_ + 1 < (int)lines_.size()) idx_++;
}

void LyricsState::Prev()
{
    std::lock_guard<std::mutex> lk(m_);
    if (idx_ >= 0) idx_--;
}

void LyricsState::Restart()
{
    std::lock_guard<std::mutex> lk(m_);
    idx_ = -1;
}

void LyricsState::Goto(int n1)
{
    std::lock_guard<std::mutex> lk(m_);
    if (n1 < 0) n1 = 0;
    if (n1 > (int)lines_.size()) n1 = (int)lines_.size();
    idx_ = n1 - 1;
}

void LyricsState::SetBlank(bool b)
{
    std::lock_guard<std::mutex> lk(m_);
    blank_ = b;
}

void LyricsState::SetFont(const std::wstring& f)
{
    std::lock_guard<std::mutex> lk(m_);
    if (!f.empty()) font_ = f;
}

void LyricsState::Touch() { lastTouch_.store(GetTickCount64()); }

bool LyricsState::IsActive() const
{
    unsigned long long t = lastTouch_.load();
    return t != 0 && (GetTickCount64() - t) < 1500ULL;
}

LyricsState::View LyricsState::GetView()
{
    std::lock_guard<std::mutex> lk(m_);
    View v;
    v.blank = blank_;
    if (idx_ >= 0 && idx_ < (int)lines_.size()) v.text = lines_[(size_t)idx_];
    v.font = font_;
    return v;
}

} // namespace lyr

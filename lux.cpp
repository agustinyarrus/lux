// ============================================================================
//  Lux — visor de imagenes nativo, dark y minimalista para Windows.
//  Win32 puro + Direct2D/DirectWrite (render GPU) + WIC (decodificacion del SO),
//  con stb_image como fallback. Sin frameworks. Un solo .exe portable.
//
//  Hermano de bajo nivel de Lumen. Paleta Nocturne (Tokyo Night refinado).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <d2d1effects.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cctype>
#include <climits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "third_party/stb_image.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// d2d1effects.h declara (extern) los CLSID de los efectos integrados, pero su
// definicion vive en una lib que no siempre se enlaza. Los definimos aca con los
// valores del SDK (selectany) para resolver el simbolo sin INITGUID global.
EXTERN_C const GUID DECLSPEC_SELECTANY CLSID_D2D1GaussianBlur =
    { 0x1feb6d69, 0x2fe6, 0x4ac9, { 0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5 } };
EXTERN_C const GUID DECLSPEC_SELECTANY CLSID_D2D1Shadow =
    { 0xc67ea361, 0x1863, 0x4e69, { 0x89, 0xdb, 0x69, 0x5d, 0x3e, 0x9a, 0x5b, 0x6b } };

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
//  Paleta Nocturne (igual que Lumen)
// ---------------------------------------------------------------------------
namespace col {
    static const D2D1_COLOR_F bg       = D2D1::ColorF(0x08090C);  // fondo casi negro azulado
    static const D2D1_COLOR_F bgSoft   = D2D1::ColorF(0x0B0E14);  // superficie elevada
    static const D2D1_COLOR_F fg       = D2D1::ColorF(0xDBDFE8);  // texto
    static const D2D1_COLOR_F fgDim    = D2D1::ColorF(0x767D8E);  // texto atenuado
    static const D2D1_COLOR_F fgFaint  = D2D1::ColorF(0x3B404D);  // texto muy tenue
    static const D2D1_COLOR_F accent   = D2D1::ColorF(0x7AA2F7);  // azul Tokyo Night
    static const D2D1_COLOR_F danger   = D2D1::ColorF(0xF7768E);  // rojo/rosa (cerrar)
}

// metricas logicas (a 96 dpi); se escalan por g.dpi
static const int TBH_L   = 38;   // alto barra de titulo
static const int BTNW_L  = 46;   // ancho de cada boton de ventana
static const int RSZ_L   = 6;    // borde de resize (hit-test)
static const UINT IDT_ANIM = 1;  // timer de animacion del cromo

// ---------------------------------------------------------------------------
//  Configuracion persistente (%APPDATA%\Lux\config.json)
// ---------------------------------------------------------------------------
struct Config {
    bool fitWindow = false;   // "ventana pegada a la imagen": ajusta la ventana al tamaño de cada imagen
    bool maximized = false;   // recordar si estaba maximizada
    bool hasWin    = false;   // ¿hay geometria guardada?
    int  winX = 100, winY = 100, winW = 1100, winH = 720; // tamaño/posicion en modo manual
};

// ---------------------------------------------------------------------------
//  Estado global
// ---------------------------------------------------------------------------
struct State {
    HWND  hwnd = nullptr;
    UINT  dpi  = 96;

    // Direct2D / DirectWrite / WIC
    ComPtr<ID2D1Factory1>        d2d;
    ComPtr<IDWriteFactory>       dw;
    ComPtr<IWICImagingFactory>   wic;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1DeviceContext>   dc;       // QI desde rt (Win8+): cubic + efectos
    ComPtr<ID2D1SolidColorBrush> brush;    // pincel reutilizable
    ComPtr<IDWriteTextFormat>    tfCap, tfHud, tfTitle, tfHint;

    // imagen actual
    ComPtr<ID2D1Bitmap>          bmp;
    UINT  imgW = 0, imgH = 0;
    std::wstring imgPath;
    std::wstring fmtLabel;   // p.ej. "JPEG", "PNG"

    // carpeta
    std::vector<std::wstring> files;
    int   idx = -1;

    // vista
    float scale = 1.f;       // px de pantalla por px de imagen
    float ox = 0.f, oy = 0.f;// esquina sup-izq de la imagen en pantalla (px)
    bool  fit = true;        // modo ajuste (vs detalle/zoom libre)

    // interaccion
    bool  panning = false;
    POINT panStart{};
    float panOX = 0, panOY = 0;
    bool  lDown = false;
    POINT downPt{};
    bool  moved = false;

    // cromo (auto-hide)
    float chrome = 1.f;      // 0..1 opacidad de barra + HUD
    float chromeTarget = 1.f;
    DWORD lastMove = 0;
    bool  cursorHidden = false;
    int   hoverBtn = -1;     // 0=min 1=max 2=close
    int   hoverEdge = 0;     // -1 prev, +1 next, 0 ninguno

    // ventana
    bool  fullscreen = false;
    WINDOWPLACEMENT prevPlace{ sizeof(WINDOWPLACEMENT) };
    bool  effectsOk = false; // blur/sombra disponibles
    ComPtr<ID2D1Effect> blur, shadow;

    // configuracion persistente
    Config cfg;
    bool   firstSize = true; // la 1a imagen centra la ventana en el monitor
};
static State g;

// ---------------------------------------------------------------------------
//  Utilidades
// ---------------------------------------------------------------------------
static int   dp(int v)   { return MulDiv(v, (int)g.dpi, 96); }
static float dpf(float v){ return v * (float)g.dpi / 96.f; }

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static std::wstring extOf(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    size_t sl  = p.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sl != std::wstring::npos && dot < sl)) return L"";
    return lower(p.substr(dot + 1));
}
static std::wstring baseName(const std::wstring& p) {
    size_t sl = p.find_last_of(L"\\/");
    return sl == std::wstring::npos ? p : p.substr(sl + 1);
}
static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
//  Configuracion persistente (mini lector/escritor JSON plano)
// ---------------------------------------------------------------------------
static std::wstring configPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\Lux";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}
static bool jsonBool(const std::string& s, const char* key, bool def) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k); if (p == std::string::npos) return def;
    p = s.find(':', p + k.size()); if (p == std::string::npos) return def;
    ++p; while (p < s.size() && isspace((unsigned char)s[p])) ++p;
    return s.compare(p, 4, "true") == 0;
}
static long jsonInt(const std::string& s, const char* key, long def) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k); if (p == std::string::npos) return def;
    p = s.find(':', p + k.size()); if (p == std::string::npos) return def;
    ++p; while (p < s.size() && isspace((unsigned char)s[p])) ++p;
    bool neg = false; if (p < s.size() && (s[p] == '-' || s[p] == '+')) { neg = (s[p] == '-'); ++p; }
    if (p >= s.size() || !isdigit((unsigned char)s[p])) return def;
    long v = 0; while (p < s.size() && isdigit((unsigned char)s[p])) { v = v * 10 + (s[p] - '0'); ++p; }
    return neg ? -v : v;
}
static void loadConfig() {
    std::ifstream f(configPath().c_str(), std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    g.cfg.fitWindow = jsonBool(s, "fitWindowToImage", false);
    g.cfg.maximized = jsonBool(s, "maximized", false);
    long x = jsonInt(s, "winX", LONG_MIN), y = jsonInt(s, "winY", LONG_MIN);
    long w = jsonInt(s, "winW", 0),        h = jsonInt(s, "winH", 0);
    if (w >= 200 && h >= 150 && x != LONG_MIN && y != LONG_MIN) {
        g.cfg.winX = (int)x; g.cfg.winY = (int)y; g.cfg.winW = (int)w; g.cfg.winH = (int)h;
        g.cfg.hasWin = true;
    }
}
static void saveConfig() {
    // capturar geometria actual si la ventana esta en estado normal (no maximizada/fullscreen)
    if (g.hwnd && !g.fullscreen) {
        g.cfg.maximized = IsZoomed(g.hwnd) != 0;
        if (!g.cfg.maximized) {
            RECT r;
            if (GetWindowRect(g.hwnd, &r)) {
                g.cfg.winX = r.left; g.cfg.winY = r.top;
                g.cfg.winW = r.right - r.left; g.cfg.winH = r.bottom - r.top;
                g.cfg.hasWin = true;
            }
        }
    }
    std::ofstream f(configPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n"
      << "  \"fitWindowToImage\": " << (g.cfg.fitWindow ? "true" : "false") << ",\n"
      << "  \"maximized\": "        << (g.cfg.maximized ? "true" : "false") << ",\n"
      << "  \"winX\": " << g.cfg.winX << ",\n"
      << "  \"winY\": " << g.cfg.winY << ",\n"
      << "  \"winW\": " << g.cfg.winW << ",\n"
      << "  \"winH\": " << g.cfg.winH << "\n"
      << "}\n";
}

// Extensiones soportadas (para listar la carpeta).
static bool isImageExt(const std::wstring& e) {
    static const wchar_t* exts[] = {
        // WIC nativo
        L"bmp",L"dib",L"rle",L"gif",L"ico",L"cur",L"jpg",L"jpeg",L"jpe",L"jfif",L"jif",
        L"png",L"apng",L"tif",L"tiff",L"dds",L"wdp",L"jxr",L"hdp",
        // WIC + codecs del Store (si estan instalados)
        L"webp",L"heic",L"heif",L"avif",L"jxl",
        // RAW comunes (codec del fabricante / Raw Image Extension)
        L"cr2",L"cr3",L"nef",L"arw",L"dng",L"orf",L"rw2",L"raf",L"srw",L"pef",
        // stb
        L"tga",L"targa",L"hdr",L"pic",L"ppm",L"pgm",L"pbm",L"pnm",L"psd",
    };
    for (auto* x : exts) if (e == x) return true;
    return false;
}
// Estos los hace mejor (o solo) stb_image.
static bool prefersStb(const std::wstring& e) {
    return e == L"tga" || e == L"targa" || e == L"hdr" || e == L"pic" ||
           e == L"ppm" || e == L"pgm"   || e == L"pbm" || e == L"pnm" || e == L"psd";
}
static std::wstring fmtLabelFor(const std::wstring& e) {
    if (e==L"jpg"||e==L"jpeg"||e==L"jpe"||e==L"jfif"||e==L"jif") return L"JPEG";
    if (e==L"tif"||e==L"tiff") return L"TIFF";
    if (e==L"jxr"||e==L"wdp"||e==L"hdp") return L"JPEG-XR";
    if (e==L"heic"||e==L"heif") return L"HEIF";
    std::wstring u = e; for (auto& c:u) c=(wchar_t)towupper(c); return u;
}

// ---------------------------------------------------------------------------
//  Listado de carpeta (orden natural)
// ---------------------------------------------------------------------------
static void buildFolderList(const std::wstring& filePath) {
    g.files.clear(); g.idx = -1;
    std::wstring dir;
    size_t sl = filePath.find_last_of(L"\\/");
    dir = (sl == std::wstring::npos) ? L"." : filePath.substr(0, sl);

    std::wstring pat = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { g.files.push_back(filePath); g.idx = 0; return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (isImageExt(extOf(name)))
            g.files.push_back(dir + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(g.files.begin(), g.files.end(),
        [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(baseName(a).c_str(), baseName(b).c_str()) < 0;
        });

    // localizar el archivo abierto (comparacion robusta por nombre completo)
    wchar_t full[MAX_PATH];
    std::wstring target = filePath;
    if (GetFullPathNameW(filePath.c_str(), MAX_PATH, full, nullptr)) target = full;
    for (size_t i = 0; i < g.files.size(); ++i) {
        wchar_t f2[MAX_PATH];
        std::wstring cand = g.files[i];
        if (GetFullPathNameW(g.files[i].c_str(), MAX_PATH, f2, nullptr)) cand = f2;
        if (lstrcmpiW(cand.c_str(), target.c_str()) == 0) { g.idx = (int)i; break; }
    }
    if (g.idx < 0) { g.files.insert(g.files.begin(), filePath); g.idx = 0; }
}

// ---------------------------------------------------------------------------
//  Decodificacion -> ID2D1Bitmap
// ---------------------------------------------------------------------------
static bool decodeWIC(const std::wstring& path, ComPtr<ID2D1Bitmap>& out, UINT& w, UINT& h) {
    if (!g.wic || !g.rt) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g.wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(g.rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp))) return false;
    D2D1_SIZE_U s = bmp->GetPixelSize();
    out = bmp; w = s.width; h = s.height;
    return true;
}

static bool decodeSTB(const std::wstring& path, ComPtr<ID2D1Bitmap>& out, UINT& w, UINT& h) {
    if (!g.rt) return false;
    std::string u8 = toUtf8(path);
    int iw, ih, n;
    stbi_uc* data = stbi_load(u8.c_str(), &iw, &ih, &n, 4); // RGBA
    if (!data) return false;

    // RGBA (straight) -> BGRA premultiplicado (lo que espera D2D)
    size_t px = (size_t)iw * ih;
    for (size_t i = 0; i < px; ++i) {
        stbi_uc* p = data + i * 4;
        stbi_uc r = p[0], gg = p[1], b = p[2], a = p[3];
        p[0] = (stbi_uc)((b * a + 127) / 255);
        p[1] = (stbi_uc)((gg * a + 127) / 255);
        p[2] = (stbi_uc)((r * a + 127) / 255);
        p[3] = a;
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap> bmp;
    HRESULT hr = g.rt->CreateBitmap(D2D1::SizeU(iw, ih), data, iw * 4, bp, &bmp);
    stbi_image_free(data);
    if (FAILED(hr)) return false;
    out = bmp; w = (UINT)iw; h = (UINT)ih;
    return true;
}

static void fitToWindow();                     // fwd
static void applyWindowSizing(bool recenter);  // fwd
static void invalidate()     { if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

static bool loadIndex(int i) {
    if (i < 0 || i >= (int)g.files.size()) return false;
    const std::wstring& path = g.files[i];
    std::wstring e = extOf(path);

    ComPtr<ID2D1Bitmap> bmp; UINT w = 0, h = 0;
    bool ok = false;
    if (prefersStb(e)) {
        ok = decodeSTB(path, bmp, w, h);
        if (!ok) ok = decodeWIC(path, bmp, w, h);
    } else {
        ok = decodeWIC(path, bmp, w, h);
        if (!ok) ok = decodeSTB(path, bmp, w, h);
    }
    if (!ok) return false;

    g.bmp = bmp; g.imgW = w; g.imgH = h;
    g.imgPath = path; g.idx = i;
    g.fmtLabel = fmtLabelFor(e);
    g.fit = true;
    fitToWindow();

    // modo "ventana pegada a la imagen": ajustar la ventana a esta imagen
    if (g.cfg.fitWindow) applyWindowSizing(g.firstSize);
    g.firstSize = false;

    // titulo de ventana (accesibilidad / barra de tareas)
    std::wstring title = baseName(path) + L"  —  Lux";
    SetWindowTextW(g.hwnd, title.c_str());
    invalidate();
    return true;
}

static void navigate(int delta) {
    if (g.files.empty()) return;
    int n = (int)g.files.size();
    int start = g.idx;
    for (int step = 1; step <= n; ++step) {
        int i = ((start + delta * step) % n + n) % n;
        if (loadIndex(i)) return;
        if (i == start) break;
    }
}

// ---------------------------------------------------------------------------
//  Geometria de vista (fit / zoom / pan)
// ---------------------------------------------------------------------------
static D2D1_RECT_F stageRect() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float top = g.fullscreen ? 0.f : (float)dp(TBH_L);
    return D2D1::RectF(0, top, (float)rc.right, (float)rc.bottom);
}
static float fitScale() {
    if (!g.imgW || !g.imgH) return 1.f;
    D2D1_RECT_F s = stageRect();
    float sw = s.right - s.left, sh = s.bottom - s.top;
    return std::min(sw / g.imgW, sh / g.imgH);
}
static void clampPan() {
    D2D1_RECT_F s = stageRect();
    float sw = s.right - s.left, sh = s.bottom - s.top;
    float iw = g.imgW * g.scale, ih = g.imgH * g.scale;
    // eje X
    if (iw <= sw) g.ox = s.left + (sw - iw) * 0.5f;
    else          g.ox = std::min(s.left, std::max(s.left + sw - iw, g.ox));
    // eje Y
    if (ih <= sh) g.oy = s.top + (sh - ih) * 0.5f;
    else          g.oy = std::min(s.top, std::max(s.top + sh - ih, g.oy));
}
static void fitToWindow() {
    g.scale = fitScale();
    g.fit = true;
    clampPan();
}
static void zoomAt(float factor, float cx, float cy) {
    float ns = g.scale * factor;
    float mn = std::min(fitScale(), 0.02f);
    float mx = 64.f;
    ns = std::max(mn, std::min(mx, ns));
    if (ns == g.scale) return;
    // mantener fijo el punto de imagen bajo (cx,cy)
    float imgX = (cx - g.ox) / g.scale;
    float imgY = (cy - g.oy) / g.scale;
    g.scale = ns;
    g.ox = cx - imgX * g.scale;
    g.oy = cy - imgY * g.scale;
    g.fit = (fabs(g.scale - fitScale()) < 0.001f);
    clampPan();
    invalidate();
}
static void zoomCenter(float factor) {
    D2D1_RECT_F s = stageRect();
    zoomAt(factor, (s.left + s.right) * 0.5f, (s.top + s.bottom) * 0.5f);
}
static void setActualSize() {       // 1:1
    D2D1_RECT_F s = stageRect();
    zoomAt(1.f / g.scale, (s.left + s.right) * 0.5f, (s.top + s.bottom) * 0.5f);
}
static void toggleFitDetail(float cx, float cy) {
    float fs = fitScale();
    if (fabs(g.scale - fs) < 0.001f) {   // estamos en fit -> ir a 100%
        zoomAt(1.f / g.scale, cx, cy);
    } else {
        fitToWindow();
        invalidate();
    }
}

// Ajusta la ventana al tamaño de la imagen actual (modo "ventana pegada"),
// limitada al area de trabajo del monitor. recenter=true la centra; si no,
// conserva el centro actual (continuidad al navegar).
static void applyWindowSizing(bool recenter) {
    if (!g.cfg.fitWindow || !g.hwnd) return;
    if (g.fullscreen || IsZoomed(g.hwnd)) return;
    if (!g.bmp || !g.imgW || !g.imgH) return;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

    int  workW = mi.rcWork.right - mi.rcWork.left;
    int  workH = mi.rcWork.bottom - mi.rcWork.top;
    int  tbh = dp(TBH_L);
    double availW = workW - dpf(60.f);
    double availH = workH - dpf(60.f) - tbh;
    double s = std::min(1.0, std::min(availW / g.imgW, availH / g.imgH));
    int winW = std::max(dp(420), (int)lround(g.imgW * s));
    int winH = std::max(dp(280), (int)lround(g.imgH * s) + tbh);

    int cx, cy;
    if (recenter) {
        cx = (mi.rcWork.left + mi.rcWork.right) / 2;
        cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    } else {
        RECT r; GetWindowRect(g.hwnd, &r);
        cx = (r.left + r.right) / 2; cy = (r.top + r.bottom) / 2;
    }
    int x = cx - winW / 2, y = cy - winH / 2;
    x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - winW));
    y = std::max((int)mi.rcWork.top,  std::min(y, (int)mi.rcWork.bottom - winH));
    SetWindowPos(g.hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
//  Recursos Direct2D
// ---------------------------------------------------------------------------
static void createTextFormats() {
    auto mk = [&](float px, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) {
        g.dw->CreateTextFormat(L"Cascadia Code", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, px, L"", out);
    };
    g.tfCap.Reset(); g.tfHud.Reset(); g.tfTitle.Reset(); g.tfHint.Reset();
    mk(dpf(12.0f), DWRITE_FONT_WEIGHT_LIGHT, &g.tfCap);
    mk(dpf(11.5f), DWRITE_FONT_WEIGHT_LIGHT, &g.tfHud);
    mk(dpf(13.5f), DWRITE_FONT_WEIGHT_LIGHT, &g.tfTitle);
    mk(dpf(11.0f), DWRITE_FONT_WEIGHT_LIGHT, &g.tfHint);
    if (g.tfCap)  { g.tfCap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);  g.tfCap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfHud)  { g.tfHud->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);  g.tfHud->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfTitle){ g.tfTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);g.tfTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfHint) { g.tfHint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g.tfHint->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
}

static void discardDeviceResources() {
    g.blur.Reset(); g.shadow.Reset();
    g.brush.Reset(); g.dc.Reset(); g.rt.Reset();
    g.effectsOk = false;
}

static bool createDeviceResources() {
    if (g.rt) return true;
    RECT rc; GetClientRect(g.hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));

    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
    rtp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(g.hwnd, size,
        D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(g.d2d->CreateHwndRenderTarget(rtp, hp, &g.rt))) return false;
    g.rt->SetDpi(96.f, 96.f); // trabajamos en pixeles fisicos; escalamos UI a mano

    g.rt.As(&g.dc); // QI -> device context (Win8+) para cubic + efectos
    if (g.dc) {
        g.effectsOk =
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1GaussianBlur, &g.blur)) &&
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1Shadow, &g.shadow));
    }
    g.rt->CreateSolidColorBrush(col::fg, &g.brush);
    createTextFormats();

    // Si ya habia imagen, recrear su bitmap en el nuevo target
    if (!g.imgPath.empty()) {
        ComPtr<ID2D1Bitmap> bmp; UINT w, h; std::wstring e = extOf(g.imgPath);
        bool ok = prefersStb(e) ? (decodeSTB(g.imgPath,bmp,w,h) || decodeWIC(g.imgPath,bmp,w,h))
                                : (decodeWIC(g.imgPath,bmp,w,h) || decodeSTB(g.imgPath,bmp,w,h));
        if (ok) { g.bmp = bmp; g.imgW = w; g.imgH = h; }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Layout de los botones de la barra
// ---------------------------------------------------------------------------
// indices: 0=min 1=max 2=close (cluster derecho) ; 3=toggle "pegar a la imagen" (a su izquierda)
static void btnRect(int i, D2D1_RECT_F& r) {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float bw = (float)dp(BTNW_L), h = (float)dp(TBH_L);
    float right = (float)rc.right;
    if (i == 3) { r = D2D1::RectF(right - bw * 4, 0, right - bw * 3, h); return; }
    r = D2D1::RectF(right - bw * (3 - i), 0, right - bw * (2 - i), h);
}
static int hitButton(int x, int y) {
    if (g.fullscreen) return -1;
    if (y >= dp(TBH_L)) return -1;
    for (int i = 0; i <= 3; ++i) { D2D1_RECT_F r; btnRect(i, r);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i; }
    return -1;
}
static int hitEdge(int x, int y) {       // zona de navegacion lateral
    if (!g.bmp) return 0;
    D2D1_RECT_F s = stageRect();
    if (y < s.top || y > s.bottom) return 0;
    float w = std::min((s.right - s.left) * 0.14f, dpf(130));
    if (x < s.left + w) return -1;
    if (x > s.right - w) return +1;
    return 0;
}

// ---------------------------------------------------------------------------
//  Render
// ---------------------------------------------------------------------------
static void setBrush(const D2D1_COLOR_F& c, float a = 1.f) {
    g.brush->SetColor(c); g.brush->SetOpacity(a);
}

static void drawWindowButtons() {
    float a = g.chrome;
    if (a <= 0.01f) return;
    float h = (float)dp(TBH_L);
    for (int i = 0; i <= 3; ++i) {
        D2D1_RECT_F r; btnRect(i, r);
        bool hot = (g.hoverBtn == i);
        if (hot) {
            // el color lleva su propio alpha; setBrush multiplica por 'a' del cromo
            D2D1_COLOR_F hc = (i == 2) ? D2D1::ColorF(0xF7768E, 0.16f) : D2D1::ColorF(1,1,1,0.06f);
            setBrush(hc, a);
            g.rt->FillRectangle(r, g.brush.Get());
        }
        float cx = (r.left + r.right) * 0.5f, cy = h * 0.5f;
        float s = dpf(5.0f), sw = dpf(1.1f);
        if (i == 3) {            // toggle "ventana pegada a la imagen": marco con imagen adentro
            bool on = g.cfg.fitWindow;
            D2D1_COLOR_F tc = on ? col::accent : (hot ? col::fg : col::fgDim);
            float bw = dpf(6.5f), bh = dpf(5.2f);
            D2D1_RECT_F frame = D2D1::RectF(cx - bw, cy - bh, cx + bw, cy + bh);
            setBrush(tc, a);
            g.rt->DrawRectangle(frame, g.brush.Get(), sw);
            float pad = dpf(1.8f);
            setBrush(tc, on ? a * 0.85f : a * 0.28f);
            g.rt->FillRectangle(D2D1::RectF(frame.left+pad, frame.top+pad, frame.right-pad, frame.bottom-pad), g.brush.Get());
            continue;
        }
        D2D1_COLOR_F ic = hot ? (i == 2 ? col::danger : col::fg) : col::fgDim;
        setBrush(ic, a);
        if (i == 0) {            // minimizar: linea
            g.rt->DrawLine(D2D1::Point2F(cx - s, cy), D2D1::Point2F(cx + s, cy), g.brush.Get(), sw);
        } else if (i == 1) {     // maximizar/restaurar: cuadrado(s)
            if (g.fullscreen || IsZoomed(g.hwnd)) {
                D2D1_RECT_F a1 = D2D1::RectF(cx - s + dpf(2), cy - s, cx + s, cy + s - dpf(2));
                D2D1_RECT_F a2 = D2D1::RectF(cx - s, cy - s + dpf(2), cx + s - dpf(2), cy + s);
                g.rt->DrawRectangle(a1, g.brush.Get(), sw);
                g.rt->DrawRectangle(a2, g.brush.Get(), sw);
            } else {
                g.rt->DrawRectangle(D2D1::RectF(cx - s, cy - s, cx + s, cy + s), g.brush.Get(), sw);
            }
        } else {                 // cerrar: X
            g.rt->DrawLine(D2D1::Point2F(cx - s, cy - s), D2D1::Point2F(cx + s, cy + s), g.brush.Get(), sw);
            g.rt->DrawLine(D2D1::Point2F(cx + s, cy - s), D2D1::Point2F(cx - s, cy + s), g.brush.Get(), sw);
        }
    }
}

static void drawTitleBar() {
    if (g.fullscreen) return;
    float a = g.chrome;
    float h = (float)dp(TBH_L);
    RECT rc; GetClientRect(g.hwnd, &rc);
    // fondo barra
    setBrush(col::bg, 1.f);
    g.rt->FillRectangle(D2D1::RectF(0, 0, (float)rc.right, h), g.brush.Get());

    // caption centrado: nombre  ·  idx / total
    if (g.bmp && a > 0.01f && g.tfCap) {
        std::wstring name = baseName(g.imgPath);
        std::wstring count = std::to_wstring(g.idx + 1) + L" / " + std::to_wstring(g.files.size());
        std::wstring cap = name + L"   ·   " + count;
        setBrush(col::fgDim, a);
        D2D1_RECT_F tr = D2D1::RectF((float)rc.right*0.25f, 0, (float)rc.right*0.75f, h);
        g.rt->DrawTextW(cap.c_str(), (UINT32)cap.size(), g.tfCap.Get(), tr, g.brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    drawWindowButtons();
}

static void drawChevron(float cx, float cy, int dir, float a) {
    // dir = -1 (prev) dibuja "<"  ;  dir = +1 (next) dibuja ">"
    float s = dpf(9), sw = dpf(1.6f);
    setBrush(D2D1::ColorF(0xCFD5E2), a);
    float w = dir * s * 0.55f;                 // vertice en (cx + dir*w0)
    g.rt->DrawLine(D2D1::Point2F(cx - w, cy - s), D2D1::Point2F(cx + w, cy), g.brush.Get(), sw);
    g.rt->DrawLine(D2D1::Point2F(cx + w, cy), D2D1::Point2F(cx - w, cy + s), g.brush.Get(), sw);
}

static void drawHud() {
    if (!g.bmp || g.fullscreen) return;
    float a = g.chrome;
    if (a <= 0.01f || !g.tfHud) return;
    wchar_t buf[128];
    int zoomPct = (int)lround(g.scale * 100.0);
    swprintf(buf, 128, L"%u × %u    ·    %d%%", g.imgW, g.imgH, zoomPct);
    std::wstring s = buf;

    // medir
    ComPtr<IDWriteTextLayout> tl;
    g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g.tfHud.Get(), 1000.f, 100.f, &tl);
    DWRITE_TEXT_METRICS m{}; if (tl) tl->GetMetrics(&m);
    float padX = dpf(14), padY = dpf(6);
    float w = (tl ? m.widthIncludingTrailingWhitespace : dpf(120)) + padX * 2;
    float hh = dpf(24);
    RECT rc; GetClientRect(g.hwnd, &rc);
    float cx = rc.right * 0.5f;
    float y2 = rc.bottom - dpf(22);
    D2D1_RECT_F pill = D2D1::RectF(cx - w*0.5f, y2 - hh, cx + w*0.5f, y2);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pill, hh*0.5f, hh*0.5f);

    setBrush(D2D1::ColorF(0x0B0E14), 0.72f * a);
    g.rt->FillRoundedRectangle(rr, g.brush.Get());
    setBrush(D2D1::ColorF(1,1,1,1), 0.06f * a);
    g.rt->DrawRoundedRectangle(rr, g.brush.Get(), dpf(1));

    setBrush(col::fgDim, a);
    D2D1_RECT_F tr = D2D1::RectF(pill.left, pill.top, pill.right, pill.bottom);
    g.rt->DrawTextW(s.c_str(), (UINT32)s.size(), g.tfHud.Get(), tr, g.brush.Get());
}

// logo "lux": un destello/sol minimalista (distinto del iris de Lumen)
static void drawSpark(float cx, float cy, float R) {
    float sw = dpf(1.3f);
    setBrush(col::accent, 0.95f);
    // nucleo
    g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), R*0.34f, R*0.34f), g.brush.Get(), sw);
    setBrush(col::fgFaint, 1.f);
    // 8 rayos
    for (int i = 0; i < 8; ++i) {
        float ang = (float)(i * 3.14159265 / 4.0);
        float r0 = (i % 2 == 0) ? R*0.52f : R*0.50f;
        float r1 = (i % 2 == 0) ? R*0.95f : R*0.74f;
        float dx = cosf(ang), dy = sinf(ang);
        const D2D1_COLOR_F& c = (i % 2 == 0) ? col::accent : col::fgFaint;
        setBrush(c, (i%2==0)?0.9f:1.f);
        g.rt->DrawLine(D2D1::Point2F(cx + dx*r0, cy + dy*r0),
                       D2D1::Point2F(cx + dx*r1, cy + dy*r1), g.brush.Get(), sw);
    }
}

static void drawEmpty() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float cx = rc.right * 0.5f, cy = rc.bottom * 0.5f;
    drawSpark(cx, cy - dpf(34), dpf(46));
    if (g.tfTitle) {
        setBrush(col::fgDim, 1.f);
        D2D1_RECT_F tr = D2D1::RectF(0, cy + dpf(16), (float)rc.right, cy + dpf(40));
        const wchar_t* t = L"Lux";
        g.rt->DrawTextW(t, 3, g.tfTitle.Get(), tr, g.brush.Get());
    }
    if (g.tfHint) {
        setBrush(col::fgFaint, 1.f);
        D2D1_RECT_F tr = D2D1::RectF(0, cy + dpf(44), (float)rc.right, cy + dpf(66));
        const wchar_t* t = L"Ctrl+O para abrir   ·   o arrastrá una imagen";
        g.rt->DrawTextW(t, (UINT32)wcslen(t), g.tfHint.Get(), tr, g.brush.Get());
    }
}

static void drawAmbient() {
    if (!g.effectsOk || !g.blur || !g.bmp) return;
    D2D1_RECT_F s = stageRect();
    float sw = s.right - s.left, sh = s.bottom - s.top;
    // escala "cover"
    float sc = std::max(sw / g.imgW, sh / g.imgH) * 1.12f;
    float iw = g.imgW * sc, ih = g.imgH * sc;
    float x = s.left + (sw - iw) * 0.5f, y = s.top + (sh - ih) * 0.5f;

    g.blur->SetInput(0, g.bmp.Get());
    g.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, dpf(46.f));
    g.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    D2D1_RECT_F clip = s;
    g.dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    g.dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(x, y));
    g.dc->DrawImage(g.blur.Get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR);
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // atenuar (simula brightness .42)
    setBrush(col::bg, 0.62f);
    g.rt->FillRectangle(s, g.brush.Get());
    g.dc->PopAxisAlignedClip();
}

static void drawImageShadow(const D2D1_RECT_F& dst) {
    if (!g.effectsOk || !g.shadow || g.scale < 0.0001f) return;
    g.shadow->SetInput(0, g.bmp.Get());
    g.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, dpf(16.f));
    g.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.55f));
    // la sombra se genera en el espacio del bitmap nativo: misma escala + offset
    D2D1::Matrix3x2F mt = D2D1::Matrix3x2F::Scale(g.scale, g.scale) *
                          D2D1::Matrix3x2F::Translation(dst.left, dst.top + dpf(14));
    g.dc->SetTransform(mt);
    g.dc->DrawImage(g.shadow.Get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR);
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

static void render() {
    if (!createDeviceResources()) return;
    g.rt->BeginDraw();
    g.rt->Clear(col::bg);

    if (g.bmp && g.imgW && g.imgH) {
        D2D1_RECT_F dst = D2D1::RectF(g.ox, g.oy, g.ox + g.imgW * g.scale, g.oy + g.imgH * g.scale);
        // recortar al escenario (no pisar la barra)
        D2D1_RECT_F s = stageRect();
        g.rt->PushAxisAlignedClip(s, D2D1_ANTIALIAS_MODE_ALIASED);
        drawAmbient();
        drawImageShadow(dst);
        D2D1_INTERPOLATION_MODE im = (g.scale >= 3.0f)
            ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
        if (g.dc) g.dc->DrawBitmap(g.bmp.Get(), &dst, 1.f, im);
        else      g.rt->DrawBitmap(g.bmp.Get(), dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.rt->PopAxisAlignedClip();

        // chevrons de navegacion (solo si hay vecinas y hover/cromo visible)
        if (g.files.size() > 1 && g.chrome > 0.01f) {
            D2D1_RECT_F st = stageRect();
            float cy = (st.top + st.bottom) * 0.5f;
            float baseA = 0.42f * g.chrome;
            if (g.hoverEdge == -1) drawChevron(st.left + dpf(28), cy, -1, g.chrome);
            else                   drawChevron(st.left + dpf(28), cy, -1, baseA);
            if (g.hoverEdge == +1) drawChevron(st.right - dpf(28), cy, +1, g.chrome);
            else                   drawChevron(st.right - dpf(28), cy, +1, baseA);
        }
        drawHud();
    } else {
        drawEmpty();
    }

    drawTitleBar();

    HRESULT hr = g.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
        invalidate();
    }
}

// ---------------------------------------------------------------------------
//  Pantalla completa
// ---------------------------------------------------------------------------
static void setFullscreen(bool on) {
    if (on == g.fullscreen) return;
    g.fullscreen = on;
    DWORD style = (DWORD)GetWindowLongPtrW(g.hwnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(g.hwnd, &g.prevPlace);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g.hwnd, &g.prevPlace);
        SetWindowPos(g.hwnd, nullptr, 0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
    }
    if (g.fit) fitToWindow();
    invalidate();
}

// ---------------------------------------------------------------------------
//  Apertura por dialogo / drag&drop
// ---------------------------------------------------------------------------
static void openDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter =
        L"Imágenes\0*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.avif;*.ico;"
        L"*.tga;*.hdr;*.ppm;*.pgm;*.pbm;*.pnm;*.psd;*.dds;*.jxr;*.dng;*.cr2;*.nef;*.arw\0"
        L"Todos\0*.*\0\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        buildFolderList(file);
        loadIndex(g.idx);
    }
}
static void openPath(const std::wstring& path) {
    buildFolderList(path);
    if (!loadIndex(g.idx)) navigate(1);
}

// ---------------------------------------------------------------------------
//  Animacion del cromo (auto-hide)
// ---------------------------------------------------------------------------
static void onMouseActivity() {
    g.lastMove = GetTickCount();
    g.chromeTarget = 1.f;
    if (g.cursorHidden) { g.cursorHidden = false; SetCursor(LoadCursor(nullptr, IDC_ARROW)); }
    SetTimer(g.hwnd, IDT_ANIM, 16, nullptr);
}
static void animTick() {
    bool changed = false;
    // idle?
    if (g.bmp && !g.panning && (GetTickCount() - g.lastMove) > 1600) {
        g.chromeTarget = 0.f;
        if (!g.cursorHidden) { g.cursorHidden = true; changed = true; }
    }
    float d = g.chromeTarget - g.chrome;
    if (fabs(d) > 0.004f) { g.chrome += d * 0.18f; changed = true; }
    else if (g.chrome != g.chromeTarget) { g.chrome = g.chromeTarget; changed = true; }

    if (changed) invalidate();
    // detener el timer en estado estable visible
    if (g.chrome == g.chromeTarget && g.chromeTarget == 1.f && !g.cursorHidden)
        KillTimer(g.hwnd, IDT_ANIM);
    // si el cromo ya esta oculto y estable, tambien podemos detener
    if (g.chrome == g.chromeTarget && g.chromeTarget == 0.f)
        KillTimer(g.hwnd, IDT_ANIM);
}

// ---------------------------------------------------------------------------
//  Window proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCREATE: {
        g.hwnd = hwnd;
        // DPI per-monitor v2 ya viene del manifest; tomamos el actual
        g.dpi = GetDpiForWindow(hwnd);
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_CREATE: {
        // esquinas redondeadas + barra oscura nativas de Win11
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        int pref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
        // sombra del sistema sobre ventana sin marco
        MARGINS m{ 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &m);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_NCCALCSIZE: {
        if (wp == TRUE) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            RECT* rc = &p->rgrc[0];
            if (IsZoomed(hwnd)) {
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                rc->left += fx; rc->right -= fx; rc->top += fy; rc->bottom -= fy;
            }
            // si no esta maximizada: client = toda la ventana (sin barra del sistema)
            return 0;
        }
        break;
    }
    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT cp = pt; ScreenToClient(hwnd, &cp);
        RECT rc; GetClientRect(hwnd, &rc);
        int bx = dp(RSZ_L);
        bool left = cp.x < bx, right = cp.x >= rc.right - bx;
        bool top = cp.y < bx, bottom = cp.y >= rc.bottom - bx;
        if (!g.fullscreen && !IsZoomed(hwnd)) {
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (!g.fullscreen && cp.y < dp(TBH_L)) {
            if (hitButton(cp.x, cp.y) >= 0) return HTCLIENT; // botones reciben clicks
            return HTCAPTION;                                // resto arrastra
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = dp(420);
        mmi->ptMinTrackSize.y = dp(280);
        // que el maximizado respete la work area
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.dw) createTextFormats();
        if (g.fit) fitToWindow();
        invalidate();
        return 0;
    }
    case WM_SIZE: {
        if (g.rt) {
            RECT rc; GetClientRect(hwnd, &rc);
            g.rt->Resize(D2D1::SizeU(std::max<LONG>(rc.right,1), std::max<LONG>(rc.bottom,1)));
        }
        if (g.fit) fitToWindow();
        invalidate();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        onMouseActivity();
        int hb = hitButton(x, y);
        if (hb != g.hoverBtn) { g.hoverBtn = hb; invalidate(); }
        int he = hitEdge(x, y);
        if (he != g.hoverEdge) { g.hoverEdge = he; invalidate(); }
        if (g.panning) {
            float dx = (float)(x - g.panStart.x), dy = (float)(y - g.panStart.y);
            g.ox = g.panOX + dx; g.oy = g.panOY + dy;
            clampPan(); invalidate();
        }
        if (g.lDown && (abs(x - g.downPt.x) > 3 || abs(y - g.downPt.y) > 3)) g.moved = true;
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            if (g.cursorHidden) { SetCursor(nullptr); return TRUE; }
            SetCursor(LoadCursor(nullptr, g.panning ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        g.lDown = true; g.moved = false; g.downPt = { x, y };
        // pan solo si la imagen excede el escenario
        D2D1_RECT_F s = stageRect();
        bool canPan = g.bmp && (g.imgW*g.scale > (s.right-s.left)+1 || g.imgH*g.scale > (s.bottom-s.top)+1);
        if (canPan && hitButton(x,y) < 0 && y >= dp(TBH_L)) {
            g.panning = true; g.panStart = { x, y }; g.panOX = g.ox; g.panOY = g.oy;
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ReleaseCapture();
        bool wasPanning = g.panning;
        g.panning = false; g.lDown = false;
        if (!g.moved && !wasPanning) {
            int hb = hitButton(x, y);
            if (hb == 0) ShowWindow(hwnd, SW_MINIMIZE);
            else if (hb == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            else if (hb == 2) PostMessage(hwnd, WM_CLOSE, 0, 0);
            else if (hb == 3) {   // toggle "ventana pegada a la imagen"
                g.cfg.fitWindow = !g.cfg.fitWindow;
                if (g.cfg.fitWindow) applyWindowSizing(true);
                saveConfig();
            }
            else {
                int he = hitEdge(x, y);
                if (he == -1) navigate(-1);
                else if (he == +1) navigate(+1);
            }
        }
        invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < dp(TBH_L) && !g.fullscreen) { // doble clic en barra = max/restore
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (g.bmp && hitEdge(x,y)==0) toggleFitDetail((float)x, (float)y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (g.bmp && pt.y >= dp(TBH_L)) {
            float factor = (delta > 0) ? 1.18f : 1.f/1.18f;
            zoomAt(factor, (float)pt.x, (float)pt.y);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (wp) {
        case VK_LEFT: case VK_UP: case VK_PRIOR:      navigate(-1); break;
        case VK_RIGHT: case VK_DOWN: case VK_NEXT: case VK_SPACE: navigate(+1); break;
        case VK_HOME:  if (!g.files.empty()) loadIndex(0); break;
        case VK_END:   if (!g.files.empty()) loadIndex((int)g.files.size()-1); break;
        case VK_OEM_PLUS: case VK_ADD:   zoomCenter(1.25f); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: zoomCenter(1.f/1.25f); break;
        case '0':      fitToWindow(); invalidate(); break;
        case '1':      setActualSize(); break;
        case 'F': case VK_F11: setFullscreen(!g.fullscreen); break;
        case VK_ESCAPE: if (g.fullscreen) setFullscreen(false); else PostMessage(hwnd, WM_CLOSE,0,0); break;
        case 'O': if (ctrl) openDialog(); break;
        case 'W':   // alternar "ventana pegada a la imagen"
            g.cfg.fitWindow = !g.cfg.fitWindow;
            if (g.cfg.fitWindow) applyWindowSizing(false);
            saveConfig(); invalidate();
            break;
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hd, 0, path, MAX_PATH)) openPath(path);
        DragFinish(hd);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_ANIM) animTick();
        return 0;

    case WM_CLOSE:
        saveConfig();          // recordar geometria + preferencia al cerrar
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        discardDeviceResources();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  WinMain
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // factories independientes del dispositivo
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
        reinterpret_cast<void**>(g.d2d.GetAddressOf()));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(g.dw.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g.wic));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(8, 9, 12)); // anti-flash: nace oscuro
    wc.lpszClassName = L"LuxWindow";
    wc.hIcon   = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    // cargar configuracion antes de crear la ventana
    loadConfig();

    // geometria inicial: en modo manual restauramos tamaño/posicion guardados
    int X = CW_USEDEFAULT, Y = CW_USEDEFAULT, W = 1100, H = 720;
    if (!g.cfg.fitWindow && g.cfg.hasWin) { X = g.cfg.winX; Y = g.cfg.winY; W = g.cfg.winW; H = g.cfg.winH; }

    HWND hwnd = CreateWindowExW(0, L"LuxWindow", L"Lux",
        WS_OVERLAPPEDWINDOW, X, Y, W, H,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // forzar el recalculo del marco -> aplica el frameless (WM_NCCALCSIZE)
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // crear los recursos Direct2D ANTES de decodificar (decode necesita el render target)
    createDeviceResources();

    // abrir el archivo pasado por linea de comandos (ajusta la ventana si el modo esta activo)
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        buildFolderList(argv[1]);
        loadIndex(g.idx);
    }
    if (argv) LocalFree(argv);

    // mostrar (maximizada si asi se guardo)
    ShowWindow(hwnd, g.cfg.maximized ? SW_MAXIMIZE : nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}

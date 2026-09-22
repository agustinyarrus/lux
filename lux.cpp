// ============================================================================
//  Lux — visor de imagenes nativo, dark y minimalista para Windows.
//  Win32 puro + Direct2D/DirectWrite (render GPU) + WIC (decodificacion del SO),
//  con stb_image como fallback. Sin frameworks. Un solo .exe portable.
//
//  Hermano de bajo nivel de Lumen. Paleta Nocturne (Tokyo Night refinado).
//
//  Como viaja una imagen:
//
//    archivo ─► cargador (hilos propios, WIC propio) ─► CpuImage: piramide PBGRA ─► cache LRU
//                · un JPEG grande se decodifica directo al tamaño que pide la pantalla
//                  (escalado DCT a 1/2..1/8); la resolucion completa llega despues, si hace falta
//                · premultiplicar, orientar y armar la piramide: SSE2 en todos los nucleos
//            ─► GpuImage: mosaicos de 2048 px por nivel, subidos solo cuando se ven
//            ─► cuadro: el nivel justo para el zoom, sin costuras; ambiente y sombra
//               calculados sobre un nivel de 512 px
//
//  La interfaz nunca espera a un decodificador: mientras llega la imagen pedida sigue la
//  anterior en pantalla con una linea de progreso, y las vecinas ya vienen precargadas en la
//  direccion en que se navega.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define PSAPI_VERSION 2

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <psapi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <d2d1effects.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "third_party/stb_image.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg.h"
#include "third_party/nanosvgrast.h"

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "third_party/qoi.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"   // provee stbi_zlib_compress para tinyexr

#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1             // reusa el zlib de stb (sin miniz)
#define TINYEXR_IMPLEMENTATION
#include "third_party/tinyexr.h"

#include "third_party/exotic.h"            // pcx, farbfeld, pfm, sun, sgi, ilbm, icns, dpx… (+ tonemap.h)
#include "third_party/retro.h"             // Atari/Amiga/C64/ZX/GEM/PSX/PhotoCD…
#include "third_party/sci.h"               // FITS (astronomia) y DICOM (medicina)
#include "third_party/texture.h"           // DDS/VTF/KTX y los bloques BC1..BC5
#include "third_party/xcf.h"               // GIMP (compone las capas a mano)
#include "third_party/unpack.h"            // gzip (svgz) y ZIP (ora/kra), sobre el zlib de stb

#include "core/pool.h"                     // pool de hilos + parallelFor
#include "core/pixels.h"                   // premultiplicar, orientar, reducir (SSE2)
#include "core/pyramid.h"                  // CpuImage: la piramide de niveles
#include "core/tiles.h"                    // mosaicos: que nivel, que mosaicos, donde cortar
#include "core/cache.h"                    // LRU con presupuesto en bytes
#include "core/plan.h"                     // que precargar y en que orden

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
EXTERN_C const GUID DECLSPEC_SELECTANY CLSID_D2D1ColorMatrix =
    { 0x921f03d6, 0x641c, 0x47df, { 0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11 } };

using Microsoft::WRL::ComPtr;
using lux::CpuImage;
using lux::JobKind;

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
static const int TBH_L   = 26;   // alto barra de titulo (extra fina)
static const int SBH_L   = 22;   // alto barra de estado inferior (detalles de la imagen)
static const int BTNW_L  = 36;   // ancho de cada boton de ventana
static const int RSZ_L   = 6;    // borde de resize (hit-test)
static const UINT IDT_ANIM  = 1; // timer de animacion del cromo
static const UINT IDT_HUD   = 2; // timer del HUD (resolucion/zoom): auto-hide a los 3 s
static const UINT IDT_LOAD  = 3; // timer de la linea de carga (mientras hay un pedido pendiente)
static const UINT IDT_BENCH = 4; // solo en la build de medicion
static const UINT IDT_DWELL = 5; // la imagen lleva un rato en pantalla: pedir la completa
static const UINT WM_APP_LOADED = WM_APP + 1;   // el cargador tiene resultados

// tiempos y presupuestos
static const DWORD    LOADBAR_DELAY_MS       = 150;           // la linea de carga aparece solo si tarda
static const DWORD    FIRST_IMAGE_WAIT_MS    = 400;           // al arrancar: esperar la imagen antes de mostrar la ventana
static const DWORD    DWELL_FULL_MS          = 600;           // mirando una vista previa este rato: decodificar la completa
static const double   UPLOAD_BUDGET_MS       = 6.0;           // subida de mosaicos por cuadro (zoom, pan)
static const double   UPLOAD_BUDGET_FIRST_MS = 40.0;          // el primer cuadro de una imagen nueva
static const size_t   GPU_BUDGET             = 768ull << 20;  // mosaicos en la GPU antes de desalojar
static const uint64_t MAX_PIXELS             = 1ull << 30;    // 1 Gpx = 4 GB en RGBA
static const uint32_t PROGRESS_ONE           = 1u << 16;
static const uint32_t PROGRESS_UNKNOWN       = 0xFFFFFFFFu;

// Ambiente y sombra, calcados de Lumen (CSS): filter: blur(74px) brightness(.42) saturate(1.25)
// y box-shadow: 0 28px 90px -28px rgba(0,0,0,.8), 0 2px 14px -6px rgba(0,0,0,.6).
// En px logicos de PANTALLA: el radio se ve igual en una foto de 48 MP que en un icono.
static const float AMBIENT_BLUR_DIP   = 74.f;
static const float AMBIENT_COVER      = 1.12f;   // el ambiente desborda el escenario (sin bordes a la vista)
static const float AMBIENT_SATURATE   = 1.25f;
static const float AMBIENT_BRIGHTNESS = 0.42f;
struct ShadowLayer { float dy, sigma, spread, alpha; };   // sigma = blur CSS / 2
static const ShadowLayer kShadow[2] = { { 28.f, 45.f, 28.f, 0.80f }, { 2.f, 7.f, 6.f, 0.60f } };

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
//  Estado global (solo lo toca el hilo de la interfaz)
// ---------------------------------------------------------------------------
enum class View : uint8_t {
    Empty,     // nada abierto
    Loading,   // se pidio una imagen y todavia no hay nada que mostrar
    Showing,   // hay imagen en pantalla (puede haber otra pedida, con la linea de carga)
    Error,     // el archivo pedido no se pudo abrir
};

struct GpuTile  { ComPtr<ID2D1Bitmap> bmp; uint32_t lastUse = 0; };
struct GpuLevel { const lux::Level* src = nullptr; lux::TileGrid grid; std::vector<GpuTile> tiles; };

struct State {
    HWND  hwnd = nullptr;
    UINT  dpi  = 96;

    // Direct2D / DirectWrite
    ComPtr<ID2D1Factory1>        d2d;
    ComPtr<IDWriteFactory>       dw;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1DeviceContext>   dc;       // QI desde rt (Win8+): cubic + efectos
    ComPtr<ID2D1SolidColorBrush> brush;    // pincel reutilizable
    ComPtr<IDWriteTextFormat>    tfCap, tfHud, tfTitle, tfHint, tfStatus;
    UINT32 maxBmp = 16384;                 // lado maximo de un bitmap en esta GPU

    // imagen en pantalla: piramide en CPU + mosaicos en GPU
    View  view = View::Empty;
    std::shared_ptr<const CpuImage> cur;
    std::vector<GpuLevel> gpu;             // uno por nivel de cur
    size_t   gpuBytes = 0;
    uint32_t frame = 0;
    bool     uploadBoost = false;          // primer cuadro de una imagen: mas presupuesto de subida
    UINT  imgW = 0, imgH = 0;
    UINT  imgBpp = 0;                      // bits por pixel del formato de origen (0 = desconocido)
    bool  imgAlpha = false;                // el formato de origen soporta canal alfa
    bool  imgHdr = false;                  // HDR comprimido con tone mapping
    std::wstring imgPath;
    std::wstring fmtLabel;                 // p.ej. "JPEG", "PNG"
    int   shownTotal = 0;                  // tamaño de la carpeta cuando se mostro (para el contador)

    // carpeta
    std::vector<std::wstring> files;
    int   idx = -1;

    // navegacion asincronica
    int   target = -1;                     // indice pedido que todavia no esta en pantalla
    int   navDir = +1;                     // direccion de la ultima navegacion
    bool  explicitOpen = false;            // pedido de "abrir archivo": si falla se muestra el error
    int   skips = 0;                       // archivos rotos salteados seguidos
    DWORD requestedAt = 0;
    bool  dwellDone = false;               // lo que se ve lleva DWELL_FULL_MS en pantalla
    DWORD presentedAt = 0;                 // cuando se mostro (por si el timer llega tarde)
    float loadShown = 0.f;                 // progreso dibujado (suavizado)
    bool  zoomNeedsFull = false;           // el zoom pide niveles que la vista previa no tiene
    std::unordered_set<std::wstring> tooBig;   // precargas que no entraron en el presupuesto

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
    bool  mouseTracked = false; // TrackMouseEvent pedido (para recibir WM_MOUSELEAVE)

    // cromo (auto-hide)
    float chrome = 1.f;      // 0..1 opacidad de barra + HUD
    float chromeTarget = 1.f;
    DWORD lastMove = 0;
    bool  cursorHidden = false;
    int   hoverBtn = -1;     // 0=min 1=max 2=close
    int   hoverEdge = 0;     // -1 prev, +1 next, 0 ninguno

    // HUD de resolucion/zoom: ciclo de vida propio (se oculta 3 s tras el ultimo cambio de zoom)
    DWORD hudShownAt = 0;
    float hudAlpha = 0.f;
    float hudTarget = 0.f;

    // caption con truncado (…) cacheado para no medir texto en cada frame
    std::wstring capText, capKeyPath;
    int  capKeyIdx = -2, capKeyTotal = -1;
    LONG capKeyW = -1;
    UINT capKeyDpi = 0;

    std::wstring loadError;  // mensaje cuando un archivo no se pudo abrir (formato/codec)

    // ventana
    bool  fullscreen = false;
    WINDOWPLACEMENT prevPlace{ sizeof(WINDOWPLACEMENT) };
    bool  effectsOk = false; // blur/sombra disponibles
    ComPtr<ID2D1Effect> blur, tone, shadow[2];

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
static float snapPx(float v) { return floorf(v + 0.5f); }   // al pixel entero mas cercano

// Trazos nitidos: un trazo de ancho entero solo cae en pixeles enteros si su
// centro esta en la mitad del pixel (ancho impar) o justo en el borde (ancho par).
// Un trazo de 1,5 px centrado en y=19,5 se reparte en tres filas grises.
static float strokePx()                    { return std::max(1.f, snapPx(dpf(1.f))); }
static float snapStroke(float v, float sw) { return floorf(v) + (((int)sw & 1) ? 0.5f : 0.f); }

static double nowMs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / freq;
}

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static std::wstring upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
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

// Ruta absoluta y normalizada, sin tope de MAX_PATH (dos llamadas: medir y copiar).
static std::wstring fullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, L'\0');
    DWORD m = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    if (!m || m >= n) return p;
    out.resize(m);
    return out;
}

// Rutas de mas de MAX_PATH: las API de archivos las aceptan con el prefijo \\?\ (con el
// manifiesto longPathAware y la directiva del sistema tambien sin el, pero la directiva
// viene apagada de fabrica). Solo aplica a rutas absolutas de disco o de red.
static std::wstring fsPath(const std::wstring& p) {
    if (p.size() < MAX_PATH - 12 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    std::wstring q = p;
    std::replace(q.begin(), q.end(), L'/', L'\\');
    if (q.size() > 2 && q[1] == L':' && q[2] == L'\\') return L"\\\\?\\" + q;
    if (q.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + q.substr(2);
    return p;
}

// ---------------------------------------------------------------------------
//  Configuracion persistente (mini lector/escritor JSON plano)
// ---------------------------------------------------------------------------
static std::wstring configDir() {
    DWORD n = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    std::wstring dir;
    if (n) {
        dir.resize(n);
        DWORD m = GetEnvironmentVariableW(L"APPDATA", dir.data(), n);
        dir.resize(m < n ? m : 0);
    }
    if (dir.empty()) dir = L".";
    dir += L"\\Lux";
    CreateDirectoryW(fsPath(dir).c_str(), nullptr);
    return dir;
}
static std::wstring configPath() { return configDir() + L"\\config.json"; }

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
    long v = 0; while (p < s.size() && isdigit((unsigned char)s[p]) && v < 100000000) { v = v * 10 + (s[p] - '0'); ++p; }
    return neg ? -v : v;
}
static void loadConfig() {
    std::ifstream f(fsPath(configPath()).c_str(), std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    g.cfg.fitWindow = jsonBool(s, "fitWindowToImage", false);
    g.cfg.maximized = jsonBool(s, "maximized", false);
    long x = jsonInt(s, "winX", LONG_MIN), y = jsonInt(s, "winY", LONG_MIN);
    long w = jsonInt(s, "winW", 0),        h = jsonInt(s, "winH", 0);
    // Solo vale si cae (aunque sea en parte) en un monitor conectado: una config
    // guardada con la ventana minimizada (-32000,-32000) o en un monitor que ya
    // no esta nos dejaria la ventana fuera de la pantalla, invisible.
    RECT wr{ (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    if (w >= 200 && h >= 150 && x != LONG_MIN && y != LONG_MIN &&
        MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr) {
        g.cfg.winX = (int)x; g.cfg.winY = (int)y; g.cfg.winW = (int)w; g.cfg.winH = (int)h;
        g.cfg.hasWin = true;
    }
}
static void saveConfig() {
    // capturar geometria actual si la ventana esta en estado normal (no maximizada/fullscreen)
    if (g.hwnd && !g.fullscreen && IsIconic(g.hwnd)) {
        // minimizada: GetWindowRect devuelve (-32000,-32000) y la proxima vez la
        // ventana naceria fuera de la pantalla. Conservar la ultima geometria buena
        // y recordar solo si al restaurar volveria maximizada.
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(g.hwnd, &wp)) g.cfg.maximized = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
    } else if (g.hwnd && !g.fullscreen) {
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
    std::ostringstream o;
    o << "{\n"
      << "  \"fitWindowToImage\": " << (g.cfg.fitWindow ? "true" : "false") << ",\n"
      << "  \"maximized\": "        << (g.cfg.maximized ? "true" : "false") << ",\n"
      << "  \"winX\": " << g.cfg.winX << ",\n"
      << "  \"winY\": " << g.cfg.winY << ",\n"
      << "  \"winW\": " << g.cfg.winW << ",\n"
      << "  \"winH\": " << g.cfg.winH << "\n"
      << "}\n";
    // Escritura atomica: temporal + reemplazo. Si el proceso muere a mitad de camino queda la
    // config anterior entera, nunca un JSON cortado.
    const std::wstring dst = configPath(), tmp = dst + L".tmp";
    {
        std::ofstream f(fsPath(tmp).c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::string s = o.str();
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return;
    }
    MoveFileExW(fsPath(tmp).c_str(), fsPath(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// ---------------------------------------------------------------------------
//  Tabla de extensiones: una sola, ordenada, con busqueda binaria (O(log n) por archivo al
//  listar una carpeta, en vez de recorrer ~200 cadenas). Cada grupo dice por que via se
//  decodifica y con que nombre se muestra el formato.
// ---------------------------------------------------------------------------
enum : uint16_t {
    FX_LIST  = 1 << 0,   // aparece al navegar la carpeta
    FX_RAW   = 1 << 1,   // RAW de camara: codec del sistema o el JPEG incrustado
    FX_META  = 1 << 2,   // metarchivo de Windows (lo dibuja GDI)
    FX_ZIP   = 1 << 3,   // ZIP con la imagen aplanada adentro
    FX_PE    = 1 << 4,   // ejecutable o biblioteca de iconos
    FX_OWN   = 1 << 5,   // decoder propio (exotic/retro/sci/texture/xcf)
    FX_STB   = 1 << 6,   // stb lo hace mejor (o solo)
    FX_CODEC = 1 << 7,   // hace falta una extension de codec de la Store
};
struct ExtGroup { uint16_t flags; const wchar_t* label; const wchar_t* exts; };
static const ExtGroup kExtGroups[] = {
    // WIC nativo
    { FX_LIST,            nullptr,           L"bmp dib rle gif ico cur ani mpo jps png apng dds" },
    { FX_LIST,            L"JPEG",           L"jpg jpeg jpe jfif jif" },
    { FX_LIST,            L"TIFF",           L"tif tiff" },
    { FX_LIST,            L"JPEG-XR",        L"jxr wdp hdp" },
    // WIC + codecs de la Store (si estan instalados)
    { FX_LIST | FX_CODEC, nullptr,           L"webp avif avifs jxl" },
    { FX_LIST | FX_CODEC, L"HEIF",           L"heic heif heics heifs avci" },
    { FX_LIST | FX_CODEC, L"JPEG 2000",      L"jp2 j2k jpf jpx jpm jpc" },
    // stb
    { FX_LIST | FX_STB,   L"TGA",            L"tga targa icb vda vst tpic" },
    { FX_LIST | FX_STB,   L"Radiance HDR",   L"hdr rgbe xyze" },
    { FX_LIST | FX_STB,   nullptr,           L"pic" },
    { FX_LIST | FX_STB,   L"Netpbm",         L"ppm pgm pbm pnm" },
    { FX_LIST | FX_STB,   L"PSD",            L"psd pdd psb" },
    // decoders propios header-only con camino dedicado
    { FX_LIST,            nullptr,           L"svg svgz qoi exr gz" },
    // metarchivos vectoriales
    { FX_LIST | FX_META,  L"EMF",            L"emf emz" },
    { FX_LIST | FX_META,  L"WMF",            L"wmf wmz" },
    // contenedores ZIP
    { FX_LIST | FX_ZIP,   L"OpenRaster",     L"ora" },
    { FX_LIST | FX_ZIP,   L"Krita",          L"kra krz" },
    { FX_LIST | FX_ZIP,   L"Sketch",         L"sketch" },
    { FX_LIST | FX_ZIP,   L"Procreate",      L"procreate" },
    // ejecutables: Lux los abre si se los pasan, pero no ensucian el carrusel
    { FX_PE,              L"Icono",          L"exe dll ocx cpl icl msstyles mun" },
    // exotic.h
    { FX_LIST | FX_OWN,   nullptr,           L"pcx pfm wbmp xbm im1 im8 im24 im32 xpm xwd dpx icns" },
    { FX_LIST | FX_OWN,   L"farbfeld",       L"ff farbfeld" },
    { FX_LIST | FX_OWN,   L"Sun Raster",     L"ras sun" },
    { FX_LIST | FX_OWN,   L"SGI",            L"sgi rgb rgba bw int inta" },
    { FX_LIST | FX_OWN,   L"Netpbm",         L"pam" },
    { FX_LIST | FX_OWN,   L"ILBM",           L"iff ilbm lbm acbm" },
    { FX_LIST | FX_OWN,   L"MacPaint",       L"mac pntg macp" },
    { FX_LIST | FX_OWN,   L"Cineon",         L"cin" },
    // retro.h
    { FX_LIST | FX_OWN,   L"Degas",          L"pi1 pi2 pi3" },
    { FX_LIST | FX_OWN,   L"Degas Elite",    L"pc1 pc2 pc3" },
    { FX_LIST | FX_OWN,   L"NEOchrome",      L"neo" },
    { FX_LIST | FX_OWN,   L"ZX Spectrum",    L"scr" },
    { FX_LIST | FX_OWN,   L"C64 Koala",      L"koa kla" },
    { FX_LIST | FX_OWN,   L"GEM IMG",        L"img ximg" },
    { FX_LIST | FX_OWN,   L"PlayStation TIM",L"tim" },
    { FX_LIST | FX_OWN,   L"Alias PIX",      L"pix als" },
    { FX_LIST | FX_OWN,   L"DCX",            L"dcx" },
    { FX_LIST | FX_OWN,   L"Photo CD",       L"pcd" },
    // sci.h
    { FX_LIST | FX_OWN,   L"FITS",           L"fits fit fts" },
    { FX_LIST | FX_OWN,   L"DICOM",          L"dcm dicom dic" },
    // texture.h (dds no: WIC lo hace mejor y cae aca solo si falla)
    { FX_LIST | FX_OWN,   L"Valve VTF",      L"vtf" },
    { FX_LIST | FX_OWN,   L"KTX",            L"ktx" },
    // xcf.h
    { FX_LIST | FX_OWN,   L"GIMP XCF",       L"xcf" },
    // RAW de camara: primero el codec del sistema; si no esta, el JPEG incrustado
    { FX_LIST | FX_RAW,   nullptr,           L"3fr ari arw bay cap cr2 cr3 crw dcr dcs dng drf eip erf fff gpr iiq k25 kdc "
                                             L"mdc mef mos mrw nef nrw obm orf pef ptx pxn raf raw rw2 rwl rwz sr2 srf srw x3f" },
};

struct ExtInfo { std::wstring ext; uint16_t flags; std::wstring label; };
static std::vector<ExtInfo> g_exts;   // se arma una vez al arrancar; despues es de solo lectura (la leen los hilos)

static void buildExtTable() {
    for (const ExtGroup& gr : kExtGroups) {
        std::wistringstream in(gr.exts);
        std::wstring e;
        while (in >> e) {
            std::wstring label = gr.label ? gr.label : upper(e);
            if (gr.flags & FX_RAW) label = upper(e) + L" (RAW)";
            g_exts.push_back({ e, gr.flags, label });
        }
    }
    std::stable_sort(g_exts.begin(), g_exts.end(), [](const ExtInfo& a, const ExtInfo& b) { return a.ext < b.ext; });
    // una extension en dos grupos suma las vias (se queda con la primera etiqueta)
    std::vector<ExtInfo> merged;
    for (auto& x : g_exts) {
        if (!merged.empty() && merged.back().ext == x.ext) merged.back().flags |= x.flags;
        else merged.push_back(std::move(x));
    }
    g_exts.swap(merged);
}
static const ExtInfo* extInfo(const std::wstring& e) {
    auto it = std::lower_bound(g_exts.begin(), g_exts.end(), e, [](const ExtInfo& x, const std::wstring& k) { return x.ext < k; });
    return (it != g_exts.end() && it->ext == e) ? &*it : nullptr;
}
static uint16_t extFlags(const std::wstring& e) { const ExtInfo* x = extInfo(e); return x ? x->flags : 0; }

static std::wstring fmtLabelFor(const std::wstring& path) {
    const std::wstring e = extOf(path);
    if (e == L"gz" || e == L"z") {             // "foto.ppm.gz" -> "Netpbm · gzip"
        const std::wstring stem = path.substr(0, path.size() - e.size() - 1);
        const std::wstring inner = extOf(stem);
        return inner.empty() ? L"GZIP" : fmtLabelFor(stem) + L" · gzip";
    }
    if (const ExtInfo* x = extInfo(e)) return x->label;
    return upper(e);
}

// ---------------------------------------------------------------------------
//  Listado de carpeta (orden natural)
// ---------------------------------------------------------------------------
// Solo nombres (no rutas) en el comparador: StrCmpLogicalW es el orden del Explorador, y sin
// armar baseName() en cada comparacion el sort no aloca nada. O(n log n) comparaciones.
// Devuelve el indice del archivo pedido. No toca g.idx: ese es el de lo que esta en pantalla, y
// mientras la imagen nueva no llega el contador tiene que seguir hablando de la vieja.
static int buildFolderList(const std::wstring& filePath) {
    g.files.clear();
    int found = -1;
    const std::wstring full = fullPath(filePath);
    const size_t sl = full.find_last_of(L"\\/");
    const std::wstring dir = (sl == std::wstring::npos) ? L"." : full.substr(0, sl);
    const std::wstring target = (sl == std::wstring::npos) ? full : full.substr(sl + 1);

    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(fsPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { g.files.push_back(full); return 0; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (extFlags(extOf(fd.cFileName)) & FX_LIST) names.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    g.files.reserve(names.size() + 1);
    for (size_t i = 0; i < names.size(); ++i) {
        // NTFS compara nombres sin distinguir mayusculas (ordinal): el mismo criterio aca
        if (found < 0 && CompareStringOrdinal(names[i].c_str(), (int)names[i].size(),
                                              target.c_str(), (int)target.size(), TRUE) == CSTR_EQUAL)
            found = (int)i;
        g.files.push_back(dir + L"\\" + names[i]);
    }
    // el archivo pedido no aparece en la lista (extension que no se lista, o un .exe): va primero
    if (found < 0) { g.files.insert(g.files.begin(), full); found = 0; }
    return found;
}

// ===========================================================================
//  DECODIFICACION — corre en los hilos del cargador. Nada de Direct2D ni del estado de la
//  ventana aca: cada hilo tiene su propia fabrica WIC y todo lo que produce es una CpuImage
//  inmutable que despues se comparte por shared_ptr.
// ===========================================================================
enum class DecodeStatus : uint8_t { Ok, Unsupported, TooLarge, IoError, Cancelled };

// Identidad de un archivo en disco (tamaño + fecha de escritura). Lo que esta en cache vale
// mientras el archivo siga siendo ese: si se edita en otro programa se vuelve a decodificar, y
// si se borra se saltea en vez de mostrar un fantasma.
struct FileStamp {
    uint64_t size = 0, mtime = 0;
    bool exists = false;
    bool operator==(const FileStamp& o) const { return exists == o.exists && size == o.size && mtime == o.mtime; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};
static FileStamp statFile(const std::wstring& path) {
    FileStamp st;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(fsPath(path).c_str(), GetFileExInfoStandard, &fad)) {
        st.exists = true;
        st.size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        st.mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    }
    return st;
}

struct Decoded {
    DecodeStatus st = DecodeStatus::Unsupported;
    std::shared_ptr<const CpuImage> img;
};
// Un paso de la cadena "resolvio" si no dijo Unsupported: exito, o un error que no tiene sentido
// seguir probando (no se pudo leer el archivo, no hay memoria, se cancelo).
static bool settled(const Decoded& d) { return d.st != DecodeStatus::Unsupported; }

enum class ReadResult : uint8_t { Ok, Missing, Empty, TooBig };
static ReadResult readFileBytes(const std::wstring& path, std::vector<unsigned char>& buf) {
    // compartido de lectura, escritura y borrado: se puede ver una captura que otro programa
    // todavia tiene abierta
    HANDLE h = CreateFileW(fsPath(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ReadResult::Missing;
    LARGE_INTEGER sz{};
    ReadResult r = ReadResult::Ok;
    if (!GetFileSizeEx(h, &sz)) r = ReadResult::Missing;
    else if (sz.QuadPart <= 0) r = ReadResult::Empty;
    else if (sz.QuadPart > (1LL << 30)) r = ReadResult::TooBig;
    if (r == ReadResult::Ok) {
        buf.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd != buf.size()) r = ReadResult::Missing;
    }
    CloseHandle(h);
    return r;
}

struct DecodeCtx {
    std::wstring path, ext;                          // ext en minusculas
    IWICImagingFactory* wic = nullptr;               // la del hilo que decodifica
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<uint32_t>* progress = nullptr;       // 0..PROGRESS_ONE, o PROGRESS_UNKNOWN
    bool allowPreview = false;                       // se acepta una decodificacion reducida (DCT)
    uint32_t boxW = 0, boxH = 0;                     // lo que la vista previa tiene que cubrir (px)
    std::shared_ptr<const CpuImage> have;            // vista previa en cache: completar solo lo fino

    // el archivo se lee una sola vez, perezosamente, para toda la cadena de decoders
    std::vector<unsigned char> bytes;
    bool read = false, readOk = false;
    DecodeStatus readStatus = DecodeStatus::Unsupported;
    bool load() {
        if (!read) {
            read = true;
            const ReadResult r = readFileBytes(path, bytes);
            readOk = r == ReadResult::Ok;
            readStatus = r == ReadResult::Missing ? DecodeStatus::IoError
                       : r == ReadResult::TooBig  ? DecodeStatus::TooLarge : DecodeStatus::Unsupported;
        }
        return readOk;
    }
    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
    void setProgress(double f) const {
        if (progress) progress->store((uint32_t)(std::clamp(f, 0.0, 1.0) * PROGRESS_ONE), std::memory_order_relaxed);
    }
};

// ¿Hay memoria fisica para `bytes` mas la piramide (4/3) y aire? Las imagenes moderadas pasan
// siempre; las enormes solo si hay lugar de verdad (mejor un mensaje claro que un swap eterno).
static bool enoughMemory(uint64_t bytes) {
    if (bytes <= (256ull << 20)) return true;
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (!GlobalMemoryStatusEx(&ms)) return true;
    return (double)bytes * 1.45 < (double)ms.ullAvailPhys;
}

// Pixeles de un decoder que no es WIC: RGBA recto (o PBGRA si pbgra).
struct Raster {
    lux::PixelBuf px;
    uint32_t w = 0, h = 0;
    bool pbgra = false;          // ya viene premultiplicado en BGRA (GDI)
    bool opaque = false;         // (con pbgra) se sabe que no hay transparencia
    lux::ImageInfo info;
    explicit operator bool() const { return (bool)px && w && h; }
};

// RGBA -> PBGRA en el lugar (SSE2, todos los nucleos) + piramide entera. O(n).
static Decoded finishRaster(Raster r, DecodeCtx& cx) {
    if (!r) return {};
    if ((uint64_t)r.w * r.h > MAX_PIXELS) return { DecodeStatus::TooLarge };
    CpuImage img;
    img.w = r.w; img.h = r.h; img.info = r.info;
    const size_t n = (size_t)r.w * r.h;
    img.opaque = r.pbgra ? (r.opaque || lux::allOpaque(r.px.data(), n)) : lux::rgbaToPbgra(r.px.data(), n);
    auto l0 = std::make_shared<lux::Level>();
    l0->w = r.w; l0->h = r.h; l0->px = std::move(r.px);
    img.levels.assign(lux::levelCount(img.w, img.h), nullptr);
    img.levels[0] = std::move(l0);
    if (!lux::buildPyramid(img, 0, -1, cx.cancel))
        return { cx.cancelled() ? DecodeStatus::Cancelled : DecodeStatus::TooLarge };
    cx.setProgress(1.0);
    return { DecodeStatus::Ok, std::make_shared<const CpuImage>(std::move(img)) };
}

// Float lineal (EXR, Radiance, PFM) -> RGBA8 con tone mapping (tonemap.h).
static Raster rasterFromFloat(const float* f, int channels, int w, int h) {
    Raster r;
    if (!f || w <= 0 || h <= 0) return r;
    r.px = lux::PixelBuf::alloc((size_t)w * h * 4);
    if (!r.px) return r;
    r.info.hdr = tm_to_rgba8(f, channels, w, h, r.px.data()) != 0;
    r.info.alphaFmt = channels == 4;
    r.w = (uint32_t)w; r.h = (uint32_t)h;
    return r;
}

// ---------------------------------------------------------------- WIC
// Etiqueta EXIF 274 (Orientation), 1..8. Devuelve 0 si el archivo no la trae.
// La consulta depende del contenedor: JPEG la guarda en el APP1, TIFF/RAW en el
// IFD principal y HEIF/WebP en sus propios caminos de metadatos.
static UINT exifOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> mq;
    if (!frame || FAILED(frame->GetMetadataQueryReader(&mq)) || !mq) return 0;
    static const wchar_t* kPaths[] = {
        L"/app1/ifd/{ushort=274}",          // JPEG
        L"/ifd/{ushort=274}",               // TIFF, DNG y la mayoria de los RAW
        L"/app1/{ushort=274}",
        L"/xmp/tiff:Orientation",           // XMP (Lightroom y compañia)
        L"/ifd/exif/{ushort=274}",
    };
    for (auto* p : kPaths) {
        PROPVARIANT pv; PropVariantInit(&pv);
        UINT v = 0;
        if (SUCCEEDED(mq->GetMetadataByName(p, &pv))) {
            if (pv.vt == VT_UI2)      v = pv.uiVal;
            else if (pv.vt == VT_UI4) v = (UINT)pv.ulVal;
            else if (pv.vt == VT_LPSTR && pv.pszVal) v = (UINT)atoi(pv.pszVal);
        }
        PropVariantClear(&pv);
        if (v >= 1 && v <= 8) return v;
    }
    return 0;
}

// Bits por pixel y alfa del formato de origen (para la barra de estado).
static lux::ImageInfo wicInfo(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame) {
    lux::ImageInfo info;
    WICPixelFormatGUID pf{};
    if (FAILED(frame->GetPixelFormat(&pf))) return info;
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(wic->CreateComponentInfo(pf, &ci))) return info;
    ComPtr<IWICPixelFormatInfo> pfi;
    if (SUCCEEDED(ci.As(&pfi))) { UINT bpp = 0; if (SUCCEEDED(pfi->GetBitsPerPixel(&bpp))) info.bpp = bpp; }
    ComPtr<IWICPixelFormatInfo2> pfi2;
    if (SUCCEEDED(ci.As(&pfi2))) { BOOL tr = FALSE; if (SUCCEEDED(pfi2->SupportsTransparency(&tr))) info.alphaFmt = tr != FALSE; }
    return info;
}

// Vista previa por escalado DCT: el decoder de JPEG (y cualquiera que implemente
// IWICBitmapSourceTransform) entrega la imagen a 1/2, 1/4 u 1/8 sin decodificar los detalles
// finos — varias veces mas rapido y con 1/4..1/64 de la memoria. Se elige el mayor factor que
// todavia cubre la caja de la pantalla, asi que a "ajustar" se ve identica a la completa. Sale
// exactamente en la grilla del nivel k de la piramide (ceil(w/2^k)); si no, no se usa.
enum class PxKind : uint8_t { None, PBGRA, BGRA, BGRX, BGR24, Gray8 };
static PxKind pxKind(const WICPixelFormatGUID& f) {
    if (f == GUID_WICPixelFormat32bppPBGRA) return PxKind::PBGRA;
    if (f == GUID_WICPixelFormat32bppBGRA)  return PxKind::BGRA;
    if (f == GUID_WICPixelFormat32bppBGR)   return PxKind::BGRX;
    if (f == GUID_WICPixelFormat24bppBGR)   return PxKind::BGR24;
    if (f == GUID_WICPixelFormat8bppGray)   return PxKind::Gray8;
    return PxKind::None;
}
static bool wicPreview(IWICBitmapFrameDecode* frame, uint32_t w, uint32_t h, int orient,
                       DecodeCtx& cx, CpuImage& img) {
    if (!cx.boxW || !cx.boxH) return false;
    ComPtr<IWICBitmapSourceTransform> st;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&st))) || !st) return false;
    const double fit = std::min((double)cx.boxW / img.w, (double)cx.boxH / img.h);
    const int count = lux::levelCount(img.w, img.h);
    const int ideal = std::min({ 3, lux::idealLevel(fit, 30), count - 1 });   // el DCT llega hasta 1/8
    for (int k = ideal; k >= 1; --k) {
        const UINT sw = lux::levelDim(w, k), sh = lux::levelDim(h, k);
        UINT cw = sw, ch = sh;
        if (FAILED(st->GetClosestSize(&cw, &ch)) || cw != sw || ch != sh) continue;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(st->GetClosestPixelFormat(&fmt))) return false;
        const PxKind kind = pxKind(fmt);
        if (kind == PxKind::None) return false;                       // CMYK y compañia: la completa
        const UINT bpp = kind == PxKind::BGR24 ? 3 : kind == PxKind::Gray8 ? 1 : 4;
        const UINT stride = (sw * bpp + 3) & ~3u;
        lux::PixelBuf out = lux::PixelBuf::alloc((size_t)sw * sh * 4);
        lux::PixelBuf tmp;
        if (!out) return false;
        uint8_t* dst = out.data();
        if (bpp != 4) { tmp = lux::PixelBuf::alloc((size_t)stride * sh); if (!tmp) return false; dst = tmp.data(); }
        if (FAILED(st->CopyPixels(nullptr, sw, sh, &fmt, WICBitmapTransformRotate0, stride, stride * sh, dst)))
            return false;
        const size_t n = (size_t)sw * sh;
        bool opaque = true;
        switch (kind) {
        case PxKind::PBGRA: opaque = lux::allOpaque(out.data(), n); break;
        case PxKind::BGRA:  opaque = lux::bgraToPbgra(out.data(), n); break;
        case PxKind::BGRX:  for (size_t i = 0; i < n; ++i) out.data()[i * 4 + 3] = 255; break;
        case PxKind::BGR24: lux::bgr24ToPbgra(tmp.data(), stride, sw, sh, out.data()); break;
        case PxKind::Gray8: lux::gray8ToPbgra(tmp.data(), stride, sw, sh, out.data()); break;
        default: return false;
        }
        uint32_t ow = sw, oh = sh;
        out = lux::orient(std::move(out), sw, sh, orient, ow, oh);
        if (ow != lux::levelDim(img.w, k) || oh != lux::levelDim(img.h, k)) return false;
        auto L = std::make_shared<lux::Level>();
        L->w = ow; L->h = oh; L->px = std::move(out);
        img.first = k;
        img.opaque = opaque;
        img.levels.assign(count, nullptr);
        img.levels[k] = std::move(L);
        return lux::buildPyramid(img, k, -1, cx.cancel);
    }
    return false;
}

// Decodificacion completa en tiras de ~8 MB: entre tira y tira se mira si se cancelo y se
// publica el progreso. Si ya hay una vista previa en cache (cx.have) solo se calculan los
// niveles mas finos que ella; los gruesos se heredan tal cual.
static Decoded wicFull(IWICBitmapFrameDecode* frame, uint32_t w, uint32_t h, int orient,
                       DecodeCtx& cx, CpuImage& img) {
    const uint64_t bytes = (uint64_t)w * h * 4;
    if (!enoughMemory(bytes * (lux::orientSwapsAxes(orient) ? 2 : 1))) return { DecodeStatus::TooLarge };
    const UINT stride = w * 4;
    const UINT rows = std::max<UINT>(16, (UINT)std::min<uint64_t>(h, (8ull << 20) / std::max<UINT>(stride, 1)));
    if ((uint64_t)stride * rows > UINT_MAX) return { DecodeStatus::TooLarge };
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(cx.wic->CreateFormatConverter(&conv))) return {};
    if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return {};
    lux::PixelBuf buf = lux::PixelBuf::alloc((size_t)bytes);
    if (!buf) return { DecodeStatus::TooLarge };
    for (UINT y = 0; y < h; y += rows) {
        if (cx.cancelled()) return { DecodeStatus::Cancelled };
        const UINT n = std::min(rows, h - y);
        WICRect rc{ 0, (INT)y, (INT)w, (INT)n };
        if (FAILED(conv->CopyPixels(&rc, stride, stride * n, buf.data() + (size_t)y * stride))) return {};
        cx.setProgress(0.92 * (y + n) / h);
    }
    img.opaque = img.info.alphaFmt ? lux::allOpaque(buf.data(), (size_t)w * h) : true;
    uint32_t ow = w, oh = h;
    buf = lux::orient(std::move(buf), w, h, orient, ow, oh);
    auto L0 = std::make_shared<lux::Level>();
    L0->w = ow; L0->h = oh; L0->px = std::move(buf);
    const bool reuse = cx.have && cx.have->w == img.w && cx.have->h == img.h && cx.have->first > 0;
    img.first = 0;
    img.levels.assign(lux::levelCount(img.w, img.h), nullptr);
    img.levels[0] = std::move(L0);
    if (!lux::buildPyramid(img, 0, reuse ? cx.have->first : -1, cx.cancel))
        return { cx.cancelled() ? DecodeStatus::Cancelled : DecodeStatus::TooLarge };
    if (reuse) img = lux::mergeFull(img, *cx.have);
    cx.setProgress(1.0);
    return { DecodeStatus::Ok, std::make_shared<const CpuImage>(std::move(img)) };
}

// De un decoder WIC ya abierto a la CpuImage. `pickLargest` es para los contenedores
// multi-resolucion (.ico/.cur): WIC suele entregar primero el frame de 16x16.
static Decoded wicDecode(IWICBitmapDecoder* dec, bool pickLargest, DecodeCtx& cx) {
    if (!dec) return {};
    UINT idx = 0;
    if (pickLargest) {
        UINT count = 0;
        if (SUCCEEDED(dec->GetFrameCount(&count)) && count > 1) {
            UINT64 best = 0;
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IWICBitmapFrameDecode> f;
                UINT fw = 0, fh = 0;
                if (FAILED(dec->GetFrame(i, &f)) || FAILED(f->GetSize(&fw, &fh))) continue;
                if ((UINT64)fw * fh > best) { best = (UINT64)fw * fh; idx = i; }
            }
        }
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(dec->GetFrame(idx, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) return {};
    // Orientacion EXIF: las fotos de celular y de camara vienen derechas en el sensor y con una
    // etiqueta que dice como girarlas. WIC no la aplica sola; se aplica en memoria (pixels.h).
    int orient = (int)exifOrientation(frame.Get());
    if (orient < 1 || orient > 8) orient = 1;
    CpuImage img;
    img.w = lux::orientSwapsAxes(orient) ? h : w;
    img.h = lux::orientSwapsAxes(orient) ? w : h;
    img.info = wicInfo(cx.wic, frame.Get());
    if ((uint64_t)w * h > MAX_PIXELS) return { DecodeStatus::TooLarge };
    if (cx.allowPreview && !pickLargest && wicPreview(frame.Get(), w, h, orient, cx, img)) {
        cx.setProgress(1.0);
        return { DecodeStatus::Ok, std::make_shared<const CpuImage>(std::move(img)) };
    }
    if (cx.cancelled()) return { DecodeStatus::Cancelled };
    img.levels.clear(); img.first = 0; img.opaque = true;
    return wicFull(frame.Get(), w, h, orient, cx, img);
}

static Decoded decodeWicFile(DecodeCtx& cx) {
    // desde el archivo (no desde memoria): WIC lee solo lo que necesita y no hay tope de tamaño
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(cx.wic->CreateDecoderFromFilename(fsPath(cx.path).c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec))) return {};
    return wicDecode(dec.Get(), cx.ext == L"ico" || cx.ext == L"cur", cx);
}

// Desde un buffer: lo usan los contenedores (ANI, ORA/KRA, gzip, el JPEG de un RAW).
static Decoded decodeWicMemory(const unsigned char* data, size_t len, bool pickLargest, DecodeCtx& cx) {
    if (!data || !len || len > UINT_MAX) return {};
    ComPtr<IWICStream> stream;
    if (FAILED(cx.wic->CreateStream(&stream))) return {};
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)len))) return {};
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(cx.wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return {};
    return wicDecode(dec.Get(), pickLargest, cx);
}

// ---------------------------------------------------------------- stb, QOI, propios
static Decoded decodeStbBytes(const unsigned char* d, size_t n, DecodeCtx& cx) {
    if (!d || !n || n > INT_MAX) return {};
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(d, (int)n, &w, &h, &comp) || w <= 0 || h <= 0) return {};
    if ((uint64_t)w * h > MAX_PIXELS || !enoughMemory((uint64_t)w * h * 4)) return { DecodeStatus::TooLarge };
    Raster r;
    if (stbi_is_hdr_from_memory(d, (int)n)) {
        // Radiance: stb lo bajaria a 8 bits recortando todo lo que pasa de 1 (luces quemadas);
        // en float pasa por el tone mapping
        float* f = stbi_loadf_from_memory(d, (int)n, &w, &h, &comp, 4);
        if (!f) return {};
        r = rasterFromFloat(f, 4, w, h);
        r.info.alphaFmt = false;
        stbi_image_free(f);
    } else {
        const bool is16 = stbi_is_16_bit_from_memory(d, (int)n) != 0;
        unsigned char* p = stbi_load_from_memory(d, (int)n, &w, &h, &comp, 4);   // RGBA
        if (!p) return {};
        r.px = lux::PixelBuf::adopt(p, (size_t)w * h * 4);   // stbi_image_free == free
        r.w = (uint32_t)w; r.h = (uint32_t)h;
        r.info.bpp = (uint32_t)comp * (is16 ? 16 : 8);
        r.info.alphaFmt = comp == 2 || comp == 4;
    }
    return finishRaster(std::move(r), cx);
}
static Decoded decodeStbFile(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    return decodeStbBytes(cx.bytes.data(), cx.bytes.size(), cx);
}

// QOI (Quite OK Image): valida su firma 'qoif'.
static Decoded decodeQoiBytes(const unsigned char* d, size_t n, DecodeCtx& cx) {
    if (!d || n < 14 || n > INT_MAX || memcmp(d, "qoif", 4)) return {};
    qoi_desc desc{};
    void* p = qoi_decode(d, (int)n, &desc, 4);   // RGBA (QOI_FREE == free)
    if (!p) return {};
    Raster r;
    r.px = lux::PixelBuf::adopt(p, (size_t)desc.width * desc.height * 4);
    r.w = desc.width; r.h = desc.height;
    r.info.bpp = desc.channels * 8u;
    r.info.alphaFmt = desc.channels == 4;
    return finishRaster(std::move(r), cx);
}
static Decoded decodeQoiFile(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    return decodeQoiBytes(cx.bytes.data(), cx.bytes.size(), cx);
}

// Cadena de decoders propios sobre un buffer: cada uno mira primero la extension y despues el
// magic, asi que se pueden encadenar sin miedo.
static Decoded decodeOwnBytes(const unsigned char* d, size_t n, const std::wstring& ext, DecodeCtx& cx) {
    if (!d || !n) return {};
    int w = 0, h = 0;
    if (ext == L"pfm") {                            // float: se queda con el HDR para el tone mapping
        int ch = 0;
        if (float* f = ex_pfm_float(d, n, &w, &h, &ch)) {
            Raster r = rasterFromFloat(f, ch, w, h);
            free(f);
            return finishRaster(std::move(r), cx);
        }
    }
    const std::string e = toUtf8(ext);
    unsigned char* px = exotic_load(d, n, e.c_str(), &w, &h);
    if (!px) px = retro_load(d, n, e.c_str(), &w, &h);
    if (!px) px = texture_load(d, n, e.c_str(), &w, &h);
    if (!px) px = sci_load(d, n, e.c_str(), &w, &h);
    if (!px && n > 9 && !memcmp(d, "gimp xcf ", 9)) px = xcf_load(d, n, e.c_str(), &w, &h);
    if (!px || w <= 0 || h <= 0) { free(px); return {}; }
    Raster r;
    r.px = lux::PixelBuf::adopt(px, (size_t)w * h * 4);
    r.w = (uint32_t)w; r.h = (uint32_t)h;
    return finishRaster(std::move(r), cx);
}
static Decoded decodeOwn(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    return decodeOwnBytes(cx.bytes.data(), cx.bytes.size(), cx.ext, cx);
}

// EXR (OpenEXR via tinyexr): float RGBA -> tone mapping.
static Decoded decodeExr(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    float* rgba = nullptr; int w = 0, h = 0; const char* err = nullptr;
    if (LoadEXRFromMemory(&rgba, &w, &h, cx.bytes.data(), cx.bytes.size(), &err) != TINYEXR_SUCCESS || !rgba) {
        if (err) FreeEXRErrorMessage(err);
        return {};
    }
    Raster r = rasterFromFloat(rgba, 4, w, h);
    free(rgba);
    return finishRaster(std::move(r), cx);
}

// ---------------------------------------------------------------- SVG
// nanosvg: se rasteriza a ~1600 px en el lado mayor (nitido al hacer zoom, y el tamaño con el
// que la "ventana pegada" lo muestra), a lo sumo 4096. El buffer se recibe por valor:
// nsvgParse lo consume in-place.
static Decoded decodeSvgBytes(std::vector<unsigned char> buf, DecodeCtx& cx) {
    if (buf.empty()) return {};
    if (buf.back() != 0) buf.push_back(0);
    NSVGimage* img = nsvgParse((char*)buf.data(), "px", 96.0f);
    if (!img) return {};
    if (img->width < 1.f || img->height < 1.f) { nsvgDelete(img); return {}; }
    const float maxDim = std::max(img->width, img->height);
    float scale = std::min(std::max(1600.f / maxDim, 0.1f), 8.0f);
    if (maxDim * scale > 4096.f) scale = 4096.f / maxDim;                  // tope absoluto
    const UINT w = std::max<UINT>(1, (UINT)lround(img->width * scale));
    const UINT h = std::max<UINT>(1, (UINT)lround(img->height * scale));
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    Raster r;
    r.px = lux::PixelBuf::alloc((size_t)w * h * 4);
    if (rast && r.px) {
        nsvgRasterize(rast, img, 0, 0, scale, r.px.data(), (int)w, (int)h, (int)(w * 4));   // limpia el destino
        r.w = w; r.h = h;
        r.info.alphaFmt = true;
    }
    if (rast) nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return finishRaster(std::move(r), cx);
}
static Decoded decodeSvgFile(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    return decodeSvgBytes(cx.bytes, cx);
}
// SVGZ = SVG comprimido con gzip (lo que exporta Inkscape con "comprimido").
static Decoded decodeSvgz(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    std::vector<unsigned char> svg;
    if (!up_gunzip(cx.bytes, svg)) return decodeSvgBytes(cx.bytes, cx);   // .svgz sin comprimir de verdad
    return decodeSvgBytes(std::move(svg), cx);
}

// ---------------------------------------------------------------- metarchivos (EMF / WMF)
static HENHMETAFILE luxLoadMetafile(const std::vector<unsigned char>& b) {
    const unsigned char* p = b.data(); size_t n = b.size();
    if (n < 20) return nullptr;
    // EMF: cabecera EMR_HEADER con la firma " EMF" en el offset 40
    if (n >= 44 && p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 0 && !memcmp(p + 40, " EMF", 4))
        return SetEnhMetaFileBits((UINT)n, p);
    // WMF: "placeable" (con bounding box) o el estandar crudo (tipo 1/2 + header de 9 words)
    METAFILEPICT mp{}; mp.mm = MM_ANISOTROPIC;
    bool haveExt = false;
    if (p[0] == 0xD7 && p[1] == 0xCD && p[2] == 0xC6 && p[3] == 0x9A && n > 22) {
        short l = *(const short*)(p + 6),  t = *(const short*)(p + 8);
        short r = *(const short*)(p + 10), bt = *(const short*)(p + 12);
        unsigned short inch = *(const unsigned short*)(p + 14);
        if (inch) {                                   // .01 mm = twips reescalados
            mp.xExt = MulDiv(r - l, 2540, inch); mp.yExt = MulDiv(bt - t, 2540, inch);
            haveExt = (mp.xExt > 0 && mp.yExt > 0);
        }
        p += 22; n -= 22;
    } else if (!((p[0] == 1 || p[0] == 2) && p[1] == 0 && p[2] == 9 && p[3] == 0)) {
        return nullptr;
    }
    return SetWinMetaFileBits((UINT)n, p, nullptr, haveExt ? &mp : nullptr);
}
static Decoded metafileFromBytes(const std::vector<unsigned char>& b, DecodeCtx& cx) {
    HENHMETAFILE emf = luxLoadMetafile(b);
    if (!emf) return {};
    ENHMETAHEADER eh{}; eh.nSize = sizeof(eh);
    int W = 0, H = 0;
    if (GetEnhMetaFileHeader(emf, sizeof(eh), &eh) && eh.nSize >= sizeof(ENHMETAHEADER)) {
        double mmW = (eh.rclFrame.right - eh.rclFrame.left) / 100.0;   // .01 mm -> mm
        double mmH = (eh.rclFrame.bottom - eh.rclFrame.top) / 100.0;
        W = (int)lround(mmW / 25.4 * 96.0); H = (int)lround(mmH / 25.4 * 96.0);
        if (W <= 0 || H <= 0) {
            W = eh.rclBounds.right - eh.rclBounds.left + 1;
            H = eh.rclBounds.bottom - eh.rclBounds.top + 1;
        }
    }
    if (W <= 0 || H <= 0) { W = 1024; H = 768; }
    // es vectorial: rasterizamos con holgura (como el SVG) para que aguante el zoom
    double sc = 1600.0 / (double)std::max(W, H);
    if (sc < 1.0) sc = 1.0;
    if (std::max(W, H) * sc > 4096.0) sc = 4096.0 / std::max(W, H);
    W = std::max(1, (int)lround(W * sc)); H = std::max(1, (int)lround(H * sc));

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    Raster r;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        RECT rc{ 0, 0, W, H };
        FillRect(mem, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));  // los metarchivos asumen papel
        SetMapMode(mem, MM_TEXT);
        PlayEnhMetaFile(mem, emf, &rc);
        GdiFlush();
        r.px = lux::PixelBuf::alloc((size_t)W * H * 4);
        if (r.px) {                                    // BGRX -> PBGRA opaco (ya es BGRA)
            const uint32_t* s = (const uint32_t*)bits;
            uint32_t* d = (uint32_t*)r.px.data();
            for (size_t i = 0; i < (size_t)W * H; ++i) d[i] = s[i] | 0xFF000000u;
            r.w = (uint32_t)W; r.h = (uint32_t)H;
            r.pbgra = true; r.opaque = true;
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem); ReleaseDC(nullptr, screen);
    DeleteEnhMetaFile(emf);
    return finishRaster(std::move(r), cx);
}
static Decoded decodeMetafileFile(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    std::vector<unsigned char> raw;
    if (cx.bytes.size() > 2 && cx.bytes[0] == 0x1F && cx.bytes[1] == 0x8B && up_gunzip(cx.bytes, raw))  // .emz/.wmz
        return metafileFromBytes(raw, cx);
    return metafileFromBytes(cx.bytes, cx);
}

// ---------------------------------------------------------------- iconos de ejecutables
static Raster rasterFromHICON(HICON ic) {
    Raster out;
    ICONINFO ii{};
    if (!ic || !GetIconInfo(ic, &ii)) return out;
    BITMAP bm{};
    HBITMAP hb = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (GetObjectW(hb, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        int W = bm.bmWidth;
        int H = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // sin color: mascara AND + XOR
        if (W > 0 && H > 0 && (INT64)W * H <= 4096LL * 4096LL) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -(ii.hbmColor ? H : H * 2);
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            HDC hdc = GetDC(nullptr);
            std::vector<unsigned char> src((size_t)W * (ii.hbmColor ? H : H * 2) * 4);
            if (GetDIBits(hdc, hb, 0, (UINT)(ii.hbmColor ? H : H * 2), src.data(), &bi, DIB_RGB_COLORS)) {
                out.px = lux::PixelBuf::alloc((size_t)W * H * 4);
                unsigned char* px = out.px.data();
                if (px && ii.hbmColor) {
                    bool anyAlpha = false;
                    for (size_t i = 0; i < (size_t)W * H; ++i) if (src[i*4+3]) { anyAlpha = true; break; }
                    std::vector<unsigned char> mk;
                    if (!anyAlpha && ii.hbmMask) {                      // sin canal alfa: usar la mascara
                        mk.resize((size_t)W * H * 4);
                        BITMAPINFO mi = bi; mi.bmiHeader.biHeight = -H;
                        if (!GetDIBits(hdc, ii.hbmMask, 0, (UINT)H, mk.data(), &mi, DIB_RGB_COLORS)) mk.clear();
                    }
                    for (size_t i = 0; i < (size_t)W * H; ++i) {
                        px[i*4+0] = src[i*4+2]; px[i*4+1] = src[i*4+1]; px[i*4+2] = src[i*4+0];
                        px[i*4+3] = anyAlpha ? src[i*4+3] : (mk.empty() ? 255 : (mk[i*4] ? 0 : 255));
                    }
                } else if (px) {                                       // icono monocromo clasico
                    for (size_t i = 0; i < (size_t)W * H; ++i) {
                        unsigned char andb = src[i*4];                 // mitad de arriba: mascara
                        unsigned char xorb = src[((size_t)W * H + i) * 4];
                        px[i*4+0] = px[i*4+1] = px[i*4+2] = xorb;
                        px[i*4+3] = andb ? 0 : 255;
                    }
                }
                if (px) { out.w = (uint32_t)W; out.h = (uint32_t)H; out.info.alphaFmt = true; }
            }
            ReleaseDC(nullptr, hdc);
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return out;
}
static Decoded decodeIconRes(DecodeCtx& cx) {
    static const int kSizes[] = { 256, 128, 64, 48, 32, 16 };
    const std::wstring p = fsPath(cx.path);
    for (int s : kSizes) {
        HICON ic = nullptr; UINT id = 0;
        if (PrivateExtractIconsW(p.c_str(), 0, s, s, &ic, &id, 1, LR_DEFAULTCOLOR) == 1 && ic) {
            Raster r = rasterFromHICON(ic);
            DestroyIcon(ic);
            if (r) return finishRaster(std::move(r), cx);
        }
    }
    return {};
}

// ---------------------------------------------------------------- JPEG incrustado
// La vista previa que todo RAW lleva adentro: es lo que hace que Lux abra un .cr3 o un .nef
// aunque no este el codec de la camara. Se busca el JPEG mas grande del archivo y se decodifica
// ese (por WIC: aplica su propia orientacion EXIF y admite la vista previa DCT).
static Decoded embeddedJpeg(const std::vector<unsigned char>& b, DecodeCtx& cx) {
    if (b.size() < 128) return {};
    size_t best = (size_t)-1; long long bestArea = 0;
    int found = 0;
    for (size_t i = 0; i + 4 < b.size() && found < 64; ++i) {
        if (b[i] != 0xFF || b[i+1] != 0xD8 || b[i+2] != 0xFF) continue;
        int w = 0, h = 0, comp = 0;
        size_t left = b.size() - i;
        if (left > INT_MAX) left = INT_MAX;
        if (stbi_info_from_memory(b.data() + i, (int)left, &w, &h, &comp) && w > 0 && h > 0) {
            ++found;
            long long area = (long long)w * h;
            if (area > bestArea) { bestArea = area; best = i; }
            i += 2;                                   // no re-escanear el mismo SOI
        }
    }
    if (best == (size_t)-1) return {};
    const size_t left = b.size() - best;
    Decoded d = decodeWicMemory(b.data() + best, left, false, cx);
    if (settled(d)) return d;
    return decodeStbBytes(b.data() + best, std::min<size_t>(left, INT_MAX), cx);
}
static Decoded decodeRawPreview(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    return embeddedJpeg(cx.bytes, cx);
}

// ---------------------------------------------------------------- contenedores
// Cadena generica sobre un buffer ya en memoria: la usan gzip y ZIP para decodificar lo que
// traen adentro sin volver al disco.
static Decoded decodeMemAny(const std::vector<unsigned char>& b, const std::wstring& e, DecodeCtx& cx) {
    if (b.empty()) return {};
    Decoded d;
    // SVG: por extension, o si el texto arranca con un prologo XML/SVG
    bool looksSvg = (e == L"svg" || e == L"svgz");
    if (!looksSvg && b.size() > 5) {
        size_t lim = std::min<size_t>(b.size() - 4, 512);
        for (size_t i = 0; i < lim; ++i)
            if (b[i] == '<' && (!memcmp(&b[i], "<svg", 4) || !memcmp(&b[i], "<?xml", 5))) { looksSvg = true; break; }
    }
    if (looksSvg && settled(d = decodeSvgBytes(b, cx))) return d;
    if (settled(d = decodeWicMemory(b.data(), b.size(), e == L"ico" || e == L"cur", cx))) return d;
    if (settled(d = decodeOwnBytes(b.data(), b.size(), e, cx))) return d;
    if (settled(d = decodeStbBytes(b.data(), b.size(), cx))) return d;
    if (settled(d = decodeQoiBytes(b.data(), b.size(), cx))) return d;
    if (settled(d = metafileFromBytes(b, cx))) return d;
    return embeddedJpeg(b, cx);
}

// Cualquier formato soportado, comprimido con gzip: foto.ppm.gz, plano.emz, …
static Decoded decodeGzip(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    std::vector<unsigned char> raw;
    if (!up_gunzip(cx.bytes, raw) || raw.empty()) return {};
    // "foto.ppm.gz" -> la extension util es la de adentro
    const std::wstring stem = cx.path.substr(0, cx.path.size() - cx.ext.size() - 1);
    return decodeMemAny(raw, extOf(stem), cx);
}

// ANI: cursor animado de Windows; mostramos el primer cuadro (a mayor resolucion).
static Decoded decodeAni(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    size_t off = 0, len = 0;
    if (!up_ani_frame(cx.bytes, &off, &len)) return {};
    return decodeWicMemory(cx.bytes.data() + off, len, true, cx);
}

// Formatos que son un ZIP con la imagen ya aplanada adentro: OpenRaster (.ora),
// Krita (.kra), Sketch (.sketch) y Procreate (.procreate).
static Decoded decodeZipImage(DecodeCtx& cx) {
    if (!cx.load()) return { cx.readStatus };
    static const char* const names[] = {
        "mergedimage.png", "mergedimage.jpg", "preview.png", "preview.jpg",
        "Thumbnails/thumbnail.png", "QuickLook/Thumbnail.png", "previews/preview.png",
        "thumbnail.png", "Document/QuickLook/Thumbnail.png",
    };
    for (const char* nm : names) {
        std::vector<unsigned char> img;
        if (!up_zip_extract(cx.bytes, nm, img) || img.empty()) continue;
        Decoded d = decodeMemAny(img, L"", cx);
        if (settled(d)) return d;
    }
    return {};
}

// Cadena unica: elige decoder por extension y cae a otros si falla. Un paso que "resuelve" con
// error (no se pudo leer, no hay memoria, cancelado) corta la cadena: no tiene sentido seguir.
static Decoded decodeFile(DecodeCtx& cx) {
    const std::wstring& e = cx.ext;
    const uint16_t f = extFlags(e);
    Decoded d;
    if (e == L"svg")  return decodeSvgFile(cx);
    if (e == L"svgz") return decodeSvgz(cx);
    if (e == L"gz" || e == L"z") return decodeGzip(cx);
    if (e == L"qoi")  return decodeQoiFile(cx);
    if (e == L"exr")  return decodeExr(cx);
    if (e == L"ani")  return decodeAni(cx);
    if (f & FX_ZIP)   return decodeZipImage(cx);
    if (f & FX_META)  return decodeMetafileFile(cx);
    if (f & FX_PE)    return decodeIconRes(cx);
    if (f & FX_OWN) {
        if (settled(d = decodeOwn(cx))) return d;
        if (e == L"scr" && settled(d = decodeIconRes(cx))) return d;   // salvapantallas, no ZX
    }
    if (f & FX_RAW) {                                   // codec de la camara, si esta
        if (settled(d = decodeWicFile(cx))) return d;
        return decodeRawPreview(cx);                    // si no, la vista previa incrustada
    }
    if (f & FX_STB) {
        if (settled(d = decodeStbFile(cx))) return d;
        if (settled(d = decodeOwn(cx))) return d;       // Netpbm en ASCII (P1/P2/P3)
        // si stb no puede (un PSB, por ejemplo) sigue la cadena general
    }
    if (settled(d = decodeWicFile(cx))) return d;
    if (settled(d = decodeStbFile(cx))) return d;
    if (settled(d = decodeQoiFile(cx))) return d;       // QOI valida su firma
    if (settled(d = decodeOwn(cx))) return d;           // los propios validan por magic
    if (settled(d = decodeMetafileFile(cx))) return d;
    if (settled(d = decodeIconRes(cx))) return d;       // ¿un ejecutable con icono?
    return decodeRawPreview(cx);                        // ultimo recurso: JPEG adentro
}

// ===========================================================================
//  CARGADOR — hilos que decodifican pedidos por prioridad. La interfaz le dice QUE quiere
//  (sync) y el cargador descarta lo que ya no hace falta, cancela lo que corre de mas y avisa
//  con un WM_APP_LOADED cuando hay resultados (drain).
// ===========================================================================
struct Job {
    uint64_t id = 0;
    int index = -1;                                  // en g.files cuando se pidio
    std::wstring path;
    JobKind kind = JobKind::Preview;
    int priority = 0;
    uint32_t boxW = 0, boxH = 0;
    std::shared_ptr<const CpuImage> have;            // vista previa a completar (Full)
    std::atomic<bool> cancel{false};
    std::atomic<uint32_t> progress{PROGRESS_UNKNOWN};
};
struct JobSpec {
    int index;
    std::wstring path;
    JobKind kind;
    int priority;
    std::shared_ptr<const CpuImage> have;
};
struct JobResult {
    std::shared_ptr<Job> job;
    DecodeStatus st = DecodeStatus::Unsupported;
    std::shared_ptr<const CpuImage> img;
    FileStamp stamp;                                 // el archivo tal como estaba al decodificarlo
    double ms = 0;
};

class Loader {
public:
    void start(HWND hwnd, unsigned workers) {
        hwnd_ = hwnd;
        for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
    }

    // Cancela todo y espera a los hilos a lo sumo waitMs. Lo que no termina a tiempo (un decoder
    // sin puntos de cancelacion, como stb) se suelta: ExitProcess lo corta despues.
    void shutdown(DWORD waitMs) {
        std::unique_lock<std::mutex> lk(m_);
        stop_ = true;
        queue_.clear();
        for (auto& j : running_) j->cancel.store(true);
        cv_.notify_all();
        const bool idle = idle_.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return running_.empty(); });
        lk.unlock();
        for (auto& t : threads_) { if (idle) t.join(); else t.detach(); }
        threads_.clear();
    }

    // Deja en la cola exactamente `specs` (vienen ordenados por prioridad). Lo que estaba en cola
    // y no se pide mas se descarta sin haber empezado; lo que esta corriendo y no se pide mas se
    // cancela, salvo que keep(job) diga que conviene dejarlo terminar.
    template <class Keep>
    void sync(const std::vector<JobSpec>& specs, uint32_t boxW, uint32_t boxH, Keep&& keep) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::shared_ptr<Job>> next;
        std::vector<char> wanted(running_.size(), 0);
        for (const JobSpec& s : specs) {
            bool covered = false;
            for (size_t i = 0; i < running_.size(); ++i) {
                const Job& r = *running_[i];
                // una completa en curso tambien sirve para un pedido de vista previa
                if (!r.cancel.load() && r.path == s.path && (r.kind == s.kind || r.kind == JobKind::Full)) {
                    wanted[i] = 1; covered = true;
                }
            }
            if (covered) continue;
            std::shared_ptr<Job> job;
            for (auto& q : queue_)
                if (q && q->path == s.path && q->kind == s.kind) { job = std::move(q); break; }
            if (!job) {
                job = std::make_shared<Job>();
                job->id = ++nextId_;
                job->path = s.path;
                job->kind = s.kind;
                job->have = s.have;
                job->boxW = boxW; job->boxH = boxH;
            }
            job->index = s.index;
            job->priority = s.priority;
            next.push_back(std::move(job));
        }
        queue_.swap(next);
        for (size_t i = 0; i < running_.size(); ++i)
            if (!wanted[i] && !keep(*running_[i])) running_[i]->cancel.store(true);
        if (!queue_.empty()) cv_.notify_all();
    }

    std::vector<JobResult> drain() {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<JobResult> out;
        out.swap(done_);
        return out;
    }
    bool busy() {
        std::lock_guard<std::mutex> lk(m_);
        return !queue_.empty() || !running_.empty();
    }
    uint32_t progressOf(const std::wstring& path) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& j : running_) if (j->path == path && !j->cancel.load()) return j->progress.load();
        return PROGRESS_UNKNOWN;
    }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
                if (stop_) break;
                job = std::move(queue_.front());   // la cola viene ordenada: el primero es el mas urgente
                queue_.erase(queue_.begin());
                running_.push_back(job);
            }
            JobResult res;
            res.job = job;
            const double t0 = nowMs();
            res.stamp = statFile(job->path);         // antes de leer: si cambia en el medio, el sello queda viejo y se relee
            if (!wic) {
                res.st = DecodeStatus::Unsupported;
            } else if (job->cancel.load()) {
                res.st = DecodeStatus::Cancelled;
            } else {
                DecodeCtx cx;
                cx.path = job->path;
                cx.ext = extOf(job->path);
                cx.wic = wic.Get();
                cx.cancel = &job->cancel;
                cx.progress = &job->progress;
                cx.allowPreview = job->kind == JobKind::Preview;
                cx.boxW = job->boxW; cx.boxH = job->boxH;
                cx.have = job->have;
                Decoded d = decodeFile(cx);
                res.st = job->cancel.load() ? DecodeStatus::Cancelled : d.st;
                res.img = std::move(d.img);
            }
            res.ms = nowMs() - t0;
            {
                std::lock_guard<std::mutex> lk(m_);
                running_.erase(std::find(running_.begin(), running_.end(), job));
                done_.push_back(std::move(res));
                if (running_.empty()) idle_.notify_all();
            }
            PostMessageW(hwnd_, WM_APP_LOADED, 0, 0);
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (running_.empty()) idle_.notify_all();
        }
        wic.Reset();
        CoUninitialize();
    }

    HWND hwnd_ = nullptr;
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    std::vector<std::shared_ptr<Job>> queue_, running_;
    std::vector<JobResult> done_;
    uint64_t nextId_ = 0;
    bool stop_ = false;
};
static Loader g_loader;

// Cache de imagenes decodificadas (solo la toca el hilo de la interfaz).
struct CacheEntry {
    std::shared_ptr<const CpuImage> img;             // nulo si fallo
    DecodeStatus st = DecodeStatus::Ok;
    FileStamp stamp;
};
static lux::LruCache<std::wstring, CacheEntry> g_cache(512ull << 20);

// Presupuesto: 1/16 de la RAM, entre 256 y 512 MB. Alcanza para la imagen en pantalla (aun
// una de 48 MP completa) mas decenas de fotos degradadas (ver demoteInCache).
static size_t cacheBudget() {
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (!GlobalMemoryStatusEx(&ms)) return 256ull << 20;
    return (size_t)std::clamp<uint64_t>(ms.ullTotalPhys / 16, 256ull << 20, 512ull << 20);
}

// ===========================================================================
//  GPU — mosaicos de la imagen en pantalla, subidos a demanda
// ===========================================================================
static void invalidate() { if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

static void gpuBuildLevel(int k) {
    GpuLevel& gl = g.gpu[k];
    gl.src = g.cur ? g.cur->level(k) : nullptr;
    gl.tiles.clear();
    if (!gl.src) return;
    gl.grid = lux::makeGrid(gl.src->w, gl.src->h, g.maxBmp);
    gl.tiles.resize((size_t)gl.grid.cols * gl.grid.rows);
}
// Imagen nueva: afuera todos los mosaicos.
static void gpuReset() {
    g.gpu.clear();
    g.gpuBytes = 0;
    if (!g.cur) return;
    g.gpu.resize(g.cur->levels.size());
    for (int k = 0; k < (int)g.gpu.size(); ++k) gpuBuildLevel(k);
}
// La misma imagen con mas niveles (llego la completa): los niveles que siguen siendo los mismos
// objetos conservan sus mosaicos ya subidos.
static void gpuAdopt() {
    std::vector<GpuLevel> old;
    old.swap(g.gpu);
    g.gpuBytes = 0;
    if (!g.cur) return;
    g.gpu.resize(g.cur->levels.size());
    for (int k = 0; k < (int)g.gpu.size(); ++k) {
        const lux::Level* L = g.cur->level(k);
        if (L && k < (int)old.size() && old[k].src == L) {
            g.gpu[k] = std::move(old[k]);
            for (const GpuTile& t : g.gpu[k].tiles) if (t.bmp) {
                D2D1_SIZE_U s = t.bmp->GetPixelSize();
                g.gpuBytes += (size_t)s.width * s.height * 4;
            }
        } else {
            gpuBuildLevel(k);
        }
    }
}
// Se perdio el dispositivo (driver reiniciado, escritorio remoto): los mosaicos se vuelven a
// subir solos desde la CPU cuando hagan falta.
static void gpuDropAll() {
    for (auto& gl : g.gpu) for (auto& t : gl.tiles) t.bmp.Reset();
    g.gpuBytes = 0;
}

static ID2D1Bitmap* gpuTile(int k, int tx, int ty, bool upload) {
    if (k < 0 || k >= (int)g.gpu.size()) return nullptr;
    GpuLevel& gl = g.gpu[k];
    if (!gl.src || tx < 0 || ty < 0 || tx >= gl.grid.cols || ty >= gl.grid.rows) return nullptr;
    GpuTile& t = gl.tiles[(size_t)ty * gl.grid.cols + tx];
    if (!t.bmp && upload && g.rt) {
        const lux::TileRect r = lux::tileRect(gl.grid, tx, ty);
        const uint8_t* p = gl.src->px.data() + ((size_t)r.by0 * gl.src->w + r.bx0) * 4;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            g.cur->opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(g.rt->CreateBitmap(D2D1::SizeU(r.bx1 - r.bx0, r.by1 - r.by0), p, gl.src->w * 4, bp, &t.bmp)))
            g.gpuBytes += (size_t)(r.bx1 - r.bx0) * (r.by1 - r.by0) * 4;
    }
    if (t.bmp) t.lastUse = g.frame;
    return t.bmp.Get();
}

// El nivel mas grueso (fallback de todo) y el de efectos (ambiente y sombra) viven siempre en la
// GPU: son un mosaico cada uno y chicos.
static void gpuEnsureBase() {
    if (!g.cur) return;
    gpuTile(g.cur->last(), 0, 0, true);
    gpuTile(g.cur->effectLevel(), 0, 0, true);
}

// Por encima del presupuesto: afuera los mosaicos menos usados (nunca los de este cuadro, ni la
// base ni el de efectos).
static void gpuTrim() {
    if (g.gpuBytes <= GPU_BUDGET || !g.cur) return;
    struct Cand { uint32_t use; int k; size_t i; };
    std::vector<Cand> c;
    const int keepA = g.cur->last(), keepB = g.cur->effectLevel();
    for (int k = 0; k < (int)g.gpu.size(); ++k) {
        if (k == keepA || k == keepB) continue;
        for (size_t i = 0; i < g.gpu[k].tiles.size(); ++i) {
            const GpuTile& t = g.gpu[k].tiles[i];
            if (t.bmp && t.lastUse != g.frame) c.push_back({ t.lastUse, k, i });
        }
    }
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.use < b.use; });
    for (const Cand& x : c) {
        if (g.gpuBytes <= GPU_BUDGET) break;
        GpuTile& t = g.gpu[x.k].tiles[x.i];
        D2D1_SIZE_U s = t.bmp->GetPixelSize();
        g.gpuBytes -= std::min(g.gpuBytes, (size_t)s.width * s.height * 4);
        t.bmp.Reset();
    }
}

// ===========================================================================
//  NAVEGACION — pedidos, resultados y lo que se muestra
// ===========================================================================
#ifdef LUX_BENCH_BUILD
static struct {
    bool on = false, nav = false, waiting = false;
    FILE* f = nullptr;
    int phase = 0;              // 0 primera imagen, 1 navegacion en frio, 2 en caliente, 3 cuadros
    double t0 = 0, openMs = 0, navT = 0, navCold = 0, navWarm = 0, f0 = 0;
    double wsOpen = 0, wsUpgrade = 0;
    int navN = 0, warmN = 0, frames = 0, openLevel = -1;
} B;
static double workingSetMB(bool peak) {
    PROCESS_MEMORY_COUNTERS pmc{ sizeof(pmc) };
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return (peak ? pmc.PeakWorkingSetSize : pmc.WorkingSetSize) / 1048576.0;
}
#endif
static void fitToWindow();                     // fwd
static void applyWindowSizing(bool recenter);  // fwd
static void openPath(const std::wstring& path);// fwd
static void schedule();                        // fwd
static void zoomCenter(float factor);          // fwd
static bool hasImage() { return g.view == View::Showing && g.cur != nullptr; }
#ifdef LUX_BENCH_BUILD
static void benchOnPresent();                  // fwd
#endif

// Muestra el HUD (resolucion + zoom) al instante y reinicia su cuenta de 3 s.
static void showHud() {
    g.hudShownAt = GetTickCount();
    g.hudTarget  = 1.f;
    g.hudAlpha   = 1.f;   // aparece sin demora (feedback inmediato del zoom)
    if (g.hwnd) { SetTimer(g.hwnd, IDT_HUD, 16, nullptr); invalidate(); }
}

// La caja que una vista previa tiene que cubrir para verse identica a la completa en "ajustar":
// el escenario de la ventana tal como esta. Con la "ventana pegada a la imagen", minimizada o en
// pantalla completa, el area util del monitor (o el monitor entero), que es lo que va a ocupar.
// Si despues se maximiza, el zoom nota que le falta resolucion y pide la completa (~0,1 s).
static void previewBox(uint32_t& w, uint32_t& h) {
    const uint32_t MIN_W = 640, MIN_H = 480;       // ventanas diminutas: igual una vista previa digna
    RECT rc{};
    const bool useWindow = g.hwnd && !g.fullscreen && !IsIconic(g.hwnd) && !g.cfg.fitWindow &&
                           GetClientRect(g.hwnd, &rc) && rc.right > 0 && rc.bottom > 0;
    if (useWindow) {
        w = (uint32_t)rc.right;
        h = (uint32_t)std::max<LONG>(1, rc.bottom - dp(TBH_L) - dp(SBH_L));
    } else {
        MONITORINFO mi{ sizeof(mi) };
        if (!g.hwnd || !GetMonitorInfo(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) { w = 1920; h = 1080; return; }
        const RECT& r = g.fullscreen ? mi.rcMonitor : mi.rcWork;
        w = (uint32_t)std::max<LONG>(1, r.right - r.left);
        h = (uint32_t)std::max<LONG>(1, r.bottom - r.top - (g.fullscreen ? 0 : dp(TBH_L) + dp(SBH_L)));
    }
    w = std::max(w, MIN_W);
    h = std::max(h, MIN_H);
}

// El zoom actual pide niveles mas finos que los que hay (se esta viendo una vista previa)?
static void updateZoomNeed() {
    const bool need = hasImage() && lux::idealLevel(g.scale, g.cur->last()) < g.cur->first;
    if (need != g.zoomNeedsFull) { g.zoomNeedsFull = need; schedule(); }
}

// La imagen que se deja de ver queda en cache solo desde el nivel que hace falta para verla
// entera en la pantalla: volver a ella sigue siendo instantaneo y ocupa 1/4..1/64 de la
// memoria (una foto de 12 MP, 16 MB en vez de 64). Si despues se hace zoom, la resolucion
// completa se vuelve a decodificar. Los niveles son los mismos objetos: no se copia nada.
static void demoteInCache(const std::wstring& path, const std::shared_ptr<const CpuImage>& img) {
    if (!img || path.empty()) return;
    const CacheEntry* e = g_cache.peek(path);
    if (!e || e->img != img) return;                  // la cache ya tiene otra version
    uint32_t bw, bh;
    previewBox(bw, bh);
    const double fit = std::min((double)bw / img->w, (double)bh / img->h);
    const int k = std::max(img->first, lux::idealLevel(fit, img->last()));
    if (k <= img->first) return;                      // ya esta en su minimo util
    auto d = std::make_shared<CpuImage>();
    d->w = img->w; d->h = img->h;
    d->opaque = img->opaque;
    d->info = img->info;
    d->first = k;
    d->levels.assign(img->levels.size(), nullptr);
    for (int j = k; j <= img->last(); ++j) d->levels[j] = img->levels[j];
    const size_t bytes = d->bytes();
    const FileStamp stamp = e->stamp;
    g_cache.put(path, CacheEntry{ std::move(d), DecodeStatus::Ok, stamp }, bytes);
}

static void setWindowTitle(const std::wstring& path) {
    if (g.hwnd) SetWindowTextW(g.hwnd, (baseName(path) + L"  —  Lux").c_str());
}

// Pone en pantalla la imagen i (ya decodificada).
static void present(int i, std::shared_ptr<const CpuImage> img) {
    if (!img || i < 0 || i >= (int)g.files.size()) return;
    const std::wstring& path = g.files[i];
    if (path != g.imgPath) {
        g.tooBig.clear();                             // cambio el vecindario: se puede volver a intentar
        if (hasImage()) demoteInCache(g.imgPath, g.cur);
    }
    g.idx = i;
    g.imgPath = path;
    g.shownTotal = (int)g.files.size();
    g.cur = std::move(img);
    gpuReset();
    g.imgW = g.cur->w; g.imgH = g.cur->h;
    g.imgBpp = g.cur->info.bpp; g.imgAlpha = g.cur->info.alphaFmt; g.imgHdr = g.cur->info.hdr;
    g.fmtLabel = fmtLabelFor(path);
    g.view = View::Showing;
    g.loadError.clear();
    g.skips = 0;
    g.fit = true;
    g.uploadBoost = true;
    g.dwellDone = false;
    g.presentedAt = GetTickCount();
    if (g.hwnd) {
        if (g.cur->full()) KillTimer(g.hwnd, IDT_DWELL);
        else SetTimer(g.hwnd, IDT_DWELL, DWELL_FULL_MS, nullptr);
    }
    fitToWindow();
    // modo "ventana pegada a la imagen": ajustar la ventana a esta imagen
    if (g.cfg.fitWindow) applyWindowSizing(g.firstSize);
    g.firstSize = false;
    setWindowTitle(path);   // titulo de ventana (accesibilidad / barra de tareas)
    invalidate();
#ifdef LUX_BENCH_BUILD
    benchOnPresent();
#endif
}

// Llego una version mejor de la imagen en pantalla (la completa de una vista previa).
static void upgrade(std::shared_ptr<const CpuImage> img) {
    if (!img || img == g.cur) return;
    g.cur = std::move(img);
    gpuAdopt();
#ifdef LUX_BENCH_BUILD
    if (B.on && B.wsUpgrade == 0) B.wsUpgrade = workingSetMB(false);
#endif
    g.imgBpp = g.cur->info.bpp; g.imgAlpha = g.cur->info.alphaFmt; g.imgHdr = g.cur->info.hdr;
    updateZoomNeed();
    invalidate();
}

static std::wstring errorText(const std::wstring& path, DecodeStatus st) {
    const uint16_t f = extFlags(extOf(path));
    std::wstring t = baseName(path) + L"\n";
    if (st == DecodeStatus::TooLarge)     t += L"Demasiado grande para la memoria disponible";
    else if (st == DecodeStatus::IoError) t += L"No se pudo leer el archivo (¿se movió, o falta permiso?)";
    else if (f & FX_CODEC)                t += L"Falta la extensión de códec — instalala gratis desde Microsoft Store";
    else if (f & FX_RAW)                  t += L"RAW sin vista previa incrustada — instalá Raw Image Extension desde Microsoft Store";
    else                                  t += L"Formato no soportado o archivo dañado";
    return t;
}

static void requestIndex(int i, int dir, bool explicitOpen);   // fwd

// El pedido i no se pudo decodificar. Si fue "abrir este archivo", se dice por que; si se venia
// navegando, se saltea y se sigue en la misma direccion (como antes, pero sin congelar nada).
static void failTarget(int i, DecodeStatus st) {
    const int n = (int)g.files.size();
    if (g.explicitOpen || n <= 1) {
        g.target = -1;
        g.idx = i;
        g.cur.reset();
        gpuReset();
        g.imgPath.clear();
        g.view = View::Error;
        g.loadError = errorText(g.files[i], st);
        setWindowTitle(g.files[i]);
        invalidate();
        return;
    }
    if (++g.skips >= n) { g.target = -1; invalidate(); return; }   // la carpeta entera esta rota
    requestIndex(i + g.navDir, g.navDir, false);
}

// Pedir la imagen i: si ya esta en cache se muestra en el acto; si no, queda pendiente (con la
// anterior en pantalla) y el cargador la decodifica primero que nada.
static void requestIndex(int i, int dir, bool explicitOpen) {
    const int n = (int)g.files.size();
    if (!n) return;
    i = lux::wrapIndex(i, n);
    g.navDir = dir < 0 ? -1 : 1;
    g.explicitOpen = explicitOpen;
    const std::wstring& path = g.files[i];
    if (CacheEntry* e = g_cache.get(path)) {
        const FileStamp now = statFile(path);
        if (now == e->stamp) {
            if (e->img) { g.target = -1; present(i, e->img); schedule(); return; }
            failTarget(i, e->st);
            schedule();
            return;
        }
        // el archivo cambio en disco (o ya no esta): lo de la cache no es ese archivo
        g_cache.erase(path);
        if (!now.exists) { failTarget(i, DecodeStatus::IoError); schedule(); return; }
    }
    if (g.target != i) { g.requestedAt = GetTickCount(); g.loadShown = 0.f; }
    g.target = i;
    if (g.view != View::Showing) {                    // nada que mostrar mientras tanto
        g.view = View::Loading;
        g.idx = i;
        g.loadError.clear();
        setWindowTitle(path);
    }
    schedule();
    if (g.hwnd) SetTimer(g.hwnd, IDT_LOAD, 16, nullptr);
    invalidate();
}

static void navigate(int delta) {
    if (g.files.empty()) return;
    // con la flecha apretada se encadena desde lo ultimo pedido, no desde lo que se ve
    const int base = g.target >= 0 ? g.target : g.idx;
    g.skips = 0;
    requestIndex(base + delta, delta, false);
}

// Que decodificar ahora: la politica (plan.h) menos lo que ya esta en cache.
static void schedule() {
    const int n = (int)g.files.size();
    if (!n) { g_loader.sync({}, 0, 0, [](const Job&) { return false; }); return; }
    const int displayed = hasImage() ? g.idx : -1;
    // la resolucion completa de una vista previa se pide si el zoom la necesita o si la imagen
    // lleva DWELL_FULL_MS en pantalla: pasando fotos rapido no se decodifica nada dos veces
    const bool wantFull = g.zoomNeedsFull || g.dwellDone ||
                          (hasImage() && GetTickCount() - g.presentedAt > DWELL_FULL_MS + 16);
    const bool full = !hasImage() || g.cur->full() || !wantFull;
    const auto wants = lux::planJobs(n, displayed, g.target, g.navDir, full, g.zoomNeedsFull);
    std::vector<JobSpec> specs;
    for (const lux::Want& w : wants) {
        const std::wstring& p = g.files[w.index];
        const CacheEntry* e = g_cache.peek(p);
        if (e && !e->img) continue;                                   // roto: no se reintenta
        if (w.kind == JobKind::Preview && (e || g.tooBig.count(p))) continue;
        if (w.kind == JobKind::Full && e && e->img->full()) continue;
        std::shared_ptr<const CpuImage> have = (e && !e->img->full()) ? e->img : nullptr;
        specs.push_back({ w.index, p, w.kind, w.priority, std::move(have) });
    }
    uint32_t bw, bh;
    previewBox(bw, bh);
    // con la flecha apretada, una vista previa intermedia que ya esta en curso se deja terminar:
    // si llega antes que la pedida se muestra (efecto "hojear") en vez de tirar el trabajo
    g_loader.sync(specs, bw, bh, [n](const Job& j) {
        return g.target >= 0 && !g.explicitOpen && j.kind == JobKind::Preview && hasImage() &&
               lux::onPath(n, g.idx, j.index, g.target, g.navDir);
    });
}

static int indexOfJob(const Job& j) {
    if (j.index >= 0 && j.index < (int)g.files.size() && g.files[j.index] == j.path) return j.index;
    return -1;                                        // la carpeta cambio desde que se pidio
}

// Resultados del cargador: a la cache, y a la pantalla si corresponde.
static void onLoaderResults() {
    std::vector<JobResult> results = g_loader.drain();
    if (results.empty()) return;
    std::vector<std::wstring> fresh;
    for (JobResult& r : results) {
        const Job& j = *r.job;
        if (r.st == DecodeStatus::Cancelled) continue;
        if (r.st == DecodeStatus::Ok && r.img) {
            // una vista previa atrasada no pisa una completa que ya este
            const CacheEntry* have = g_cache.peek(j.path);
            if (!(have && have->img && have->img->full() && !r.img->full())) {
                g_cache.put(j.path, CacheEntry{ r.img, r.st, r.stamp }, r.img->bytes());
                fresh.push_back(j.path);
            }
        } else {
            g_cache.put(j.path, CacheEntry{ nullptr, r.st, r.stamp }, 0);
        }
        const int i = indexOfJob(j);
        if (i < 0) continue;
        const int n = (int)g.files.size();
        CacheEntry* e = g_cache.get(j.path);
        if (g.target >= 0 && i == g.target) {
            if (e && e->img) { g.target = -1; present(i, e->img); }
            else failTarget(i, r.st == DecodeStatus::Ok ? DecodeStatus::Unsupported : r.st);
        } else if (g.target >= 0 && !g.explicitOpen && e && e->img && hasImage() &&
                   lux::onPath(n, g.idx, i, g.target, g.navDir)) {
            present(i, e->img);                       // hojear: intermedia que llego antes (solo con flechas)
        } else if (e && e->img && hasImage() && j.path == g.imgPath) {
            upgrade(e->img);
        }
    }
    // presupuesto: lo que se ve y lo pedido nunca se van. Una precarga recien llegada que no
    // entra se va ella misma y queda anotada para no pedirla de nuevo en un bucle.
    g_cache.trim([](const std::wstring& k) {
        return k == g.imgPath || (g.target >= 0 && g.target < (int)g.files.size() && k == g.files[g.target]);
    });
    for (const std::wstring& p : fresh) if (!g_cache.peek(p)) g.tooBig.insert(p);
    schedule();
    invalidate();
}

// Al arrancar con un archivo: esperar un rato a que llegue antes de mostrar la ventana, asi
// aparece de una con la imagen (y del tamaño justo en el modo "ventana pegada"). Si tarda mas,
// la ventana sale igual con la pantalla de carga.
static void awaitFirstImage(DWORD timeoutMs) {
    const DWORD t0 = GetTickCount();
    while (g.target >= 0) {
        const DWORD el = GetTickCount() - t0;
        if (el >= timeoutMs) break;
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, timeoutMs - el, QS_POSTMESSAGE) == WAIT_TIMEOUT) break;
        MSG msg;
        while (PeekMessageW(&msg, g.hwnd, WM_APP_LOADED, WM_APP_LOADED, PM_REMOVE)) onLoaderResults();
    }
}

// ---------------------------------------------------------------------------
//  Geometria de vista (fit / zoom / pan)
// ---------------------------------------------------------------------------
static D2D1_RECT_F stageRect() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float top = g.fullscreen ? 0.f : (float)dp(TBH_L);
    // la barra de estado inferior solo existe cuando hay imagen cargada
    float bot = (g.fullscreen || !hasImage()) ? (float)rc.bottom
                                              : (float)rc.bottom - (float)dp(SBH_L);
    return D2D1::RectF(0, top, (float)rc.right, bot);
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
    // origen en pixel entero: centrar con medio pixel de resto remuestrea TODA la
    // imagen (al 100 % cada pixel quedaria repartido entre dos y se veria blanda)
    g.ox = snapPx(g.ox); g.oy = snapPx(g.oy);
}
static void fitToWindow() {
    g.scale = fitScale();
    g.fit = true;
    clampPan();
    showHud();
    updateZoomNeed();
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
    showHud();
    updateZoomNeed();
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
    if (!hasImage() || !g.imgW || !g.imgH) return;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

    int  workW = mi.rcWork.right - mi.rcWork.left;
    int  workH = mi.rcWork.bottom - mi.rcWork.top;
    int  tbh = dp(TBH_L), sbh = dp(SBH_L);   // el escenario = ventana - barra de titulo - barra de estado
    double availW = workW - dpf(60.f);
    double availH = workH - dpf(60.f) - tbh - sbh;
    double s = std::min(1.0, std::min(availW / g.imgW, availH / g.imgH));
    int winW = std::max(dp(420), (int)lround(g.imgW * s));
    int winH = std::max(dp(280), (int)lround(g.imgH * s) + tbh + sbh);

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
//  Tipografia nitida
//
//  Tres cosas hacen que el texto chico de Cascadia se vea gris y borroso, y las
//  tres se resuelven aca:
//   1. El peso se elige por el TAMAÑO FINAL en pixeles, no por el rol. Las caras
//      finas (ExtraLight/Light) son hermosas grandes y se deshacen chicas: por
//      debajo de cierto tamaño el trazo mide menos de un pixel y el antialiasing
//      lo pinta gris sucio. Cuanto mas chica la letra, mas cuerpo. Los textos de
//      datos (numeros, medidas) llevan un escalon extra: la jerarquia la dan
//      tamaño + color + peso, no solo el tamaño.
//   2. Medicion y render GDI-compatibles (hinting completo, avances enteros): es
//      el ClearType "clasico", cada asta cae en una columna de pixeles.
//   3. ClearType de verdad, que exige un render target OPACO (ver
//      createDeviceResources) y origenes de texto en pixel entero (drawTextIn).
//  Umbrales calibrados a 150 %: 20/15/12 px = 10/7,5/6 pt logicos.
// ---------------------------------------------------------------------------
static const float TEXT_MIN_PX = 10.f;   // piso absoluto del tamaño final (5 pt a 150 %)

static DWRITE_FONT_WEIGHT weightForPx(float px, int masCuerpo) {
    static const DWRITE_FONT_WEIGHT escalera[] = {   // de la mas fina a la mas solida
        DWRITE_FONT_WEIGHT_EXTRA_LIGHT, DWRITE_FONT_WEIGHT_LIGHT,
        DWRITE_FONT_WEIGHT_SEMI_LIGHT,  DWRITE_FONT_WEIGHT_NORMAL,
    };
    int e = px >= 20.f ? 0 : px >= 15.f ? 1 : px >= 12.f ? 2 : 3;
    e = std::min(3, e + masCuerpo);
    return escalera[e];
}

// Antialiasing de texto segun lo que tenga configurado el sistema (ClearType,
// gris o nada) y parametros de render (gamma, contraste, geometria RGB/BGR) del
// monitor donde esta la ventana. Se vuelve a llamar si cambia el DPI, el monitor
// o la configuracion de fuentes.
static void applyTextRendering() {
    if (!g.rt || !g.dw) return;
    BOOL smooth = TRUE; UINT type = FE_FONTSMOOTHINGCLEARTYPE;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smooth, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    g.rt->SetTextAntialiasMode(!smooth ? D2D1_TEXT_ANTIALIAS_MODE_ALIASED
        : type == FE_FONTSMOOTHINGCLEARTYPE ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
                                            : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<IDWriteRenderingParams> rp;
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    if (SUCCEEDED(g.dw->CreateMonitorRenderingParams(mon, &rp)) && rp) g.rt->SetTextRenderingParams(rp.Get());
}

// ---------------------------------------------------------------------------
//  Recursos Direct2D
// ---------------------------------------------------------------------------
static void createTextFormats() {
    // dip = tamaño de diseño a 96 dpi; el final (px fisicos, porque el target corre a
    // 96 dpi y escalamos a mano) se redondea a entero: el hinting de la fuente esta
    // afinado por ppem entero. masCuerpo = escalon extra para los textos de datos.
    auto mk = [&](float dip, int masCuerpo, IDWriteTextFormat** out) {
        float px = snapPx(std::max(TEXT_MIN_PX, dpf(dip)));
        g.dw->CreateTextFormat(L"Cascadia Code", nullptr, weightForPx(px, masCuerpo),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", out);
    };
    g.tfCap.Reset(); g.tfHud.Reset(); g.tfTitle.Reset(); g.tfHint.Reset(); g.tfStatus.Reset();
    mk(10.0f, 0, &g.tfCap);     // caption: nombre + contador
    mk(11.5f, 1, &g.tfHud);     // HUD: dimensiones y zoom (datos)
    mk(13.5f, 0, &g.tfTitle);   // "Lux" / "No se pudo abrir"
    mk(11.0f, 0, &g.tfHint);    // ayuda / detalle del error
    mk(10.0f, 1, &g.tfStatus);  // barra de estado (datos)
    if (g.tfCap)  {
        g.tfCap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g.tfCap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g.tfCap->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);   // una sola linea (no se parte en dos)
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(g.dw->CreateEllipsisTrimmingSign(g.tfCap.Get(), &sign)))
            g.tfCap->SetTrimming(&trim, sign.Get());            // fallback: … si aun no entra
    }
    if (g.tfHud)  { g.tfHud->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);  g.tfHud->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfTitle){ g.tfTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);g.tfTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfHint) { g.tfHint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g.tfHint->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (g.tfStatus){ g.tfStatus->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); g.tfStatus->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); } // izquierda (LEADING), centrado vertical
    g.capKeyW = -1;   // forzar recalculo del caption con el nuevo formato
}

static void discardDeviceResources() {
    g.blur.Reset(); g.tone.Reset(); g.shadow[0].Reset(); g.shadow[1].Reset();
    gpuDropAll();
    g.brush.Reset(); g.dc.Reset(); g.rt.Reset();
    g.effectsOk = false;
}

// Matriz de color del ambiente: saturate(s) y despues brightness(b), como el filtro CSS de
// Lumen (misma matriz de la especificacion de Filter Effects, sobre valores sRGB).
static D2D1_MATRIX_5X4_F ambientMatrix(float s, float b) {
    return D2D1::Matrix5x4F(
        (0.213f + 0.787f * s) * b, (0.213f - 0.213f * s) * b, (0.213f - 0.213f * s) * b, 0.f,
        (0.715f - 0.715f * s) * b, (0.715f + 0.285f * s) * b, (0.715f - 0.715f * s) * b, 0.f,
        (0.072f - 0.072f * s) * b, (0.072f - 0.072f * s) * b, (0.072f + 0.928f * s) * b, 0.f,
        0.f, 0.f, 0.f, 1.f,
        0.f, 0.f, 0.f, 0.f);
}

static bool createDeviceResources() {
    if (g.rt) return true;
    RECT rc; GetClientRect(g.hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));

    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
    // OPACO (IGNORE): la ventana no es translucida y ClearType solo existe sobre un
    // target sin alfa; con PREMULTIPLIED Direct2D cae a antialiasing gris.
    rtp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(g.hwnd, size,
        D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(g.d2d->CreateHwndRenderTarget(rtp, hp, &g.rt))) return false;
    g.rt->SetDpi(96.f, 96.f); // trabajamos en pixeles fisicos; escalamos UI a mano
    g.maxBmp = g.rt->GetMaximumBitmapSize();

    g.rt.As(&g.dc); // QI -> device context (Win8+) para cubic + efectos
    if (g.dc) {
        g.effectsOk =
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1GaussianBlur, &g.blur)) &&
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1ColorMatrix, &g.tone)) &&
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1Shadow, &g.shadow[0])) &&
            SUCCEEDED(g.dc->CreateEffect(CLSID_D2D1Shadow, &g.shadow[1]));
        if (g.effectsOk) {
            // ambiente: desenfoque -> saturacion y brillo (una cadena de efectos, un solo DrawImage)
            g.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            g.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
            g.tone->SetInputEffect(0, g.blur.Get());
            g.tone->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, ambientMatrix(AMBIENT_SATURATE, AMBIENT_BRIGHTNESS));
            g.tone->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
            for (int i = 0; i < 2; ++i) {
                g.shadow[i]->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, kShadow[i].alpha));
                g.shadow[i]->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_SPEED);
            }
        }
    }
    g.rt->CreateSolidColorBrush(col::fg, &g.brush);
    createTextFormats();
    applyTextRendering();
    // la imagen que ya estaba se vuelve a subir sola desde la CPU (sin decodificar de nuevo);
    // la grilla de mosaicos depende del maximo de bitmap de ESTA GPU
    if (g.cur) gpuReset();
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
    if (!hasImage()) return 0;
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
        // trazo de ancho entero y centro clavado al pixel: lineas y marcos nitidos
        float sw = strokePx();
        float cx = snapStroke((r.left + r.right) * 0.5f, sw), cy = snapStroke(h * 0.5f, sw);
        float s = snapPx(dpf(3.7f));
        if (i == 3) {            // toggle "ventana pegada a la imagen": marco con imagen adentro
            bool on = g.cfg.fitWindow;
            D2D1_COLOR_F tc = on ? col::accent : (hot ? col::fg : col::fgDim);
            float bw = snapPx(dpf(5.0f)), bh = snapPx(dpf(3.9f));
            D2D1_RECT_F frame = D2D1::RectF(cx - bw, cy - bh, cx + bw, cy + bh);
            setBrush(tc, a);
            g.rt->DrawRectangle(frame, g.brush.Get(), sw);
            float pad = snapPx(dpf(1.8f)) + (((int)sw & 1) ? 0.5f : 0.f);   // relleno con bordes en pixel entero
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
                float o = snapPx(dpf(2));
                D2D1_RECT_F a1 = D2D1::RectF(cx - s + o, cy - s, cx + s, cy + s - o);
                D2D1_RECT_F a2 = D2D1::RectF(cx - s, cy - s + o, cx + s - o, cy + s);
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

// Layout GDI-compatible: metricas y avances redondeados a pixel entero, y al
// dibujarlo Direct2D usa el modo de render GDI_CLASSIC (hinting completo, sin
// posicionamiento subpixel). Es lo que separa un texto chico nitido de uno gris.
// Se mide y se dibuja con el MISMO layout para que nunca difieran.
static ComPtr<IDWriteTextLayout> makeLayout(IDWriteTextFormat* fmt, const std::wstring& s, float maxW, float maxH) {
    ComPtr<IDWriteTextLayout> tl;
    if (!g.dw || !fmt) return tl;
    g.dw->CreateGdiCompatibleTextLayout(s.c_str(), (UINT32)s.size(), fmt, maxW, maxH,
                                        1.f /*px por dip: el target corre a 96 dpi*/, nullptr, FALSE, &tl);
    return tl;
}
// Mide el ancho (px) de un texto con un formato dado.
static float measureW(IDWriteTextFormat* fmt, const std::wstring& s) {
    if (s.empty()) return 0.f;
    ComPtr<IDWriteTextLayout> tl = makeLayout(fmt, s, 100000.f, 100.f);
    if (!tl) return 0.f;
    DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}
static float measureCapW(const std::wstring& s) { return measureW(g.tfCap.Get(), s); }

// Dibuja `s` dentro de `r` con la alineacion del formato, corriendo el origen para
// que la primera linea arranque en un pixel entero (x e y). Un origen fraccional
// desplaza todos los glifos medio pixel y los vuelve borrosos aunque el hinting
// sea perfecto; con la alineacion centrada eso pasa una de cada dos veces.
static void drawTextIn(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       D2D1_DRAW_TEXT_OPTIONS opts = D2D1_DRAW_TEXT_OPTIONS_NONE) {
    if (s.empty() || !fmt || !g.rt) return;
    float w = std::max(1.f, snapPx(r.right - r.left)), h = std::max(1.f, snapPx(r.bottom - r.top));
    ComPtr<IDWriteTextLayout> tl = makeLayout(fmt, s, w, h);
    if (!tl) return;
    DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m);
    float x = snapPx(r.left + m.left) - m.left;
    float y = snapPx(r.top  + m.top)  - m.top;
    g.rt->DrawTextLayout(D2D1::Point2F(x, y), tl.Get(), g.brush.Get(), opts);
}

// Peso de archivo humanizado (B / KB / MB / GB).
static std::wstring humanSize(UINT64 b) {
    wchar_t buf[32];
    if      (b >= 1024ull*1024*1024) swprintf(buf, 32, L"%.1f GB", (double)b / (1024.0*1024*1024));
    else if (b >= 1024ull*1024)      swprintf(buf, 32, L"%.1f MB", (double)b / (1024.0*1024));
    else if (b >= 1024)              swprintf(buf, 32, L"%.0f KB", (double)b / 1024.0);
    else                             swprintf(buf, 32, L"%llu B", (unsigned long long)b);
    return buf;
}
// Tamaño + fecha de modificación del archivo actual, cacheados por ruta (evita stat por frame).
struct FileMeta { UINT64 size = 0; std::wstring mdate; };
static const FileMeta& fileMetaCached(const std::wstring& path) {
    static std::wstring cachedPath; static FileMeta fm;
    if (path != cachedPath) {
        cachedPath = path; fm = FileMeta{};
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!path.empty() && GetFileAttributesExW(fsPath(path).c_str(), GetFileExInfoStandard, &fad)) {
            fm.size = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            FILETIME lt; SYSTEMTIME st;
            if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &lt) && FileTimeToSystemTime(&lt, &st)) {
                wchar_t b[16]; swprintf(b, 16, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
                fm.mdate = b;
            }
        }
    }
    return fm;
}

// Relación de aspecto: "16:9", "4:3"… reduciendo por MCD; decimal si no reduce lindo.
static std::wstring aspectRatio(UINT w, UINT h) {
    if (!w || !h) return L"";
    UINT a = w, b = h; while (b) { UINT t = a % b; a = b; b = t; }   // gcd
    UINT rw = w / a, rh = h / a;
    if (rw <= 64 && rh <= 64) return std::to_wstring(rw) + L":" + std::to_wstring(rh);
    wchar_t buf[24]; swprintf(buf, 24, L"%.2f:1", (double)w / (double)h);
    return buf;
}

// Arma "nombre   ·   idx / total" truncando SOLO el nombre con … si no entra en la barra
// (el contador queda siempre visible). El ancho disponible es simetrico para centrar en la ventana.
static std::wstring buildCaption(LONG winW) {
    std::wstring name = baseName(g.imgPath);
    std::wstring tail = L"   ·   " + std::to_wstring(g.idx + 1) + L" / " + std::to_wstring(g.shownTotal);
    if (!g.tfCap) return name + tail;
    float btnsW = (float)dp(BTNW_L) * 4.f;                 // 4 botones a la derecha
    float avail = (float)winW - 2.f * (btnsW + dpf(14.f)); // margen simetrico (no choca botones)
    if (avail < dpf(140.f)) avail = (float)winW * 0.6f;    // ventana muy angosta
    float nameAvail = avail - measureCapW(tail);
    if (nameAvail <= dpf(26.f) || measureCapW(name) <= nameAvail) return name + tail;
    const std::wstring ell = L"…";                    // …
    size_t lo = 0, hi = name.size();                       // mayor prefijo del nombre que entra
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (measureCapW(name.substr(0, mid) + ell) <= nameAvail) lo = mid; else hi = mid - 1;
    }
    return name.substr(0, lo) + ell + tail;
}

static void drawTitleBar() {
    if (g.fullscreen) return;
    float a = g.chrome;
    float h = (float)dp(TBH_L);
    RECT rc; GetClientRect(g.hwnd, &rc);
    // fondo barra
    setBrush(col::bg, 1.f);
    g.rt->FillRectangle(D2D1::RectF(0, 0, (float)rc.right, h), g.brush.Get());

    // caption centrado: nombre  ·  idx / total  (nombre truncado con … si es largo)
    if (hasImage() && a > 0.01f && g.tfCap) {
        if (g.imgPath != g.capKeyPath || g.idx != g.capKeyIdx ||
            g.shownTotal != g.capKeyTotal || rc.right != g.capKeyW || g.dpi != g.capKeyDpi) {
            g.capKeyPath = g.imgPath; g.capKeyIdx = g.idx; g.capKeyTotal = g.shownTotal;
            g.capKeyW = rc.right; g.capKeyDpi = g.dpi;
            g.capText = buildCaption(rc.right);
        }
        setBrush(col::fgDim, a);
        drawTextIn(g.capText, g.tfCap.Get(), D2D1::RectF(0, 0, (float)rc.right, h), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
    if (!hasImage() || g.fullscreen) return;
    float a = g.hudAlpha * 0.9f;   // opacidad propia del HUD + 10% mas transparente
    if (a <= 0.01f || !g.tfHud) return;
    wchar_t buf[128];
    int zoomPct = (int)lround(g.scale * 100.0);
    swprintf(buf, 128, L"%u × %u    ·    %d%%", g.imgW, g.imgH, zoomPct);
    std::wstring s = buf;

    // medir (mismo layout GDI-compatible con el que se dibuja)
    float tw = measureW(g.tfHud.Get(), s);
    float padX = dpf(14);
    float w = snapPx((tw > 0.f ? tw : dpf(120)) + padX * 2);
    float hh = snapPx(dpf(24));
    RECT rc; GetClientRect(g.hwnd, &rc);
    float cx = rc.right * 0.5f;
    float y2 = snapPx(rc.bottom - (float)dp(SBH_L) - dpf(14));   // por encima de la barra de estado
    float px0 = snapPx(cx - w * 0.5f);
    D2D1_RECT_F pill = D2D1::RectF(px0, y2 - hh, px0 + w, y2);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pill, hh*0.5f, hh*0.5f);

    setBrush(D2D1::ColorF(0x0B0E14), 0.72f * a);
    g.rt->FillRoundedRectangle(rr, g.brush.Get());
    setBrush(D2D1::ColorF(1,1,1,1), 0.06f * a);
    g.rt->DrawRoundedRectangle(rr, g.brush.Get(), dpf(1));

    setBrush(col::fgDim, a);
    drawTextIn(s, g.tfHud.Get(), pill);
}

// Barra de estado inferior: detalles de la imagen. Fina como la de titulo y con
// el mismo fondo; se desvanece con el cromo. Izquierda: FORMATO · WxH · MP · peso.
// Derecha: zoom %.
static void drawStatusBar() {
    if (g.fullscreen || !hasImage() || !g.tfStatus) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    float h   = (float)dp(SBH_L);
    float top = (float)rc.bottom - h;
    float a   = g.chrome;

    // fondo (igual que la barra de titulo) + hairline superior sutil
    setBrush(col::bg, 1.f);
    g.rt->FillRectangle(D2D1::RectF(0, top, (float)rc.right, (float)rc.bottom), g.brush.Get());
    setBrush(D2D1::ColorF(1, 1, 1, 1), 0.05f * a);
    g.rt->DrawLine(D2D1::Point2F(0, top + 0.5f),               // hairline de 1 px exacto
                   D2D1::Point2F((float)rc.right, top + 0.5f), g.brush.Get(), 1.f);
    if (a <= 0.01f) return;

    float padX = dpf(14.f);

    // derecha: zoom % (se mide primero para reservar su ancho y no pisar la izquierda)
    wchar_t zb[24]; swprintf(zb, 24, L"%d %%", (int)lround(g.scale * 100.0));
    std::wstring zoom = zb;
    float zw = measureW(g.tfStatus.Get(), zoom);
    setBrush(col::fgFaint, a);
    D2D1_RECT_F zr = D2D1::RectF((float)rc.right - padX - zw, top, (float)rc.right - padX, (float)rc.bottom);
    drawTextIn(zoom, g.tfStatus.Get(), zr, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    // izquierda: cadena de datos de la imagen, unidos por " · " (los vacios se omiten)
    const std::wstring sep = L"   ·   ";
    std::wstring fmt = g.fmtLabel;
    if (fmt.empty()) fmt = upper(extOf(g.imgPath));
    const FileMeta& fm = fileMetaCached(g.imgPath);
    UINT64 ramBytes = (UINT64)g.imgW * (UINT64)g.imgH * 4ull;   // descomprimido a RGBA8

    std::vector<std::wstring> seg;
    seg.push_back(fmt);                                                           // formato
    seg.push_back(std::to_wstring(g.imgW) + L" × " + std::to_wstring(g.imgH));     // dimensiones
    { double mp = (double)g.imgW * (double)g.imgH / 1e6; wchar_t b[24];
      swprintf(b, 24, mp >= 1.0 ? L"%.1f MP" : L"%.2f MP", mp); seg.push_back(b); }   // megapixeles
    { std::wstring ar = aspectRatio(g.imgW, g.imgH); if (!ar.empty()) seg.push_back(ar); }  // relacion de aspecto
    if (g.imgBpp) { std::wstring d = std::to_wstring(g.imgBpp) + L"-bit"; if (g.imgAlpha) d += L" alfa"; seg.push_back(d); } // profundidad + alfa
    if (g.imgHdr) seg.push_back(L"HDR");                                         // rango alto comprimido
    if (fm.size)  seg.push_back(humanSize(fm.size));                              // peso en disco
    seg.push_back(humanSize(ramBytes) + L" en RAM");                             // tamaño descomprimido
    if (fm.size) { double r = (double)ramBytes / (double)fm.size;
      if (r >= 1.5) { wchar_t b[24]; swprintf(b, 24, L"%.0f:1", r); seg.push_back(b); } }   // ratio de compresion
    if (!fm.mdate.empty()) seg.push_back(fm.mdate);                              // fecha de modificacion

    std::wstring left;
    for (size_t i = 0; i < seg.size(); ++i) { if (i) left += sep; left += seg[i]; }

    setBrush(col::fgDim, a);
    D2D1_RECT_F lr = D2D1::RectF(padX, top, (float)rc.right - padX - zw - dpf(18.f), (float)rc.bottom);
    drawTextIn(left, g.tfStatus.Get(), lr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

// Pantalla sin imagen: vacia, cargando la primera o con el error.
static void drawEmpty() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float cx = rc.right * 0.5f, cy = rc.bottom * 0.5f;
    drawSpark(cx, cy - dpf(34), dpf(46));
    const bool err = g.view == View::Error && !g.loadError.empty();
    const bool loading = g.view == View::Loading && g.target >= 0 && g.target < (int)g.files.size();
    if (g.tfTitle) {
        setBrush(err ? col::danger : col::fgDim, err ? 0.9f : 1.f);
        D2D1_RECT_F tr = D2D1::RectF(0, cy + dpf(16), (float)rc.right, cy + dpf(40));
        drawTextIn(err ? L"No se pudo abrir" : loading ? L"Cargando" : L"Lux", g.tfTitle.Get(), tr);
    }
    if (g.tfHint) {
        setBrush(col::fgFaint, 1.f);
        D2D1_RECT_F tr = D2D1::RectF(dpf(20), cy + dpf(44), (float)rc.right - dpf(20), cy + dpf(112));
        std::wstring t = err ? g.loadError
                       : loading ? baseName(g.files[g.target])
                       : std::wstring(L"Ctrl+O para abrir   ·   o arrastrá una imagen");
        drawTextIn(t, g.tfHint.Get(), tr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

// Linea de carga: 2 px de acento pegados al borde de arriba del escenario, solo si el pedido
// tarda mas de LOADBAR_DELAY_MS (lo que llega rapido no parpadea). Con progreso conocido (WIC
// en tiras) avanza de verdad, suavizado; sin el, un tramo que va y viene.
static void drawLoadBar() {
    if (g.target < 0 || g.target >= (int)g.files.size()) return;
    const DWORD el = GetTickCount() - g.requestedAt;
    if (el < LOADBAR_DELAY_MS) return;
    const float fade = std::min(1.f, (el - LOADBAR_DELAY_MS) / 220.f);
    const D2D1_RECT_F st = stageRect();
    const float w = st.right - st.left, h = std::max(2.f, snapPx(dpf(2.f)));
    setBrush(col::accent, 0.10f * fade);
    g.rt->FillRectangle(D2D1::RectF(st.left, st.top, st.right, st.top + h), g.brush.Get());
    const uint32_t p = g_loader.progressOf(g.files[g.target]);
    setBrush(col::accent, 0.85f * fade);
    if (p != PROGRESS_UNKNOWN) {
        const float f = std::min(1.f, (float)p / PROGRESS_ONE);
        g.loadShown += (std::max(g.loadShown, f) - g.loadShown) * 0.35f;
        g.rt->FillRectangle(D2D1::RectF(st.left, st.top, st.left + snapPx(w * g.loadShown), st.top + h), g.brush.Get());
    } else {
        const float seg = w * 0.22f;
        const float t = (float)fmod(el / 1100.0, 1.0);
        const float x = st.left - seg + (w + seg) * (0.5f - 0.5f * cosf(t * 6.2831853f));
        g.rt->FillRectangle(D2D1::RectF(std::max(st.left, x), st.top, std::min(st.right, x + seg), st.top + h), g.brush.Get());
    }
}

// Ambiente: la imagen en modo "cover", desenfocada y apagada, detras de todo. Se calcula sobre
// el nivel de efectos (<= 512 px): el mismo resultado que desenfocar la foto entera a una
// fraccion del costo, y el radio queda fijo en px de pantalla.
static void drawAmbient(const D2D1_RECT_F& stage) {
    if (!g.effectsOk || !g.cur) return;
    const int e = g.cur->effectLevel();
    ID2D1Bitmap* bmp = gpuTile(e, 0, 0, true);
    if (!bmp) return;
    const float sw = stage.right - stage.left, sh = stage.bottom - stage.top;
    const double cover = std::max(sw / (double)g.imgW, sh / (double)g.imgH) * AMBIENT_COVER;
    const double sEff = cover * std::ldexp(1.0, e);                 // px de pantalla por px del nivel
    const float x = stage.left + (float)(sw - g.imgW * cover) * 0.5f;
    const float y = stage.top + (float)(sh - g.imgH * cover) * 0.5f;
    g.blur->SetInput(0, bmp);
    g.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::clamp(dpf(AMBIENT_BLUR_DIP) / (float)sEff, 0.5f, 250.f));
    g.dc->SetTransform(D2D1::Matrix3x2F::Scale((float)sEff, (float)sEff) * D2D1::Matrix3x2F::Translation(x, y));
    g.dc->DrawImage(g.tone.Get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR);
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// Sombra en dos capas (difusa + de contacto), con el "spread" negativo de CSS emulado achicando
// la silueta hacia su centro. Tambien sale del nivel de efectos: sigue la forma de la imagen
// (un PNG recortado proyecta su silueta, no un rectangulo).
static void drawShadow(const D2D1_RECT_F& dst, const D2D1_RECT_F& stage) {
    if (!g.effectsOk || !g.cur) return;
    if (dst.left <= stage.left && dst.top <= stage.top && dst.right >= stage.right && dst.bottom >= stage.bottom)
        return;                                                     // la imagen tapa todo: no se ve
    const int e = g.cur->effectLevel();
    ID2D1Bitmap* bmp = gpuTile(e, 0, 0, true);
    if (!bmp) return;
    const double sEff = g.scale * std::ldexp(1.0, e);
    const float iw = dst.right - dst.left, ih = dst.bottom - dst.top;
    for (int i = 0; i < 2; ++i) {
        const ShadowLayer& L = kShadow[i];
        const float sp = std::min(dpf(L.spread), 0.25f * std::min(iw, ih));
        const float fx = (iw - 2 * sp) / iw, fy = (ih - 2 * sp) / ih;
        if (fx <= 0.f || fy <= 0.f) continue;
        const float sx = (float)sEff * fx, sy = (float)sEff * fy;
        g.shadow[i]->SetInput(0, bmp);
        g.shadow[i]->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                              std::clamp(dpf(L.sigma) / std::min(sx, sy), 0.5f, 250.f));
        g.dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) *
                           D2D1::Matrix3x2F::Translation(dst.left + sp, dst.top + sp + dpf(L.dy)));
        g.dc->DrawImage(g.shadow[i].Get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR);
    }
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// Un mosaico del nivel k, recortado a su celda (tiles.h: sin costuras). s = px de pantalla por
// px del nivel.
static void drawTile(const GpuLevel& gl, int tx, int ty, double s, ID2D1Bitmap* bmp,
                     D2D1_INTERPOLATION_MODE im, const D2D1_RECT_F& stage) {
    const lux::TileGrid& G = gl.grid;
    const lux::TileRect r = lux::tileRect(G, tx, ty);
    const bool single = G.cols == 1 && G.rows == 1;
    const D2D1_RECT_F dst = D2D1::RectF((float)(g.ox + r.bx0 * s), (float)(g.oy + r.by0 * s),
                                        (float)(g.ox + r.bx1 * s), (float)(g.oy + r.by1 * s));
    if (!single) {
        const lux::Cut cx = lux::cellCut(g.ox, s, r.x0, r.x1, tx == 0, tx + 1 == G.cols);
        const lux::Cut cy = lux::cellCut(g.oy, s, r.y0, r.y1, ty == 0, ty + 1 == G.rows);
        const D2D1_RECT_F c = D2D1::RectF((float)std::max<double>(cx.lo, stage.left), (float)std::max<double>(cy.lo, stage.top),
                                          (float)std::min<double>(cx.hi, stage.right), (float)std::min<double>(cy.hi, stage.bottom));
        if (c.right <= c.left || c.bottom <= c.top) return;
        g.rt->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_ALIASED);
    }
    if (g.dc) g.dc->DrawBitmap(bmp, &dst, 1.f, im);
    else      g.rt->DrawBitmap(bmp, dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (!single) g.rt->PopAxisAlignedClip();
}

// Mientras un mosaico no llego a la GPU se dibuja su celda con el nivel mas fino que ya este
// completo para esa zona (el mas grueso siempre esta): un cuadro apenas mas blando, nunca un
// hueco ni una espera.
static void drawFallback(int k, int tx, int ty, const D2D1_RECT_F& stage) {
    const GpuLevel& gl = g.gpu[k];
    const lux::TileRect r = lux::tileRect(gl.grid, tx, ty);
    const double s = g.scale * std::ldexp(1.0, k);
    const lux::Cut cx = lux::cellCut(g.ox, s, r.x0, r.x1, tx == 0, tx + 1 == gl.grid.cols);
    const lux::Cut cy = lux::cellCut(g.oy, s, r.y0, r.y1, ty == 0, ty + 1 == gl.grid.rows);
    const D2D1_RECT_F c = D2D1::RectF((float)std::max<double>(cx.lo, stage.left), (float)std::max<double>(cy.lo, stage.top),
                                      (float)std::min<double>(cx.hi, stage.right), (float)std::min<double>(cy.hi, stage.bottom));
    if (c.right <= c.left || c.bottom <= c.top) return;
    g.rt->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int j = k + 1; j < (int)g.gpu.size(); ++j) {
        const GpuLevel& fl = g.gpu[j];
        if (!fl.src) continue;
        const int sh = j - k;
        const uint32_t x0 = r.x0 >> sh, x1 = std::min(fl.src->w, (r.x1 + (1u << sh) - 1) >> sh);
        const uint32_t y0 = r.y0 >> sh, y1 = std::min(fl.src->h, (r.y1 + (1u << sh) - 1) >> sh);
        const int c0 = (int)(x0 / fl.grid.content), c1 = (int)((std::max(x1, x0 + 1) - 1) / fl.grid.content);
        const int r0 = (int)(y0 / fl.grid.content), r1 = (int)((std::max(y1, y0 + 1) - 1) / fl.grid.content);
        bool ready = true;
        for (int yy = r0; yy <= r1 && ready; ++yy)
            for (int xx = c0; xx <= c1 && ready; ++xx)
                ready = fl.tiles[(size_t)yy * fl.grid.cols + xx].bmp != nullptr;
        if (!ready) continue;
        const double sj = g.scale * std::ldexp(1.0, j);
        for (int yy = r0; yy <= r1; ++yy)
            for (int xx = c0; xx <= c1; ++xx)
                drawTile(fl, xx, yy, sj, gpuTile(j, xx, yy, false), D2D1_INTERPOLATION_MODE_LINEAR, stage);
        break;
    }
    g.rt->PopAxisAlignedClip();
}

// La imagen: el nivel justo para el zoom, los mosaicos visibles del centro hacia afuera, con
// presupuesto de subida por cuadro. Lo que no llego se completa con un nivel mas grueso y se
// pide otro cuadro.
static void drawImage(const D2D1_RECT_F& stage, const D2D1_RECT_F& dst) {
    const CpuImage& img = *g.cur;
    const int k = lux::chooseLevel(g.scale, img.first, img.last());
    if (k < 0 || k >= (int)g.gpu.size() || !g.gpu[k].src) return;
    const GpuLevel& gl = g.gpu[k];
    const lux::TileGrid& G = gl.grid;
    const double s = g.scale * std::ldexp(1.0, k);
    const lux::Span sx = lux::visibleSpan(g.ox, s, stage.left, stage.right, G.content, G.cols);
    const lux::Span sy = lux::visibleSpan(g.oy, s, stage.top, stage.bottom, G.content, G.rows);

    struct Vis { int tx, ty; double d; };
    std::vector<Vis> vis;
    vis.reserve((size_t)std::max(0, sx.b - sx.a) * std::max(0, sy.b - sy.a));
    const double cxl = ((stage.left + stage.right) * 0.5 - g.ox) / s;   // centro de la vista en px del nivel
    const double cyl = ((stage.top + stage.bottom) * 0.5 - g.oy) / s;
    for (int ty = sy.a; ty < sy.b; ++ty)
        for (int tx = sx.a; tx < sx.b; ++tx) {
            const lux::TileRect r = lux::tileRect(G, tx, ty);
            const double dx = (r.x0 + r.x1) * 0.5 - cxl, dy = (r.y0 + r.y1) * 0.5 - cyl;
            vis.push_back({ tx, ty, dx * dx + dy * dy });
        }
    std::sort(vis.begin(), vis.end(), [](const Vis& a, const Vis& b) { return a.d < b.d; });

    // subir lo que falta, del centro hacia afuera, hasta agotar el presupuesto del cuadro
    const double budget = g.uploadBoost ? UPLOAD_BUDGET_FIRST_MS : UPLOAD_BUDGET_MS;
    const double t0 = nowMs();
    bool uploaded = false, missing = false;
    for (const Vis& v : vis) {
        if (gl.tiles[(size_t)v.ty * G.cols + v.tx].bmp) continue;
        if (uploaded && nowMs() - t0 > budget) { missing = true; continue; }
        gpuTile(k, v.tx, v.ty, true);
        uploaded = true;
    }
    g.uploadBoost = false;

    // a 300 % o mas, vecino mas cercano: se ven los pixeles (arte de pixel, capturas)
    const D2D1_INTERPOLATION_MODE im = (k == 0 && g.scale >= 3.0f) ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                                  : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    // el ultimo pixel de un nivel grueso cubre mas alla del borde real si el lado no es multiplo
    // de 2^k: recorte al borde verdadero, con antialiasing como el de siempre
    const bool partial = k > 0 && ((img.w & ((1u << k) - 1)) || (img.h & ((1u << k) - 1)));
    if (partial) g.rt->PushAxisAlignedClip(dst, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const Vis& v : vis) {
        if (ID2D1Bitmap* bmp = gpuTile(k, v.tx, v.ty, false)) drawTile(gl, v.tx, v.ty, s, bmp, im, stage);
        else { drawFallback(k, v.tx, v.ty, stage); missing = true; }
    }
    if (partial) g.rt->PopAxisAlignedClip();
    if (missing) invalidate();                        // el cuadro que viene sigue subiendo
}

static void render() {
    if (!createDeviceResources()) return;
    g.rt->BeginDraw();
    g.rt->Clear(col::bg);

    if (hasImage() && g.imgW && g.imgH) {
        gpuEnsureBase();
        D2D1_RECT_F dst = D2D1::RectF(g.ox, g.oy, g.ox + g.imgW * g.scale, g.oy + g.imgH * g.scale);
        // recortar al escenario (no pisar la barra)
        D2D1_RECT_F s = stageRect();
        g.rt->PushAxisAlignedClip(s, D2D1_ANTIALIAS_MODE_ALIASED);
        drawAmbient(s);
        drawShadow(dst, s);
        drawImage(s, dst);
        g.rt->PopAxisAlignedClip();

        // chevrons de navegacion (solo si hay vecinas y hover/cromo visible)
        if (g.files.size() > 1 && g.chrome > 0.01f) {
            float cy = (s.top + s.bottom) * 0.5f;
            float baseA = 0.42f * g.chrome;
            if (g.hoverEdge == -1) drawChevron(s.left + dpf(28), cy, -1, g.chrome);
            else                   drawChevron(s.left + dpf(28), cy, -1, baseA);
            if (g.hoverEdge == +1) drawChevron(s.right - dpf(28), cy, +1, g.chrome);
            else                   drawChevron(s.right - dpf(28), cy, +1, baseA);
        }
        drawHud();
    } else {
        drawEmpty();
    }

    drawStatusBar();
    drawTitleBar();
    drawLoadBar();

    HRESULT hr = g.rt->EndDraw();
    ++g.frame;
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
        invalidate();
    } else {
        gpuTrim();
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
        GetMonitorInfo(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);   // el monitor de la ventana, no el primario
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
    std::vector<wchar_t> file(32768, L'\0');       // rutas largas: el tope de OPENFILENAME es 32 K
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter =
        L"Imágenes\0*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxl;*.svg;*.svgz;*.qoi;*.exr;"
        L"*.ico;*.cur;*.ani;*.icns;*.tga;*.hdr;*.dds;*.jxr;*.ppm;*.pgm;*.pbm;*.pnm;*.pam;*.psd;*.pcx;*.pfm;*.ras;*.sgi;*.rgb;*.bw;*.wbmp;"
        L"*.xbm;*.xpm;*.xwd;*.ff;*.iff;*.ilbm;*.lbm;*.mac;*.pntg;*.dpx;*.cin;*.ora;*.kra;*.sketch;*.procreate;"
        L"*.emf;*.wmf;*.emz;*.wmz;*.vtf;*.ktx;*.fits;*.fit;*.dcm;*.pcd;*.tim;*.pix;*.dcx;*.img;*.neo;*.scr;*.koa;*.pi1;*.pc1;"
        L"*.dng;*.cr2;*.cr3;*.nef;*.arw;*.orf;*.rw2;*.raf;*.srw;*.pef;*.3fr;*.iiq;*.x3f;*.mrw;*.kdc;*.erf\0"
        L"Fotos y RAW\0*.jpg;*.jpeg;*.png;*.heic;*.avif;*.tif;*.tiff;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.orf;*.rw2;*.raf;*.srw;*.pef\0"
        L"Todos\0*.*\0\0";
    ofn.lpstrFile = file.data(); ofn.nMaxFile = (DWORD)file.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) openPath(file.data());
}
static void openPath(const std::wstring& path) {
    const int i = buildFolderList(path);
    g.target = -1;
    g.skips = 0;
    requestIndex(i, +1, true);
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
    if (hasImage() && !g.panning && (GetTickCount() - g.lastMove) > 1600) {
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

// HUD (resolucion/zoom): se desvanece a los 3 s del ultimo cambio de zoom.
static void hudTick() {
    bool changed = false;
    if (g.hudTarget > 0.f && (GetTickCount() - g.hudShownAt) > 3000) g.hudTarget = 0.f;
    float d = g.hudTarget - g.hudAlpha;
    if (fabs(d) > 0.004f) { g.hudAlpha += d * 0.12f; changed = true; }
    else if (g.hudAlpha != g.hudTarget) { g.hudAlpha = g.hudTarget; changed = true; }
    if (changed) invalidate();
    if (g.hudAlpha == g.hudTarget && g.hudTarget == 0.f) KillTimer(g.hwnd, IDT_HUD);
}

// Linea de carga: repinta mientras haya un pedido pendiente (y despues de la demora).
static void loadTick() {
    if (g.target < 0) { KillTimer(g.hwnd, IDT_LOAD); invalidate(); return; }
    if (GetTickCount() - g.requestedAt >= LOADBAR_DELAY_MS) invalidate();
}

// ---------------------------------------------------------------------------
//  Build de medicion (build-bench.ps1 -Defines LUX_BENCH_BUILD). Mismo protocolo que la copia
//  vieja: LUX_BENCH=<log> escribe una linea con cuadros por segundo y tiempos de apertura; con
//  LUX_BENCH_NAV=1 mide tambien la navegacion: 10 pasos en frio (el siguiente apenas se mostro
//  el anterior) y 5 en caliente (con la precarga terminada).
// ---------------------------------------------------------------------------
#ifdef LUX_BENCH_BUILD
static void benchOnPresent() {
    if (!B.on) return;
    const double t = nowMs();
    switch (B.phase) {
    case 0: B.openMs = t - B.t0; B.phase = B.nav ? 1 : 3; B.wsOpen = workingSetMB(false); B.openLevel = g.cur ? g.cur->first : -1; break;
    case 1: B.navCold += t - B.navT; if (++B.navN >= 10) B.phase = 2; break;
    case 2: B.navWarm += t - B.navT; ++B.warmN; break;
    default: break;
    }
    B.waiting = false;
    SetTimer(g.hwnd, IDT_BENCH, 1, nullptr);
}
static void benchTick() {
    if (!B.on || B.waiting) return;
    switch (B.phase) {
    case 1: B.waiting = true; B.navT = nowMs(); navigate(+1); return;
    case 2:
        if (g.target >= 0 || g_loader.busy()) return;           // esperar la precarga
        if (B.warmN >= 5) { B.phase = 3; return; }
        B.waiting = true; B.navT = nowMs(); navigate(+1); return;
    case 3: KillTimer(g.hwnd, IDT_BENCH); B.frames = 0; invalidate(); return;
    default: return;
    }
}
static void benchAfterFrame() {
    if (!B.on || B.phase != 3 || !hasImage()) return;
    if (B.frames == 0) B.f0 = nowMs();
    B.frames++;
    zoomCenter(((B.frames / 30) % 2 == 0) ? 1.02f : 1.f / 1.02f);
    if (B.frames >= 240) {
        const double el = nowMs() - B.f0;
        PROCESS_MEMORY_COUNTERS pmc{ sizeof(pmc) };
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        fprintf(B.f, "fps=%.1f frame_ms=%.2f decode_ms=%.1f nav_ms=%.1f nav_warm_ms=%.1f maxbmp=%u img=%ux%u loaded=%d "
                     "open_level=%d first_level=%d cpu_mb=%.0f cache_mb=%.0f gpu_mb=%.0f ws_open_mb=%.0f ws_upgrade_mb=%.0f "
                     "ws_end_mb=%.0f peak_ws_mb=%.0f\n",
            240000.0 / el, el / 240, B.openMs, B.navN ? B.navCold / B.navN : 0.0, B.warmN ? B.navWarm / B.warmN : 0.0,
            g.maxBmp, g.imgW, g.imgH, hasImage() ? 1 : 0, B.openLevel, g.cur ? g.cur->first : -1,
            g.cur ? g.cur->bytes() / 1048576.0 : 0.0, g_cache.bytes() / 1048576.0, g.gpuBytes / 1048576.0,
            B.wsOpen, B.wsUpgrade, pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
        fclose(B.f);
        B.on = false;
        PostMessage(g.hwnd, WM_CLOSE, 0, 0);
    }
}
#endif

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
        // borde negro en vez del gris claro por defecto de Win11 (COLORREF 0x00BBGGRR)
        COLORREF border = RGB(0, 0, 0);
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
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
            if (IsZoomed(hwnd)) {   // metricas del DPI de ESTA ventana (en otro monitor difieren)
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, g.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g.dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, g.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g.dpi);
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
        applyTextRendering();
        if (g.fit) fitToWindow();
        invalidate();
        return 0;
    }
    case WM_EXITSIZEMOVE:   // pudo cambiar de monitor (misma DPI, otra geometria de subpixel)
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:  // ClearType prendido/apagado, ajustes del afinador
        applyTextRendering();
        invalidate();
        break;
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
#ifdef LUX_BENCH_BUILD
        benchAfterFrame();
#endif
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    case WM_APP_LOADED:
        onLoaderResults();
        return 0;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        onMouseActivity();
        if (!g.mouseTracked) {   // avisar cuando el mouse salga del area cliente (una vez por entrada)
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            g.mouseTracked = TrackMouseEvent(&tme) != FALSE;
        }
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
    case WM_MOUSELEAVE: {
        // el mouse salio de la ventana (o paso a la barra, que es area no-cliente):
        // sin esto un boton o un chevron quedaban resaltados hasta la proxima entrada
        g.mouseTracked = false;
        if (g.hoverBtn != -1 || g.hoverEdge != 0) { g.hoverBtn = -1; g.hoverEdge = 0; invalidate(); }
        return 0;
    }
    case WM_NCMOUSEMOVE:
        // sobre la barra de titulo (HTCAPTION) no llega WM_MOUSEMOVE: cuenta igual
        // como actividad, si no el cromo se desvanecia con el mouse apoyado ahi
        onMouseActivity();
        break;
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
        bool canPan = hasImage() && (g.imgW*g.scale > (s.right-s.left)+1 || g.imgH*g.scale > (s.bottom-s.top)+1);
        if (canPan && hitButton(x,y) < 0 && (float)y >= s.top && (float)y < s.bottom) {
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
        if (y < dp(TBH_L) && !g.fullscreen) {
            // La barra devuelve HTCAPTION, asi que su doble clic (max/restore) lo
            // resuelve DefWindowProc en WM_NCLBUTTONDBLCLK. Aca solo llega el doble
            // clic sobre un BOTON: el primer clic ya actuo, que el UP de este no repita.
            g.moved = true;
            return 0;
        }
        if (hasImage() && hitEdge(x,y)==0) toggleFitDetail((float)x, (float)y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (hasImage() && pt.y >= dp(TBH_L) && delta) {
            // proporcional al giro: un panel tactil de precision manda muchos deltas chicos y
            // con un paso fijo por mensaje el zoom se disparaba
            float factor = powf(1.18f, (float)delta / WHEEL_DELTA);
            zoomAt(factor, (float)pt.x, (float)pt.y);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (wp) {
        case VK_LEFT: case VK_UP: case VK_PRIOR:      navigate(-1); break;
        case VK_RIGHT: case VK_DOWN: case VK_NEXT: case VK_SPACE: navigate(+1); break;
        case VK_HOME:  if (!g.files.empty()) { g.skips = 0; requestIndex(0, +1, false); } break;
        case VK_END:   if (!g.files.empty()) { g.skips = 0; requestIndex((int)g.files.size() - 1, -1, false); } break;
        case VK_OEM_PLUS: case VK_ADD:   if (hasImage()) zoomCenter(1.25f); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: if (hasImage()) zoomCenter(1.f/1.25f); break;
        case '0':      if (hasImage()) { fitToWindow(); invalidate(); } break;
        case '1':      if (hasImage()) setActualSize(); break;
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
        UINT n = DragQueryFileW(hd, 0, nullptr, 0);     // largo sin el NUL: sin tope de MAX_PATH
        if (n) {
            std::wstring path(n + 1, L'\0');
            if (DragQueryFileW(hd, 0, path.data(), n + 1)) { path.resize(n); openPath(path); }
        }
        DragFinish(hd);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_ANIM) animTick();
        else if (wp == IDT_HUD) hudTick();
        else if (wp == IDT_LOAD) loadTick();
        else if (wp == IDT_DWELL) { KillTimer(hwnd, IDT_DWELL); g.dwellDone = true; schedule(); }
#ifdef LUX_BENCH_BUILD
        else if (wp == IDT_BENCH) benchTick();
#endif
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

    // tablas y ganchos de una sola vez, ANTES de que arranquen los hilos (despues son de solo lectura)
    buildExtTable();
    ex_decode_embedded = [](const unsigned char* d, size_t n, int* w, int* h) -> unsigned char* {
        // los decoders propios delegan en stb las imagenes que vienen dentro de un contenedor
        // (los PNG de un .icns o el JPEG de un DICOM comprimido)
        if (!d || !n || n > INT_MAX) return nullptr;
        int comp = 0;
        return stbi_load_from_memory(d, (int)n, w, h, &comp, 4);   // RGBA; lo liberan con free()
    };
    xcf_inflate = [](unsigned char* dst, int dl, const unsigned char* src, int sl) {
        return stbi_zlib_decode_buffer((char*)dst, dl, (const char*)src, sl);
    };
    ex_build_cineon_lut();
    g_cache.setBudget(cacheBudget());

    // factories independientes del dispositivo
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
        reinterpret_cast<void**>(g.d2d.GetAddressOf()));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(g.dw.GetAddressOf()));

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

    createDeviceResources();

    // dos decodificadores alcanzan para "la pedida + la siguiente"; el trabajo pesado de cada
    // uno (premultiplicar, orientar, piramide) ya se reparte en todos los nucleos
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    g_loader.start(hwnd, cores >= 4 ? 2u : 1u);

#ifdef LUX_BENCH_BUILD
    if (const char* bf = getenv("LUX_BENCH")) { B.f = fopen(bf, "w"); B.on = B.f != nullptr; B.nav = getenv("LUX_BENCH_NAV") != nullptr; }
    B.t0 = nowMs();
#endif

    // abrir el archivo pasado por linea de comandos (ajusta la ventana si el modo esta activo)
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        openPath(argv[1]);
        awaitFirstImage(FIRST_IMAGE_WAIT_MS);
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
    g_loader.shutdown(400);
    CoUninitialize();
    ExitProcess(0);   // sin esperar a un decoder que no se puede cancelar (y sin destructores con hilos vivos)
}

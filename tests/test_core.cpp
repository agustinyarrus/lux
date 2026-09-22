// ============================================================================
//  test_core — el motor de Lux que no toca Windows: pool de hilos, pixeles,
//  piramide, mosaicos, cache LRU y la politica de precarga. Todo contra una
//  referencia escalar escrita de la forma mas obvia posible, con los casos borde
//  que rompen estas cosas (1x1, lados impares, colas de menos de 4 pixeles, alfa
//  en la cola, las 8 orientaciones EXIF, costuras a escalas al azar, hilos a la
//  vez) y un benchmark de 48 MP al final.
//
//    .\run.ps1        compila y corre esto y test_decoders
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../core/pool.h"
#include "../core/pixels.h"
#include "../core/pyramid.h"
#include "../core/tiles.h"
#include "../core/cache.h"
#include "../core/plan.h"

using namespace lux;

// ---------------------------------------------------------------- andamiaje
static int checks = 0, fails = 0;
static const char* g_case = "";

static void ok(bool cond, const char* fmt, ...) {
    checks++;
    if (cond) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("  [FAIL] %-12s ", g_case);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static std::mt19937 rng(20260922);
static uint32_t rnd(uint32_t n) { return n ? (uint32_t)(rng() % n) : 0; }

// Pixeles al azar, con una proporcion dada de opacos (el resto con alfa cualquiera).
static std::vector<uint8_t> randomRgba(size_t count, int opaquePct) {
    std::vector<uint8_t> v(count * 4);
    for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) v[i * 4 + c] = (uint8_t)rnd(256);
        v[i * 4 + 3] = (int)rnd(100) < opaquePct ? 255 : (uint8_t)rnd(256);
    }
    return v;
}

static double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

// ---------------------------------------------------------------- premultiplicar
static void testMul255() {
    g_case = "mul255";
    int bad = 0;
    for (unsigned c = 0; c < 256; ++c)
        for (unsigned a = 0; a < 256; ++a)
            if (mul255(c, a) != (uint8_t)std::lround(c * a / 255.0)) ++bad;
    ok(bad == 0, "%d de 65536 pares (c, a) no redondean como round(c*a/255)", bad);
}

// Referencia: la cuenta en su forma mas directa, un pixel a la vez.
static bool refPremul(std::vector<uint8_t>& v, bool swizzle) {
    bool opaque = true;
    for (size_t i = 0; i < v.size() / 4; ++i) {
        uint8_t* q = &v[i * 4];
        uint8_t r = swizzle ? q[0] : q[2], g = q[1], b = swizzle ? q[2] : q[0], a = q[3];
        if (a != 255) opaque = false;
        q[0] = (uint8_t)std::lround(b * a / 255.0);
        q[1] = (uint8_t)std::lround(g * a / 255.0);
        q[2] = (uint8_t)std::lround(r * a / 255.0);
        q[3] = a;
    }
    return opaque;
}

static void testPremultiply() {
    g_case = "premul";
    const size_t sizes[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 63, 64, 65, 1000, 65536 + 3, 300007 };
    for (int swz = 0; swz < 2; ++swz)
        for (size_t n : sizes)
            for (int pct : { 100, 97, 50, 0 }) {
                std::vector<uint8_t> a = randomRgba(n, pct), b = a;
                bool opA = swz ? rgbaToPbgra(a.data(), n) : bgraToPbgra(a.data(), n);
                bool opB = refPremul(b, swz != 0);
                ok(a == b, "%s n=%zu opacos=%d%%: los pixeles difieren de la referencia", swz ? "rgba" : "bgra", n, pct);
                ok(opA == opB, "%s n=%zu opacos=%d%%: dijo opaca=%d y es %d", swz ? "rgba" : "bgra", n, pct, opA, opB);
            }
    // un unico pixel translucido escondido en la cola (despues del ultimo bloque de 4)
    {
        std::vector<uint8_t> v = randomRgba(4 * 1000 + 3, 100);
        v[(4 * 1000 + 2) * 4 + 3] = 7;
        ok(!rgbaToPbgra(v.data(), 4 * 1000 + 3), "el alfa de la cola no se vio");
    }
}

static void testOpaqueAndGray() {
    g_case = "opaque";
    std::vector<uint8_t> v(4 * 200003, 255);
    ok(allOpaque(v.data(), 200003), "todo 255 tiene que ser opaco");
    ok(allOpaque(v.data(), 0), "vacio cuenta como opaco");
    for (size_t at : { (size_t)0, (size_t)1, (size_t)100000, (size_t)200000, (size_t)200002 }) {
        v[at * 4 + 3] = 254;
        ok(!allOpaque(v.data(), 200003), "alfa 254 en %zu no se detecto", at);
        v[at * 4 + 3] = 255;
    }
    v[5 * 4 + 0] = 0;   // un canal de color en 0 no es transparencia
    ok(allOpaque(v.data(), 200003), "un canal de color en 0 no deberia contar");

    g_case = "bgr/gris";
    const uint32_t w = 5, h = 3;
    const size_t stride = 5 * 3 + 1;                       // fila con relleno, como WIC
    std::vector<uint8_t> bgr(stride * h, 0xEE), gray(8 * h, 0xEE), out(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        uint8_t* p = &bgr[y * stride + x * 3]; p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x + y);
        gray[y * 8 + x] = (uint8_t)(10 * x + y);
    }
    bgr24ToPbgra(bgr.data(), stride, w, h, out.data());
    bool good = true;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* q = &out[(y * w + x) * 4];
        good = good && q[0] == x && q[1] == y && q[2] == x + y && q[3] == 255;
    }
    ok(good, "BGR24 con stride de relleno");
    gray8ToPbgra(gray.data(), 8, w, h, out.data());
    good = true;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* q = &out[(y * w + x) * 4];
        good = good && q[0] == 10 * x + y && q[1] == q[0] && q[2] == q[0] && q[3] == 255;
    }
    ok(good, "gris de 8 bits");
}

// ---------------------------------------------------------------- orientacion
static void refMap(int o, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t& X, uint32_t& Y) {
    switch (o) {
    case 1: X = x;         Y = y;         break;
    case 2: X = w - 1 - x; Y = y;         break;
    case 3: X = w - 1 - x; Y = h - 1 - y; break;
    case 4: X = x;         Y = h - 1 - y; break;
    case 5: X = y;         Y = x;         break;
    case 6: X = h - 1 - y; Y = x;         break;
    case 7: X = h - 1 - y; Y = w - 1 - x; break;
    default: X = y;        Y = w - 1 - x; break;
    }
}

static PixelBuf numbered(uint32_t w, uint32_t h) {
    PixelBuf b = PixelBuf::alloc((size_t)w * h * 4);
    uint32_t* p = (uint32_t*)b.data();
    for (size_t i = 0; i < (size_t)w * h; ++i) p[i] = (uint32_t)i * 2654435761u;   // todos distintos
    return b;
}

static void testOrient() {
    g_case = "orient";
    const uint32_t dims[][2] = { {1,1}, {1,5}, {5,1}, {3,2}, {2,3}, {64,64}, {65,130}, {200,3}, {129,257} };
    for (auto& d : dims) {
        const uint32_t w = d[0], h = d[1];
        for (int o = 1; o <= 8; ++o) {
            PixelBuf src = numbered(w, h);
            std::vector<uint32_t> orig((const uint32_t*)src.data(), (const uint32_t*)src.data() + (size_t)w * h);
            uint32_t ow = 0, oh = 0;
            PixelBuf out = orient(std::move(src), w, h, o, ow, oh);
            const bool swap = o >= 5;
            ok(ow == (swap ? h : w) && oh == (swap ? w : h), "o=%d %ux%u: salio de %ux%u", o, w, h, ow, oh);
            const uint32_t* q = (const uint32_t*)out.data();
            int bad = 0;
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
                uint32_t X, Y; refMap(o, w, h, x, y, X, Y);
                if (q[(size_t)Y * ow + X] != orig[(size_t)y * w + x]) ++bad;
            }
            ok(bad == 0, "o=%d %ux%u: %d pixeles fuera de lugar", o, w, h, bad);
        }
    }
    // 90 horario seguido de 90 antihorario = identidad; 180 dos veces = identidad
    {
        PixelBuf a = numbered(37, 11);
        std::vector<uint32_t> orig((const uint32_t*)a.data(), (const uint32_t*)a.data() + 37 * 11);
        uint32_t w1, h1, w2, h2;
        PixelBuf b = orient(std::move(a), 37, 11, 6, w1, h1);
        PixelBuf c = orient(std::move(b), w1, h1, 8, w2, h2);
        ok(w2 == 37 && h2 == 11 && !memcmp(c.data(), orig.data(), orig.size() * 4), "6 y despues 8 tiene que volver al original");
        PixelBuf d = orient(std::move(c), 37, 11, 3, w1, h1);
        PixelBuf e = orient(std::move(d), 37, 11, 3, w2, h2);
        ok(!memcmp(e.data(), orig.data(), orig.size() * 4), "180 dos veces tiene que volver al original");
    }
    // valores fuera de rango: la imagen queda como esta
    {
        PixelBuf a = numbered(4, 3);
        const uint8_t* before = a.data();
        uint32_t w, h;
        PixelBuf b = orient(std::move(a), 4, 3, 0, w, h);
        PixelBuf c = orient(std::move(b), 4, 3, 9, w, h);
        ok(c.data() == before && w == 4 && h == 3, "orientacion 0 o 9 no deberia tocar nada");
    }
}

// ---------------------------------------------------------------- piramide
static std::vector<uint8_t> refDown(const std::vector<uint8_t>& s, uint32_t sw, uint32_t sh) {
    uint32_t dw = (sw + 1) / 2, dh = (sh + 1) / 2;
    std::vector<uint8_t> d((size_t)dw * dh * 4);
    auto at = [&](uint32_t x, uint32_t y, int c) {
        x = std::min(x, sw - 1); y = std::min(y, sh - 1);            // borde replicado
        return (unsigned)s[((size_t)y * sw + x) * 4 + c];
    };
    for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) for (int c = 0; c < 4; ++c) {
        unsigned sum = at(2 * x, 2 * y, c) + at(2 * x + 1, 2 * y, c) + at(2 * x, 2 * y + 1, c) + at(2 * x + 1, 2 * y + 1, c);
        d[((size_t)y * dw + x) * 4 + c] = (uint8_t)((sum + 2) >> 2);
    }
    return d;
}

static void testDownsample() {
    g_case = "downsample";
    const uint32_t dims[][2] = { {1,1}, {2,2}, {3,3}, {1,9}, {9,1}, {4,4}, {5,4}, {4,5}, {7,7}, {8,3}, {33,17}, {4097,3}, {640,480} };
    for (auto& d : dims) {
        const uint32_t w = d[0], h = d[1];
        std::vector<uint8_t> src = randomRgba((size_t)w * h, 60);
        rgbaToPbgra(src.data(), (size_t)w * h);                       // entrada valida: c <= a
        std::vector<uint8_t> dst((size_t)((w + 1) / 2) * ((h + 1) / 2) * 4);
        downsample2x(src.data(), w, h, dst.data());
        ok(dst == refDown(src, w, h), "%ux%u: difiere de la referencia", w, h);
        bool premulOk = true;
        for (size_t i = 0; i < dst.size(); i += 4) premulOk = premulOk && dst[i] <= dst[i + 3] && dst[i + 1] <= dst[i + 3] && dst[i + 2] <= dst[i + 3];
        ok(premulOk, "%ux%u: quedo un canal mayor que su alfa", w, h);
    }
}

static void testPyramid() {
    g_case = "piramide";
    ok(levelDim(24000, 0) == 24000 && levelDim(24000, 3) == 3000 && levelDim(24001, 3) == 3001, "levelDim redondea para arriba");
    ok(levelDim(1, 10) == 1 && levelDim(0, 2) == 1, "levelDim nunca baja de 1");
    ok(levelCount(256, 256) == 1 && levelCount(257, 1) == 2 && levelCount(1, 1) == 1, "levelCount en los bordes");
    ok(levelCount(24000, 4000) == 8, "24000x4000: %d niveles, esperaba 8", levelCount(24000, 4000));
    ok(levelCount(8000, 6000) == 6, "8000x6000: %d niveles, esperaba 6", levelCount(8000, 6000));

    CpuImage img;
    img.w = 1001; img.h = 333;
    auto l0 = std::make_shared<Level>();
    l0->w = img.w; l0->h = img.h;
    l0->px = PixelBuf::alloc(l0->bytes());
    std::vector<uint8_t> src = randomRgba((size_t)img.w * img.h, 80);
    rgbaToPbgra(src.data(), src.size() / 4);
    memcpy(l0->px.data(), src.data(), src.size());
    img.levels.push_back(l0);
    ok(buildPyramid(img, 0, -1, nullptr), "buildPyramid fallo");
    ok(img.last() == levelCount(1001, 333) - 1, "cantidad de niveles");
    bool dimsOk = true, contentOk = true;
    for (int k = 1; k <= img.last(); ++k) {
        const Level* L = img.level(k);
        dimsOk = dimsOk && L && L->w == levelDim(1001, k) && L->h == levelDim(333, k);
        const Level* P = img.level(k - 1);
        std::vector<uint8_t> prev(P->px.data(), P->px.data() + P->bytes());
        std::vector<uint8_t> got(L->px.data(), L->px.data() + L->bytes());
        contentOk = contentOk && got == refDown(prev, P->w, P->h);
    }
    ok(dimsOk, "medidas de los niveles");
    ok(contentOk, "cada nivel tiene que ser el 2x2 del anterior");
    ok(std::max(img.level(img.last())->w, img.level(img.last())->h) <= PYRAMID_MIN_SIDE, "el ultimo nivel entra en %u", PYRAMID_MIN_SIDE);
    ok(img.effectLevel() == 1, "nivel de efectos de 1001 px: %d, esperaba 1 (501 px)", img.effectLevel());

    // cancelar entre niveles
    CpuImage c2;
    c2.w = 5000; c2.h = 10;
    auto z = std::make_shared<Level>(); z->w = 5000; z->h = 10; z->px = PixelBuf::alloc(z->bytes());
    memset(z->px.data(), 0, z->bytes());
    c2.levels.push_back(z);
    std::atomic<bool> cancel{true};
    ok(!buildPyramid(c2, 0, -1, &cancel), "con cancel=true tiene que volver false");

    // vista previa (niveles 2..) + completa (0..1): la mezcla comparte los niveles gruesos
    CpuImage prev;
    prev.w = 1001; prev.h = 333; prev.first = 2;
    prev.levels.resize(img.levels.size());
    for (int k = 2; k <= img.last(); ++k) prev.levels[k] = img.levels[k];
    CpuImage fullOnly;
    fullOnly.w = 1001; fullOnly.h = 333;
    fullOnly.levels = { img.levels[0], img.levels[1] };
    CpuImage m = mergeFull(fullOnly, prev);
    bool shared = m.first == 0 && m.last() == img.last();
    for (int k = 0; k <= img.last(); ++k) shared = shared && m.levels[k].get() == img.levels[k].get();
    ok(shared, "mergeFull tiene que reutilizar los mismos objetos de nivel");
    ok(!prev.full() && m.full(), "full()");
}

// ---------------------------------------------------------------- mosaicos
static void testTiles() {
    g_case = "mosaicos";
    TileGrid g1 = makeGrid(2048, 100);
    ok(g1.cols == 1 && g1.rows == 1, "2048 px entra en un mosaico");
    TileGrid g2 = makeGrid(2049, 4097);
    ok(g2.cols == 2 && g2.rows == 3, "2049x4097: %dx%d mosaicos", g2.cols, g2.rows);
    TileRect a = tileRect(g2, 0, 0), b = tileRect(g2, 1, 0), c = tileRect(g2, 1, 2);
    ok(a.bx0 == 0 && a.bx1 == 2049 && a.x1 == 2048, "margen derecho recortado al nivel (vecino de 1 px)");
    ok(b.x0 == 2048 && b.x1 == 2049 && b.bx0 == 2040 && b.bx1 == 2049, "mosaico de 1 px con margen izquierdo");
    ok(c.y0 == 4096 && c.y1 == 4097 && c.by0 == 4088 && c.by1 == 4097, "ultima fila");
    TileGrid small = makeGrid(10000, 10, 2048 + 16);      // GPU con maximo 2064: content 2048
    ok(small.content == 2048, "content con maximo 2064: %u", small.content);
    TileGrid tiny = makeGrid(10000, 10, 1024);
    ok(tiny.content == 1024 - 16 && tileRect(tiny, 1, 0).bx1 - tileRect(tiny, 1, 0).bx0 <= 1024, "el bitmap con margenes no pasa el maximo");

    // las celdas propias particionan el nivel
    bool part = true;
    for (uint32_t lw : { 1u, 7u, 2048u, 2049u, 5000u, 24000u }) {
        TileGrid g = makeGrid(lw, 3);
        uint32_t next = 0;
        for (int tx = 0; tx < g.cols; ++tx) { TileRect r = tileRect(g, tx, 0); part = part && r.x0 == next && r.x1 > r.x0; next = r.x1; }
        part = part && next == lw;
    }
    ok(part, "las celdas tienen que cubrir el nivel sin huecos ni solapes");

    // idealLevel / chooseLevel
    ok(idealLevel(1.0, 7) == 0 && idealLevel(3.0, 7) == 0, "escala >= 1 -> nivel 0");
    ok(idealLevel(0.5, 7) == 1 && idealLevel(0.51, 7) == 0, "0.5 -> nivel 1, 0.51 -> nivel 0");
    ok(idealLevel(0.25, 7) == 2 && idealLevel(0.26, 7) == 1, "0.25 -> 2, 0.26 -> 1");
    ok(idealLevel(1e-6, 7) == 7, "escala minima -> el ultimo nivel");
    ok(chooseLevel(0.9, 2, 7) == 2, "sin nivel 0 ni 1 (vista previa) se usa el 2");
    bool relOk = true;
    for (int i = 0; i < 2000; ++i) {
        double s = std::exp(-8.0 * rnd(100000) / 100000.0);          // (e^-8, 1]
        int k = idealLevel(s, 30);
        double rel = s * std::ldexp(1.0, k);
        relOk = relOk && rel > 0.5 - 1e-9 && rel <= 1.0 + 1e-9;
    }
    ok(relOk, "la escala relativa al nivel elegido tiene que caer en (0.5, 1]");

    // costuras: cada columna de pixeles de la imagen la pinta exactamente un mosaico
    int badCols = 0, badMargin = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const uint32_t lw = 1 + rnd(9000);
        const uint32_t content = 16 + rnd(3000);
        TileGrid g = makeGrid(lw, 1);
        g.content = content;
        g.cols = (int)((lw + content - 1) / content);
        const double o = (double)((int)rnd(4000) - 2000);            // origen entero (clampPan lo redondea)
        const double s = 0.5 + rnd(100000) / 100000.0 * 11.5;         // (0.5, 12]
        const double left = std::floor(o), right = std::ceil(o + lw * s);
        for (double p = left; p < right; p += 1.0) {
            int owners = 0;
            for (int tx = 0; tx < g.cols; ++tx) {
                TileRect r = tileRect(g, tx, 0);
                Cut cut = cellCut(o, s, r.x0, r.x1, tx == 0, tx + 1 == g.cols);
                if (p >= cut.lo && p < cut.hi) ++owners;
                // el corte interior cae dentro del bitmap con margen de sobra para el filtro
                if (tx > 0 && cut.lo - (o + r.bx0 * s) < 3.0) ++badMargin;
                if (tx + 1 < g.cols && (o + r.bx1 * s) - cut.hi < 3.0) ++badMargin;
            }
            if (owners != 1) ++badCols;
        }
    }
    ok(badCols == 0, "%d columnas de pixeles pintadas por 0 o 2 mosaicos", badCols);
    ok(badMargin == 0, "%d cortes a menos de 3 px del borde del bitmap", badMargin);

    // visibleSpan
    Span sp = visibleSpan(0, 1.0, 0, 1400, 2048, 12);
    ok(sp.a == 0 && sp.b == 1, "a escala 1 se ve el primer mosaico");
    sp = visibleSpan(-5000, 1.0, 0, 1400, 2048, 12);
    ok(sp.a == 2 && sp.b == 4, "corrido 5000 px: mosaicos [%d,%d)", sp.a, sp.b);
    sp = visibleSpan(3000, 1.0, 0, 1400, 2048, 12);
    ok(sp.a == 0 && sp.b == 0, "imagen fuera de la ventana: nada que dibujar");
}

// ---------------------------------------------------------------- cache
static void testCache() {
    g_case = "cache";
    LruCache<std::string, int> c(100);
    c.put("a", 1, 40); c.put("b", 2, 40); c.put("c", 3, 40);
    ok(c.bytes() == 120 && c.size() == 3, "bytes %zu", c.bytes());
    ok(c.get("a") && *c.get("a") == 1, "get");
    size_t ev = c.trim([](const std::string&) { return false; });
    ok(ev == 1 && !c.peek("b") && c.peek("a") && c.peek("c"), "desaloja la menos usada (b), no la que se acaba de leer (a)");
    // el presupuesto le gana a la recencia; lo fijado le gana al presupuesto. Una recien
    // llegada que no entra se va ella misma (el cargador se entera y no la vuelve a pedir).
    c.put("d", 4, 90);
    ev = c.trim([](const std::string& k) { return k == "a"; });
    ok(ev == 2 && c.peek("a") && !c.peek("c") && !c.peek("d") && c.bytes() == 40,
       "a fijada se queda; c (vieja) y d (no entra) se van");
    c.put("big", 9, 500);
    ev = c.trim([](const std::string& k) { return k == "a" || k == "big"; });
    ok(ev == 0 && c.bytes() == 540, "todo fijado: se pasa del presupuesto y no desaloja nada");
    ok(c.erase("big") && !c.erase("big"), "erase una sola vez");
    c.put("a", 5, 10);
    ok(*c.peek("a") == 5 && c.bytes() == 10 && c.size() == 1, "reemplazar actualiza el valor y los bytes");
    LruCache<std::string, int> z(0);
    z.put("x", 1, 1);
    z.trim([](const std::string&) { return true; });
    ok(z.size() == 1, "presupuesto 0 con todo fijado no desaloja nada");
    z.trim([](const std::string&) { return false; });
    ok(z.size() == 0 && z.bytes() == 0, "presupuesto 0 sin fijadas: queda vacia");
}

// ---------------------------------------------------------------- precarga
static void testPlan() {
    g_case = "plan";
    auto v = planJobs(10, 3, 7, +1, true, false);
    ok(v.size() == 2 && v[0].index == 7 && v[0].priority == 0 && v[1].index == 8, "con pedido pendiente: el pedido y el siguiente");
    v = planJobs(10, 9, -1, +1, true, false);
    ok(v.size() == 3 && v[0].index == 0 && v[1].index == 8 && v[2].index == 1, "al final de la carpeta da la vuelta (0, 8, 1)");
    v = planJobs(10, 5, -1, -1, false, false);
    ok(v.size() == 4 && v[0].index == 4 && v[1].index == 5 && v[1].kind == JobKind::Full && v[2].index == 6,
       "hacia atras: 4, la completa de 5, 6, 3");
    v = planJobs(10, 5, -1, +1, false, true);
    ok(v[0].index == 5 && v[0].kind == JobKind::Full && v[0].priority == 0, "el zoom pide la completa ya");
    v = planJobs(1, 0, -1, +1, true, false);
    ok(v.empty(), "una sola imagen, completa: nada que precargar");
    v = planJobs(2, 0, -1, +1, true, false);
    ok(v.size() == 1 && v[0].index == 1, "dos imagenes: la otra una sola vez");
    v = planJobs(0, -1, -1, +1, true, false);
    ok(v.empty(), "carpeta vacia");
    ok(onPath(10, 3, 5, 7, +1) && onPath(10, 3, 7, 7, +1) && !onPath(10, 3, 3, 7, +1) && !onPath(10, 3, 8, 7, +1), "onPath hacia adelante");
    ok(onPath(10, 1, 9, 8, -1) && onPath(10, 1, 0, 8, -1) && !onPath(10, 1, 2, 8, -1), "onPath hacia atras dando la vuelta");
}

// ---------------------------------------------------------------- pool
static void testPool() {
    g_case = "pool";
    const size_t n = 10000000;
    std::atomic<uint64_t> sum{0};
    parallelFor(n, 4096, [&](size_t b, size_t e) {
        uint64_t s = 0;
        for (size_t i = b; i < e; ++i) s += i;
        sum += s;
    });
    ok(sum == (uint64_t)n * (n - 1) / 2, "suma 0..n-1");
    int calls = 0;
    parallelFor(0, 16, [&](size_t, size_t) { ++calls; });
    ok(calls == 0, "n = 0 no llama nunca");
    parallelFor(5, 100, [&](size_t b, size_t e) { calls += (int)(e - b) + 1000 * (int)b; });
    ok(calls == 5, "un solo bloque: una llamada con [0, 5)");

    // cuatro hilos haciendo parallelFor a la vez, y un parallelFor adentro de otro
    std::vector<std::thread> ts;
    std::atomic<int> good{0};
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&, t] {
            std::atomic<uint64_t> s{0};
            parallelFor(1000000 + t, 1000, [&](size_t b, size_t e) {
                std::atomic<uint64_t> inner{0};
                parallelFor(e - b, 64, [&](size_t ib, size_t ie) { inner += (uint64_t)(ie - ib); });
                s += inner;
            });
            if (s == (uint64_t)(1000000 + t)) ++good;
        });
    for (auto& t : ts) t.join();
    ok(good == 4, "concurrentes y anidados: %d de 4 bien", good.load());
}

// ---------------------------------------------------------------- benchmark
static void bench() {
    const uint32_t W = 8000, H = 6000;
    const size_t N = (size_t)W * H;
    std::vector<uint8_t> base = randomRgba(N, 100);                  // una foto: todo opaco
    std::vector<uint8_t> alpha = randomRgba(N, 20);                  // un PNG con transparencia
    auto best = [](auto fn) { double b = 1e9; for (int i = 0; i < 3; ++i) b = std::min(b, fn()); return b; };

    double tOpaque = best([&] { std::vector<uint8_t> v = base; auto t = std::chrono::steady_clock::now(); rgbaToPbgra(v.data(), N); return ms(t); });
    double tAlpha = best([&] { std::vector<uint8_t> v = alpha; auto t = std::chrono::steady_clock::now(); rgbaToPbgra(v.data(), N); return ms(t); });
    double tRef = [&] {
        std::vector<uint8_t> v = alpha; auto t = std::chrono::steady_clock::now();
        for (size_t i = 0; i < N; ++i) {                             // el loop de antes (bitmapFromRGBA)
            uint8_t* p = &v[i * 4]; uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
            p[0] = (uint8_t)((b * a + 127) / 255); p[1] = (uint8_t)((g * a + 127) / 255); p[2] = (uint8_t)((r * a + 127) / 255); p[3] = a;
        }
        return ms(t);
    }();
    std::vector<uint8_t> pb = base;
    rgbaToPbgra(pb.data(), N);
    std::vector<uint8_t> half((size_t)(W / 2) * (H / 2) * 4);
    double tDown = best([&] { auto t = std::chrono::steady_clock::now(); downsample2x(pb.data(), W, H, half.data()); return ms(t); });
    double tOrient = best([&] {
        PixelBuf b = PixelBuf::alloc(N * 4); memcpy(b.data(), pb.data(), N * 4);
        uint32_t ow, oh; auto t = std::chrono::steady_clock::now(); PixelBuf o = orient(std::move(b), W, H, 6, ow, oh); return ms(t);
    });
    double tPyr = best([&] {
        CpuImage img; img.w = W; img.h = H;
        auto l0 = std::make_shared<Level>(); l0->w = W; l0->h = H; l0->px = PixelBuf::alloc(N * 4);
        memcpy(l0->px.data(), pb.data(), N * 4);
        img.levels.push_back(l0);
        auto t = std::chrono::steady_clock::now(); buildPyramid(img, 0, -1, nullptr); return ms(t);
    });

    printf("\n  \x1b[38;5;111mbenchmark 48 MP (8000x6000)\x1b[0m   %u hilos\n", Pool::get().threads() + 1);
    printf("  \x1b[38;5;60m%-34s %9s\x1b[0m\n", "etapa", "ms");
    printf("  %-34s %9.1f\n", "premultiplicar (loop de antes)", tRef);
    printf("  %-34s %9.1f   \x1b[38;5;150mx%.0f\x1b[0m\n", "premultiplicar con alfa (nuevo)", tAlpha, tRef / tAlpha);
    printf("  %-34s %9.1f   \x1b[38;5;150mx%.0f\x1b[0m\n", "premultiplicar opaca (atajo)", tOpaque, tRef / tOpaque);
    printf("  %-34s %9.1f\n", "un escalon de piramide (-> 12 MP)", tDown);
    printf("  %-34s %9.1f\n", "piramide entera (6 niveles)", tPyr);
    printf("  %-34s %9.1f\n", "girar 90 grados (EXIF 6)", tOrient);
}

int main(int argc, char** argv) {
    printf("\n  \x1b[38;5;111mlux\x1b[0m \x1b[38;5;60m— motor (pool, pixeles, piramide, mosaicos, cache, precarga)\x1b[0m\n\n");
    testMul255();
    testPremultiply();
    testOpaqueAndGray();
    testOrient();
    testDownsample();
    testPyramid();
    testTiles();
    testCache();
    testPlan();
    testPool();
    printf("  %d asserts, %d fallaron\n", checks, fails);
    if (!(argc > 1 && !strcmp(argv[1], "--no-bench"))) bench();
    printf("\n  %s\n", fails ? "\x1b[38;5;210mHAY FALLAS\x1b[0m" : "\x1b[38;5;150mtodo OK\x1b[0m");
    return fails ? 1 : 0;
}

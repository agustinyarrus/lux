// ============================================================================
//  test_decoders — verifica los decoders propios de Lux contra las muestras que
//  genera gen_samples.py. Compila y corre suelto: no necesita ventana, Direct2D
//  ni WIC, porque prueba exactamente la parte que es codigo nuestro.
//
//    .\run.ps1        genera las muestras, compila y corre
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "../third_party/exotic.h"
#include "../third_party/retro.h"
#include "../third_party/sci.h"
#include "../third_party/texture.h"
#include "../third_party/xcf.h"
#include "../third_party/unpack.h"

// ---------------------------------------------------------------- andamiaje
static int checks = 0, fails = 0;
static const char* g_case = "";

static void ok(bool cond, const char* fmt, ...) {
    checks++;
    if (cond) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("  [FAIL] %-14s ", g_case);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static bool slurp(const char* name, std::vector<unsigned char>& buf) {
    std::string path = std::string("samples/") + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    buf.resize(n > 0 ? (size_t)n : 0);
    size_t rd = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    return rd == buf.size() && !buf.empty();
}

// Decodifica una muestra con el despachador que le corresponda.
typedef unsigned char* (*LoadFn)(const unsigned char*, size_t, const char*, int*, int*);
static unsigned char* decodeVia(LoadFn fn, const char* name, const char* ext, int* w, int* h) {
    g_case = name;
    std::vector<unsigned char> buf;
    if (!slurp(name, buf)) { ok(false, "no se pudo leer el archivo"); return nullptr; }
    unsigned char* px = fn(buf.data(), buf.size(), ext, w, h);
    ok(px != nullptr, "el decoder devolvio NULL");
    return px;
}
static unsigned char* decode(const char* name, const char* ext, int* w, int* h) {
    return decodeVia(exotic_load, name, ext, w, h);
}

static void dims(const unsigned char* px, int w, int h, int ew, int eh) {
    if (!px) return;
    ok(w == ew && h == eh, "dimensiones %dx%d, esperaba %dx%d", w, h, ew, eh);
}

// Compara un pixel con tolerancia (las conversiones de gamma no son exactas).
static void pix(const unsigned char* px, int w, int h, int x, int y,
                int r, int g, int b, int a, int tol = 0) {
    if (!px || x >= w || y >= h) { ok(false, "pixel (%d,%d) fuera de %dx%d", x, y, w, h); return; }
    const unsigned char* p = px + ((size_t)y * w + x) * 4;
    bool good = abs(p[0]-r) <= tol && abs(p[1]-g) <= tol && abs(p[2]-b) <= tol && abs(p[3]-a) <= tol;
    ok(good, "(%d,%d) = %d,%d,%d,%d  esperaba %d,%d,%d,%d", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
}

// La referencia de 4x2 que usan casi todas las muestras.
static const int REF[2][4][3] = {
    { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255} },
    { {0,0,0}, {128,128,128}, {255,255,0}, {0,255,255} },
};
// Verifica los 8 colores, opcionalmente repetidos en mosaico sobre una imagen mayor.
static void refGrid(const unsigned char* px, int w, int h, int alpha = 255) {
    if (!px) return;
    for (int y = 0; y < 2 && y < h; y++)
        for (int x = 0; x < 4 && x < w; x++)
            pix(px, w, h, x, y, REF[y][x][0], REF[y][x][1], REF[y][x][2], alpha);
}

// ------------------------------------------------------------------- pruebas
static void testPnmAscii() {
    int w, h;
    if (unsigned char* p = decode("ascii_p1.pbm", "pbm", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);          // P1: el 1 es negro
        pix(p, w, h, 1, 0, 255, 255, 255, 255);
        pix(p, w, h, 1, 1, 0, 0, 0, 255);
        free(p);
    }
    if (unsigned char* p = decode("ascii_p2.pgm", "pgm", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);          // maxval 100 -> reescalado a 255
        pix(p, w, h, 1, 0, 63, 63, 63, 255);
        pix(p, w, h, 3, 0, 255, 255, 255, 255);
        free(p);
    }
    if (unsigned char* p = decode("ascii_p3.ppm", "ppm", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

static void testXpm() {
    int w, h;
    if (unsigned char* p = decode("ref.xpm", "xpm", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

static void testIlbm() {
    int w, h;
    for (const char* f : { "ilbm_raw.iff", "ilbm_rle.iff", "pbm_chunky.iff" }) {
        if (unsigned char* p = decode(f, "iff", &w, &h)) {
            dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
        }
    }
    // EHB: los indices 32..63 son el color 0..31 a la mitad del brillo
    if (unsigned char* p = decode("ilbm_ehb.iff", "iff", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 255, 128, 64, 255);
        pix(p, w, h, 0, 1, 127, 64, 32, 255);
        free(p);
    }
}

static void testMacPaint() {
    int w, h;
    if (unsigned char* p = decode("ref.mac", "mac", &w, &h)) {
        dims(p, w, h, 576, 720);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);            // fila 0 = 0xAA: negro, blanco…
        pix(p, w, h, 1, 0, 255, 255, 255, 255);
        pix(p, w, h, 2, 0, 0, 0, 0, 255);
        pix(p, w, h, 0, 1, 255, 255, 255, 255);      // el resto es blanco
        pix(p, w, h, 575, 719, 255, 255, 255, 255);
        free(p);
    }
}

static void testXwd() {
    int w, h;
    if (unsigned char* p = decode("ref.xwd", "xwd", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

static void testDpx() {
    int w, h;
    if (unsigned char* p = decode("ref10.dpx", "dpx", &w, &h)) {   // 10 bits empaquetado
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
    if (unsigned char* p = decode("ref8.dpx", "dpx", &w, &h)) {    // 8 bits plano
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
    // Cineon: la curva log tiene que llevar el 95 a negro y el 685 a blanco
    if (unsigned char* p = decode("ref.cin", "cin", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 0, 0, 0, 255, 1);
        pix(p, w, h, 3, 1, 255, 255, 255, 255, 1);
        free(p);
    }
}

static void testIcns() {
    ex_decode_embedded = [](const unsigned char* d, size_t n, int* w, int* h) -> unsigned char* {
        int comp = 0;
        return stbi_load_from_memory(d, (int)n, w, h, &comp, 4);
    };
    int w, h;
    // trae ic11 (32x32) e ic12 (64x64): tiene que quedarse con el grande
    if (unsigned char* p = decode("png.icns", "icns", &w, &h)) {
        dims(p, w, h, 64, 64); refGrid(p, w, h); free(p);
    }
    // clasico: il32 con RLE de 3 canales + mascara l8mk al 50%
    if (unsigned char* p = decode("rle.icns", "icns", &w, &h)) {
        dims(p, w, h, 32, 32); refGrid(p, w, h, 128); free(p);
    }
}

static void testContainers() {
    std::vector<unsigned char> buf, out;

    g_case = "ref.svgz";
    if (slurp("ref.svgz", buf)) {
        ok(up_gunzip(buf, out), "gunzip fallo");
        ok(out.size() > 40 && !memcmp(out.data(), "<?xml", 5), "el gzip no devolvio el SVG");
        std::string s((const char*)out.data(), out.size());
        ok(s.find("<svg") != std::string::npos, "no aparece la etiqueta <svg>");
        ok(s.find("#ff0000") != std::string::npos, "no aparece el color del rect");
    } else ok(false, "no se pudo leer");

    // ORA (deflate) y KRA (stored): los dos guardan mergedimage.png en la raiz
    for (const char* f : { "ref.ora", "ref.kra" }) {
        g_case = f;
        out.clear();
        if (!slurp(f, buf)) { ok(false, "no se pudo leer"); continue; }
        ok(up_zip_extract(buf, "mergedimage.png", out), "no se extrajo mergedimage.png");
        ok(out.size() > 8 && !memcmp(out.data(), "\x89PNG\r\n\x1a\n", 8), "lo extraido no es un PNG");
        int w = 0, h = 0, comp = 0;
        unsigned char* px = out.empty() ? nullptr
            : stbi_load_from_memory(out.data(), (int)out.size(), &w, &h, &comp, 4);
        ok(px != nullptr, "el PNG de adentro no decodifica");
        if (px) { dims(px, w, h, 4, 2); refGrid(px, w, h); stbi_image_free(px); }
        // una entrada que no existe no puede dar un falso positivo
        std::vector<unsigned char> nope;
        ok(!up_zip_extract(buf, "no-existe.png", nope), "encontro una entrada inexistente");
    }

    g_case = "ref.ani";
    if (slurp("ref.ani", buf)) {
        size_t off = 0, len = 0;
        ok(up_ani_frame(buf, &off, &len), "no se encontro el primer cuadro");
        ok(off + len <= buf.size(), "el cuadro se sale del archivo");
        // el cuadro tiene que ser un .ico: reservado=0, tipo=1, un solo frame
        ok(len > 22 && buf[off] == 0 && buf[off+1] == 0 && buf[off+2] == 1 && buf[off+3] == 0,
           "el cuadro no arranca con una cabecera ICO");
    } else ok(false, "no se pudo leer");
}

// ------------------------------------------------- formatos de 8 y 16 bits
static void testAcbm() {
    int w, h;
    if (unsigned char* p = decode("ref.acbm", "acbm", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

static void testSunRle() {
    int w, h;
    if (unsigned char* p = decode("rle.ras", "ras", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

static void testPcx() {
    int w, h;
    if (unsigned char* p = decode("ref8.pcx", "pcx", &w, &h)) {      // 256 colores
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
    if (unsigned char* p = decode("ref4.pcx", "pcx", &w, &h)) {      // 16 colores EGA
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
    if (unsigned char* p = decode("ref1.pcx", "pcx", &w, &h)) {      // monocromo
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 255, 255, 255, 255);
        pix(p, w, h, 1, 0, 0, 0, 0, 255);
        pix(p, w, h, 0, 1, 0, 0, 0, 255);
        pix(p, w, h, 1, 1, 255, 255, 255, 255);
        free(p);
    }
    if (unsigned char* p = decodeVia(retro_load, "ref.dcx", "dcx", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

// Los 8 colores exactos de la paleta de 3 bits del ST.
static const int ST[8][3] = { {255,0,0},{0,255,0},{0,0,255},{255,255,255},
                              {0,0,0},{255,255,0},{0,255,255},{255,0,255} };
static void stRow(const unsigned char* p, int w, int h) {
    if (!p) return;
    for (int x = 0; x < 8; x++) pix(p, w, h, x, 0, ST[x][0], ST[x][1], ST[x][2], 255);
    pix(p, w, h, 0, 1, 0, 0, 0, 255);            // el resto de la pantalla es negro
    pix(p, w, h, 319, 199, 0, 0, 0, 255);
}
static void testAtari() {
    int w, h;
    for (const char* f : { "ref.pi1", "ref.pc1" }) {                 // Degas y Degas Elite
        if (unsigned char* p = decodeVia(retro_load, f, "pi1", &w, &h)) {
            dims(p, w, h, 320, 200); stRow(p, w, h); free(p);
        }
    }
    if (unsigned char* p = decodeVia(retro_load, "ref.neo", "neo", &w, &h)) {
        dims(p, w, h, 320, 200); stRow(p, w, h); free(p);
    }
}

static void testZx() {
    int w, h;
    if (unsigned char* p = decodeVia(retro_load, "ref.scr", "scr", &w, &h)) {
        dims(p, w, h, 256, 192);
        pix(p, w, h, 0, 0, 255, 0, 0, 255);        // tinta 2 con brillo
        pix(p, w, h, 3, 0, 255, 0, 0, 255);
        pix(p, w, h, 4, 0, 0, 255, 255, 255);      // papel 5 con brillo
        pix(p, w, h, 0, 191, 255, 0, 0, 255);      // ultima fila (direccionamiento raro)
        free(p);
    }
}

static void testKoala() {
    int w, h;
    if (unsigned char* p = decodeVia(retro_load, "ref.koa", "koa", &w, &h)) {
        dims(p, w, h, 320, 200);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);          // 00 -> fondo
        pix(p, w, h, 2, 0, 255, 255, 255, 255);    // 01 -> pantalla, nibble alto
        pix(p, w, h, 4, 0, 104, 55, 43, 255);      // 10 -> pantalla, nibble bajo (rojo C64)
        pix(p, w, h, 6, 0, 112, 164, 178, 255);    // 11 -> RAM de color (cian C64)
        pix(p, w, h, 7, 0, 112, 164, 178, 255);    // doble ancho
        free(p);
    }
}

static void testGem() {
    int w, h;
    if (unsigned char* p = decodeVia(retro_load, "ref.img", "img", &w, &h)) {
        dims(p, w, h, 16, 2);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);          // 0xF0: 4 negros…
        pix(p, w, h, 4, 0, 255, 255, 255, 255);    // …y 4 blancos
        pix(p, w, h, 15, 0, 0, 0, 0, 255);         // 0x0F al final
        pix(p, w, h, 0, 1, 255, 255, 255, 255);    // run solido de ceros
        free(p);
    }
}

static void testTim() {
    int w, h;
    if (unsigned char* p = decodeVia(retro_load, "ref.tim", "tim", &w, &h)) {
        dims(p, w, h, 8, 2);
        // CLUT de 15 bits (BGR555): el gris no es exacto, asi que la paleta usa
        // solo colores puros. Fila 0: rojo, verde, azul, blanco.
        pix(p, w, h, 0, 0, 255, 0, 0, 255);
        pix(p, w, h, 1, 0, 0, 255, 0, 255);
        pix(p, w, h, 2, 0, 0, 0, 255, 255);
        pix(p, w, h, 3, 0, 255, 255, 255, 255);
        pix(p, w, h, 0, 1, 0, 0, 0, 255);          // negro con STP: opaco
        pix(p, w, h, 1, 1, 255, 255, 0, 255);
        pix(p, w, h, 2, 1, 0, 255, 255, 255);
        pix(p, w, h, 3, 1, 0, 0, 0, 0);            // negro sin STP = transparente
        free(p);
    }
}

static void testPix() {
    int w, h;
    if (unsigned char* p = decodeVia(retro_load, "ref.pix", "pix", &w, &h)) {
        dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
    }
}

// Photo CD: 768 KB de muestra serian demasiado para el repo, asi que se arma
// una en memoria (croma neutra -> gris = luma * 1.3584).
static void testPcd() {
    g_case = "pcd";
    std::vector<unsigned char> b(0xC0000, 0);
    memcpy(&b[0x800], "PCD_IPI", 7);
    for (int pair = 0; pair < 256; pair++) {
        unsigned char* q = &b[0x30000 + (size_t)pair * 2304];
        for (int x = 0; x < 768; x++) { q[x] = 100; q[768 + x] = 200; }
        for (int x = 0; x < 384; x++) { q[1536 + x] = 156; q[1920 + x] = 137; }
    }
    int w = 0, h = 0;
    unsigned char* p = retro_load(b.data(), b.size(), "pcd", &w, &h);
    ok(p != nullptr, "retro_load devolvio NULL");
    if (p) {
        dims(p, w, h, 768, 512);
        pix(p, w, h, 0, 0, 135, 135, 135, 255, 1);
        pix(p, w, h, 0, 1, 255, 255, 255, 255);    // 200*1.3584 se pasa de 255
        free(p);
    }
}

// ------------------------------------------------------------- texturas BCn
static void bcRows(const unsigned char* p, int w, int h, bool flipped) {
    if (!p) return;
    const int C[4][3] = { {255,0,0}, {0,0,255}, {170,0,85}, {85,0,170} };
    for (int y = 0; y < 4; y++) {
        const int* c = C[flipped ? 3 - y : y];
        pix(p, w, h, 0, y, c[0], c[1], c[2], 255);
        pix(p, w, h, 3, y, c[0], c[1], c[2], 255);
    }
}
static void testTextures() {
    int w, h;
    if (unsigned char* p = decodeVia(texture_load, "bc1.dds", "dds", &w, &h)) {
        dims(p, w, h, 4, 4); bcRows(p, w, h, false); free(p);
    }
    if (unsigned char* p = decodeVia(texture_load, "bc3.dds", "dds", &w, &h)) {
        dims(p, w, h, 4, 4);
        pix(p, w, h, 0, 0, 255, 0, 0, 255);        // alfa 255 arriba
        pix(p, w, h, 0, 3, 85, 0, 170, 0);         // alfa 0 abajo
        free(p);
    }
    if (unsigned char* p = decodeVia(texture_load, "ref.vtf", "vtf", &w, &h)) {
        dims(p, w, h, 4, 4); bcRows(p, w, h, false); free(p);
    }
    if (unsigned char* p = decodeVia(texture_load, "ref.ktx", "ktx", &w, &h)) {
        dims(p, w, h, 4, 4); bcRows(p, w, h, true); free(p);   // OpenGL: al reves
    }
}

// -------------------------------------------------- cientificas: FITS y DICOM
static void testSci() {
    int w, h;
    if (unsigned char* p = decodeVia(sci_load, "ref.fits", "fits", &w, &h)) {
        dims(p, w, h, 4, 2);
        // FITS arranca por abajo: la 1a fila del archivo termina abajo de todo
        pix(p, w, h, 0, 0, 255, 255, 255, 255);
        pix(p, w, h, 3, 0, 0, 0, 0, 255);
        pix(p, w, h, 1, 1, 85, 85, 85, 255, 1);
        free(p);
    }
    if (unsigned char* p = decodeVia(sci_load, "ref.dcm", "dcm", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 0, 0, 0, 255);
        pix(p, w, h, 3, 0, 255, 255, 255, 255);
        pix(p, w, h, 1, 1, 170, 170, 170, 255, 1);
        free(p);
    }
    // VR implicita + MONOCHROME1 (los valores se invierten)
    if (unsigned char* p = decodeVia(sci_load, "impl.dcm", "dcm", &w, &h)) {
        dims(p, w, h, 4, 2);
        pix(p, w, h, 0, 0, 255, 255, 255, 255);
        pix(p, w, h, 3, 0, 0, 0, 0, 255);
        free(p);
    }
}

// ------------------------------------------------------------------- XCF
static void testXcf() {
    int w, h;
    for (const char* f : { "v1.xcf", "v11.xcf" }) {   // punteros de 32 y de 64 bits
        if (unsigned char* p = decodeVia(xcf_load, f, "xcf", &w, &h)) {
            dims(p, w, h, 4, 2); refGrid(p, w, h); free(p);
        }
    }
}

// Nada de esto es una imagen valida: lo que importa es que devuelva NULL sin
// romperse ni leerse de rango (los buffers no estan NUL-terminados).
static void testGarbage() {
    g_case = "basura";
    static const char* exts[] = { "pcx","pfm","ras","sgi","wbmp","pam","xbm","xpm",
                                  "iff","mac","xwd","dpx","cin","icns","ppm","ff", nullptr };
    unsigned char junk[512];
    for (int i = 0; i < 512; i++) junk[i] = (unsigned char)(i * 37 + 11);
    for (int k = 0; exts[k]; k++) {
        int w = 0, h = 0;
        unsigned char* p = exotic_load(junk, sizeof junk, exts[k], &w, &h);
        ok(p == nullptr, "'%s' acepto basura (%dx%d)", exts[k], w, h);
        free(p);
    }
    // lo mismo para los despachadores nuevos
    static const char* rexts[] = { "pi1","pc1","neo","scr","koa","img","tim","pix","dcx","pcd", nullptr };
    for (int k = 0; rexts[k]; k++) {
        int w = 0, h = 0;
        unsigned char* p = retro_load(junk, sizeof junk, rexts[k], &w, &h);
        ok(p == nullptr, "retro '%s' acepto basura (%dx%d)", rexts[k], w, h);
        free(p);
    }
    static const char* sexts[] = { "fits","dcm", nullptr };
    for (int k = 0; sexts[k]; k++) {
        int w = 0, h = 0;
        unsigned char* p = sci_load(junk, sizeof junk, sexts[k], &w, &h);
        ok(p == nullptr, "sci '%s' acepto basura (%dx%d)", sexts[k], w, h);
        free(p);
    }
    static const char* texts[] = { "dds","vtf","ktx", nullptr };
    for (int k = 0; texts[k]; k++) {
        int w = 0, h = 0;
        unsigned char* p = texture_load(junk, sizeof junk, texts[k], &w, &h);
        ok(p == nullptr, "texture '%s' acepto basura (%dx%d)", texts[k], w, h);
        free(p);
    }
    // cabeceras validas con el cuerpo cortado, para los nuevos
    std::vector<unsigned char> buf;
    const char* rfiles[] = { "ref.pi1", "ref.tim", "ref.koa", "bc1.dds", "ref.dcm", nullptr };
    const char* rfexts[] = { "pi1", "tim", "koa", "dds", "dcm", nullptr };
    LoadFn rfns[] = { retro_load, retro_load, retro_load, texture_load, sci_load, nullptr };
    for (int k = 0; rfiles[k]; k++) {
        if (!slurp(rfiles[k], buf)) continue;
        for (size_t cut : { (size_t)16, buf.size() / 3, buf.size() / 2 }) {
            int w = 0, h = 0;
            free(rfns[k](buf.data(), cut, rfexts[k], &w, &h));
        }
        ok(true, "%s truncado no rompe", rfiles[k]);
    }
    // cabeceras validas pero con el cuerpo cortado
    const char* files[] = { "ilbm_raw.iff", "ref.xwd", "ref10.dpx", "rle.icns", "ref.mac", nullptr };
    const char* fexts[] = { "iff", "xwd", "dpx", "icns", "mac", nullptr };
    for (int k = 0; files[k]; k++) {
        if (!slurp(files[k], buf)) continue;
        for (size_t cut : { (size_t)8, buf.size() / 3, buf.size() / 2 }) {
            int w = 0, h = 0;
            unsigned char* p = exotic_load(buf.data(), cut, fexts[k], &w, &h);
            free(p);                                  // vale devolver algo; no vale romperse
        }
        ok(true, "%s truncado no rompe", files[k]);
    }
    // un ZIP y un gzip corruptos tampoco
    std::vector<unsigned char> bad(64, 0x5A), out;
    ok(!up_zip_extract(bad, "x.png", out), "el ZIP corrupto devolvio algo");
    ok(!up_gunzip(bad, out), "el gzip corrupto devolvio algo");
    size_t o, l;
    ok(!up_ani_frame(bad, &o, &l), "el ANI corrupto devolvio algo");
}

int main() {
    printf("lux — decoders\n");
    testPnmAscii();
    testXpm();
    testIlbm();
    testMacPaint();
    testXwd();
    testDpx();
    testIcns();
    testContainers();
    testAcbm();
    testSunRle();
    testPcx();
    testAtari();
    testZx();
    testKoala();
    testGem();
    testTim();
    testPix();
    testPcd();
    testTextures();
    testSci();
    testXcf();
    testGarbage();
    printf("\n%d asserts, %d fallaron\n", checks, fails);
    if (!fails) printf("todo OK\n");
    return fails ? 1 : 0;
}

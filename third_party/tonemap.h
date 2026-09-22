// tonemap.h — de imagen HDR (float lineal) a 8 bits sRGB, para EXR, Radiance .hdr y PFM.
//
// Lo de antes: dividir por el maximo y aplicar gamma 2,2. Con una sola lampara o un sol en
// cuadro (valor 400 cuando el resto vive entre 0 y 3) toda la escena quedaba casi negra.
//
// Ahora, dos caminos:
//   · El rango ya entra en [0,1]: no hay nada que comprimir -> solo la curva sRGB de verdad.
//   · Hay luces por encima de 1: exposicion automatica por la luminancia media LOGARITMICA
//     (el "key" de Reinhard, lo que un fotometro llama gris medio), estimada con un histograma de
//     log2 L que descarta el 1 % mas oscuro y el 0,5 % mas brillante —robusta a un sol o una
//     lampara—, curva filmica ACES (aproximacion de Narkowicz, que comprime las luces con
//     suavidad en vez de recortarlas) y codificacion sRGB por tabla.
//
// Costo: dos pasadas O(n) y ninguna pow por pixel (la sRGB sale de una LUT de 4096 entradas).
// Header-only y sin dependencias de Windows: se prueba suelto desde tests/test_decoders.cpp.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define TM_HIST_BINS 1024
#define TM_LOG_MIN   (-20.0f)   // log2 de la luminancia mas chica que se distingue
#define TM_LOG_MAX   ( 20.0f)
#define TM_MIDGRAY   0.18f      // a donde va la luminancia media (gris medio fotografico)
#define TM_LUT_SIZE  4096

static float tm_lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// ACES filmico (Narkowicz 2015): x lineal expuesto -> [0,1] lineal.
static float tm_aces(float x) {
    if (x <= 0.f) return 0.f;
    float v = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

// sRGB OETF exacta, usada solo para llenar la tabla.
static float tm_srgb(float v) {
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
}

typedef struct { unsigned char lut[TM_LUT_SIZE + 1]; } tm_table;

static void tm_build(tm_table* t) {
    for (int i = 0; i <= TM_LUT_SIZE; i++) {
        int v = (int)(tm_srgb((float)i / TM_LUT_SIZE) * 255.f + 0.5f);
        t->lut[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static unsigned char tm_encode(const tm_table* t, float lin) {
    if (!(lin > 0.f)) return 0;           // tambien NaN
    if (lin >= 1.f) return 255;
    return t->lut[(int)(lin * TM_LUT_SIZE + 0.5f)];
}

// Exposicion automatica: devuelve el factor por el que se multiplica la imagen antes de ACES.
// Histograma de log2 L (O(n)), recorte de colas por percentil y media logaritmica del resto.
static float tm_auto_exposure(const float* px, size_t count, int stride) {
    static const float span = TM_LOG_MAX - TM_LOG_MIN;
    size_t hist[TM_HIST_BINS] = {0};
    size_t valid = 0;
    for (size_t i = 0; i < count; i++) {
        const float* p = px + i * (size_t)stride;
        float L = stride >= 3 ? tm_lum(p[0], p[1], p[2]) : p[0];
        if (!(L > 0.f) || L > 1e30f) continue;           // negros absolutos, NaN, infinitos: fuera
        float lg = log2f(L);
        int b = (int)((lg - TM_LOG_MIN) / span * TM_HIST_BINS);
        b = b < 0 ? 0 : (b >= TM_HIST_BINS ? TM_HIST_BINS - 1 : b);
        hist[b]++;
        valid++;
    }
    if (!valid) return 1.f;
    size_t lo = valid / 100, hi = valid - valid / 200;    // 1 % abajo, 0,5 % arriba
    double sum = 0; size_t n = 0, seen = 0;
    for (int b = 0; b < TM_HIST_BINS; b++) {
        size_t c = hist[b];
        if (!c) continue;
        size_t from = seen, to = seen + c;               // este bin ocupa [from, to) en el orden
        seen = to;
        size_t a = from < lo ? lo : from, z = to > hi ? hi : to;
        if (z <= a) continue;
        double center = TM_LOG_MIN + (b + 0.5) * span / TM_HIST_BINS;
        sum += center * (double)(z - a);
        n += z - a;
    }
    if (!n) return 1.f;
    float key = exp2f((float)(sum / (double)n));
    float exposure = TM_MIDGRAY / key;
    if (exposure < 1.f / 4096.f) exposure = 1.f / 4096.f;
    if (exposure > 4096.f) exposure = 4096.f;
    return exposure;
}

// tm_to_rgba8: float lineal (channels = 1, 3 o 4; el 4° es alfa) -> RGBA8 sRGB.
// out tiene w*h*4 bytes. Devuelve 1 si hizo falta comprimir rango (HDR de verdad), 0 si no.
static int tm_to_rgba8(const float* px, int channels, int w, int h, unsigned char* out) {
    size_t count = (size_t)w * (size_t)h;
    float mx = 0.f;
    for (size_t i = 0; i < count; i++) {
        const float* p = px + i * (size_t)channels;
        for (int c = 0; c < (channels >= 3 ? 3 : 1); c++)
            if (p[c] > mx && p[c] < 1e30f) mx = p[c];
    }
    tm_table t;
    tm_build(&t);
    int hdr = mx > 1.0001f;
    float exposure = hdr ? tm_auto_exposure(px, count, channels) : 1.f;
    for (size_t i = 0; i < count; i++) {
        const float* p = px + i * (size_t)channels;
        unsigned char* o = out + i * 4;
        float r = p[0], g = channels >= 3 ? p[1] : p[0], b = channels >= 3 ? p[2] : p[0];
        if (hdr) { r = tm_aces(r * exposure); g = tm_aces(g * exposure); b = tm_aces(b * exposure); }
        o[0] = tm_encode(&t, r);
        o[1] = tm_encode(&t, g);
        o[2] = tm_encode(&t, b);
        float a = channels == 4 ? p[3] : 1.f;
        int ai = (int)(a * 255.f + 0.5f);
        o[3] = (unsigned char)(!(a > 0.f) ? 0 : (ai > 255 ? 255 : ai));
    }
    return hdr;
}

// core/pyramid.h — la imagen decodificada en CPU: una piramide de niveles PBGRA.
//
// Nivel k = la imagen a 1/2^k (ceil en cada lado), con la grilla alineada a la del original:
// el pixel x del nivel k cubre los pixeles [x*2^k, (x+1)*2^k) del nivel 0. La piramide baja
// hasta que el lado mayor entra en PYRAMID_MIN_SIDE.
//
// Para que sirve:
//   · Dibujar: en vez de pedirle a Direct2D que achique 48 MP en cada cuadro (el cubico de alta
//     calidad arma su propio mipmap en cada DrawBitmap: 39 fps), se dibuja el nivel que ya
//     tiene la resolucion de pantalla y el cubico achica a lo sumo 2x.
//   · Vista previa: un JPEG se puede decodificar directamente a 1/2, 1/4 o 1/8 (escalado en el
//     dominio DCT, varias veces mas rapido). Eso es exactamente el nivel 1, 2 o 3: la imagen
//     llega con `first` > 0 y los niveles finos se completan despues, si hacen falta.
//   · Ambiente y sombra: se calculan sobre un nivel chico (<= 512 px), no sobre la foto entera.
//
// Los niveles son inmutables y compartidos (shared_ptr<const Level>): cuando la decodificacion
// completa reemplaza a la vista previa, los niveles gruesos se reutilizan tal cual — ni se
// recalculan ni se vuelven a subir a la GPU.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "pixels.h"

namespace lux {

constexpr uint32_t PYRAMID_MIN_SIDE = 256;   // el nivel mas grueso entra en 256 px
constexpr uint32_t EFFECT_MAX_SIDE  = 512;   // entrada del desenfoque ambiente y la sombra

struct Level {
    uint32_t w = 0, h = 0;
    PixelBuf px;                              // PBGRA, stride = w * 4
    size_t bytes() const { return (size_t)w * h * 4; }
};
using LevelPtr = std::shared_ptr<const Level>;

// Lado del nivel k: ceil(full / 2^k), nunca menos de 1.
inline uint32_t levelDim(uint32_t full, int k) {
    const uint64_t d = ((uint64_t)full + ((1ull << k) - 1)) >> k;
    return (uint32_t)std::max<uint64_t>(1, d);
}

// Cantidad de niveles: el ultimo es el primero cuyo lado mayor entra en PYRAMID_MIN_SIDE.
inline int levelCount(uint32_t w, uint32_t h) {
    int n = 1;
    while (std::max(levelDim(w, n - 1), levelDim(h, n - 1)) > PYRAMID_MIN_SIDE) ++n;
    return n;
}

// Lo que se sabe del formato de origen (para la barra de estado).
struct ImageInfo {
    uint32_t bpp = 0;                         // bits por pixel del archivo (0 = no se sabe)
    bool alphaFmt = false;                    // el formato admite transparencia
    bool hdr = false;                         // rango alto comprimido con tone mapping
};

struct CpuImage {
    uint32_t w = 0, h = 0;                    // medidas del nivel 0 (ya orientado)
    int first = 0;                            // primer nivel con pixeles (0 = resolucion completa)
    std::vector<LevelPtr> levels;             // levels[k] vacio para k < first
    bool opaque = true;                       // ningun pixel con alfa < 255
    ImageInfo info;

    int last() const { return (int)levels.size() - 1; }
    bool full() const { return first == 0; }
    const Level* level(int k) const { return (k >= 0 && k < (int)levels.size()) ? levels[k].get() : nullptr; }
    size_t bytes() const {
        size_t n = 0;
        for (const auto& l : levels) if (l) n += l->bytes();
        return n;
    }
    // Nivel de entrada para el ambiente y la sombra: el mas fino que entra en EFFECT_MAX_SIDE.
    int effectLevel() const {
        for (int k = first; k <= last(); ++k)
            if (levels[k] && std::max(levels[k]->w, levels[k]->h) <= EFFECT_MAX_SIDE) return k;
        return last();
    }
};

// Arma una CpuImage a partir de un nivel ya decodificado (el nivel `k0`, con las medidas
// levelDim(w, k0) x levelDim(h, k0)) y calcula los niveles mas gruesos hasta el final, o
// hasta `stopBefore` (exclusivo) si se pasa (para completar solo los finos de una vista previa).
// `cancel` se consulta entre niveles. Devuelve false si se cancelo o falto memoria.
// Costo: sum de los niveles = 4/3 del primero, O(n) en paralelo.
inline bool buildPyramid(CpuImage& img, int k0, int stopBefore, const std::atomic<bool>* cancel) {
    const int count = levelCount(img.w, img.h);
    if (stopBefore < 0 || stopBefore > count) stopBefore = count;
    if ((int)img.levels.size() < count) img.levels.resize(count);
    for (int k = k0 + 1; k < stopBefore; ++k) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const Level* src = img.levels[k - 1].get();
        auto dst = std::make_shared<Level>();
        dst->w = levelDim(img.w, k);
        dst->h = levelDim(img.h, k);
        dst->px = PixelBuf::alloc(dst->bytes());
        if (!dst->px) return false;
        downsample2x(src->px.data(), src->w, src->h, dst->px.data());
        img.levels[k] = std::move(dst);
    }
    return true;
}

// La imagen completa hereda de la vista previa los niveles que esta ya tenia (preview.first y
// mas gruesos): mismos objetos, sin recalcular, y la GPU no los vuelve a subir. `fullImg` trae
// los niveles 0 .. preview.first-1.
inline CpuImage mergeFull(const CpuImage& fullImg, const CpuImage& preview) {
    CpuImage out = fullImg;
    const int count = levelCount(out.w, out.h);
    out.levels.resize(count);
    for (int k = std::max(0, preview.first); k < count && k < (int)preview.levels.size(); ++k)
        if (preview.levels[k]) out.levels[k] = preview.levels[k];
    out.first = 0;
    out.opaque = fullImg.opaque && preview.opaque;
    return out;
}

} // namespace lux

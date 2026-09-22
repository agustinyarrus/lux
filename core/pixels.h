// core/pixels.h — operaciones de CPU sobre pixeles de 32 bits, todas en paralelo.
//
// El formato canonico de Lux en memoria es BGRA premultiplicado (PBGRA): el que Direct2D
// sube a la GPU sin convertir. Aca vive todo lo que lleva un buffer decodificado hasta ese
// formato y lo que se hace despues con el:
//
//   · PixelBuf          buffer malloc'eado SIN inicializar (un std::vector de 192 MB pierde
//                       ~30 ms poniendolo en cero) que adopta sin copiar lo que devuelven
//                       stb, qoi y los decoders propios.
//   · rgbaToPbgra       RGBA recto -> PBGRA en el lugar. SSE2, 4 pixeles por vuelta, con un
//                       atajo cuando los 4 son opacos (el caso de casi todas las fotos).
//   · orient            orientacion EXIF 1..8. 2/3/4 en el lugar; 5..8 son transposiciones
//                       por bloques de 64x64 (los dos buffers quedan en cache).
//   · downsample2x      un escalon de la piramide: promedio de cajas 2x2 en premultiplicado
//                       (lo correcto con alfa) con redondeo exacto, SSE2.
//   · allOpaque         ¿hay algun pixel con alfa < 255?
//
// La premultiplicacion es exacta: round(c * a / 255) = (t + (t >> 8)) >> 8 con t = c*a + 128
// (la identidad de Blinn). tests/test_core.cpp la verifica para los 65 536 pares (c, a).
//
// Header-only, sin Windows: se prueba suelto.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include "pool.h"

namespace lux {

// ---------------------------------------------------------------- buffer
class PixelBuf {
public:
    PixelBuf() = default;
    static PixelBuf alloc(size_t bytes) {
        PixelBuf b;
        b.p_ = (uint8_t*)malloc(bytes ? bytes : 1);
        b.n_ = b.p_ ? bytes : 0;
        return b;
    }
    // Toma posesion de un bloque de malloc (stbi_image_free, QOI_FREE y los decoders propios
    // liberan con free, asi que se adopta tal cual).
    static PixelBuf adopt(void* p, size_t bytes) {
        PixelBuf b;
        b.p_ = (uint8_t*)p;
        b.n_ = p ? bytes : 0;
        return b;
    }
    PixelBuf(PixelBuf&& o) noexcept : p_(o.p_), n_(o.n_) { o.p_ = nullptr; o.n_ = 0; }
    PixelBuf& operator=(PixelBuf&& o) noexcept {
        if (this != &o) { free(p_); p_ = o.p_; n_ = o.n_; o.p_ = nullptr; o.n_ = 0; }
        return *this;
    }
    ~PixelBuf() { free(p_); }
    PixelBuf(const PixelBuf&) = delete;
    PixelBuf& operator=(const PixelBuf&) = delete;

    uint8_t* data() { return p_; }
    const uint8_t* data() const { return p_; }
    size_t size() const { return n_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    uint8_t* p_ = nullptr;
    size_t n_ = 0;
};

constexpr size_t PX_GRAIN = 1u << 16;    // pixeles por bloque de parallelFor (256 KB)

inline size_t rowsGrain(size_t rowPixels) { return std::max<size_t>(1, PX_GRAIN / std::max<size_t>(1, rowPixels)); }

// round(c * a / 255) exacto para c, a en [0, 255].
inline uint8_t mul255(unsigned c, unsigned a) {
    unsigned t = c * a + 128u;
    return (uint8_t)((t + (t >> 8)) >> 8);
}

// ---------------------------------------------------------------- premultiplicar
namespace detail {

// Nucleo SSE2 de 4 pixeles: multiplica B, G y R por su alfa (el alfa por 255, o sea queda
// igual). Entra y sale en BGRA; los 16 bits alcanzan: 255*255 + 128 + 254 < 65 536.
inline __m128i premul4(__m128i v) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i keepBGR = _mm_set_epi16(0, -1, -1, -1, 0, -1, -1, -1);
    const __m128i alpha255 = _mm_set_epi16(255, 0, 0, 0, 255, 0, 0, 0);
    const __m128i c128 = _mm_set1_epi16(128);
    __m128i lo = _mm_unpacklo_epi8(v, zero);
    __m128i hi = _mm_unpackhi_epi8(v, zero);
    __m128i alo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
    __m128i ahi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
    alo = _mm_or_si128(_mm_and_si128(alo, keepBGR), alpha255);
    ahi = _mm_or_si128(_mm_and_si128(ahi, keepBGR), alpha255);
    __m128i tlo = _mm_add_epi16(_mm_mullo_epi16(lo, alo), c128);
    __m128i thi = _mm_add_epi16(_mm_mullo_epi16(hi, ahi), c128);
    tlo = _mm_srli_epi16(_mm_add_epi16(tlo, _mm_srli_epi16(tlo, 8)), 8);
    thi = _mm_srli_epi16(_mm_add_epi16(thi, _mm_srli_epi16(thi, 8)), 8);
    return _mm_packus_epi16(tlo, thi);
}

// Intercambia R y B en cada pixel (RGBA <-> BGRA).
inline __m128i swapRB4(__m128i v) {
    const __m128i maskAG = _mm_set1_epi32((int)0xFF00FF00u);
    const __m128i maskLo = _mm_set1_epi32(0x000000FF);
    __m128i ag = _mm_and_si128(v, maskAG);
    __m128i r = _mm_and_si128(v, maskLo);
    __m128i b = _mm_and_si128(_mm_srli_epi32(v, 16), maskLo);
    return _mm_or_si128(ag, _mm_or_si128(b, _mm_slli_epi32(r, 16)));
}

// Premultiplica en el lugar [p, p + n pixeles). Swizzle = el origen es RGBA (sino ya es BGRA).
// Devuelve true si los n pixeles son opacos.
template <bool Swizzle>
inline bool premulRange(uint8_t* p, size_t n) {
    const __m128i alphaMask = _mm_set1_epi32((int)0xFF000000u);
    bool opaque = true;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i * 4));
        if constexpr (Swizzle) v = swapRB4(v);
        __m128i a = _mm_and_si128(v, alphaMask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(a, alphaMask)) != 0xFFFF) {   // atajo: 4 opacos
            opaque = false;
            v = premul4(v);
        }
        _mm_storeu_si128((__m128i*)(p + i * 4), v);
    }
    for (; i < n; ++i) {
        uint8_t* q = p + i * 4;
        uint8_t cr = Swizzle ? q[0] : q[2], cg = q[1], cb = Swizzle ? q[2] : q[0], a = q[3];
        if (a != 255) {
            opaque = false;
            cr = mul255(cr, a); cg = mul255(cg, a); cb = mul255(cb, a);
        }
        q[0] = cb; q[1] = cg; q[2] = cr; q[3] = a;
    }
    return opaque;
}

} // namespace detail

// RGBA recto (stb, qoi, los decoders propios) -> BGRA premultiplicado, en el lugar.
// Devuelve true si la imagen es completamente opaca. O(n), en paralelo.
inline bool rgbaToPbgra(uint8_t* px, size_t count) {
    std::atomic<bool> opaque{true};
    parallelFor(count, PX_GRAIN, [&](size_t b, size_t e) {
        if (!detail::premulRange<true>(px + b * 4, e - b)) opaque.store(false, std::memory_order_relaxed);
    });
    return opaque.load();
}

// BGRA recto -> BGRA premultiplicado (salida de WIC en 32bppBGRA). O(n), en paralelo.
inline bool bgraToPbgra(uint8_t* px, size_t count) {
    std::atomic<bool> opaque{true};
    parallelFor(count, PX_GRAIN, [&](size_t b, size_t e) {
        if (!detail::premulRange<false>(px + b * 4, e - b)) opaque.store(false, std::memory_order_relaxed);
    });
    return opaque.load();
}

// ¿Todos los pixeles de un buffer PBGRA tienen alfa 255? O(n), en paralelo, corta temprano.
inline bool allOpaque(const uint8_t* px, size_t count) {
    std::atomic<bool> opaque{true};
    parallelFor(count, PX_GRAIN, [&](size_t b, size_t e) {
        if (!opaque.load(std::memory_order_relaxed)) return;
        const __m128i alphaMask = _mm_set1_epi32((int)0xFF000000u);
        __m128i acc = alphaMask;
        size_t i = b;
        for (; i + 4 <= e; i += 4)
            acc = _mm_and_si128(acc, _mm_loadu_si128((const __m128i*)(px + i * 4)));
        bool ok = _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(acc, alphaMask), alphaMask)) == 0xFFFF;
        for (; i < e && ok; ++i) ok = px[i * 4 + 3] == 255;
        if (!ok) opaque.store(false, std::memory_order_relaxed);
    });
    return opaque.load();
}

// Filas de 24 bits BGR (la salida tipica de un JPEG) -> PBGRA opaco. dst sin stride extra.
inline void bgr24ToPbgra(const uint8_t* src, size_t srcStride, uint32_t w, uint32_t h, uint8_t* dst) {
    parallelFor(h, rowsGrain(w), [&](size_t y0, size_t y1) {
        for (size_t y = y0; y < y1; ++y) {
            const uint8_t* s = src + y * srcStride;
            uint8_t* d = dst + y * (size_t)w * 4;
            for (uint32_t x = 0; x < w; ++x, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
        }
    });
}

// Gris de 8 bits -> PBGRA opaco.
inline void gray8ToPbgra(const uint8_t* src, size_t srcStride, uint32_t w, uint32_t h, uint8_t* dst) {
    parallelFor(h, rowsGrain(w), [&](size_t y0, size_t y1) {
        for (size_t y = y0; y < y1; ++y) {
            const uint8_t* s = src + y * srcStride;
            uint8_t* d = dst + y * (size_t)w * 4;
            for (uint32_t x = 0; x < w; ++x, d += 4) { d[0] = d[1] = d[2] = s[x]; d[3] = 255; }
        }
    });
}

// ---------------------------------------------------------------- orientacion EXIF
//
// La etiqueta dice donde quedaron la fila 0 y la columna 0 guardadas respecto de la imagen
// que hay que mostrar (TIFF 6.0, "Orientation"). Un pixel guardado en (x, y), con la imagen
// de w x h, va a parar a (X, Y):
//
//   1  (x, y)                 identidad
//   2  (w-1-x, y)             espejo horizontal
//   3  (w-1-x, h-1-y)         180
//   4  (x, h-1-y)             espejo vertical
//   5  (y, x)                 transpuesta               (sale de h x w)
//   6  (h-1-y, x)             90 horario                (sale de h x w)
//   7  (h-1-y, w-1-x)         transversa                (sale de h x w)
//   8  (y, w-1-x)             90 antihorario            (sale de h x w)
inline bool orientSwapsAxes(int o) { return o >= 5 && o <= 8; }

namespace detail {
constexpr uint32_t ORIENT_BLOCK = 64;   // 64x64 px = 16 KB por lado: los dos buffers entran en L1/L2

inline void orientInPlace(uint32_t* p, uint32_t w, uint32_t h, int o) {
    if (o == 2) {            // cada fila al reves
        parallelFor(h, rowsGrain(w), [&](size_t y0, size_t y1) {
            for (size_t y = y0; y < y1; ++y) std::reverse(p + y * w, p + y * w + w);
        });
    } else if (o == 3) {     // todo el arreglo al reves = 180
        const size_t n = (size_t)w * h, half = n / 2;
        parallelFor(half, PX_GRAIN, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) std::swap(p[i], p[n - 1 - i]);
        });
    } else if (o == 4) {     // filas espejadas
        parallelFor(h / 2, rowsGrain(w), [&](size_t y0, size_t y1) {
            for (size_t y = y0; y < y1; ++y) std::swap_ranges(p + y * w, p + y * w + w, p + (size_t)(h - 1 - y) * w);
        });
    }
}

// 5..8: out tiene h x w. Las cuatro son "columna del origen -> fila del destino", con o sin
// inversion en cada eje:  revX: X = h-1-y (6, 7)   revY: Y = w-1-x (7, 8).
//
// Reparto por FRANJAS DEL DESTINO: cada hilo es dueño de 64 filas del resultado (= 64 columnas
// del origen) y es el unico que las toca. Asi el primer acceso a esas paginas (el SO las pone
// en cero bajo demanda: ~49 000 fallos de pagina para 192 MB) queda repartido entre los hilos
// sin que dos se peleen por la misma pagina.
// Dentro de la franja, bloques de 4x4 transpuestos en registros SSE2 (4 lecturas y 4
// escrituras de 16 bytes en vez de 16 de 4).
inline void orientTranspose(const uint32_t* s, uint32_t w, uint32_t h, int o, uint32_t* d) {
    const size_t W = h;                                  // ancho del resultado
    const bool revX = (o == 6 || o == 7), revY = (o == 7 || o == 8);
    auto dst = [&](uint32_t x, uint32_t y) -> uint32_t* {
        const size_t X = revX ? (size_t)(h - 1 - y) : y;
        const size_t Y = revY ? (size_t)(w - 1 - x) : x;
        return d + Y * W + X;
    };
    const uint32_t B = ORIENT_BLOCK;
    const size_t bands = (w + B - 1) / B;                // franjas de columnas del origen
    parallelFor(bands, 1, [&](size_t b0, size_t b1) {
        for (size_t band = b0; band < b1; ++band) {
            const uint32_t x0 = (uint32_t)(band * B), x1 = std::min(w, x0 + B);
            uint32_t y = 0;
            for (; y + 4 <= h; y += 4) {
                const uint32_t* r = s + (size_t)y * w;
                uint32_t x = x0;
                for (; x + 4 <= x1; x += 4) {
                    __m128i a = _mm_loadu_si128((const __m128i*)(r + x));
                    __m128i b = _mm_loadu_si128((const __m128i*)(r + w + x));
                    __m128i c = _mm_loadu_si128((const __m128i*)(r + 2 * (size_t)w + x));
                    __m128i e = _mm_loadu_si128((const __m128i*)(r + 3 * (size_t)w + x));
                    __m128i t0 = _mm_unpacklo_epi32(a, b), t1 = _mm_unpacklo_epi32(c, e);
                    __m128i t2 = _mm_unpackhi_epi32(a, b), t3 = _mm_unpackhi_epi32(c, e);
                    __m128i col[4] = { _mm_unpacklo_epi64(t0, t1), _mm_unpackhi_epi64(t0, t1),
                                       _mm_unpacklo_epi64(t2, t3), _mm_unpackhi_epi64(t2, t3) };
                    for (uint32_t j = 0; j < 4; ++j) {   // col[j] = columna x+j, filas y..y+3
                        __m128i v = revX ? _mm_shuffle_epi32(col[j], _MM_SHUFFLE(0, 1, 2, 3)) : col[j];
                        _mm_storeu_si128((__m128i*)dst(x + j, revX ? y + 3 : y), v);
                    }
                }
                for (; x < x1; ++x)
                    for (uint32_t k = 0; k < 4; ++k) *dst(x, y + k) = r[(size_t)k * w + x];
            }
            for (; y < h; ++y)
                for (uint32_t x = x0; x < x1; ++x) *dst(x, y) = s[(size_t)y * w + x];
        }
    });
}
} // namespace detail

// Aplica la orientacion EXIF `o` a un buffer de w x h pixeles de 32 bits. Devuelve el buffer
// resultante (el mismo para 1..4) y sus medidas en ow/oh. Si no hay memoria para la
// transpuesta, degrada con gracia: devuelve la imagen sin girar.
inline PixelBuf orient(PixelBuf src, uint32_t w, uint32_t h, int o, uint32_t& ow, uint32_t& oh) {
    ow = w; oh = h;
    if (!src || o < 2 || o > 8) return src;
    if (!orientSwapsAxes(o)) {
        detail::orientInPlace((uint32_t*)src.data(), w, h, o);
        return src;
    }
    PixelBuf out = PixelBuf::alloc((size_t)w * h * 4);
    if (!out) return src;
    detail::orientTranspose((const uint32_t*)src.data(), w, h, o, (uint32_t*)out.data());
    ow = h; oh = w;
    return out;
}

// ---------------------------------------------------------------- piramide: un escalon
//
// dst = ceil(sw/2) x ceil(sh/2). Cada pixel es el promedio de su caja 2x2 en premultiplicado,
// redondeado: (s + 2) >> 2. Una columna o fila impar replica el borde, que es lo mismo que
// promediar solo lo que existe. El resultado mantiene c <= a (la suma es monotona).
// O(sw * sh), SSE2 de a 2 pixeles de salida, en paralelo por filas.
inline void downsample2x(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst) {
    const uint32_t dw = (sw + 1) / 2, dh = (sh + 1) / 2;
    const size_t sstride = (size_t)sw * 4, dstride = (size_t)dw * 4;
    const uint32_t pairs = sw / 2;                       // salidas con dos columnas de origen
    parallelFor(dh, rowsGrain(dw), [&](size_t y0, size_t y1) {
        const __m128i zero = _mm_setzero_si128();
        const __m128i two = _mm_set1_epi16(2);
        for (size_t y = y0; y < y1; ++y) {
            const uint8_t* r0 = src + (2 * y) * sstride;
            const uint8_t* r1 = (2 * y + 1 < sh) ? r0 + sstride : r0;
            uint8_t* o = dst + y * dstride;
            uint32_t x = 0;
            for (; x + 2 <= pairs; x += 2) {
                __m128i a = _mm_loadu_si128((const __m128i*)(r0 + (size_t)x * 8));
                __m128i b = _mm_loadu_si128((const __m128i*)(r1 + (size_t)x * 8));
                __m128i slo = _mm_add_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero));
                __m128i shi = _mm_add_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero));
                __m128i sum = _mm_add_epi16(_mm_unpacklo_epi64(slo, shi), _mm_unpackhi_epi64(slo, shi));
                sum = _mm_srli_epi16(_mm_add_epi16(sum, two), 2);
                _mm_storel_epi64((__m128i*)(o + (size_t)x * 4), _mm_packus_epi16(sum, sum));
            }
            for (; x < pairs; ++x) {
                const uint8_t *p = r0 + (size_t)x * 8, *q = r1 + (size_t)x * 8;
                for (int c = 0; c < 4; ++c) o[x * 4 + c] = (uint8_t)((p[c] + p[4 + c] + q[c] + q[4 + c] + 2) >> 2);
            }
            if (sw & 1) {                                // ultima columna sola
                const uint8_t *p = r0 + (size_t)(sw - 1) * 4, *q = r1 + (size_t)(sw - 1) * 4;
                for (int c = 0; c < 4; ++c) o[(size_t)pairs * 4 + c] = (uint8_t)((p[c] + q[c] + 1) >> 1);
            }
        }
    });
}

} // namespace lux

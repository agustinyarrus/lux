// core/tiles.h — geometria del dibujo por mosaicos: que nivel, que mosaicos y donde cortar.
//
// Una imagen de 24 000 px no entra en un solo bitmap de GPU (el maximo es 16 384 por lado) y
// aunque entrara, subirla entera para mostrarla a 1400 px es desperdicio. Cada nivel de la
// piramide se parte en mosaicos de TILE_CONTENT px, que se suben a la GPU solo cuando se ven.
//
// Sin costuras: al dibujar a escala fraccionaria el filtro cubico de un mosaico necesita los
// pixeles del vecino. Por eso cada bitmap lleva TILE_MARGIN px de mas del vecino (solo en los
// lados que tienen vecino), y se dibuja recortado a su propia celda con un clip ALINEADO A
// PIXEL: las celdas de dos mosaicos contiguos comparten exactamente el mismo borde redondeado,
// asi que cada pixel de pantalla lo pinta un solo mosaico, completo, con el filtro viendo
// pixeles verdaderos del otro lado. El margen (8 px) supera el radio del cubico (2 px) aun
// achicando 2x, que es lo maximo que achica un nivel.
//
// Todo en funciones puras: tests/test_core.cpp verifica que las celdas particionan la fila de
// pixeles sin huecos ni solapes para escalas y origenes al azar.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lux {

constexpr uint32_t TILE_CONTENT = 2048;   // lado util de un mosaico (16 MB por mosaico lleno)
constexpr uint32_t TILE_MARGIN  = 8;      // pixeles prestados del vecino a cada lado interior

struct TileGrid {
    uint32_t lw = 0, lh = 0;              // medidas del nivel
    uint32_t content = TILE_CONTENT;
    uint32_t margin = TILE_MARGIN;
    int cols = 0, rows = 0;
};

// content se achica si la GPU tiene un maximo de bitmap menor (hardware muy viejo).
inline TileGrid makeGrid(uint32_t lw, uint32_t lh, uint32_t maxBitmap = 16384) {
    TileGrid g;
    g.lw = lw; g.lh = lh;
    g.margin = TILE_MARGIN;
    g.content = std::min<uint32_t>(TILE_CONTENT, maxBitmap > 2 * TILE_MARGIN + 64 ? maxBitmap - 2 * TILE_MARGIN : 64);
    g.cols = (int)((lw + g.content - 1) / g.content);
    g.rows = (int)((lh + g.content - 1) / g.content);
    return g;
}

struct TileRect {
    uint32_t x0, y0, x1, y1;              // celda propia [x0, x1) x [y0, y1)
    uint32_t bx0, by0, bx1, by1;          // lo que va al bitmap: la celda + margenes interiores
};

inline TileRect tileRect(const TileGrid& g, int tx, int ty) {
    TileRect r;
    r.x0 = (uint32_t)tx * g.content; r.x1 = std::min(g.lw, r.x0 + g.content);
    r.y0 = (uint32_t)ty * g.content; r.y1 = std::min(g.lh, r.y0 + g.content);
    r.bx0 = tx > 0 ? r.x0 - g.margin : 0;
    r.by0 = ty > 0 ? r.y0 - g.margin : 0;
    r.bx1 = std::min(g.lw, r.x1 + (tx + 1 < g.cols ? g.margin : 0));
    r.by1 = std::min(g.lh, r.y1 + (ty + 1 < g.rows ? g.margin : 0));
    return r;
}

// Nivel a dibujar para una escala (px de pantalla por px del original): el mas grueso cuya
// resolucion todavia es >= la de pantalla, o sea escala relativa en (0.5, 1]. Acotado a los
// niveles que existen: si solo hay vista previa (first > 0) se usa esa, ampliada.
inline int idealLevel(double scale, int last) {
    int k = 0;
    while (k < last && scale * std::ldexp(1.0, k + 1) <= 1.0 + 1e-9) ++k;
    return k;
}
inline int chooseLevel(double scale, int first, int last) {
    return std::max(first, idealLevel(scale, last));
}

// Rango [a, b) de mosaicos que cortan la ventana [v0, v1) de pantalla en un eje.
// o = origen de la imagen en pantalla, s = px de pantalla por px del nivel.
struct Span { int a = 0, b = 0; };
inline Span visibleSpan(double o, double s, double v0, double v1, uint32_t content, int count) {
    Span sp;
    if (s <= 0 || count <= 0) return sp;
    const double l0 = (v0 - o) / s, l1 = (v1 - o) / s;          // ventana en px del nivel
    sp.a = (int)std::clamp(std::floor(l0 / content), 0.0, (double)count);
    sp.b = (int)std::clamp(std::ceil(l1 / content), 0.0, (double)count);
    if (sp.b < sp.a) sp.b = sp.a;
    return sp;
}

// Borde de pantalla de una costura interior: el pixel entero mas cercano. Dos mosaicos vecinos
// usan el mismo valor (uno como derecha, el otro como izquierda), asi que se tocan sin hueco.
inline double seamPx(double o, double s, uint32_t levelCoord) {
    return std::floor(o + (double)levelCoord * s + 0.5);
}

// Celda de recorte de un mosaico en un eje: [lo, hi). Los bordes exteriores de la imagen no se
// recortan (quedan en +-inf: los acota el clip del escenario) para que el borde de la foto
// conserve su antialiasing.
struct Cut { double lo, hi; };
inline Cut cellCut(double o, double s, uint32_t c0, uint32_t c1, bool firstTile, bool lastTile) {
    Cut c;
    c.lo = firstTile ? -1e9 : seamPx(o, s, c0);
    c.hi = lastTile ? 1e9 : seamPx(o, s, c1);
    return c;
}

} // namespace lux

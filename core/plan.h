// core/plan.h — que decodificar y en que orden, segun lo que se ve y hacia donde se navega.
//
// La politica, en una funcion pura (se prueba sin ventana ni hilos):
//
//   · Hay un pedido pendiente (el usuario apreto una flecha y esa imagen todavia no esta):
//     solo importan ella (prioridad 0) y la siguiente en la misma direccion. Mientras se
//     navega no se gasta nada en lo que queda atras.
//   · No hay pedido pendiente: precargar alrededor de lo que se ve, primero hacia donde se
//     viene navegando (+1), despues la anterior (-1) y la de dos pasos (+2). Si lo que se ve es
//     una vista previa (JPEG decodificado a 1/2..1/8), la resolucion completa va detras de la
//     siguiente — o primera de todas si el zoom ya la esta pidiendo.
//
// Las vistas previas cuestan poco (un JPEG de 48 MP a 1/4 se decodifica varias veces mas
// rapido y ocupa 1/16 de memoria), asi que precargar vecinas es casi gratis.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace lux {

enum class JobKind : uint8_t { Preview, Full };

struct Want {
    int index;
    JobKind kind;
    int priority;                          // 0 = ya; mas alto = puede esperar
};

inline int wrapIndex(int i, int n) { return n > 0 ? ((i % n) + n) % n : 0; }

// ¿x queda en el camino de `from` a `to` avanzando en `dir`? (sin contar from, contando to)
// Sirve para no cancelar la decodificacion de una imagen intermedia cuando se mantiene
// apretada la flecha: si termina antes que la pedida, se muestra (efecto "hojear").
inline bool onPath(int n, int from, int x, int to, int dir) {
    if (n <= 0 || dir == 0 || from < 0 || to < 0) return false;
    const int s = dir < 0 ? -1 : 1;
    const int dx = wrapIndex((x - from) * s, n), dt = wrapIndex((to - from) * s, n);
    return dx > 0 && dx <= dt;
}

// displayed: indice en pantalla (-1 = nada); target: pedido pendiente (-1 = ninguno);
// dir: direccion de la ultima navegacion; displayedFull: lo que se ve ya tiene el nivel 0;
// zoomNeedsFull: el zoom actual necesita niveles que la vista previa no tiene.
// Devuelve los pedidos ordenados por prioridad, sin indices repetidos.
inline std::vector<Want> planJobs(int n, int displayed, int target, int dir,
                                  bool displayedFull, bool zoomNeedsFull) {
    std::vector<Want> out;
    if (n <= 0) return out;
    dir = dir < 0 ? -1 : 1;
    auto add = [&](int idx, JobKind kind, int prio) {
        idx = wrapIndex(idx, n);
        for (auto& w : out) {
            if (w.index != idx) continue;
            if (kind == JobKind::Full) w.kind = JobKind::Full;
            w.priority = std::min(w.priority, prio);
            return;
        }
        out.push_back(Want{ idx, kind, prio });
    };
    auto neighbor = [&](int idx, int prio) {
        if (wrapIndex(idx, n) != wrapIndex(displayed, n)) add(idx, JobKind::Preview, prio);
    };
    if (target >= 0) {
        add(target, JobKind::Preview, 0);
        if (n > 1 && wrapIndex(target + dir, n) != wrapIndex(target, n)) add(target + dir, JobKind::Preview, 1);
    } else if (displayed >= 0) {
        if (!displayedFull) add(displayed, JobKind::Full, zoomNeedsFull ? 0 : 2);
        neighbor(displayed + dir, 1);
        neighbor(displayed - dir, 3);
        neighbor(displayed + 2 * dir, 4);
    }
    std::stable_sort(out.begin(), out.end(), [](const Want& a, const Want& b) { return a.priority < b.priority; });
    return out;
}

} // namespace lux

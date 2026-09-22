// core/pool.h — pool de hilos persistente y parallelFor con reparto dinamico de bloques.
//
// Lo usan las etapas de CPU que escalan con los nucleos: premultiplicar, orientar, armar la
// piramide y buscar alfa. Crear hilos en cada llamada cuesta ~50-100 us por hilo; con un pool
// fijo el costo de despachar es un push a una cola.
//
// Reparto: [0, n) se corta en bloques de `grain` y cada participante toma el siguiente con un
// fetch_add sobre un contador compartido (work sharing dinamico). Un bloque lento no deja a
// los demas mirando, y el hilo que llama trabaja tambien: si el pool esta ocupado con otro
// parallelFor (dos decodificadores a la vez), el que llama termina solo lo suyo — nunca hay
// deadlock ni espera circular.
//
// Header-only y sin Windows: se prueba suelto desde tests/test_core.cpp.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lux {

class Pool {
public:
    // Singleton perezoso: tantos hilos como nucleos logicos menos uno (el que llama es el otro).
    // Se construye la primera vez que alguien lo pide y NO se destruye (se filtra a proposito):
    // al cerrar el proceso los hilos quedan dormidos en su cola y ExitProcess los termina, sin
    // carreras entre destructores estaticos y trabajadores que todavia corren.
    static Pool& get() {
        static Pool* pool = new Pool(std::max(1u, std::thread::hardware_concurrency()) - 1);
        return *pool;
    }

    explicit Pool(unsigned threads) {
        for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    unsigned threads() const { return (unsigned)workers_.size(); }

    void submit(std::function<void()> task) {
        { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(task)); }
        cv_.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;          // stop_ y nada pendiente
                task = std::move(q_.front());
                q_.pop_front();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// parallelFor(n, grain, fn): llama fn(begin, end) sobre bloques disjuntos que cubren [0, n).
// fn tiene que ser segura para correr en paralelo sobre bloques distintos y no tirar
// excepciones. Vuelve cuando TODOS los bloques terminaron.
// Costo: O(n) de trabajo repartido + O(bloques) de despacho.
template <class Fn>
void parallelFor(size_t n, size_t grain, Fn&& fn, Pool& pool = Pool::get()) {
    if (n == 0) return;
    grain = std::max<size_t>(1, grain);
    const size_t chunks = (n + grain - 1) / grain;
    if (chunks == 1 || pool.threads() == 0) { fn((size_t)0, n); return; }

    // Estado compartido por shared_ptr: un ayudante que arranca tarde (con todo ya hecho) lo
    // sigue encontrando vivo aunque el que llamo ya haya vuelto. fn solo se invoca para
    // bloques pendientes, y el que llama no vuelve hasta que el ultimo bloque termino, asi
    // que las referencias que fn capture del stack del llamador nunca quedan colgando.
    struct State {
        std::atomic<size_t> next{0}, done{0};
        size_t chunks = 0, n = 0, grain = 0;
        std::function<void(size_t, size_t)> fn;
        std::mutex m;
        std::condition_variable cv;
    };
    auto st = std::make_shared<State>();
    st->chunks = chunks; st->n = n; st->grain = grain;
    st->fn = [&fn](size_t b, size_t e) { fn(b, e); };

    auto run = [](State& s) {
        for (;;) {
            size_t c = s.next.fetch_add(1, std::memory_order_relaxed);
            if (c >= s.chunks) return;
            size_t b = c * s.grain, e = std::min(s.n, b + s.grain);
            s.fn(b, e);
            if (s.done.fetch_add(1, std::memory_order_acq_rel) + 1 == s.chunks) {
                std::lock_guard<std::mutex> lk(s.m);   // evita el "lost wakeup" del que espera
                s.cv.notify_all();
            }
        }
    };
    const size_t helpers = std::min<size_t>(chunks - 1, pool.threads());
    for (size_t i = 0; i < helpers; ++i) pool.submit([st, run] { run(*st); });
    run(*st);
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait(lk, [&] { return st->done.load(std::memory_order_acquire) == st->chunks; });
}

} // namespace lux

// core/cache.h — cache LRU con presupuesto en bytes y entradas fijadas.
//
// Guarda las imagenes ya decodificadas (la que se ve, las vecinas precargadas y las que se
// vieron hace poco). Lista doblemente enlazada + tabla hash: get/put en O(1); recortar cuesta
// O(desalojadas + fijadas salteadas). Las fijadas (la imagen en pantalla y la pedida) nunca se
// desalojan aunque solas superen el presupuesto: una foto de 400 MB se muestra igual, y lo que
// se va es todo lo demas.
#pragma once
#include <cstddef>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace lux {

template <class K, class V, class Hash = std::hash<K>>
class LruCache {
public:
    explicit LruCache(size_t budget) : budget_(budget) {}

    void setBudget(size_t b) { budget_ = b; }
    size_t budget() const { return budget_; }
    size_t bytes() const { return used_; }
    size_t size() const { return index_.size(); }

    // Inserta o reemplaza y la deja como la mas reciente. No recorta: eso lo decide trim().
    void put(const K& k, V v, size_t bytes) {
        auto it = index_.find(k);
        if (it != index_.end()) {
            used_ -= it->second->bytes;
            it->second->val = std::move(v);
            it->second->bytes = bytes;
            order_.splice(order_.begin(), order_, it->second);
        } else {
            order_.push_front(Node{ k, std::move(v), bytes });
            index_.emplace(k, order_.begin());
        }
        used_ += bytes;
    }

    // Busca y marca como usada recien. nullptr si no esta.
    V* get(const K& k) {
        auto it = index_.find(k);
        if (it == index_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->val;
    }

    // Busca sin tocar el orden.
    const V* peek(const K& k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &it->second->val;
    }

    bool erase(const K& k) {
        auto it = index_.find(k);
        if (it == index_.end()) return false;
        used_ -= it->second->bytes;
        order_.erase(it->second);
        index_.erase(it);
        return true;
    }

    // Desaloja de la menos reciente a la mas reciente hasta entrar en el presupuesto,
    // salteando las que isPinned(key) marque. Devuelve cuantas desalojo.
    template <class Pinned>
    size_t trim(Pinned&& isPinned) {
        size_t evicted = 0;
        auto it = order_.end();
        while (used_ > budget_ && it != order_.begin()) {
            --it;
            if (isPinned(it->key)) continue;
            used_ -= it->bytes;
            index_.erase(it->key);
            it = order_.erase(it);
            ++evicted;
        }
        return evicted;
    }

    template <class Fn>
    void forEach(Fn&& fn) const { for (const auto& n : order_) fn(n.key, n.val, n.bytes); }

private:
    struct Node { K key; V val; size_t bytes; };
    std::list<Node> order_;                                         // frente = mas reciente
    std::unordered_map<K, typename std::list<Node>::iterator, Hash> index_;
    size_t budget_ = 0, used_ = 0;
};

} // namespace lux

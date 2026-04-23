#pragma once
#include "core/types.h"
#include <unordered_map>
#include <typeindex>
#include <any>
#include <vector>
#include <functional>

// Minimal ECS-lite entity/component registry.
// Components are stored as type-erased maps: type → (EntityID → component).

class EntityManager {
public:
    // first_local_id: the first ID used by create(). Set to a high value on
    // the client so locally-created entities (dropped items, etc.) never
    // share IDs with server-replicated entities (which start at 1).
    explicit EntityManager(EntityID first_local_id = 1)
        : m_next_id(first_local_id) {}

    EntityID create();
    void     destroy(EntityID id);
    bool     alive(EntityID id) const;

    // Adopt an externally-created entity ID as alive in this manager.
    // Used when transferring entities between grids (vehicle ↔ map) to
    // preserve the ID while moving components across EntityManagers.
    // Advances m_next_id so future create() calls never collide with this ID.
    void adopt(EntityID id) { m_alive[id] = true; if (id >= m_next_id) m_next_id = id + 1; }

    template<typename T>
    T& add_component(EntityID id, T component = {}) {
        auto& store = m_stores[std::type_index(typeid(T))];
        store[id] = std::move(component);
        return std::any_cast<T&>(store[id]);
    }

    template<typename T>
    T* get_component(EntityID id) {
        auto it = m_stores.find(std::type_index(typeid(T)));
        if (it == m_stores.end()) return nullptr;
        auto jt = it->second.find(id);
        if (jt == it->second.end()) return nullptr;
        return &std::any_cast<T&>(jt->second);
    }

    template<typename T>
    bool has_component(EntityID id) const {
        auto it = m_stores.find(std::type_index(typeid(T)));
        if (it == m_stores.end()) return false;
        return it->second.count(id) > 0;
    }

    template<typename T>
    void remove_component(EntityID id) {
        auto it = m_stores.find(std::type_index(typeid(T)));
        if (it != m_stores.end()) it->second.erase(id);
    }

    // Iterate all entities that have component T
    template<typename T>
    void each(std::function<void(EntityID, T&)> fn) {
        auto it = m_stores.find(std::type_index(typeid(T)));
        if (it == m_stores.end()) return;
        for (auto& [id, val] : it->second)
            fn(id, std::any_cast<T&>(val));
    }

    std::vector<EntityID> all_entities() const;

    // Register a callback that fires whenever an entity is destroyed.
    void on_destroy(std::function<void(EntityID)> cb)
    { m_on_destroy_cbs.push_back(std::move(cb)); }

private:
    EntityID m_next_id = 1;
    std::unordered_map<EntityID, bool> m_alive;
    std::unordered_map<std::type_index,
        std::unordered_map<EntityID, std::any>> m_stores;
    std::vector<std::function<void(EntityID)>> m_on_destroy_cbs;
};

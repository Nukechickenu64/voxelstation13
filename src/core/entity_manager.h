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
    EntityID create();
    void     destroy(EntityID id);
    bool     alive(EntityID id) const;

    // Adopt an externally-created entity ID as alive in this manager.
    // Used when transferring entities between grids (vehicle ↔ map) to
    // preserve the ID while moving components across EntityManagers.
    void adopt(EntityID id) { m_alive[id] = true; }

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

private:
    EntityID m_next_id = 1;
    std::unordered_map<EntityID, bool> m_alive;
    std::unordered_map<std::type_index,
        std::unordered_map<EntityID, std::any>> m_stores;
};

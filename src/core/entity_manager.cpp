#include "core/entity_manager.h"

EntityID EntityManager::create()
{
    EntityID id = m_next_id++;
    m_alive[id] = true;
    return id;
}

void EntityManager::destroy(EntityID id)
{
    for (auto& cb : m_on_destroy_cbs)
        cb(id);
    m_alive.erase(id);
    for (auto& [ti, store] : m_stores)
        store.erase(id);
}

bool EntityManager::alive(EntityID id) const
{
    auto it = m_alive.find(id);
    return it != m_alive.end() && it->second;
}

std::vector<EntityID> EntityManager::all_entities() const
{
    std::vector<EntityID> out;
    out.reserve(m_alive.size());
    for (auto& [id, live] : m_alive)
        if (live) out.push_back(id);
    return out;
}

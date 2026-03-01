#include "core/master_controller.h"
#include <algorithm>
#include <cassert>

// ── ProcessingList ────────────────────────────────────────────────────────────

void ProcessingList::start(EntityID id, ProcessFn fn)
{
    if (m_ticking) {
        // Queue for post-tick insertion
        m_pending_add.push_back({ id, std::move(fn) });
        return;
    }

    auto it = m_idx.find(id);
    if (it != m_idx.end()) {
        // Replace existing callback
        m_entries[it->second].fn = std::move(fn);
        return;
    }

    m_idx[id] = static_cast<int>(m_entries.size());
    m_entries.push_back({ id, std::move(fn) });
}

void ProcessingList::stop(EntityID id)
{
    if (m_ticking) {
        m_pending_remove.push_back(id);
        return;
    }

    auto it = m_idx.find(id);
    if (it == m_idx.end()) return;

    int  idx  = it->second;
    int  last = static_cast<int>(m_entries.size()) - 1;

    if (idx != last) {
        // Swap with last to avoid O(n) shifts
        std::swap(m_entries[idx], m_entries[last]);
        m_idx[m_entries[idx].id] = idx;
    }

    m_entries.pop_back();
    m_idx.erase(it);
}

bool ProcessingList::is_running(EntityID id) const
{
    return m_idx.count(id) > 0;
}

void ProcessingList::tick(double dt)
{
    m_ticking = true;
    for (auto& e : m_entries)
        if (e.fn) e.fn(e.id, dt);
    m_ticking = false;
    flush_pending();
}

void ProcessingList::flush_pending()
{
    for (EntityID id : m_pending_remove)
        stop(id);
    m_pending_remove.clear();

    for (auto& e : m_pending_add)
        start(e.id, std::move(e.fn));
    m_pending_add.clear();
}

// ── MasterController ──────────────────────────────────────────────────────────

ProcessingList& MasterController::list(std::string_view name)
{
    return m_lists[std::string(name)];
}

const ProcessingList* MasterController::find_list(std::string_view name) const
{
    auto it = m_lists.find(std::string(name));
    return it != m_lists.end() ? &it->second : nullptr;
}

void MasterController::start_processing(std::string_view list_name,
                                         EntityID id, ProcessFn fn)
{
    list(list_name).start(id, std::move(fn));
}

void MasterController::stop_processing(std::string_view list_name, EntityID id)
{
    list(list_name).stop(id);
}

bool MasterController::is_processing(std::string_view list_name, EntityID id) const
{
    const ProcessingList* pl = find_list(list_name);
    return pl && pl->is_running(id);
}

void MasterController::tick(double dt)
{
    // Canonical TG order
    for (const char* name : { SS_LIVING, SS_MOBS, SS_OBJECTS, SS_ITEMS })
        tick_list(name, dt);
}

void MasterController::tick_list(std::string_view name, double dt)
{
    auto it = m_lists.find(std::string(name));
    if (it != m_lists.end())
        it->second.tick(dt);
}

void MasterController::purge(EntityID id)
{
    for (auto& [name, plist] : m_lists)
        plist.purge(id);
}

// ── Global singleton ──────────────────────────────────────────────────────────
static MasterController* g_mc = nullptr;

void init_master_controller()
{
    static MasterController instance;
    g_mc = &instance;
}

MasterController& master_controller()
{
    assert(g_mc && "init_master_controller() not called");
    return *g_mc;
}

MasterController& MasterController::get()
{
    return master_controller();
}

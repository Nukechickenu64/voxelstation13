#include "core/signals.h"
#include <algorithm>
#include <cassert>

// ── SignalBus implementation ──────────────────────────────────────────────────

SignalHandle SignalBus::register_signal(EntityID entity, std::string_view signal,
                                        SignalHandler fn)
{
    SignalHandle handle = m_next_handle++;
    m_by_handle[handle] = Registration{ entity, std::string(signal), std::move(fn) };
    m_index[entity][std::string(signal)].push_back(handle);
    return handle;
}

void SignalBus::unregister_signal(SignalHandle handle)
{
    auto it = m_by_handle.find(handle);
    if (it == m_by_handle.end()) return;

    const Registration& reg = it->second;
    // Remove from index
    auto eit = m_index.find(reg.entity);
    if (eit != m_index.end()) {
        auto& sig_map = eit->second;
        auto sit = sig_map.find(reg.signal);
        if (sit != sig_map.end()) {
            auto& vec = sit->second;
            auto hit = std::find(vec.begin(), vec.end(), handle);
            if (hit != vec.end()) vec.erase(hit);
            if (vec.empty())  sig_map.erase(sit);
        }
        if (sig_map.empty()) m_index.erase(eit);
    }

    m_by_handle.erase(it);
}

SignalResult SignalBus::send_signal(EntityID entity, std::string_view signal,
                                     SignalArgs args) const
{
    SignalResult result = 0;

    auto eit = m_index.find(entity);
    if (eit == m_index.end()) return result;

    auto sit = eit->second.find(std::string(signal));
    if (sit == eit->second.end()) return result;

    // Copy handle list — registration may change during dispatch
    std::vector<SignalHandle> handles = sit->second;
    for (SignalHandle h : handles) {
        auto fit = m_by_handle.find(h);
        if (fit == m_by_handle.end()) continue; // was unregistered mid-dispatch
        result |= fit->second.fn(entity, args);
        if (result & COMPONENT_SIGNAL_CANCEL) break;
    }

    return result;
}

void SignalBus::purge_entity(EntityID entity)
{
    auto eit = m_index.find(entity);
    if (eit == m_index.end()) return;

    for (auto& [sig, handles] : eit->second)
        for (SignalHandle h : handles)
            m_by_handle.erase(h);

    m_index.erase(eit);
}

// ── Global singleton ──────────────────────────────────────────────────────────
static SignalBus* g_signals = nullptr;

void init_signals()
{
    static SignalBus instance;
    g_signals = &instance;
}

SignalBus& signals()
{
    assert(g_signals && "init_signals() not called");
    return *g_signals;
}

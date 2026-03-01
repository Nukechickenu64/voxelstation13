#include "simulation/reagents.h"
#include "simulation/status_effects.h"
#include "simulation/mob_system.h"   // HealthComponent
#include <SDL3/SDL.h>
#include <algorithm>
#include <cassert>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  ReagentContainer
// ─────────────────────────────────────────────────────────────────────────────

float ReagentContainer::total_volume() const
{
    float total = 0.f;
    for (const auto& r : m_reagents) total += r.volume;
    return total;
}

float ReagentContainer::add(std::string_view id, float amount)
{
    amount = std::min(amount, free_space());
    if (amount <= 0.f) return 0.f;

    auto* slot = find(id);
    if (slot) {
        slot->volume += amount;
    } else {
        m_reagents.push_back({ std::string(id), amount });
    }
    return amount;
}

float ReagentContainer::remove(std::string_view id, float amount)
{
    auto* slot = find(id);
    if (!slot || slot->volume <= 0.f) return 0.f;

    float removed = std::min(amount, slot->volume);
    slot->volume -= removed;
    if (slot->volume < 1e-4f) {
        m_reagents.erase(std::remove_if(m_reagents.begin(), m_reagents.end(),
            [&id](const Reagent& r){ return r.id == id; }),
            m_reagents.end());
    }
    return removed;
}

void ReagentContainer::clear()
{
    m_reagents.clear();
}

float ReagentContainer::transfer_to(std::string_view id, float amount,
                                     ReagentContainer& dst)
{
    float removed = remove(id, amount);
    if (removed > 0.f) dst.add(id, removed);
    return removed;
}

void ReagentContainer::splash_to(float amount, ReagentContainer& dst)
{
    float total = total_volume();
    if (total <= 0.f) return;

    // Proportional split
    std::vector<std::pair<std::string, float>> to_move;
    for (const auto& r : m_reagents)
        to_move.push_back({ r.id, (r.volume / total) * amount });

    for (auto& [id, vol] : to_move)
        transfer_to(id, vol, dst);
}

const Reagent* ReagentContainer::find(std::string_view id) const
{
    for (const auto& r : m_reagents)
        if (r.id == id) return &r;
    return nullptr;
}

Reagent* ReagentContainer::find(std::string_view id)
{
    for (auto& r : m_reagents)
        if (r.id == id) return &r;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ReagentRegistry
// ─────────────────────────────────────────────────────────────────────────────

ReagentRegistry& ReagentRegistry::get()
{
    static ReagentRegistry instance;
    return instance;
}

bool ReagentRegistry::register_def(ReagentDef def)
{
    if (m_defs.count(def.id)) return false;
    m_defs.emplace(def.id, std::move(def));
    return true;
}

const ReagentDef* ReagentRegistry::find(std::string_view id) const
{
    auto it = m_defs.find(std::string(id));
    return it != m_defs.end() ? &it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Built-in reagent definitions (mirrors TG's /code/modules/reagents/)
// ─────────────────────────────────────────────────────────────────────────────
void ReagentRegistry::init_defaults()
{
    auto& reg = ReagentRegistry::get();

    auto def = [&](const char* id, const char* name) -> ReagentDef& {
        static ReagentDef d;
        d = ReagentDef{};
        d.id   = id;
        d.name = name;
        return d;
    };
    (void)def; // suppress unused if not used

    // ── Healing reagents ──────────────────────────────────────────────────────
    { ReagentDef d; d.id="bicaridine"; d.name="Bicaridine";
      d.metabolise_rate=0.2f; d.overdose_threshold=30.f; d.heal_brute=1.f;
      reg.register_def(d); }

    { ReagentDef d; d.id="kelotane"; d.name="Kelotane";
      d.metabolise_rate=0.2f; d.overdose_threshold=30.f; d.heal_burn=1.f;
      reg.register_def(d); }

    { ReagentDef d; d.id="dexalin"; d.name="Dexalin";
      d.metabolise_rate=0.2f; d.overdose_threshold=30.f; d.heal_oxy=1.5f;
      reg.register_def(d); }

    { ReagentDef d; d.id="dylovene"; d.name="Dylovene";
      d.metabolise_rate=0.2f; d.overdose_threshold=30.f; d.heal_tox=1.f;
      reg.register_def(d); }

    { ReagentDef d; d.id="epinephrine"; d.name="Epinephrine";
      d.metabolise_rate=0.2f; d.overdose_threshold=30.f;
      d.heal_brute=0.2f; d.heal_burn=0.2f; d.heal_oxy=0.5f; d.heal_tox=0.2f;
      reg.register_def(d); }

    // ── Harmful base chemicals ────────────────────────────────────────────────
    { ReagentDef d; d.id="choral_hydrate"; d.name="Chloral Hydrate";
      d.metabolise_rate=0.2f; d.overdose_threshold=15.f;
      d.tox_damage=0.5f; d.knockdown_time=1.f;
      reg.register_def(d); }

    { ReagentDef d; d.id="lexorin"; d.name="Lexorin";
      d.metabolise_rate=0.05f; d.oxy_damage=4.f;
      reg.register_def(d); }

    // ── Stimulants / drugs ────────────────────────────────────────────────────
    { ReagentDef d; d.id="caffeine"; d.name="Caffeine";
      d.metabolise_rate=0.1f; d.overdose_threshold=50.f; d.jitter_time=0.1f;
      reg.register_def(d); }

    { ReagentDef d; d.id="space_drugs"; d.name="Space Drugs";
      d.metabolise_rate=0.1f; d.overdose_threshold=20.f;
      d.dizzy_time=0.5f; d.drowsy_time=0.1f;
      reg.register_def(d); }

    // ── Alien substances ──────────────────────────────────────────────────────
    { ReagentDef d; d.id="plasma"; d.name="Plasma";
      d.metabolise_rate=0.1f; d.tox_damage=2.f;
      reg.register_def(d); }

    // ── Neutral / carrier ─────────────────────────────────────────────────────
    { ReagentDef d; d.id="water"; d.name="Water";
      d.metabolise_rate=0.4f;
      reg.register_def(d); }

    { ReagentDef d; d.id="blood"; d.name="Blood";
      d.metabolise_rate=0.05f;
      reg.register_def(d); }

    { ReagentDef d; d.id="iron"; d.name="Iron";
      d.metabolise_rate=0.3f;
      reg.register_def(d); }
}

void init_reagents()
{
    ReagentRegistry::init_defaults();
}

// ─────────────────────────────────────────────────────────────────────────────
//  metabolise_reagents — called once per server tick per mob
// ─────────────────────────────────────────────────────────────────────────────
void metabolise_reagents(EntityID mob, EntityManager& entities,
                          SignalBus& signals, double dt)
{
    auto* rcc = entities.get_component<ReagentContainerComponent>(mob);
    if (!rcc) return;

    auto* hp  = entities.get_component<HealthComponent>(mob);
    auto* status = entities.get_component<StatusEffectsComponent>(mob);

    ReagentContainer& blood = rcc->container;
    const float fdt = static_cast<float>(dt);

    // Build list of reagents present this tick (copy; we mutate as we go)
    std::vector<Reagent> snapshot = blood.reagents();

    for (const Reagent& slot : snapshot) {
        if (slot.volume <= 0.f) continue;

        const ReagentDef* def = ReagentRegistry::get().find(slot.id);
        if (!def) {
            // Unknown reagent: drain it silently
            blood.remove(slot.id, def ? def->metabolise_rate * fdt : 0.1f * fdt);
            continue;
        }

        // ── Apply health effects (scaled by presence × dt) ─────────────────
        if (hp && !hp->dead) {
            float scale = slot.volume * fdt;
            if (def->oxy_damage   > 0.f) hp->apply("oxy",   def->oxy_damage   * scale);
            if (def->tox_damage   > 0.f) hp->apply("tox",   def->tox_damage   * scale);
            if (def->brute_damage > 0.f) hp->apply("brute", def->brute_damage * scale);
            if (def->burn_damage  > 0.f) hp->apply("burn",  def->burn_damage  * scale);

            if (def->heal_brute > 0.f) hp->apply("brute", -def->heal_brute * scale);
            if (def->heal_burn  > 0.f) hp->apply("burn",  -def->heal_burn  * scale);
            if (def->heal_oxy   > 0.f) hp->apply("oxy",   -def->heal_oxy   * scale);
            if (def->heal_tox   > 0.f) hp->apply("tox",   -def->heal_tox   * scale);

            // Death check after heals/damages
            if (hp->current() <= 0.f && !hp->dead) {
                hp->dead = true;
                signals.send_signal(mob, COMSIG_MOB_LIVING_DEATH,
                    SigDeath{ NULL_ENTITY });
            } else if (hp->current() > 0.f && hp->dead) {
                hp->dead = false;
                signals.send_signal(mob, COMSIG_MOB_LIVING_REVIVE, SignalArgs{});
            }
        }

        // ── Apply status effects ───────────────────────────────────────────
        if (status) {
            // Each effect scales by slot.volume (stronger dose = longer stun)
            float vol_scale = std::min(slot.volume / 10.f, 2.f); // cap at 2× volume mult

            if (def->stun_time      > 0.f)
                status->apply(StatusEffectType::Stun,
                               def->stun_time * vol_scale * fdt, 1.f);
            if (def->knockdown_time > 0.f)
                status->apply(StatusEffectType::Knockdown,
                               def->knockdown_time * vol_scale * fdt, 1.f);
            if (def->drowsy_time    > 0.f)
                status->apply(StatusEffectType::Drowsy,
                               def->drowsy_time * vol_scale * fdt, 1.f);
            if (def->slowdown       > 0.f)
                status->apply(StatusEffectType::Slowdown,
                               fdt, def->slowdown);
            if (def->jitter_time    > 0.f)
                status->apply(StatusEffectType::Jitter,
                               def->jitter_time * fdt, 1.f);
            if (def->dizzy_time     > 0.f)
                status->apply(StatusEffectType::Dizzy,
                               def->dizzy_time * fdt, 1.f);
        }

        // ── Overdose check ─────────────────────────────────────────────────
        if (def->overdose_threshold > 0.f) {
            auto& od_level = rcc->overdose_levels[slot.id];
            od_level += def->metabolise_rate * fdt;
            if (od_level >= def->overdose_threshold) {
                signals.send_signal(mob, COMSIG_REAGENT_OVERDOSE,
                    SigReagentChanged{ slot.id, od_level });
                // Overdose tox damage
                if (hp) hp->apply("tox", 2.f * fdt);
                SDL_Log("[reagents] %u overdosed on %s (%.1f units)",
                        mob, slot.id.c_str(), od_level);
            }
        }

        // ── Drain the reagent ─────────────────────────────────────────────
        float drain = def->metabolise_rate * fdt;
        bool was_present = blood.find(slot.id) != nullptr;
        blood.remove(slot.id, drain);
        bool now_gone = blood.find(slot.id) == nullptr;

        if (was_present && now_gone) {
            signals.send_signal(mob, COMSIG_REAGENT_REMOVED,
                SigReagentChanged{ slot.id, 0.f });
        }
    }

    // Emit health-update signal so UI can refresh
    if (hp)
        signals.send_signal(mob, COMSIG_MOB_HEALTHUPDATE, SignalArgs{});
}

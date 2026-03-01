#include "simulation/status_effects.h"
#include <algorithm>
#include <cmath>

// ── String names ─────────────────────────────────────────────────────────────
std::string_view status_effect_name(StatusEffectType t)
{
    switch (t) {
        case StatusEffectType::Stun:      return "stun";
        case StatusEffectType::Paralysis: return "paralysis";
        case StatusEffectType::Knockdown: return "knockdown";
        case StatusEffectType::Slowdown:  return "slowdown";
        case StatusEffectType::Drowsy:    return "drowsy";
        case StatusEffectType::Blind:     return "blind";
        case StatusEffectType::Deaf:      return "deaf";
        case StatusEffectType::Confusion: return "confusion";
        case StatusEffectType::Silence:   return "silence";
        case StatusEffectType::Jitter:    return "jitter";
        case StatusEffectType::Dizzy:     return "dizzy";
        default:                          return "unknown";
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────
bool StatusEffectsComponent::has(StatusEffectType t) const
{
    for (const auto& e : effects)
        if (e.type == t) return true;
    return false;
}

const ActiveStatusEffect* StatusEffectsComponent::get(StatusEffectType t) const
{
    for (const auto& e : effects)
        if (e.type == t) return &e;
    return nullptr;
}

// ── Modification ──────────────────────────────────────────────────────────────
void StatusEffectsComponent::apply(StatusEffectType t, float duration, float strength)
{
    for (auto& e : effects) {
        if (e.type != t) continue;
        // TG "grouped" semantics: keep highest values
        e.duration = std::max(e.duration, duration);
        e.strength = std::max(e.strength, strength);
        return;
    }
    effects.push_back({ t, duration, strength });
}

void StatusEffectsComponent::remove(StatusEffectType t)
{
    effects.erase(
        std::remove_if(effects.begin(), effects.end(),
                       [t](const ActiveStatusEffect& e) { return e.type == t; }),
        effects.end());
}

void StatusEffectsComponent::clear()
{
    effects.clear();
}

// ── Tick ──────────────────────────────────────────────────────────────────────
MobState StatusEffectsComponent::tick(double dt)
{
    const float fdt = static_cast<float>(dt);

    // Drain durations and expire effects
    effects.erase(
        std::remove_if(effects.begin(), effects.end(),
            [fdt](ActiveStatusEffect& e) {
                if (e.duration < 0.f) return false; // permanent
                e.duration -= fdt;
                // Drowsy gradually accumulates into Knockdown — handled below
                return e.duration <= 0.f;
            }),
        effects.end());

    // Drowsy → Knockdown conversion (TG: prolonged drowsiness causes collapse)
    auto* drowsy = const_cast<ActiveStatusEffect*>(get(StatusEffectType::Drowsy));
    if (drowsy) {
        // Reuse strength as an accumulator for drowsy exposure time
        drowsy->strength += fdt;
        constexpr float DROWSY_COLLAPSE_THRESHOLD = 10.f; // seconds
        if (drowsy->strength >= DROWSY_COLLAPSE_THRESHOLD) {
            apply(StatusEffectType::Knockdown, 3.f, 1.f);
            remove(StatusEffectType::Drowsy);
        }
    }

    // Determine MobState from highest-priority active effect
    if (has(StatusEffectType::Stun) || has(StatusEffectType::Paralysis))
        return MobState::Hardcrit;

    if (has(StatusEffectType::Knockdown))
        return MobState::Softcrit;

    return MobState::Normal;
}

// ── current_mob_state (non-advancing snapshot) ───────────────────────────────
MobState StatusEffectsComponent::current_mob_state() const
{
    if (has(StatusEffectType::Stun) || has(StatusEffectType::Paralysis))
        return MobState::Hardcrit;
    if (has(StatusEffectType::Knockdown))
        return MobState::Softcrit;
    return MobState::Normal;
}

// ── Speed multiplier ──────────────────────────────────────────────────────────
float StatusEffectsComponent::speed_multiplier() const
{
    float mult = 1.f;
    for (const auto& e : effects) {
        if (e.type == StatusEffectType::Slowdown)
            mult -= e.strength; // strength 1.0 = full stop
        else if (e.type == StatusEffectType::Drowsy)
            mult -= 0.3f;       // static drowsy penalty
    }
    return std::clamp(mult, 0.f, 1.f);
}

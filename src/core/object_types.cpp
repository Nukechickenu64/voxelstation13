#include "core/object_types.h"
#include <algorithm>
#include <unordered_map>
#include <string>

// ── Custom parent overrides ───────────────────────────────────────────────────
// For type paths whose parent is NOT simply "everything before the last slash".
// e.g. /obj → /atom/movable  (not /atom via simple trim)
static std::unordered_map<std::string, std::string>& parent_overrides()
{
    static std::unordered_map<std::string, std::string> s_overrides;
    return s_overrides;
}

void register_type_parent(std::string_view type_path, std::string_view parent_path)
{
    parent_overrides()[std::string(type_path)] = std::string(parent_path);
}

// ── Path utilities ────────────────────────────────────────────────────────────

std::string parent_type(std::string_view path)
{
    // Check explicit override table first
    auto it = parent_overrides().find(std::string(path));
    if (it != parent_overrides().end()) return it->second;

    // Default: trim the last path segment
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos || pos == 0) {
        // Single-segment path (e.g. "/atom") — no parent
        return {};
    }
    return std::string(path.substr(0, pos));
}

bool istype(std::string_view derived, std::string_view ancestor)
{
    if (derived == ancestor) return true;
    std::string_view cur = derived;
    // Walk up the hierarchy until we find ancestor or exhaust the chain
    for (;;) {
        std::string p = parent_type(cur);
        if (p.empty()) return false;
        if (p == ancestor) return true;
        cur = p;
        // Prevent infinite loops from bad override data
        if (cur == derived) return false;
    }
}

std::vector<std::string> type_ancestry(std::string_view path)
{
    std::vector<std::string> chain;
    std::string cur(path);
    while (!cur.empty()) {
        chain.push_back(cur);
        std::string p = parent_type(cur);
        if (p == cur || p.empty()) break;
        cur = std::move(p);
    }
    return chain;
}

std::string_view type_short(std::string_view path)
{
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return path;
    return path.substr(pos + 1);
}

// ── PrototypeRegistry ─────────────────────────────────────────────────────────

bool PrototypeRegistry::register_prototype(PrototypeDef def)
{
    auto [it, inserted] = m_defs.emplace(def.type_path, std::move(def));
    return inserted;
}

const PrototypeDef* PrototypeRegistry::get(std::string_view type_path) const
{
    auto it = m_defs.find(std::string(type_path));
    return it != m_defs.end() ? &it->second : nullptr;
}

const PrototypeDef* PrototypeRegistry::resolve_weight(std::string_view type_path) const
{
    for (const auto& p : type_ancestry(type_path)) {
        auto it = m_defs.find(p);
        if (it != m_defs.end() && it->second.weight >= 0.f) return &it->second;
    }
    return nullptr;
}

const PrototypeDef* PrototypeRegistry::resolve_volume(std::string_view type_path) const
{
    for (const auto& p : type_ancestry(type_path)) {
        auto it = m_defs.find(p);
        if (it != m_defs.end() && it->second.volume >= 0.f) return &it->second;
    }
    return nullptr;
}

const PrototypeDef* PrototypeRegistry::resolve_stack_max(std::string_view type_path) const
{
    for (const auto& p : type_ancestry(type_path)) {
        auto it = m_defs.find(p);
        if (it != m_defs.end() && it->second.stack_max >= 0) return &it->second;
    }
    return nullptr;
}

std::vector<std::string> PrototypeRegistry::resolve_tags(std::string_view type_path) const
{
    std::vector<std::string> result;
    for (const auto& p : type_ancestry(type_path)) {
        auto it = m_defs.find(p);
        if (it == m_defs.end()) continue;
        for (const auto& tag : it->second.tags) {
            if (std::find(result.begin(), result.end(), tag) == result.end())
                result.push_back(tag);
        }
    }
    return result;
}

// ── Global singleton ──────────────────────────────────────────────────────────

PrototypeRegistry& prototype_registry()
{
    static PrototypeRegistry s_registry;
    return s_registry;
}

void init_prototype_registry()
{
    // ── Canonical parent overrides ──
    // The top-level atom classes have special canonical parents that deviate
    // from simple path trimming.
    register_type_parent("/obj",           "/atom/movable");
    register_type_parent("/mob",           "/atom/movable");
    register_type_parent("/turf",          "/atom");
    register_type_parent("/area",          "/atom");
    register_type_parent("/atom/movable",  "/atom");

    // ── Built-in prototype defaults ──
    // These mirror TG SS13 default var values for common type categories.
    // Item defs loaded from JSON may override any of these.
    auto& reg = prototype_registry();

    // /atom — base of everything
    reg.register_prototype({ "/atom",                    "",             -1.f, -1.f, -1,   {} });
    reg.register_prototype({ "/atom/movable",            "",             -1.f, -1.f, -1,   {} });

    // /obj/item — generic pick-up-able item defaults
    reg.register_prototype({ "/obj",                     "Object",        0.1f,  0.5f,  1,   {} });
    reg.register_prototype({ "/obj/item",                "Item",          0.1f,  0.5f,  1,   {} });

    // /obj/item/tool — hand tools
    reg.register_prototype({ "/obj/item/tool",           "Tool",          0.3f,  1.0f,  1,   {"tool"} });

    // /obj/item/clothing — wearables
    reg.register_prototype({ "/obj/item/clothing",       "Clothing",      0.5f,  2.0f,  1,   {"clothing"} });
    reg.register_prototype({ "/obj/item/clothing/head",  "Headwear",      0.3f,  1.5f,  1,   {"clothing", "head"} });
    reg.register_prototype({ "/obj/item/clothing/suit",  "Suit",          2.0f,  5.0f,  1,   {"clothing", "suit"} });
    reg.register_prototype({ "/obj/item/clothing/gloves","Gloves",        0.2f,  1.0f,  1,   {"clothing", "gloves"} });
    reg.register_prototype({ "/obj/item/clothing/shoes", "Shoes",         0.5f,  2.0f,  1,   {"clothing", "shoes"} });
    reg.register_prototype({ "/obj/item/clothing/mask",  "Mask",          0.2f,  1.0f,  1,   {"clothing", "mask"} });

    // /obj/item/storage — container items (bags, boxes)
    reg.register_prototype({ "/obj/item/storage",        "Storage",       1.0f,  4.0f,  1,   {"bag"} });

    // /obj/item/stack — stackable items
    reg.register_prototype({ "/obj/item/stack",          "Stack",         0.05f, 0.2f, 50,   {"stack"} });

    // /obj/item/medical — medical supplies
    reg.register_prototype({ "/obj/item/medical",        "Medical",       0.1f,  0.5f,  1,   {"medical"} });

    // /obj/item/weapon — weapons
    reg.register_prototype({ "/obj/item/weapon",         "Weapon",        0.8f,  2.0f,  1,   {"weapon"} });

    // /obj/structure — non-carryable world structures
    reg.register_prototype({ "/obj/structure",           "Structure",    -1.f,  -1.f,   1,   {} });

    // /obj/machinery — powered machines
    reg.register_prototype({ "/obj/machinery",           "Machine",      -1.f,  -1.f,   1,   {"machinery"} });
    reg.register_prototype({ "/obj/machinery/power",     "Power Machine",-1.f,  -1.f,   1,   {"machinery", "power"} });

    // /mob — living entities
    reg.register_prototype({ "/mob",                     "Mob",          -1.f,  -1.f,   1,   {} });
    reg.register_prototype({ "/mob/living",              "Living",       -1.f,  -1.f,   1,   {} });
    reg.register_prototype({ "/mob/living/carbon",       "Carbon Mob",   -1.f,  -1.f,   1,   {} });
    reg.register_prototype({ "/mob/living/carbon/human", "Human",        -1.f,  -1.f,   1,   {} });

    // /turf — map tiles
    reg.register_prototype({ "/turf",                    "Turf",         -1.f,  -1.f,   1,   {} });
    reg.register_prototype({ "/turf/floor",              "Floor",        -1.f,  -1.f,   1,   {} });
    reg.register_prototype({ "/turf/wall",               "Wall",         -1.f,  -1.f,   1,   {} });

    // /area — gameplay regions
    reg.register_prototype({ "/area",                    "Area",         -1.f,  -1.f,   1,   {} });
}

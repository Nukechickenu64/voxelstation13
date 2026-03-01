#include "inventory/item_registry.h"
#include "core/object_types.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

bool ItemRegistry::load_directory(const std::string& path)
{
    bool ok = true;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.path().extension() == ".json")
            ok &= load_file(entry.path().string());
    }
    // After all files are loaded, wire up prototype inheritance.
    resolve_prototypes();
    return ok;
}

bool ItemRegistry::load_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("ItemRegistry: cannot open %s", path.c_str());
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        SDL_Log("ItemRegistry: JSON parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    // Support a single object or an array of items
    if (j.is_array()) {
        bool ok = true;
        for (auto& item : j) ok &= load_from_json(item, path);
        return ok;
    }
    return load_from_json(j, path);
}

bool ItemRegistry::load_from_json(const nlohmann::json& j, const std::string& src)
{
    try {
        ItemDef def;
        def.id        = j.at("id").get<std::string>();
        def.name      = j.at("name").get<std::string>();

        // ── Prototype parent ──────────────────────────────────────────────────
        // "parent": "some_item_id" — inherit all unset fields from that item
        // (BYOND-style prototype chain).  Resolved after all files are loaded.
        def.parent_id = j.value("parent", "");

        // ── Explicit-field tracking ───────────────────────────────────────────
        // Each field is only loaded AND marked as explicitly set when the key
        // is actually present in the JSON object.  Fields absent from JSON keep
        // their C++ defaults and may be inherited from the parent in resolve_prototypes().

        if (j.contains("icon")) {
            def.icon = j["icon"].get<std::string>();
            def.explicit_json_fields |= ItemDef::EJF_ICON;
        }

        if (j.contains("weight")) {
            def.weight = j["weight"].get<float>();
            def.explicit_json_fields |= ItemDef::EJF_WEIGHT;
        }

        if (j.contains("volume")) {
            def.volume = j["volume"].get<float>();
            def.explicit_json_fields |= ItemDef::EJF_VOLUME;
        }

        if (j.contains("stack_max")) {
            def.stack_max = j["stack_max"].get<int>();
            def.explicit_json_fields |= ItemDef::EJF_STACK_MAX;
        }

        if (j.contains("is_container")) {
            def.is_container = j["is_container"].get<bool>();
            def.explicit_json_fields |= ItemDef::EJF_IS_CONTAINER;
        }

        if (j.contains("container_volume")) {
            def.container_volume = j["container_volume"].get<float>();
            def.explicit_json_fields |= ItemDef::EJF_CONTAINER_VOLUME;
        }

        if (j.contains("equip_slot")) {
            def.equip_slot = j["equip_slot"].get<std::string>();
            def.explicit_json_fields |= ItemDef::EJF_EQUIP_SLOT;
        }

        if (j.contains("two_handed")) {
            def.two_handed = j["two_handed"].get<bool>();
            def.explicit_json_fields |= ItemDef::EJF_TWO_HANDED;
        }

        if (j.contains("places_voxel")) {
            def.places_voxel = j["places_voxel"].get<std::string>();
            def.explicit_json_fields |= ItemDef::EJF_PLACES_VOXEL;
        }

        // ── Type-path prototype system ────────────────────────────────────────
        if (j.contains("type_path")) {
            def.type_path = j["type_path"].get<std::string>();
            def.explicit_json_fields |= ItemDef::EJF_TYPE_PATH;
        }

        if (j.contains("tags")) {
            for (const auto& t : j["tags"])
                def.tags.push_back(t.get<std::string>());
            def.explicit_json_fields |= ItemDef::EJF_TAGS;
        }

        if (j.contains("container_accepts_tags")) {
            for (const auto& t : j["container_accepts_tags"])
                def.container_accepts_tags.push_back(t.get<std::string>());
            def.explicit_json_fields |= ItemDef::EJF_CONTAINER_ACCEPTS_TAGS;
        }

        if (j.contains("verbs")) {
            for (const auto& v : j["verbs"]) {
                ItemVerb verb;
                verb.name    = v.at("name").get<std::string>();
                verb.range   = v.value("range", 1.5f);
                verb.handler = v.value("handler", "");
                def.verbs.push_back(std::move(verb));
            }
            def.explicit_json_fields |= ItemDef::EJF_VERBS;
        }

        m_defs[def.id] = std::move(def);
        return true;
    } catch (const json::exception& e) {
        SDL_Log("ItemRegistry: error parsing item in %s: %s", src.c_str(), e.what());
        return false;
    }
}

// ── resolve_parent_chain ──────────────────────────────────────────────────────
// Recursively applies BYOND-style field inheritance from a def's parent_id.
// Depth-first: the parent's own chain is resolved before the child inherits,
// so multi-level chains (C→B→A) propagate correctly in one pass.
// Circular references are broken on detection with an SDL_Log warning.
static void resolve_parent_chain(
    ItemDef&                                   def,
    std::unordered_map<std::string, ItemDef>&  all_defs,
    std::unordered_set<std::string>&           in_progress)
{
    if (def.parent_id.empty()) return;

    // Cycle guard: if we're already in the process of resolving this def's
    // chain, its parent_id creates a loop — sever the link.
    if (in_progress.count(def.id)) {
        SDL_Log("ItemRegistry: circular parent chain at '%s' — breaking link",
                def.id.c_str());
        def.parent_id.clear();
        return;
    }

    auto it = all_defs.find(def.parent_id);
    if (it == all_defs.end()) {
        SDL_Log("ItemRegistry: parent '%s' not found for '%s'",
                def.parent_id.c_str(), def.id.c_str());
        def.parent_id.clear();
        return;
    }

    ItemDef& parent = it->second;

    // Recursively resolve the parent's own chain first (depth-first).
    if (!parent.parent_id.empty()) {
        in_progress.insert(def.id);
        resolve_parent_chain(parent, all_defs, in_progress);
        in_progress.erase(def.id);
    }

    // ── Scalar / string fields: inherit when not explicitly set in child ──────
    if (!(def.explicit_json_fields & ItemDef::EJF_ICON) && !parent.icon.empty())
        def.icon = parent.icon;
    if (!(def.explicit_json_fields & ItemDef::EJF_WEIGHT))
        def.weight = parent.weight;
    if (!(def.explicit_json_fields & ItemDef::EJF_VOLUME))
        def.volume = parent.volume;
    if (!(def.explicit_json_fields & ItemDef::EJF_STACK_MAX))
        def.stack_max = parent.stack_max;
    if (!(def.explicit_json_fields & ItemDef::EJF_IS_CONTAINER) && parent.is_container)
        def.is_container = parent.is_container;
    if (!(def.explicit_json_fields & ItemDef::EJF_CONTAINER_VOLUME) && parent.container_volume > 0.f)
        def.container_volume = parent.container_volume;
    if (!(def.explicit_json_fields & ItemDef::EJF_EQUIP_SLOT) && !parent.equip_slot.empty())
        def.equip_slot = parent.equip_slot;
    if (!(def.explicit_json_fields & ItemDef::EJF_TWO_HANDED) && parent.two_handed)
        def.two_handed = parent.two_handed;
    if (!(def.explicit_json_fields & ItemDef::EJF_PLACES_VOXEL) && !parent.places_voxel.empty())
        def.places_voxel = parent.places_voxel;
    if (!(def.explicit_json_fields & ItemDef::EJF_CONTAINER_ACCEPTS_TAGS) &&
            !parent.container_accepts_tags.empty())
        def.container_accepts_tags = parent.container_accepts_tags;

    // ── Verb list: merge parent verbs; child overrides by display name ────────
    // Mirrors BYOND: child verbs shadow parent verbs sharing the same name.
    if (!parent.verbs.empty()) {
        if (!(def.explicit_json_fields & ItemDef::EJF_VERBS)) {
            // Child has no verbs in JSON — inherit parent's full list.
            def.verbs = parent.verbs;
        } else {
            // Child declares some verbs — start from parent list, override by name.
            std::vector<ItemVerb> merged = parent.verbs;
            for (const auto& cv : def.verbs) {
                auto fit = std::find_if(merged.begin(), merged.end(),
                    [&](const ItemVerb& pv) { return pv.name == cv.name; });
                if (fit != merged.end()) *fit = cv;  // override same-named verb
                else merged.push_back(cv);           // add new verb not in parent
            }
            def.verbs = std::move(merged);
        }
    }

    // ── Tags: additive merge ──────────────────────────────────────────────────
    // Parent tags are appended when not already present in the child.
    for (const auto& ptag : parent.tags) {
        bool found = false;
        for (const auto& ctag : def.tags) if (ctag == ptag) { found = true; break; }
        if (!found) def.tags.push_back(ptag);
    }

    // ── Type path: seed from parent's category prefix when child has none ─────
    // The tag-based derivation below may overwrite this; it's only a hint.
    if (!(def.explicit_json_fields & ItemDef::EJF_TYPE_PATH) &&
            def.type_path.empty() && !parent.type_path.empty()) {
        auto slash = parent.type_path.rfind('/');
        if (slash != std::string::npos)
            def.type_path = parent.type_path.substr(0, slash) + "/" + def.id;
    }
}

void ItemRegistry::resolve_prototypes()
{
    // ── Pass 0: BYOND-style parent-chain field inheritance ────────────────────
    // Walk every def that declares a parent_id and copy unset fields from the
    // parent chain (depth-first so ancestors are processed before descendants).
    {
        std::unordered_set<std::string> in_progress;
        for (auto& [id, def] : m_defs)
            if (!def.parent_id.empty())
                resolve_parent_chain(def, m_defs, in_progress);
    }

    auto& proto_reg = prototype_registry();

    for (auto& [id, def] : m_defs) {
        // ── 1. Build a type_path if the JSON didn't supply one ──────────────
        if (def.type_path.empty()) {
            // Infer category from tags
            bool is_tool     = false, is_clothing = false, is_storage = false,
                 is_stack    = false, is_medical  = false, is_weapon  = false;
            for (const auto& tag : def.tags) {
                if (tag == "tool")     is_tool     = true;
                if (tag == "clothing") is_clothing = true;
                if (tag == "bag")      is_storage  = true;
                if (tag == "stack")    is_stack    = true;
                if (tag == "medical")  is_medical  = true;
                if (tag == "weapon")   is_weapon   = true;
            }

            // Items with an equip_slot are always clothing/equipment regardless
            // of whether they carry the "clothing" tag.
            const bool has_slot = !def.equip_slot.empty();
            if (has_slot) is_clothing = true;

            if      (is_tool)     def.type_path = "/obj/item/tool/"     + id;
            else if (is_clothing) {
                // Map the equip_slot name to a sub-category when available.
                if      (def.equip_slot == "suit"  || def.equip_slot == "back")
                    def.type_path = "/obj/item/clothing/suit/" + id;
                else if (def.equip_slot == "head")
                    def.type_path = "/obj/item/clothing/head/" + id;
                else if (def.equip_slot == "gloves")
                    def.type_path = "/obj/item/clothing/gloves/" + id;
                else if (def.equip_slot == "shoes")
                    def.type_path = "/obj/item/clothing/shoes/" + id;
                else if (def.equip_slot == "mask")
                    def.type_path = "/obj/item/clothing/mask/" + id;
                else
                    def.type_path = "/obj/item/clothing/" + id;
            }
            else if (is_storage)  def.type_path = "/obj/item/storage/"  + id;
            else if (is_stack)    def.type_path = "/obj/item/stack/"     + id;
            else if (is_medical)  def.type_path = "/obj/item/medical/"   + id;
            else if (is_weapon)   def.type_path = "/obj/item/weapon/"    + id;
            else                  def.type_path = "/obj/item/"           + id;
        }

        // ── 2. Inherit tags from prototype ancestry ──────────────────────────
        // Collect ancestor tags that aren't already on this def.
        auto ancestor_tags = proto_reg.resolve_tags(def.type_path);
        for (const auto& tag : ancestor_tags) {
            bool found = false;
            for (const auto& t : def.tags) if (t == tag) { found = true; break; }
            if (!found) def.tags.push_back(tag);
        }

        // ── 3. Set prototype_parent pointer ─────────────────────────────────
        // Walk ancestry and find the first parent that has an ItemDef entry.
        def.prototype_parent = nullptr;
        std::string par = parent_type(def.type_path);
        while (!par.empty()) {
            // Check if any ItemDef has this type_path as its own
            for (auto& [oid, odef] : m_defs) {
                if (&odef == &def) continue;
                if (odef.type_path == par) {
                    def.prototype_parent = &odef;
                    goto done_parent;
                }
            }
            par = parent_type(par);
        }
        done_parent:;
    }
}

const ItemDef* ItemRegistry::get(const std::string& id) const
{
    auto it = m_defs.find(id);
    return it != m_defs.end() ? &it->second : nullptr;
}

bool ItemRegistry::has(const std::string& id) const
{
    return m_defs.count(id) > 0;
}

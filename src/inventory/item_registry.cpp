#include "inventory/item_registry.h"
#include "core/object_types.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
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
        def.id               = j.at("id").get<std::string>();
        def.name             = j.at("name").get<std::string>();
        def.icon             = j.value("icon", "");
        def.weight           = j.value("weight", 0.1f);
        def.volume           = j.value("volume", 0.5f);
        def.stack_max        = j.value("stack_max", 1);
        def.is_container     = j.value("is_container", false);
        def.container_volume = j.value("container_volume", 0.f);
        def.equip_slot       = j.value("equip_slot", "");
        def.two_handed       = j.value("two_handed", false);

        // ── Type-path prototype system ───────────────────────────────────────
        // Load an explicit type_path if provided, else derive from tags or id.
        if (j.contains("type_path")) {
            def.type_path = j["type_path"].get<std::string>();
        }
        // type_path will be finalised in resolve_prototypes() if still empty.

        if (j.contains("tags"))
            for (const auto& t : j["tags"])
                def.tags.push_back(t.get<std::string>());

        if (j.contains("container_accepts_tags"))
            for (const auto& t : j["container_accepts_tags"])
                def.container_accepts_tags.push_back(t.get<std::string>());

        if (j.contains("verbs"))
            for (const auto& v : j["verbs"]) {
                ItemVerb verb;
                verb.name    = v.at("name").get<std::string>();
                verb.range   = v.value("range", 1.5f);
                verb.handler = v.value("handler", "");
                def.verbs.push_back(std::move(verb));
            }

        m_defs[def.id] = std::move(def);
        return true;
    } catch (const json::exception& e) {
        SDL_Log("ItemRegistry: error parsing item in %s: %s", src.c_str(), e.what());
        return false;
    }
}

void ItemRegistry::resolve_prototypes()
{
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

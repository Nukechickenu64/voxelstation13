// ui_item_icons.cpp — implements UIRenderer::load_item_icons / item_icon.
// Kept in its own TU so that ui_renderer.cpp never has to include the heavy
// nlohmann/json.hpp that item_registry.h pulls in.
#include "render/ui_renderer.h"
#include "inventory/item_registry.h"

void UIRenderer::load_item_icons(const ItemRegistry& reg, const char* texture_dir)
{
    m_item_icons.clear();
    for (const auto& [id, def] : reg.all()) {
        if (def.icon.empty()) continue;
        std::string path = std::string(texture_dir) + "/" + def.icon + ".png";
        SDL_GPUTexture* tex = load_texture(path.c_str());
        if (tex) m_item_icons[id] = tex;
    }
    SDL_Log("UIRenderer: cached %u item icons", (unsigned)m_item_icons.size());
}

SDL_GPUTexture* UIRenderer::item_icon(const std::string& item_id) const
{
    auto it = m_item_icons.find(item_id);
    return it != m_item_icons.end() ? it->second : nullptr;
}

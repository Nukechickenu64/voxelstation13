#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>

struct ContextEntry {
    std::string           label;
    bool                  enabled  = true;
    bool                  separator= false; // draw a divider instead of button
    std::function<void()> action;
};

// Right-click verb context menu.
// Spawns at cursor position; dismissed on any outside click or Alt release.
class ContextMenu {
public:
    explicit ContextMenu(UIRenderer& ui);

    void open(glm::vec2 screen_pos, std::vector<ContextEntry> entries);
    void close();
    bool is_open() const { return m_open; }

    // Draw and process input; returns true if menu should remain open
    bool draw(glm::vec2 cursor, bool lmb_pressed);

private:
    UIRenderer& m_ui;
    bool        m_open = false;
    glm::vec2   m_pos{};
    std::vector<ContextEntry> m_entries;

    static constexpr float MENU_WIDTH  = 160.f;
    static constexpr float ENTRY_H     = 24.f;
    static constexpr float PADDING     = 4.f;
};

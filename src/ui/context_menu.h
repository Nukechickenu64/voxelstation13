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

    // Draw and process input; returns true if menu should remain open.
    // scroll_delta: negative = scroll down (select next), positive = up (select prev).
    // confirm_pressed: fires the currently scroll-selected entry.
    // rmb_pressed:     dismisses the menu.
    bool draw(glm::vec2 cursor, bool lmb_pressed,
              float scroll_delta = 0.f, bool confirm_pressed = false,
              bool rmb_pressed = false);

private:
    UIRenderer& m_ui;
    bool        m_open = false;
    glm::vec2   m_pos{};
    std::vector<ContextEntry> m_entries;
    int         m_selected_idx = 0;   // scroll-navigation cursor
    int         m_view_top     = 0;   // first visible entry index (for viewport scrolling)
    float       m_scroll_accum = 0.f; // accumulated wheel input; step fires at ±SCROLL_THRESHOLD

    void advance_selection(int dir);          // skip separators
    void scroll_view_to_selection(int max_visible); // keep selection in viewport

    static constexpr float MENU_WIDTH        = 172.f;
    static constexpr float ENTRY_H           = 24.f;
    static constexpr float PADDING           = 4.f;
    static constexpr int   MAX_VISIBLE       = 12;   // max rows shown before viewport scrolls
    static constexpr float SCROLL_THRESHOLD  = 1.0f; // wheel units required per step
};

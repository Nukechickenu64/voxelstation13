#include "ui/context_menu.h"

ContextMenu::ContextMenu(UIRenderer& ui)
    : m_ui(ui)
{}

void ContextMenu::open(glm::vec2 screen_pos, std::vector<ContextEntry> entries)
{
    m_pos     = screen_pos;
    m_entries = std::move(entries);
    m_open    = true;
    m_view_top = 0;
    m_scroll_accum = 0.f;
    // Start selection on the first non-separator entry
    m_selected_idx = 0;
    while (m_selected_idx < static_cast<int>(m_entries.size())
           && m_entries[m_selected_idx].separator)
        ++m_selected_idx;
    if (m_selected_idx >= static_cast<int>(m_entries.size()))
        m_selected_idx = 0;
}

void ContextMenu::advance_selection(int dir)
{
    if (m_entries.empty()) return;
    int n    = static_cast<int>(m_entries.size());
    int next = m_selected_idx + dir;
    for (int i = 0; i < n; ++i, next += dir) {
        int idx = ((next % n) + n) % n;
        if (!m_entries[idx].separator) { m_selected_idx = idx; return; }
    }
}

void ContextMenu::close()
{
    m_open     = false;
    m_view_top = 0;
    m_scroll_accum = 0.f;
    m_entries.clear();
}

void ContextMenu::scroll_view_to_selection(int max_visible)
{
    if (m_selected_idx < m_view_top)
        m_view_top = m_selected_idx;
    else if (m_selected_idx >= m_view_top + max_visible)
        m_view_top = m_selected_idx - max_visible + 1;
    // Clamp so we don't scroll past the end
    int n = static_cast<int>(m_entries.size());
    m_view_top = std::max(0, std::min(m_view_top, n - max_visible));
}

bool ContextMenu::draw(glm::vec2 cursor, bool lmb_pressed,
                       float scroll_delta, bool confirm_pressed, bool rmb_pressed)
{
    if (!m_open) return false;

    int n = static_cast<int>(m_entries.size());
    // Cap visible rows to MAX_VISIBLE and whatever fits on screen
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());
    int max_visible = std::min(MAX_VISIBLE,
                               static_cast<int>((fb_h - PADDING * 2 - 16.f) / ENTRY_H));
    max_visible = std::max(1, std::min(max_visible, n));

    // Scroll wheel: accumulate input and only step when threshold is reached
    m_scroll_accum += scroll_delta;
    while (m_scroll_accum <= -SCROLL_THRESHOLD) { m_scroll_accum += SCROLL_THRESHOLD; advance_selection(1);  scroll_view_to_selection(max_visible); }
    while (m_scroll_accum >=  SCROLL_THRESHOLD) { m_scroll_accum -= SCROLL_THRESHOLD; advance_selection(-1); scroll_view_to_selection(max_visible); }

    float menu_h = static_cast<float>(max_visible) * ENTRY_H + PADDING * 2;
    glm::vec2 pos = m_pos;

    // Keep within screen bounds
    if (pos.x + MENU_WIDTH > fb_w) pos.x = fb_w - MENU_WIDTH - 4.f;
    if (pos.y + menu_h     > fb_h) pos.y = fb_h - menu_h - 4.f;
    if (pos.y < 0.f) pos.y = 4.f;

    // Background + thin border
    m_ui.rect(pos, {MENU_WIDTH, menu_h}, {0.06f, 0.06f, 0.12f, 0.96f}, 4.f);
    m_ui.rect(pos, {MENU_WIDTH, menu_h}, {0.3f, 0.35f, 0.55f, 0.5f},   4.f);

    // Scroll-up indicator
    if (m_view_top > 0)
        m_ui.text({pos.x + MENU_WIDTH * 0.5f - 6.f, pos.y + 1.f},
                  "^", {0.7f, 0.7f, 0.8f, 0.9f}, 10.f);
    // Scroll-down indicator
    if (m_view_top + max_visible < n)
        m_ui.text({pos.x + MENU_WIDTH * 0.5f - 6.f, pos.y + menu_h - 12.f},
                  "v", {0.7f, 0.7f, 0.8f, 0.9f}, 10.f);

    float ey  = pos.y + PADDING;
    int   idx = 0;
    int   row = 0;  // visible-row counter

    for (auto& entry : m_entries) {
        if (idx < m_view_top) { ++idx; continue; }      // above viewport
        if (row >= max_visible) { ++idx; continue; }    // below viewport

        if (entry.separator) {
            m_ui.line({pos.x + 4, ey + ENTRY_H * 0.5f},
                      {pos.x + MENU_WIDTH - 4, ey + ENTRY_H * 0.5f},
                      {0.3f, 0.3f, 0.35f, 0.8f});
            ey += ENTRY_H;
            ++idx; ++row;
            continue;
        }

        bool hovering  = cursor.x >= pos.x && cursor.x <= pos.x + MENU_WIDTH
                      && cursor.y >= ey    && cursor.y <= ey + ENTRY_H;
        bool selected  = (idx == m_selected_idx);

        // If mouse hovers an entry, transfer keyboard selection to it
        if (hovering && !entry.separator)
            m_selected_idx = idx;

        // Background: selected = amber, hover = blue, both = bright amber
        glm::vec4 bg{0, 0, 0, 0};
        if      (selected && hovering) bg = {0.65f, 0.48f, 0.08f, 0.95f};
        else if (selected)             bg = {0.42f, 0.28f, 0.05f, 0.90f};
        else if (hovering)             bg = {0.22f, 0.28f, 0.50f, 0.90f};
        if (bg.a > 0.f)
            m_ui.rect({pos.x + 2, ey}, {MENU_WIDTH - 4, ENTRY_H}, bg, 2.f);

        // Selection arrow
        if (selected)
            m_ui.text({pos.x + 5, ey + 4}, ">", {1.f, 0.80f, 0.20f, 1.f}, 12.f);

        glm::vec4 col = entry.enabled
            ? (selected ? glm::vec4{1.f, 0.95f, 0.75f, 1.f} : glm::vec4{1, 1, 1, 1})
            : glm::vec4{0.5f, 0.5f, 0.5f, 1};
        m_ui.text({pos.x + 18, ey + 4}, entry.label, col, 12.f);

        // Fire: scroll-select + confirm key, OR mouse hover + LMB
        bool fire = entry.enabled && entry.action
                 && ((selected && confirm_pressed) || (hovering && lmb_pressed));
        if (fire) {
            entry.action();
            close();
            return false;
        }
        ey += ENTRY_H;
        ++idx; ++row;
    }

    // Dismiss: click outside, or RMB
    bool inside = cursor.x >= pos.x && cursor.x <= pos.x + MENU_WIDTH
               && cursor.y >= pos.y && cursor.y <= pos.y + menu_h;
    if (rmb_pressed || (lmb_pressed && !inside)) {
        close();
        return false;
    }

    return true;
}

#include "ui/context_menu.h"

ContextMenu::ContextMenu(UIRenderer& ui)
    : m_ui(ui)
{}

void ContextMenu::open(glm::vec2 screen_pos, std::vector<ContextEntry> entries)
{
    m_pos     = screen_pos;
    m_entries = std::move(entries);
    m_open    = true;
}

void ContextMenu::close()
{
    m_open = false;
    m_entries.clear();
}

bool ContextMenu::draw(glm::vec2 cursor, bool lmb_pressed)
{
    if (!m_open) return false;

    float menu_h = (static_cast<float>(m_entries.size()) * ENTRY_H) + PADDING * 2;
    glm::vec2 pos = m_pos;

    // Keep within screen bounds
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());
    if (pos.x + MENU_WIDTH > fb_w) pos.x = fb_w - MENU_WIDTH - 4.f;
    if (pos.y + menu_h     > fb_h) pos.y = fb_h - menu_h - 4.f;

    // Background
    m_ui.rect(pos, {MENU_WIDTH, menu_h}, {0.08f, 0.08f, 0.14f, 0.95f}, 4.f);

    float ey = pos.y + PADDING;
    bool over_any = false;

    for (auto& entry : m_entries) {
        if (entry.separator) {
            m_ui.line({pos.x + 4, ey + ENTRY_H * 0.5f},
                      {pos.x + MENU_WIDTH - 4, ey + ENTRY_H * 0.5f},
                      {0.3f,0.3f,0.35f,0.8f});
            ey += ENTRY_H;
            continue;
        }

        bool hovering = cursor.x >= pos.x          &&
                        cursor.x <= pos.x+MENU_WIDTH &&
                        cursor.y >= ey              &&
                        cursor.y <= ey + ENTRY_H;

        if (hovering) over_any = true;

        glm::vec4 bg  = hovering ? glm::vec4{0.25f,0.3f,0.5f,0.9f}
                                  : glm::vec4{0,0,0,0};
        if (hovering)
            m_ui.rect({pos.x + 2, ey}, {MENU_WIDTH - 4, ENTRY_H}, bg, 2.f);

        glm::vec4 col = entry.enabled ? glm::vec4{1,1,1,1} : glm::vec4{0.5f,0.5f,0.5f,1};
        m_ui.text({pos.x + 8, ey + 4}, entry.label, col, 12.f);

        if (hovering && lmb_pressed && entry.enabled && entry.action) {
            entry.action();
            close();
            return false;
        }
        ey += ENTRY_H;
    }

    // Dismiss on click outside
    bool inside = cursor.x >= pos.x && cursor.x <= pos.x + MENU_WIDTH &&
                  cursor.y >= pos.y && cursor.y <= pos.y + menu_h;
    if (lmb_pressed && !inside) {
        close();
        return false;
    }

    return true;
}

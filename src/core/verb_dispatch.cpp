#include "core/verb_dispatch.h"
#include "inventory/inventory.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <string>

// ── VerbDispatch methods ──────────────────────────────────────────────────────

void VerbDispatch::register_handler(const std::string& name, VerbFn fn)
{
    m_handlers[name] = std::move(fn);
}

bool VerbDispatch::invoke(const std::string& handler_name, const VerbContext& ctx) const
{
    auto it = m_handlers.find(handler_name);
    if (it == m_handlers.end()) {
        SDL_Log("VerbDispatch: no handler registered for '%s'", handler_name.c_str());
        return false;
    }
    it->second(ctx);
    return true;
}

bool VerbDispatch::has(const std::string& handler_name) const
{
    return m_handlers.count(handler_name) > 0;
}

VerbDispatch& VerbDispatch::get()
{
    static VerbDispatch s_instance;
    return s_instance;
}

VerbDispatch& verb_dispatch()
{
    return VerbDispatch::get();
}

// ── Built-in verb handler implementations ─────────────────────────────────────

void init_verb_dispatch()
{
    auto& vd = verb_dispatch();

    // ── verb_examine ──────────────────────────────────────────────────────────
    // Prints item name, weight, volume, and condition to the HUD log.
    // main.cpp may override this with a richer version that captures additional
    // simulation context (e.g. item type-path, prototype ancestry).
    vd.register_handler("verb_examine", [](const VerbContext& ctx) {
        if (!ctx.item_def) {
            ctx.log("[Examine] You see something.");
            return;
        }
        const ItemStack* st  = ctx.item_stack;
        std::string      disp = ctx.item_def->name;
        if (st && !st->custom_name.empty())
            disp += " \"" + st->custom_name + "\"";
        if (st && st->count > 1)
            disp += " x" + std::to_string(st->count);

        // Type path ancestry — e.g. "/obj/item/tool/wrench"
        const std::string& tp = ctx.item_def->type_path;

        char buf[280];
        std::snprintf(buf, sizeof(buf),
            "[Examine] %s \xe2\x80\x94 %.2f kg / %.1f L \xe2\x80\x94 %s%s",
            disp.c_str(),
            ctx.item_def->weight,
            ctx.item_def->volume,
            st ? condition_label(st->integrity) : "unknown condition",
            tp.empty() ? "" : ("  (" + tp + ")").c_str());
        ctx.log(buf);
    });

    // ── verb_open  ────────────────────────────────────────────────────────────
    // Toggle open/closed on a container item.
    // The full implementation in main.cpp also refreshes the inventory panel.
    // This stub fires as a fallback if main.cpp hasn't registered an override.
    vd.register_handler("verb_open", [](const VerbContext& ctx) {
        if (!ctx.item_def || !ctx.item_def->is_container) {
            ctx.log("[Open] This item cannot be opened.");
            return;
        }
        if (!ctx.item_stack) return;
        ctx.item_stack->container_open = !ctx.item_stack->container_open;
        ctx.log(ctx.item_stack->container_open
                ? "[Open] You open the " + ctx.item_def->name + "."
                : "[Close] You close the " + ctx.item_def->name + ".");
    });

    // ── verb_throw ────────────────────────────────────────────────────────────
    // Placeholder: emits a HUD message.
    // main.cpp provides the full implementation that spawns a thrown entity.
    vd.register_handler("verb_throw", [](const VerbContext& ctx) {
        if (ctx.item_def)
            ctx.log("[Throw] You ready the " + ctx.item_def->name + " to throw.");
        else
            ctx.log("[Throw] Nothing to throw.");
    });

    // ── verb_pry_floor ────────────────────────────────────────────────────────
    // Placeholder — requires voxel + world context; consult main.cpp override.
    vd.register_handler("verb_pry_floor", [](const VerbContext& ctx) {
        if (ctx.has_target_voxel) {
            ctx.log("[Crowbar] You pry at the floor tile.");
        } else {
            ctx.log("[Crowbar] Aim at a floor tile to pry it up.");
        }
    });

    // ── verb_stun ─────────────────────────────────────────────────────────────
    // Placeholder.
    vd.register_handler("verb_stun", [](const VerbContext& ctx) {
        ctx.log("[Stun Baton] You ready the stun baton.");
        (void)ctx;
    });

    // ── verb_show_id ──────────────────────────────────────────────────────────
    // Print the ID card name to the HUD.
    vd.register_handler("verb_show_id", [](const VerbContext& ctx) {
        if (!ctx.item_stack) return;
        const std::string& display = ctx.item_stack->custom_name.empty()
            ? (ctx.item_def ? ctx.item_def->name : "ID Card")
            : ctx.item_stack->custom_name;
        ctx.log("[ID] Showing ID: " + display);
    });

    // ── verb_open_pda ─────────────────────────────────────────────────────────
    // Stub — PDA UI not yet implemented.
    vd.register_handler("verb_open_pda", [](const VerbContext& ctx) {
        ctx.log("[PDA] PDA interface not yet implemented.");
        (void)ctx;
    });
}

#ifndef SWITCH_MENU_PATCH_H
#define SWITCH_MENU_PATCH_H

// Rules that adapt loaded retail menus to this port: drop the Multiplayer
// entry (the SP-only build has no multiplayer to start) and send the PC
// options/controls pages to the port-authored ones. Templated on the menu and
// item types so the host test can use plain stand-ins; switch_ui_menus.cpp
// instantiates it with menuDef_t / itemDef_s.

#include <math.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <vector>

struct SwActionRetarget
{
    const char *from;  // menu named by "open <from>"
    const char *to;
};

// Menu scripts are stored as tokens ("open" "options_look" ; ...), quoted by
// the loader, or as plain text in scripts written by hand; both are matched.
// "open options_graphics" must not rewrite "open options_graphics_texture".
static const SwActionRetarget kSwActionRetargets[] = {
    {"options_graphics", "sw_graphics"},
    {"options_graphics_texture", "sw_graphics"},
    {"options_graphics_defaults", "sw_graphics"},
    {"options_look", "sw_controller"},
    {"options_move", "sw_controller"},
    {"options_shoot", "sw_controller"},
    {"options_misc", "sw_controller"},
    {"options_control_defaults", "sw_controller"},
    {"options_sound", "sw_sound"},
    {"options_game", "sw_game"},
    {"main_options", "sw_graphics"},
    {"main_controls", "sw_controller"},
    {"ingameoptions", "sw_graphics"},
};

static const char kSwHideEntryMenu[] = "multi_popmenu";

struct SwScriptToken
{
    size_t begin;  // span in the script, quotes included
    size_t end;
    std::string text;  // without quotes
};

static inline std::vector<SwScriptToken> SwPatch_Tokenize(const std::string &script)
{
    std::vector<SwScriptToken> tokens;
    size_t i = 0;
    while (i < script.size())
    {
        const char c = script[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++i;
            continue;
        }
        SwScriptToken t;
        t.begin = i;
        if (c == '"')
        {
            const size_t close = script.find('"', i + 1);
            const size_t stop = close == std::string::npos ? script.size() : close;
            t.text = script.substr(i + 1, stop - i - 1);
            i = close == std::string::npos ? script.size() : close + 1;
        }
        else if (c == ';')
        {
            t.text = ";";
            ++i;
        }
        else
        {
            while (i < script.size() && script[i] != ' ' && script[i] != '\t' && script[i] != '\r' &&
                   script[i] != '\n' && script[i] != ';')
                ++i;
            t.text = script.substr(t.begin, i - t.begin);
        }
        t.end = i;
        tokens.push_back(t);
    }
    return tokens;
}

static inline bool SwPatch_TokenEqual(const std::string &a, const char *b)
{
    return strcasecmp(a.c_str(), b) == 0;
}

// True when the script contains the command "open <menu>".
static inline bool SwPatch_OpensMenu(const char *action, const char *menu)
{
    if (!action || !*action)
        return false;
    const std::vector<SwScriptToken> tokens = SwPatch_Tokenize(action);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if (SwPatch_TokenEqual(tokens[i].text, "open") && SwPatch_TokenEqual(tokens[i + 1].text, menu) &&
            (i + 2 == tokens.size() || tokens[i + 2].text == ";"))
            return true;
    return false;
}

// Returns the retargeted copy of `action`, or an empty string when no rule
// applies.
static inline std::string SwPatch_RetargetAction(const char *action)
{
    if (!action || !*action)
        return std::string();
    std::string text(action);
    const std::vector<SwScriptToken> tokens = SwPatch_Tokenize(text);
    bool changed = false;
    // Back to front so earlier spans stay valid.
    for (size_t i = tokens.size(); i-- > 1;)
    {
        if (!SwPatch_TokenEqual(tokens[i - 1].text, "open") ||
            !(i + 1 == tokens.size() || tokens[i + 1].text == ";"))
            continue;
        for (const SwActionRetarget &rule : kSwActionRetargets)
        {
            if (!SwPatch_TokenEqual(tokens[i].text, rule.from))
                continue;
            const bool quoted = text[tokens[i].begin] == '"';
            text.replace(tokens[i].begin, tokens[i].end - tokens[i].begin,
                         quoted ? std::string("\"") + rule.to + "\"" : std::string(rule.to));
            changed = true;
            break;
        }
    }
    return changed ? text : std::string();
}

template <class Menu, class Item>
static inline bool SwPatch_ItemOnRow(const Item *item, float y, int horzAlign)
{
    return fabsf(item->window.rectClient.y - y) < 0.5f && item->window.rectClient.horzAlign == horzAlign;
}

// Removes every item on the row of the item whose action contains `needle`
// and closes the gap by moving the rows below it up. Returns items removed.
template <class Menu, class Item>
static inline int SwPatch_HideRowByAction(Menu *menu, const char *openedMenu)
{
    const Item *anchor = 0;
    for (int i = 0; i < menu->itemCount && !anchor; ++i)
    {
        const Item *item = menu->items[i];
        if (SwPatch_OpensMenu(item->action, openedMenu))
            anchor = item;
    }
    if (!anchor)
        return 0;

    const float rowY = anchor->window.rectClient.y;
    const int horz = anchor->window.rectClient.horzAlign;

    // The next row below fixes the pitch to close up.
    float nextY = 0.0f;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const float y = menu->items[i]->window.rectClient.y;
        if (menu->items[i]->window.rectClient.horzAlign == horz && y > rowY + 0.5f &&
            (nextY == 0.0f || y < nextY))
            nextY = y;
    }
    const float pitch = nextY > 0.0f ? nextY - rowY : 0.0f;

    int kept = 0;
    int removed = 0;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        Item *item = menu->items[i];
        if (SwPatch_ItemOnRow<Menu, Item>(item, rowY, horz))
        {
            ++removed;
            continue;
        }
        if (item->window.rectClient.horzAlign == horz && item->window.rectClient.y > rowY + 0.5f)
            item->window.rectClient.y -= pitch;
        menu->items[kept++] = item;
    }
    menu->itemCount = kept;
    return removed;
}

#endif

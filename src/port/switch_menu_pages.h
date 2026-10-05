#ifndef SWITCH_MENU_PAGES_H
#define SWITCH_MENU_PAGES_H

// Source text of the port-authored options pages (Graphics, Controller,
// Sound, Game), in the engine's own .menu syntax. Pure string building so the
// host test can check the structure; switch_ui_menus.cpp feeds the text to
// the engine's menu parser. Every control writes its dvar directly, so a
// change applies at once; the few that need more go through the sw_* commands.

#include <string>

enum SwRowKind
{
    SW_ROW_MULTI_NUM,  // choices: "label" number ...
    SW_ROW_MULTI_STR,  // choices: "label", "string", ... (commas keep the loader from joining adjacent strings)
    SW_ROW_YESNO,
    SW_ROW_BUTTON,
};

struct SwRow
{
    SwRowKind kind;
    const char *label;
    const char *dvar;     // buttons: unused
    const char *choices;  // multi lists
    const char *action;   // script run after a change / on activation
    const char *visible;  // "when(...)" expression, or 0 for always
};

struct SwPage
{
    const char *menuName;
    const char *tabLabel;
    const SwRow *rows;
    int rowCount;
    const char *note;  // fixed line under the rows, may be 0
};

#define SW_SYNC "exec \"sw_sync\";"

#define SW_WHEN_DYNRES "when(dvarint(\"sw_ui_dynres\") == 1)"

// Order: preset, frame rate, upscaling, resolution, effects. Rows that have
// no effect in the current mode are hidden.
static const SwRow kSwGraphicsRows[] = {
    {SW_ROW_MULTI_NUM, "Preset", "sw_ui_preset", "\"Quality\" 0 \"Balanced\" 1 \"Performance\" 2 \"Custom\" 3",
     "exec \"sw_preset\";", 0},
    {SW_ROW_MULTI_NUM, "Frame rate limit", "com_maxfps", "\"60 FPS\" 0 \"30 FPS\" 30", SW_SYNC, 0},
    {SW_ROW_MULTI_NUM, "Temporal upscaling (TAAU)", "r_taau", "\"Off\" 0 \"On\" 1", SW_SYNC, 0},
    {SW_ROW_MULTI_STR, "Spatial upscaler", "r_fsrMode",
     "\"SGSR\", \"sgsr\", \"Bilinear + sharpen\", \"bilinear_rcas\", \"Bilinear\", \"bilinear\"", SW_SYNC, 0},
    {SW_ROW_MULTI_NUM, "Sharpening", "r_fsrSharpness",
     "\"Strongest\" 0 \"Strong\" 0.2 \"Medium\" 0.5 \"Mild\" 1 \"Off\" 4", SW_SYNC,
     "when(dvarstring(\"r_fsrMode\") == \"bilinear_rcas\")"},
    {SW_ROW_MULTI_NUM, "Automatic resolution (restart)", "sw_ui_dynres", "\"Off\" 0 \"On\" 1", SW_SYNC, 0},
    {SW_ROW_MULTI_NUM, "Lowest resolution", "r_dynresMin",
     "\"50%\" 0.5 \"60%\" 0.6 \"67%\" 0.67 \"75%\" 0.75 \"85%\" 0.85 \"100%\" 1", SW_SYNC, SW_WHEN_DYNRES},
    {SW_ROW_MULTI_NUM, "Highest resolution", "r_dynresMax", "\"75%\" 0.75 \"85%\" 0.85 \"100%\" 1", SW_SYNC,
     SW_WHEN_DYNRES},
    {SW_ROW_MULTI_NUM, "Particle resolution", "r_halfResParticles", "\"Full\" 0 \"Half\" 1 \"Auto\" 3", SW_SYNC, 0},
    {SW_ROW_MULTI_NUM, "Shadows", "sm_enable", "\"Off\" 0 \"On\" 1", SW_SYNC, 0},
    {SW_ROW_MULTI_NUM, "Shadow softness", "r_shadowFilter", "\"Smooth\" 0 \"Fast\" 1", SW_SYNC,
     "when(dvarint(\"sm_enable\") == 1)"},
    {SW_ROW_BUTTON, "Apply and save", 0, 0, "exec \"sw_apply\";", 0},
    {SW_ROW_BUTTON, "Reset to defaults", 0, 0, "exec \"sw_reset graphics\";", 0},
};

static const SwRow kSwControllerRows[] = {
    {SW_ROW_MULTI_NUM, "Look sensitivity", "input_viewSensitivity",
     "\"50%\" 0.5 \"75%\" 0.75 \"100%\" 1 \"125%\" 1.25 \"150%\" 1.5 \"200%\" 2 \"250%\" 2.5 \"300%\" 3", 0, 0},
    {SW_ROW_MULTI_NUM, "Look response", "input_lookCurve",
     "\"Linear\" 1 \"Gentle\" 1.5 \"Standard\" 2 \"Fine\" 3 \"Finest\" 4", 0, 0},
    {SW_ROW_MULTI_NUM, "Horizontal turn speed", "cl_yawspeed",
     "\"Slow\" 90 \"Reduced\" 115 \"Standard\" 140 \"Fast\" 180 \"Faster\" 220", 0, 0},
    {SW_ROW_MULTI_NUM, "Vertical turn speed", "cl_pitchspeed",
     "\"Slow\" 90 \"Reduced\" 115 \"Standard\" 140 \"Fast\" 180 \"Faster\" 220", 0, 0},
    {SW_ROW_YESNO, "Invert look", "input_invertPitch", 0, 0, 0},
    {SW_ROW_MULTI_NUM, "Gyro aiming", "gyro_enable", "\"Off\" 0 \"Always\" 1 \"Aiming down sights\" 2", 0, 0},
    {SW_ROW_MULTI_NUM, "Gyro sensitivity", "gyro_sensitivity",
     "\"25%\" 0.25 \"50%\" 0.5 \"75%\" 0.75 \"100%\" 1 \"150%\" 1.5 \"200%\" 2 \"300%\" 3", 0, 0},
    {SW_ROW_YESNO, "Invert gyro horizontal", "gyro_invertYaw", 0, 0, 0},
    {SW_ROW_YESNO, "Invert gyro vertical", "gyro_invertPitch", 0, 0, 0},
    {SW_ROW_YESNO, "Vibration", "rumble_enable", 0, 0, 0},
    {SW_ROW_MULTI_NUM, "Vibration strength", "rumble_intensity",
     "\"25%\" 0.25 \"50%\" 0.5 \"80%\" 0.8 \"100%\" 1 \"150%\" 1.5 \"200%\" 2", 0, 0},
    {SW_ROW_YESNO, "HD Rumble effects", "rumble_hd", 0, 0, 0},
    {SW_ROW_BUTTON, "Reset to defaults", 0, 0, "exec \"sw_reset controller\";", 0},
};

static const SwRow kSwSoundRows[] = {
    {SW_ROW_MULTI_NUM, "Master volume", "snd_volume",
     "\"0%\" 0 \"20%\" 0.2 \"40%\" 0.4 \"60%\" 0.6 \"80%\" 0.8 \"100%\" 1", 0, 0},
    {SW_ROW_BUTTON, "Reset to defaults", 0, 0, "exec \"sw_reset audio\";", 0},
};

static const SwRow kSwGameRows[] = {
    {SW_ROW_YESNO, "Blood", "cg_blood", 0, 0, 0},
    {SW_ROW_YESNO, "Subtitles", "cg_subtitles", 0, 0, 0},
    {SW_ROW_YESNO, "Crosshair", "cg_drawCrosshair", 0, 0, 0},
};

#define SW_PAGE_COUNT 4
static const SwPage kSwPages[SW_PAGE_COUNT] = {
    {"sw_graphics", "Graphics", kSwGraphicsRows, (int)(sizeof(kSwGraphicsRows) / sizeof(kSwGraphicsRows[0])),
     "Changing any setting switches Preset to Custom. (restart) items apply at the next launch."},
    {"sw_controller", "Controller", kSwControllerRows, (int)(sizeof(kSwControllerRows) / sizeof(kSwControllerRows[0])),
     0},
    {"sw_sound", "Sound", kSwSoundRows, (int)(sizeof(kSwSoundRows) / sizeof(kSwSoundRows[0])), 0},
    {"sw_game", "Game", kSwGameRows, (int)(sizeof(kSwGameRows) / sizeof(kSwGameRows[0])), 0},
};

namespace swpages
{
constexpr int kRowX = -74;
constexpr int kRowW = 330;
constexpr int kRowH = 22;
constexpr int kRowStep = 24;
constexpr int kFirstRowY = 40;
constexpr int kTabX = -330;
constexpr int kTabW = 220;

inline void Add(std::string &out, const char *s)
{
    out += s;
}

inline std::string Num(int v)
{
    return std::to_string(v);
}

inline void Rect(std::string &out, int x, int y, int w, int h)
{
    out += "rect " + Num(x) + " " + Num(y) + " " + Num(w) + " " + Num(h) + " 2 1\n";
}

inline std::string VisibleLine(const char *when)
{
    return when ? std::string("visible ") + when + ";\n" : std::string("visible 1\n");
}

inline void Panel(std::string &out, int x, int y, int w, int h, const char *color, const char *visible)
{
    out += "itemDef {\n";
    Rect(out, x, y, w, h);
    out += "style 3 decoration borderSize 1\n";
    out += VisibleLine(visible);
    out += std::string("forecolor ") + color + "\nbackground \"white\"\n}\n";
}

inline void RowItems(std::string &out, const SwRow &row, int index)
{
    const int y = kFirstRowY + index * kRowStep;
    const std::string hl = Num(index + 1);

    Panel(out, kRowX, y, kRowW, kRowH, "0.9 0.9 1 0.07", row.visible);
    std::string focusVisible = std::string("when(localvarint(\"sw_hl\") == ") + hl + ")";
    if (row.visible)
        focusVisible = std::string("when(localvarint(\"sw_hl\") == ") + hl + " && " + (row.visible + 4) + ")";
    Panel(out, kRowX, y, kRowW, kRowH, "0.9 0.95 1 0.3", focusVisible.c_str());

    // Label: a decoration, so only the control takes focus. Buttons carry
    // their own text.
    if (row.kind != SW_ROW_BUTTON)
    {
        out += "itemDef {\n";
        Rect(out, kRowX + 4, y, 200, kRowH);
        out += "decoration borderSize 1\n";
        out += VisibleLine(row.visible);
        out += "textalign 8 textalignx 4 textscale 0.4 textstyle 3 textfont 1\n";
        out += "forecolor 0.9 0.9 0.9 1\n";
        out += std::string("text \"") + row.label + "\"\n}\n";
    }

    out += "itemDef {\n";
    if (index == 0)
        out += "name \"sw_first\"\n";
    Rect(out, kRowX, y, kRowW, kRowH);
    out += "borderSize 1\n";
    out += VisibleLine(row.visible);
    switch (row.kind)
    {
    case SW_ROW_MULTI_NUM:
    case SW_ROW_MULTI_STR:
        out += "type 12 style 1\ntextalign 8 textalignx 210 textscale 0.4 textstyle 3 textfont 1\n";
        break;
    case SW_ROW_YESNO:
        out += "type 11 style 1\ntextalign 8 textalignx 210 textscale 0.4 textstyle 3 textfont 1\n";
        break;
    case SW_ROW_BUTTON:
        out += "type 1\ntextalign 8 textalignx 8 textscale 0.4 textstyle 3 textfont 1\n";
        break;
    }
    out += "forecolor 0.69 0.69 0.69 1\n";
    out += "onFocus { play mouse_over; setLocalVarInt sw_hl " + hl + "; }\n";
    out += "leaveFocus { setLocalVarInt sw_hl 0; }\n";
    if (row.kind == SW_ROW_BUTTON)
    {
        out += std::string("text \"") + row.label + "\"\n";
        out += std::string("action { play mouse_click; ") + row.action + " }\n";
    }
    else
    {
        out += std::string("action { play mouse_click; ") + (row.action ? row.action : "") + " }\n";
        out += std::string("dvar \"") + row.dvar + "\"\n";
        if (row.kind == SW_ROW_MULTI_NUM)
            out += std::string("dvarFloatList { ") + row.choices + " }\n";
        else if (row.kind == SW_ROW_MULTI_STR)
            out += std::string("dvarStrList { ") + row.choices + " }\n";
    }
    out += "}\n";
}

inline void TabItems(std::string &out, int pageIndex, int tabIndex)
{
    const SwPage &tab = kSwPages[tabIndex];
    const int y = kFirstRowY + tabIndex * kRowStep;
    const bool current = tabIndex == pageIndex;
    const std::string hl = Num(100 + tabIndex);

    if (current)
        Panel(out, kTabX, y, kTabW, kRowH, "0.9 0.95 1 0.2", 0);
    Panel(out, kTabX, y, kTabW, kRowH, "0.9 0.95 1 0.3",
          (std::string("when(localvarint(\"sw_hl\") == ") + hl + ")").c_str());

    out += "itemDef {\n";
    out += std::string("name \"") + tab.menuName + "_tab\"\n";
    Rect(out, kTabX, y, kTabW, kRowH);
    out += "type 1 borderSize 1 visible 1\n";
    out += "textalign 10 textalignx -10 textscale 0.4 textstyle 6 textfont 1\n";
    out += current ? "forecolor 1 0.8 0.4 1\n" : "forecolor 0.69 0.69 0.69 1\n";
    out += "onFocus { play mouse_over; setLocalVarInt sw_hl " + hl + "; }\n";
    out += "leaveFocus { setLocalVarInt sw_hl 0; }\n";
    out += std::string("text \"") + tab.tabLabel + "\"\n";
    out += "action { play mouse_click; ";
    if (!current)
    {
        out += std::string("close ") + kSwPages[pageIndex].menuName + "; open " + tab.menuName + ";";
    }
    out += " }\n}\n";
}

inline std::string BuildPage(int pageIndex)
{
    const SwPage &page = kSwPages[pageIndex];
    std::string out;
    out += "{\nmenuDef {\n";
    out += std::string("name \"") + page.menuName + "\"\n";
    out += "rect 0 0 640 480 0 0\nborderSize 1\nfocuscolor 1 1 1 1\n";
    out += "fadeClamp 1 fadeCycle 1 fadeAmount 0.1 blurWorld 7\n";
    out += "onOpen { setLocalVarBool ui_hideBack 1; setLocalVarInt sw_hl 0; exec \"sw_open\"; setfocus \"sw_first\"; }\n";
    out += "onClose { setLocalVarBool ui_hideBack 0; }\n";
    out += "onESC { close self; }\n";

    // Dim backdrop over the whole screen, including widescreen margins.
    out += "itemDef {\nrect -120 0 880 480 0 0\nstyle 3 decoration borderSize 1 visible 1\n";
    out += "forecolor 0 0 0 0.8\nbackground \"white\"\n}\n";
    out += "itemDef {\nrect -330 6 450 26 2 1\ndecoration borderSize 1 visible 1\n";
    out += "textalign 8 textalignx 6 textscale 0.5 textstyle 6 textfont 1\nforecolor 1 0.8 0.4 1\n";
    out += "text \"OPTIONS\"\n}\n";

    for (int i = 0; i < page.rowCount; ++i)
        RowItems(out, page.rows[i], i);

    for (int i = 0; i < SW_PAGE_COUNT; ++i)
        TabItems(out, pageIndex, i);

    // Status line: results of Apply/Reset and the restart notice.
    out += "itemDef {\nrect -74 392 330 20 2 1\ndecoration borderSize 1 visible 1\n";
    out += "textalign 8 textalignx 4 textscale 0.36 textstyle 3 textfont 1\nforecolor 1 0.8 0.4 1\n";
    out += "exp text (dvarstring(\"sw_status\"));\n}\n";
    if (page.note)
    {
        out += "itemDef {\nrect -74 412 330 20 2 1\ndecoration borderSize 1 visible 1\n";
        out += "textalign 8 textalignx 4 textscale 0.33 textstyle 3 textfont 1\nforecolor 0.8 0.8 0.8 1\n";
        out += std::string("text \"") + page.note + "\"\n}\n";
    }

    out += "itemDef {\nname \"back\"\ntext \"Back\"\n";
    out += "rect -330 " + Num(kFirstRowY + SW_PAGE_COUNT * kRowStep + 12) + " 100 22 2 1\n";
    out += "type 1 borderSize 1 visible 1\ntextalign 8 textalignx 10 textscale 0.4 textstyle 6 textfont 1\n";
    out += "forecolor 0.69 0.69 0.69 1\n";
    out += "onFocus { play mouse_over; }\naction { play mouse_click; close self; }\n}\n";

    out += "}\n}\n";
    return out;
}
} // namespace swpages

#endif

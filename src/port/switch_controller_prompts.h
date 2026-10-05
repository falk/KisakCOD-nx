#ifndef SWITCH_CONTROLLER_PROMPTS_H
#define SWITCH_CONTROLLER_PROMPTS_H

#include <cstring>

// Script binding parameters are localized tokens. These port-owned names
// must resolve even though the retail localization assets have no Joy-Con labels.
inline const char *Switch_ControllerControlLabel(const char *reference)
{
    if (!reference)
        return nullptr;
    static const char *labels[] = {"L Stick", "R Stick", "ZL", "ZR", "Up", "Down", "Left", "Right"};
    for (const char *label : labels)
        if (!std::strcmp(reference, label))
            return label;
    return nullptr;
}

// English instructions for controls whose PC variants describe a different
// gesture. Keep conversion slots so script-supplied arguments are consumed.
inline const char *Switch_ControllerPrompt(const char *reference, int language)
{
    if (!reference || language != 0)
        return nullptr;
    struct Prompt { const char *reference; const char *text; };
    static const Prompt prompts[] = {
        {"SCRIPT_PLATFORM_HINT_MOVEFORWARD", "Move ^3&&1^7 forward."},
        {"SCRIPT_PLATFORM_HINT_MOVEBACKWARD", "Move ^3&&1^7 backward."},
        {"SCRIPT_PLATFORM_HINT_STRAFELEFT", "Move ^3&&1^7 left."},
        {"SCRIPT_PLATFORM_HINT_STRAFERIGHT", "Move ^3&&1^7 right."},
        {"SCRIPT_PLATFORM_HINT_USECINDERBLOCKWALL", "Press ^3&&1^7 to use the wall."},
        {"SCRIPT_PLATFORM_FIRE_TO_SKIP", "Press ^3ZR^7 to skip."},
        {"PLATFORM_DYK_MSG36", "With C4 selected, press ^3ZR^7 to detonate placed charges."},
        {"SCRIPT_PLATFORM_HINT_DOUBLETAPPRONEKEY", "Hold ^3&&1^7 to go prone."},
        {"SCRIPT_PLATFORM_HINT_PRONEKEY", "Hold ^3&&1^7 to go prone."},
        {"SCRIPT_PLATFORM_HINT_HOLDDOWNPRONEKEY", "Hold ^3&&1^7 to go prone."},
        {"SCRIPT_PLATFORM_HINT_HOLDDOWNCROUCHKEY", "Tap ^3&&1^7 to crouch."},
        {"SCRIPT_PLATFORM_HINT_DOUBLETAPSTANDKEY", "Hold ^3&&1^7 to stand from prone."},
        {"SCRIPT_PLATFORM_HINT_STANDLETGOPRONEKEY", "Hold ^3&&1^7 to stand from prone."},
        {"SCRIPT_PLATFORM_HINT_STANDLETGOSECONDKEY", "Hold ^3&&1^7 to stand from prone."},
        {"SCRIPT_PLATFORM_HINT_STANDFROMCROUCHKEY", "Tap ^3&&1^7 to stand from crouch."},
        {"SCRIPT_PLATFORM_HINT_STANDLETGOCROUCHKEY", "Tap ^3&&1^7 to stand from crouch."},
        {"SCRIPT_PLATFORM_HINT_ADSSTOP", "Release ^3&&1^7 to stop aiming."},
        {"KILLHOUSE_HINT_BREATH_MELEE", "Hold ^3L Stick^7 while scoped to steady your aim."},
        {"KILLHOUSE_HINT_MELEE_BREATH", "Click ^3R Stick^7 to melee."},
        {"KILLHOUSE_HINT_MELEE_BREATH_CLICK", "Click ^3R Stick^7 to melee."},
        {"KILLHOUSE_HINT_SIDEARM", "Press ^3X^7 to switch weapons."},
        {"KILLHOUSE_HINT_CHECK_OBJECTIVES_PAUSED", "Hold ^3Minus^7 to view objectives."},
        {"KILLHOUSE_HINT_CHECK_OBJECTIVES_SCORES", "Hold ^3Minus^7 to view objectives."},
        {"KILLHOUSE_HINT_ADS_TOGGLE", "Hold ^3ZL^7 to aim."},
        {"KILLHOUSE_HINT_ADS_TOGGLE_THROW", "Hold ^3ZL^7 to aim."},
        {"KILLHOUSE_HINT_STOP_ADS_TOGGLE", "Release ^3ZL^7 to fire from the hip."},
        {"KILLHOUSE_HINT_STOP_ADS_TOGGLE_THROW", "Release ^3ZL^7 to fire from the hip."},
    };
    for (const Prompt &prompt : prompts)
        if (!std::strcmp(reference, prompt.reference))
            return prompt.text;
    // All authored prone variants share the controller's hold-B gesture.
    const char *prone = nullptr;
    if (!std::strncmp(reference, "KILLHOUSE_HINT_PRONE", sizeof("KILLHOUSE_HINT_PRONE") - 1))
        prone = reference + sizeof("KILLHOUSE_HINT_PRONE") - 1;
    else if (!std::strncmp(reference, "SCOUTSNIPER_HINT_PRONE", sizeof("SCOUTSNIPER_HINT_PRONE") - 1))
        prone = reference + sizeof("SCOUTSNIPER_HINT_PRONE") - 1;
    if (prone && (!*prone || !std::strcmp(prone, "_DOUBLE") || !std::strcmp(prone, "_TOGGLE")
                  || !std::strcmp(prone, "_HOLD") || !std::strcmp(prone, "_STANCE")))
        return "Hold ^3B^7 to go prone.";
    return nullptr;
}

#endif

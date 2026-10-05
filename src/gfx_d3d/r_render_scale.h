#pragma once

// Scene render scale for the deko3d renderer: which layout the scene uses,
// who sets its size and which upscaler resolves it. Pure functions, no
// engine or deko3d dependency, so a host test checks the whole truth table.
//
// The scale is one value per frame. r_renderScale sets it; the dynamic
// resolution controller (r_dynres) is only a driver that, while it runs,
// replaces that value with one it picks from the GPU frame time inside
// r_dynresMin..r_dynresMax. Everything downstream (scene targets, TAAU
// jitter and resolve, the r_fsrMode upscale, the pre-HUD resolve, logs)
// reads the frame's scale and never asks where it came from.
//
//   r_dynres  r_renderScale  layout  frame scale             r_taau 1  r_taau 0
//   0         1              native  1 (back buffer)          none      none
//   0         S < 1          scene   S (fixed)                TAAU      r_fsrMode
//   1         ignored        scene   controller (min..max)    TAAU      r_fsrMode
//
// The layout is chosen when the render targets are created (startup and
// vid_restart). On the scene layout r_renderScale changes apply on the next
// frame, 1 included (then TAAU runs as anti-aliasing only, or the scene is
// moved to the back buffer). Started native, a new r_renderScale waits for
// vid_restart.

namespace render_scale
{

// The smallest scene scale per axis.
constexpr float kMinScale = 0.25f;

// The scene has its own target, sized per frame and upscaled before the
// HUD, when the controller is asked for or the scale starts below 1.
inline bool SceneLayoutFor(bool controller, float renderScale)
{
    return controller || renderScale < 1.0f;
}

enum class Mode
{
    Native,   // no scene layout: the scene renders into the back buffer
    Fixed,    // scene layout, size from r_renderScale
    Adaptive, // scene layout, size from the controller
};

// controllerDriving: r_dynres is on and nothing holds the controller.
inline Mode ModeFor(bool sceneLayout, bool controllerDriving)
{
    if (!sceneLayout)
        return Mode::Native;
    return controllerDriving ? Mode::Adaptive : Mode::Fixed;
}

inline float ClampScale(float scale)
{
    return scale < kMinScale ? kMinScale : scale > 1.0f ? 1.0f : scale;
}

// The frame's scale per axis: 1 natively, r_renderScale when fixed, the
// controller's pick when adaptive.
inline float FrameScale(Mode mode, float renderScale, float controllerScale)
{
    switch (mode)
    {
    case Mode::Native:
        return 1.0f;
    case Mode::Fixed:
        return ClampScale(renderScale);
    case Mode::Adaptive:
        return ClampScale(controllerScale);
    }
    return 1.0f;
}

enum class Upscaler
{
    None,    // native: nothing to resolve
    Taau,    // temporal resolve of the jittered scene (any scale, 1 included)
    Spatial, // r_fsrMode, or a plain copy/move when the frame is full size
};

inline Upscaler UpscalerFor(Mode mode, bool taau)
{
    if (mode == Mode::Native)
        return Upscaler::None;
    return taau ? Upscaler::Taau : Upscaler::Spatial;
}

inline const char *ModeName(Mode mode)
{
    return mode == Mode::Native ? "native" : mode == Mode::Fixed ? "fixed" : "adaptive";
}

inline const char *UpscalerName(Upscaler u)
{
    return u == Upscaler::None ? "none" : u == Upscaler::Taau ? "taau" : "spatial";
}

} // namespace render_scale

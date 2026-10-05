#pragma once

// Dynamic render resolution controller (r_dynres): one driver of the scene
// render scale (r_render_scale.h), replacing r_renderScale while it runs.
// Pure arithmetic, no engine or deko3d dependency:
// switch_dynres_test.cpp drives it with synthetic GPU-time sequences.
//
// The scene renders at a size between minScale and maxScale of the output
// (per axis) inside targets allocated for the output size. Sizes are
// quantised: the width to multiples of 16 px (one "level"), the height to
// the nearest multiple of 8 px of the width's aspect-correct height (block
// linear GOBs are 64 B x 8 rows, i.e. 16 x 8 pixels at 4 B; the upscaler
// maps the rectangle onto the whole output, so the sub-percent aspect error
// only changes the sampling density, never the geometry). The top level is
// exactly the output size.
//
// Input: one GPU frame time per completed frame, tagged with the size that
// frame rendered at (deko9 publishes it when the frame's fence retires, 2-3
// frames after it was built; no CPU wait). Samples of any other size than
// the current one are ignored, so a change is judged only on frames that
// rendered at the new size.
//
// Decisions:
//   - drop (fast): a sample over the budget drops at once to the level whose
//     pixel count would bring the time to dropTarget x budget if time were
//     proportional to pixels (fixed costs make this optimistic; the next
//     over-budget sample drops again), at least one level and at most
//     maxDropLevels;
//   - raise (slow): over a window of raiseFrames samples at the current
//     size, the time predicted for the next size up (same proportional
//     model, which overestimates the growth when part of the frame is fixed
//     cost, so it errs towards staying) must average at most raiseMargin x
//     budget and never exceed raisePeak x budget; then raise by raiseLevels.
//     An over-budget sample (a drop) restarts the window;
//   - anti-oscillation: the margin between the drop line (budget) and the
//     raise line (raiseMargin x budget, on a predicted time) is the
//     hysteresis; a raise that is undone by a drop within 2 x raiseFrames
//     samples doubles the raise wait (up to backoffMax x), and the wait
//     halves again after 8 x raiseFrames samples without a drop.
// After any change the first settleFrames samples at the new size are
// skipped (the first frames at a size also carry the switch itself).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dynres
{

constexpr int kWidthQuantum = 16;
constexpr int kHeightQuantum = 8;

// GPU budget implied by the frame cap: a 30 fps cap leaves ~31 ms, anything
// else targets the 60 Hz vsync pace. Derived so no setting mix can pair a
// 60 fps target with a 30 fps budget.
inline float BudgetForFrameCap(int maxFps)
{
    return maxFps > 0 && maxFps <= 30 ? 31.0f : 15.5f;
}

struct Config
{
    int maxWidth = 1280, maxHeight = 720; // the output size: scale 1.0
    float minScale = 0.75f, maxScale = 1.0f;
    float budgetMs = 15.5f;
    float dropTarget = 0.95f;  // a drop aims at this fraction of the budget
    float raiseMargin = 0.9f;  // raise when the window's mean predicted time <= budget x this
    float raisePeak = 1.0f;    // ... and its highest predicted time <= budget x this
    int raiseFrames = 45;      // samples per raise window
    int raiseLevels = 2;       // raise step, in 16-px width levels
    int maxDropLevels = 8;     // largest drop per sample, in levels
    int settleFrames = 2;      // samples skipped after a change
    int backoffMax = 8;        // raise-wait multiplier cap
};

struct Size
{
    int width = 0, height = 0;
    bool operator==(const Size &o) const { return width == o.width && height == o.height; }
    bool operator!=(const Size &o) const { return !(*this == o); }
    int64_t Pixels() const { return (int64_t)width * height; }
};

inline int MaxLevel(const Config &c) { return (c.maxWidth + kWidthQuantum - 1) / kWidthQuantum; }

// The size of a width level; the top level is exactly the output size.
inline Size SizeForLevel(const Config &c, int level)
{
    const int top = MaxLevel(c);
    level = std::min(std::max(level, 1), top);
    if (level == top)
        return {c.maxWidth, c.maxHeight};
    const int w = level * kWidthQuantum;
    const double exactH = (double)w * c.maxHeight / c.maxWidth;
    int h = (int)std::lround(exactH / kHeightQuantum) * kHeightQuantum;
    h = std::min(std::max(h, kHeightQuantum), c.maxHeight);
    return {w, h};
}

// Nearest level for a per-axis scale (0 < scale <= 1).
inline int LevelForScale(const Config &c, float scale)
{
    const double w = (double)scale * c.maxWidth;
    const int level = (int)std::lround(w / kWidthQuantum);
    return std::min(std::max(level, 1), MaxLevel(c));
}

inline int MinLevel(const Config &c) { return LevelForScale(c, c.minScale); }
inline int TopLevel(const Config &c) { return std::max(LevelForScale(c, c.maxScale), MinLevel(c)); }

class Controller
{
public:
    void Reset(const Config &config, int level)
    {
        m_config = config;
        m_level = std::min(std::max(level, MinLevel(config)), TopLevel(config));
        ResetWindow();
        m_settle = 0;
        m_backoff = 1;
        m_sinceRaise = m_sinceDrop = 1 << 30;
        changes = drops = raises = ignored = 0;
    }

    // New limits/budget (dvars changed); the level is clamped into range.
    // Returns true when that moved the level.
    bool Configure(const Config &config)
    {
        m_config = config;
        const int level = std::min(std::max(m_level, MinLevel(config)), TopLevel(config));
        if (level == m_level)
            return false;
        SetLevel(level);
        return true;
    }

    // One GPU frame time (ms) of a frame rendered at `rendered`. Returns
    // true when the controller changed the size.
    bool Sample(float gpuMs, Size rendered)
    {
        if (rendered != Current() || !(gpuMs > 0.0f))
        {
            ++ignored;
            return false;
        }
        m_sinceRaise = std::min(m_sinceRaise + 1, 1 << 30);
        m_sinceDrop = std::min(m_sinceDrop + 1, 1 << 30);
        if (m_settle > 0)
        {
            --m_settle;
            return false;
        }
        const Config &c = m_config;
        const Size cur = Current();
        if (gpuMs > c.budgetMs)
        {
            ResetWindow();
            if (m_level <= MinLevel(c))
                return false;
            // Proportional model: pixels x (target / observed GPU time).
            const double ratio = (double)c.budgetMs * c.dropTarget / gpuMs;
            const double wantW = std::sqrt(ratio) * cur.width;
            int level = (int)std::floor(wantW / kWidthQuantum);
            level = std::min(level, m_level - 1);
            level = std::max(level, m_level - c.maxDropLevels);
            level = std::max(level, MinLevel(c));
            if (m_sinceRaise < 2 * c.raiseFrames)
                m_backoff = std::min(m_backoff * 2, c.backoffMax);
            SetLevel(level);
            ++drops;
            m_sinceDrop = 0;
            return true;
        }
        if (m_sinceDrop >= 8 * c.raiseFrames && m_backoff > 1)
        {
            m_backoff /= 2;
            m_sinceDrop = 0;
        }
        const int top = TopLevel(c);
        if (m_level >= top)
        {
            ResetWindow();
            return false;
        }
        const int next = std::min(m_level + c.raiseLevels, top);
        const double predicted = (double)gpuMs * (double)SizeForLevel(c, next).Pixels() / (double)cur.Pixels();
        ++m_raiseCount;
        m_raiseSum += predicted;
        m_raiseMax = std::max(m_raiseMax, predicted);
        if (m_raiseCount < c.raiseFrames * m_backoff)
            return false;
        const bool fits = m_raiseSum / m_raiseCount <= (double)c.budgetMs * c.raiseMargin &&
                          m_raiseMax <= (double)c.budgetMs * c.raisePeak;
        ResetWindow();
        if (!fits)
            return false;
        SetLevel(next);
        ++raises;
        m_sinceRaise = 0;
        return true;
    }

    int Level() const { return m_level; }
    Size Current() const { return SizeForLevel(m_config, m_level); }
    float Scale() const { return (float)Current().width / (float)m_config.maxWidth; }
    int Backoff() const { return m_backoff; }
    const Config &GetConfig() const { return m_config; }

    uint64_t changes = 0, drops = 0, raises = 0, ignored = 0;

private:
    void SetLevel(int level)
    {
        if (level == m_level)
            return;
        m_level = level;
        ResetWindow();
        m_settle = m_config.settleFrames;
        ++changes;
    }

    void ResetWindow()
    {
        m_raiseCount = 0;
        m_raiseSum = m_raiseMax = 0.0;
    }

    Config m_config;
    int m_level = 1;
    int m_raiseCount = 0;
    double m_raiseSum = 0.0, m_raiseMax = 0.0;
    int m_settle = 0;
    int m_backoff = 1;
    int m_sinceRaise = 1 << 30, m_sinceDrop = 1 << 30;
};

} // namespace dynres

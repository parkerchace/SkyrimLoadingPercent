#include "PCH.h"
#include <Xinput.h>
#include "ScaleformManager.h"
#include "ProgressTracker.h"
#include "config/Settings.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <thread>
#include <vector>
#include <algorithm>

namespace ScaleformManager {

std::atomic<bool> g_loadMenuCurrentlyOpen{ false };
void ReleaseLoadingMenuHold();

namespace {

static std::atomic<bool> g_holdActive{ false };

// ── BGSSaveLoadManager::Thread::Unk_02 ──────────────────────────────────────
using ThreadFn = void(*)(RE::BGSSaveLoadManager::Thread*);
static ThreadFn s_origThreadFn = nullptr;

void HookThreadUnk02(RE::BGSSaveLoadManager::Thread* a_this)
{
    s_origThreadFn(a_this);
    logger::info("ScaleformManager: Unk_02 done — waiting for hold release");
    // Failsafe cap (only reachable while holdScreen has no key pressed, or if
    // AdvanceMovie stopped ticking): never wedge the load thread indefinitely
    // unless the user explicitly opted into the hold-for-keypress feature.
    bool userHold = Settings::GetSingleton().holdScreen;
    for (int i = 0; g_holdActive.load(std::memory_order_acquire); ++i) {
        if (!userHold && i >= 1500) {  // 15 s
            logger::warn("ScaleformManager: Unk_02 hold timed out — forcing release");
            g_holdActive.store(false, std::memory_order_release);
            break;
        }
        ::Sleep(10);
    }
    logger::info("ScaleformManager: Unk_02 hold released");
}

// Hoisted here so HookProcessMessage (defined before the main state block) can see them.
static bool              s_loading          = false;
static bool              s_draining         = false;
static std::atomic<bool> s_kHideReceived    { false };
static std::atomic<bool> s_hidePending      { false };  // game tried to close while held → load is really done
static bool              s_frozenAtComplete = false;

// ── LoadingMenu::ProcessMessage (vtbl[4]) ────────────────────────────────────
using ProcessMsgFn = RE::UI_MESSAGE_RESULTS(*)(RE::IMenu*, RE::UIMessage&);
static ProcessMsgFn s_origPM = nullptr;

static RE::UI_MESSAGE_RESULTS HookProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_msg)
{
    auto msgType = a_msg.type.get();
    if (msgType == RE::UI_MESSAGE_TYPE::kHide ||
        msgType == RE::UI_MESSAGE_TYPE::kForceHide) {
        if (g_holdActive.load(std::memory_order_acquire)) {
            // The game closing the menu is a REAL completion signal — record it
            // so the state machine can sweep the bar to 100 before we let the
            // menu actually hide.
            s_hidePending.store(true, std::memory_order_release);
            logger::info("ProcessMessage kHide BLOCKED (hold active)");
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
        // Clear our drawing now so no stale frame lingers on the cached Scaleform movie.
        // Also signal UpdateLoadingState to freeze at 100% — AdvanceMovie will re-draw
        // the completed bar in the very next tick (during the fade), then the post-close
        // tick clears everything once the menu is fully gone.
        if (a_this && a_this->uiMovie) {
            auto* mv = a_this->uiMovie.get();
            mv->Invoke("_root.clear", nullptr, nullptr, 0);
            RE::GFxValue tf, r;
            mv->GetVariable(&tf, "_root.slpT");
            if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
            mv->GetVariable(&tf, "_root.slpK");
            if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
        }
        if (s_loading || s_draining)
            s_kHideReceived.store(true, std::memory_order_release);
    }
    return s_origPM(a_this, a_msg);
}

// ── GFx drawing state ────────────────────────────────────────────────────────
// Set by GfxUpdate before any draw call; helpers read these globals.
static RE::GFxMovieView* s_mv = nullptr;
static float             s_ga = 1.0f;  // cfg.overlayAlpha — applied inside every helper

// Canvas centre in stage coords — set in GfxInit, read by GMoveTo/GLineTo offsets.
static float s_animCX = 80.0f;
static float s_animCY = 360.0f;
// Skyrim loading screens use pillarbox (height-fill uniform scale) on all displays,
// so both axes scale by the same factor and no x-correction is needed.
static float s_xAspectCorr = 1.0f;

static constexpr float kPi  = 3.14159265f;
static constexpr float kDeg = kPi / 180.0f;

// Low-level GFx Invoke wrappers — draw on _root directly (immune to timeline resets)
static void GClear() { s_mv->Invoke("_root.clear", nullptr, nullptr, 0); }
static void GMoveTo(double x, double y) {
    RE::GFxValue a[2];
    a[0].SetNumber(x * double(s_xAspectCorr) + s_animCX);
    a[1].SetNumber(y + s_animCY);
    s_mv->Invoke("_root.moveTo", nullptr, a, 2);
}
static void GLineTo(double x, double y) {
    RE::GFxValue a[2];
    a[0].SetNumber(x * double(s_xAspectCorr) + s_animCX);
    a[1].SetNumber(y + s_animCY);
    s_mv->Invoke("_root.lineTo", nullptr, a, 2);
}
// alpha in [0,1], multiplied by s_ga inside
static void GLineStyle(double thick, uint32_t rgb, float alpha) {
    RE::GFxValue a[3];
    a[0].SetNumber(thick);
    a[1].SetNumber(double(rgb & 0xFFFFFF));
    a[2].SetNumber(double(alpha * s_ga * 100.0f));
    s_mv->Invoke("_root.lineStyle", nullptr, a, 3);
}
static void GNoLine() { s_mv->Invoke("_root.lineStyle", nullptr, nullptr, 0); }
static void GBeginFill(uint32_t rgb, float alpha) {
    RE::GFxValue a[2];
    a[0].SetNumber(double(rgb & 0xFFFFFF));
    a[1].SetNumber(double(alpha * s_ga * 100.0f));
    s_mv->Invoke("_root.beginFill", nullptr, a, 2);
}
static void GEndFill() { s_mv->Invoke("_root.endFill", nullptr, nullptr, 0); }

// Convert IM_COL32-style alpha (0-255) to [0,1] for passing to helpers
static float A255(int a) { return a / 255.0f; }

// ── Geometric helpers ────────────────────────────────────────────────────────
static void GFillCircle(double cx, double cy, double r, uint32_t rgb, float alpha, int segs = 24) {
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(cx + r, cy);
    for (int i = 1; i <= segs; i++) {
        float ang = (float(i) / segs) * 2.0f * kPi;
        GLineTo(cx + r * cosf(ang), cy + r * sinf(ang));
    }
    GEndFill();
}

static void GStrokeCircle(double cx, double cy, double r, uint32_t rgb, float alpha, float thick, int segs = 24) {
    GLineStyle(thick, rgb, alpha);
    GMoveTo(cx + r, cy);
    for (int i = 1; i <= segs; i++) {
        float ang = (float(i) / segs) * 2.0f * kPi;
        GLineTo(cx + r * cosf(ang), cy + r * sinf(ang));
    }
}

static void GFillArcDeg(double cx, double cy, double r, float startDeg, float endDeg, uint32_t rgb, float alpha) {
    if (endDeg <= startDeg) return;
    int segs = (std::max)(6, int((endDeg - startDeg) / 3.0f));
    float sd = startDeg * kDeg, ed = endDeg * kDeg;
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(cx, cy);
    for (int i = 0; i <= segs; i++) {
        float a = sd + (ed - sd) * (float(i) / segs);
        GLineTo(cx + r * cosf(a), cy + r * sinf(a));
    }
    GLineTo(cx, cy); GEndFill();
}

static void GStrokeArcDeg(double cx, double cy, double r, float startDeg, float endDeg, uint32_t rgb, float alpha, float thick) {
    if (endDeg <= startDeg) return;
    int segs = (std::max)(6, int((endDeg - startDeg) / 3.0f));
    float sd = startDeg * kDeg, ed = endDeg * kDeg;
    GLineStyle(thick, rgb, alpha);
    for (int i = 0; i <= segs; i++) {
        float a = sd + (ed - sd) * (float(i) / segs);
        if (i == 0) GMoveTo(cx + r * cosf(a), cy + r * sinf(a));
        else GLineTo(cx + r * cosf(a), cy + r * sinf(a));
    }
}

static void GFillEllipse(double cx, double cy, double rx, double ry, uint32_t rgb, float alpha, int segs = 24) {
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(cx + rx, cy);
    for (int i = 1; i <= segs; i++) {
        float a = (float(i) / segs) * 2.0f * kPi;
        GLineTo(cx + rx * cosf(a), cy + ry * sinf(a));
    }
    GEndFill();
}

static void GStrokeEllipse(double cx, double cy, double rx, double ry, uint32_t rgb, float alpha, float thick, int segs = 24) {
    GLineStyle(thick, rgb, alpha);
    GMoveTo(cx + rx, cy);
    for (int i = 1; i <= segs; i++) {
        float a = (float(i) / segs) * 2.0f * kPi;
        GLineTo(cx + rx * cosf(a), cy + ry * sinf(a));
    }
}

static void GLine(double x0, double y0, double x1, double y1, uint32_t rgb, float alpha, float thick) {
    GLineStyle(thick, rgb, alpha); GMoveTo(x0, y0); GLineTo(x1, y1);
}

static void GFillRect(double x0, double y0, double x1, double y1, uint32_t rgb, float alpha) {
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(x0, y0); GLineTo(x1, y0); GLineTo(x1, y1); GLineTo(x0, y1); GLineTo(x0, y0);
    GEndFill();
}

static void GStrokeRect(double x0, double y0, double x1, double y1, uint32_t rgb, float alpha, float thick) {
    GLineStyle(thick, rgb, alpha);
    GMoveTo(x0, y0); GLineTo(x1, y0); GLineTo(x1, y1); GLineTo(x0, y1); GLineTo(x0, y0);
}

static void GFillPoly(const float* xs, const float* ys, int n, uint32_t rgb, float alpha) {
    if (n < 3) return;
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(xs[0], ys[0]);
    for (int i = 1; i < n; i++) GLineTo(xs[i], ys[i]);
    GLineTo(xs[0], ys[0]); GEndFill();
}

static void GStrokePoly(const float* xs, const float* ys, int n, bool closed, uint32_t rgb, float alpha, float thick) {
    if (n < 2) return;
    GLineStyle(thick, rgb, alpha);
    GMoveTo(xs[0], ys[0]);
    for (int i = 1; i < n; i++) GLineTo(xs[i], ys[i]);
    if (closed) GLineTo(xs[0], ys[0]);
}

static void GFillTri(double x0, double y0, double x1, double y1, double x2, double y2, uint32_t rgb, float alpha) {
    GNoLine(); GBeginFill(rgb, alpha);
    GMoveTo(x0, y0); GLineTo(x1, y1); GLineTo(x2, y2); GLineTo(x0, y0); GEndFill();
}

static void GStrokeTri(double x0, double y0, double x1, double y1, double x2, double y2, uint32_t rgb, float alpha, float thick) {
    GLineStyle(thick, rgb, alpha);
    GMoveTo(x0, y0); GLineTo(x1, y1); GLineTo(x2, y2); GLineTo(x0, y0);
}

// Gear helper — used by GAnim_DwemerCogs
static void GfxGear(float gx, float gy, float r, float rot, int teeth, uint32_t col, float alpha) {
    float toothH  = r * 0.24f;
    float halfAng = kPi / float(teeth) * 0.38f;
    for (int i = 0; i < teeth; i++) {
        float a  = rot + (i / float(teeth)) * 2.0f * kPi;
        float a1 = a - halfAng, a2 = a + halfAng;
        float tx[4] = { gx+cosf(a1)*r, gx+cosf(a1)*(r+toothH), gx+cosf(a2)*(r+toothH), gx+cosf(a2)*r };
        float ty[4] = { gy+sinf(a1)*r, gy+sinf(a1)*(r+toothH), gy+sinf(a2)*(r+toothH), gy+sinf(a2)*r };
        GFillPoly(tx, ty, 4, col, alpha * 0.55f);
        GStrokePoly(tx, ty, 4, true, col, alpha * 0.90f, 1.2f);
    }
    GFillCircle(gx, gy, r * 0.68f, col, alpha * 0.25f, 32);
    GStrokeCircle(gx, gy, r * 0.68f, col, alpha * 0.80f, 1.5f, 32);
    for (int i = 0; i < 3; i++) {
        float sa = rot + (i / 3.0f) * 2.0f * kPi;
        GStrokeCircle(gx + cosf(sa)*r*0.40f, gy + sinf(sa)*r*0.40f, r * 0.09f, col, alpha * 0.55f, 1.0f, 6);
    }
    GFillCircle(gx, gy, r * 0.12f, col, alpha, 8);
}

// ── Animation state ───────────────────────────────────────────────────────────
static int   s_tick         = 0;
static int   s_loadIndex    = 0;
static int   s_activeStyle  = 0;
static int   s_constPattern = 0;
static int   s_wallSeed     = 0;
static int   s_crackSeed    = 0;
static int   s_mountainSeed = 0;
static std::vector<int> s_blockOrder;

// Shuffle-bag for Random Style: each animation plays once before any repeat.
// Reshuffled whenever the bag empties or the lore-friendly pool size changes.
static std::vector<int> s_animBag;
static size_t s_animBagPos  = 0;
static int    s_animBagPool = -1;  // pool size the current bag was built for

// ── GFx overlay state ─────────────────────────────────────────────────────────
// s_loading, s_draining, s_kHideReceived, s_hidePending, s_frozenAtComplete
// hoisted above HookProcessMessage.
static bool      s_lingering      = false;
static bool      s_awaitingKey    = false;
static float     s_display        = 0.0f;
static LONGLONG  s_prevRawBytes   = -1;
static bool      s_openSeen       = false;  // close events without a matching open carry garbage timings
static bool      s_completionReleased = false;  // one-shot guard for ReleaseLoadingMenuHold
static bool      s_completeViaEvent   = false;  // true = kPostLoadGame/kNewGame; false = blocked-kHide fallback
static bool      s_completionDetected = false;  // one-shot guard so the completion check only fires once per load
static std::chrono::steady_clock::time_point s_lingerStart;
static std::chrono::steady_clock::time_point s_loadStart;
static std::chrono::steady_clock::time_point s_lastIoChange;

static bool  g_gfxCreated  = false;
static bool  g_customSwf   = false;  // true when interface/LoadingMenu.swf is deployed
static float s_stageW      = 1280.0f;
static float s_stageH      = 720.0f;
// s_animCX / s_animCY declared above (needed by GMoveTo/GLineTo before their first use)

// ── State machine ─────────────────────────────────────────────────────────────
static void UpdateLoadingState()
{
    auto& tracker = ProgressTracker::GetSingleton();
    auto  phase   = tracker.GetPhase();
    auto& cfg     = Settings::GetSingleton();

    // A kHide made it through (hold already released) — the menu is fading out.
    // Freeze at 100 so every remaining AdvanceMovie frame shows a complete bar.
    if (s_kHideReceived.exchange(false, std::memory_order_acq_rel) && !s_frozenAtComplete) {
        tracker.OnLoadComplete();
        s_frozenAtComplete = true;
        s_draining         = false;
        logger::info("ScaleformManager: kHide passed — freezing display at 100%");
    }
    if (s_frozenAtComplete) {
        s_display = 100.0f;
        return;
    }

    if (!s_loading && !s_draining && phase == LoadPhase::Tracking) {
        s_loading      = true;
        s_display      = 0.0f;
        s_prevRawBytes = -1;
        logger::info("ScaleformManager: GFx overlay load started");
    }

    if (!s_loading && !s_draining && !s_lingering && !s_awaitingKey) {
        s_display = 0.0f;
        return;
    }

    // Record when byte flow last changed. Used by the times log to split a load
    // into I/O and CPU tails, and by the cached-load detection below to tell a
    // genuinely-finished load from a cold one still streaming. Never used to
    // declare completion (100% stays gated on the real completion event).
    if (s_loading) {
        LONGLONG raw = tracker.GetBytesThisLoad();
        if (raw != s_prevRawBytes) {
            s_prevRawBytes = raw;
            s_lastIoChange = std::chrono::steady_clock::now();
        }
    }

    // Completion comes ONLY from real signals: kPostLoadGame / kNewGame set the
    // phase to Complete, and a blocked kHide means the game itself decided the
    // load is over (the only signal door/cell transitions emit). I/O going
    // quiet is NOT completion — initial loads have multi-second CPU-only tails.
    //
    // One-shot: ProgressTracker's phase stays Complete for the rest of the load
    // (it only resets on menu close), so without this guard the check below
    // re-fires every frame once s_draining flips back to false at 100% — and
    // since s_loading is deliberately never cleared here (see the render-skip
    // fix elsewhere), that re-fire would overwrite a genuine kHide-detected
    // s_completeViaEvent=false back to true on the very next frame, silently
    // defeating the "only extend the hold for real events" gate below.
    if (s_loading && !s_draining && !s_completionDetected) {
        bool byEvent = (phase == LoadPhase::Complete);
        bool byHide  = s_hidePending.load(std::memory_order_acquire);
        if (byEvent || byHide) {
            tracker.OnLoadComplete();  // idempotent — covers the kHide-only path
            s_draining          = true;
            s_completeViaEvent  = byEvent;
            s_completionDetected = true;
            logger::info("ScaleformManager: load done ({}) — sweeping from {:.0f}%",
                byEvent ? "event" : "kHide", s_display);
        }
    }

    if (s_draining) {
        // Fast sweep to 100. The menu is held (kHide blocked) until the sweep
        // lands, so the player sees the bar reach 100 on every load — it can
        // never vanish at 67%.
        float gap  = 100.0f - s_display;
        s_display += std::clamp(gap * 0.25f, 0.6f, 1.5f);
        if (s_display >= 99.9f) {
            s_display  = 100.0f;
            s_draining = false;
            // holdScreen/lingerSeconds extend g_holdActive well past the brief
            // sweep window. That's safe for save loads: kPostLoadGame's
            // WaitForHoldRelease genuinely blocks the game thread until we
            // release, so nothing else can proceed out of order. Cell/door
            // transitions have no such guarantee: their only "hold" is the
            // blocked kHide message, and the world becomes visible during an
            // extended wait regardless (confirmed experimentally — the reveal
            // isn't driven by anything in Scaleform/AS2, so it can't be
            // intercepted from here). Extending the hold there just leaves the
            // player stuck looking at a frozen, already-revealed world, so it
            // is intentionally restricted to real completion events only.
            if (s_completeViaEvent) {
                if (cfg.lingerSeconds > 0) {
                    s_lingering   = true;
                    s_lingerStart = std::chrono::steady_clock::now();
                }
                if (cfg.holdScreen) s_awaitingKey = true;
            }
        }
    } else if (s_loading) {
        // Mid-load target blends two real, current-load-only signals:
        //
        // 1. "base" (0-15%) — an exact fraction where one exists: save parse
        //    (.ess bytes read ÷ its real file size) or cell attach (refs
        //    attached ÷ the cells' real reference counts). Both denominators
        //    can be known only approximately early on, so this is kept small.
        //
        // 2. "crawl" (15-99%) — driven by this load's own cumulative bytes
        //    read (tracker.GetBytesThisLoad(), real, no cross-load history).
        //    There is no knowable total for streamed asset data (BSA archives
        //    report their whole-container size, not what a single cell will
        //    read from them), so this cannot be an exact fraction — but it is
        //    monotonic in real data and never plateaus while I/O keeps
        //    flowing, unlike a fixed-cap fraction of a bad denominator.
        double fSave  = tracker.GetSaveFraction();
        double fRefs  = tracker.GetRefsFraction();
        float  base   = tracker.WasSaveLoad()
            ? 15.0f * static_cast<float>(fSave)
            : 15.0f * (fRefs > 0.0 ? static_cast<float>(fRefs) : 0.0f);

        double mb    = static_cast<double>(tracker.GetBytesThisLoad()) / (1024.0 * 1024.0);
        float  crawl = 84.0f * static_cast<float>(1.0 - std::exp(-mb / 80.0));

        float target   = std::clamp(base + crawl, 0.0f, 99.0f);
        float rampRate = 0.10f;

        // Cached/fast-load rescue (cell loads only): the byte-crawl can only
        // report "whatever small number the bytes reached" for a load that was
        // served almost entirely from the OS cache — a 20 KB transition lands
        // near 13%, then the real completion event snaps it to 100. That low
        // number was never a fraction of anything. When the cell's references
        // are (essentially) all attached AND byte I/O has gone quiet, the load
        // is genuinely finished — no more data is coming — so ride the target up
        // to ~99 (at a snappier rate, since we know it's basically done) instead
        // of sitting on a fabricated low byte number. This can only trip when
        // streaming has stopped, so a COLD load (disk still churning,
        // s_lastIoChange keeps refreshing) never enters here and its byte-crawl
        // is untouched. 100% remains gated on the completion event.
        if (!tracker.WasSaveLoad() && fRefs >= 0.98) {
            float ioQuietMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - s_lastIoChange).count();
            if (ioQuietMs >= 150.0f) {
                target   = 99.0f;
                rampRate = 0.25f;
            }
        }

        float gap = target - s_display;
        if (gap > 0.0f) s_display += gap * rampRate;
    }

    if (s_lingering) {
        float lingerElapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - s_lingerStart).count();
        if (lingerElapsed >= static_cast<float>(cfg.lingerSeconds))
            s_lingering = s_awaitingKey = false;
    }

    if (s_awaitingKey) {
        for (int vk = 0x08; vk <= 0xFE; vk++) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000) { s_awaitingKey = false; break; }
        }
        if (s_awaitingKey) {
            XINPUT_STATE xi{};
            for (DWORD i = 0; i < XUSER_MAX_COUNT && s_awaitingKey; ++i) {
                if (XInputGetState(i, &xi) == ERROR_SUCCESS && xi.Gamepad.wButtons)
                    s_awaitingKey = false;
            }
        }
    }

    // Release the hold exactly once, but keep s_loading true (and therefore keep
    // drawing) so the 100% frame is actually rendered at least once — the real
    // menu-close event (MenuEventSink) is what finally clears s_loading, once
    // the game has processed the resent kHide and truly closed the menu. Setting
    // s_loading=false here would make shouldDraw false on this very tick and the
    // 100% frame would never be drawn — it would jump straight to a clear.
    if (s_display >= 100.0f && !s_draining && !s_lingering && !s_awaitingKey
        && !s_completionReleased) {
        s_completionReleased = true;
        ReleaseLoadingMenuHold();
    }
}

// ── 20 Animation functions ────────────────────────────────────────────────────
// All coords relative to (0,0) = MovieClip centre.
// sc = cfg.scale * s_stageH / 1080.f    p = progress 0-100    t = s_tick (float)

static void GAnim_CircleFill(float sc, float p, float t, uint32_t col)
{
    float glow   = 0.5f + 0.5f * sinf(t * 0.08f);
    float filled = p / 100.0f;

    GStrokeCircle(0, 0, 92*sc, col, 0.35f, 1.0f, 72);
    for (int i = 0; i < 12; i++) {
        float a = i / 12.0f * 2.0f * kPi;
        GLine(cosf(a)*86*sc, sinf(a)*86*sc, cosf(a)*92*sc, sinf(a)*92*sc, col, 0.35f, 1.5f);
    }
    if (filled > 0.001f) {
        GFillArcDeg(0, 0, 74*sc, -90.0f, -90.0f + 360.0f*filled, col, 0.18f);
        GStrokeArcDeg(0, 0, 74*sc, -90.0f, -90.0f + 360.0f*filled, col, 0.8f + glow*0.2f, 4.0f);
    }
    if (filled < 0.999f)
        GStrokeArcDeg(0, 0, 74*sc, -90.0f + 360.0f*filled, 270.0f, 0x333355, 0.55f, 2.0f);

    GFillCircle(0, 0, (18.0f + 3.0f*glow)*sc, col, 0.30f + 0.20f*glow, 32);

    if (filled > 0.001f) {
        float ta = (-90.0f + 360.0f*filled) * kDeg;
        GLine(cosf(ta)*65*sc, sinf(ta)*65*sc, cosf(ta)*85*sc, sinf(ta)*85*sc, 0xFFFFFF, A255(210), 3.0f);
    }
}

static void GAnim_DragonEye(float sc, float p, float t, uint32_t col)
{
    float f   = p / 100.0f;
    float eye = f * 80.0f * sc;
    float ww  = 100.0f * sc;

    GLineStyle(3.0f, col, 0.90f);
    GMoveTo(-ww, 0);
    for (int i = 0; i <= 32; i++) {
        float frac = i / 32.0f;
        GLineTo((frac*2.0f-1.0f)*ww, -eye*sinf(frac*kPi));
    }
    GLineTo(ww, 0);
    GMoveTo(-ww, 0);
    for (int i = 0; i <= 32; i++) {
        float frac = i / 32.0f;
        GLineTo((frac*2.0f-1.0f)*ww, eye*sinf(frac*kPi));
    }
    GLineTo(ww, 0);

    if (eye > 2.0f) {
        float pw = 12.0f * sc;
        GFillEllipse(0, 0, pw, eye * 0.85f, col, 0.70f, 32);
        GStrokeEllipse(0, 0, pw, eye * 0.85f, col, 1.0f, 1.5f, 32);
    }
    int   wc    = static_cast<int>(f * 6);
    float pulse = 0.10f + 0.06f * sinf(t * 0.05f);
    for (int i = 0; i < wc; i++)
        GStrokeCircle(0, 0, (i+1)*16.0f*sc, col, pulse, 1.0f, 48);
}

static void GAnim_NordicRunes(float sc, float p, float t, uint32_t col)
{
    struct Ring { int n; float r, spd, sz; };
    Ring rings[] = { {8,40,0.6f,5},{12,65,-0.35f,4},{16,88,0.2f,3} };
    int litCount = static_cast<int>(p / 100.0f * 36);
    int idx      = 0;

    for (auto& ring : rings) {
        float off = t * ring.spd * kDeg;
        int idxStart = idx;
        // Unlit dots
        GNoLine(); GBeginFill(col, 0.18f);
        for (int i = 0; i < ring.n; i++) {
            if (idxStart + i < litCount) continue;
            float a  = (i / float(ring.n)) * 2.0f * kPi + off;
            float dx = cosf(a)*ring.r*sc, dy = sinf(a)*ring.r*sc;
            float r2 = ring.sz * sc;
            GMoveTo(dx + r2, dy);
            for (int k = 1; k <= 12; k++) GLineTo(dx + r2*cosf(k/12.f*2.f*kPi), dy + r2*sinf(k/12.f*2.f*kPi));
        }
        GEndFill();
        // Lit dots
        GNoLine(); GBeginFill(col, 0.90f);
        for (int i = 0; i < ring.n; i++) {
            if (idxStart + i >= litCount) continue;
            float a  = (i / float(ring.n)) * 2.0f * kPi + off;
            float dx = cosf(a)*ring.r*sc, dy = sinf(a)*ring.r*sc;
            float r2 = ring.sz * sc;
            GMoveTo(dx + r2, dy);
            for (int k = 1; k <= 12; k++) GLineTo(dx + r2*cosf(k/12.f*2.f*kPi), dy + r2*sinf(k/12.f*2.f*kPi));
        }
        GEndFill();
        idx += ring.n;
    }

    for (int i = 0; i < 4; i++) {
        float a   = i * 90.0f * kDeg;
        float cx2 = cosf(a)*100*sc, cy2 = sinf(a)*100*sc;
        float px2 = cosf(a+kPi/2)*6*sc, py2 = sinf(a+kPi/2)*6*sc;
        GLine(cx2-px2, cy2-py2, cx2+px2, cy2+py2, col, 0.30f, 1.5f);
        GLine(cx2, cy2-10*sc, cx2, cy2+10*sc, col, 0.30f, 1.5f);
    }
    GStrokeCircle(0, 0, 14*sc, col, 0.50f, 2.0f, 24);
    GFillCircle(0, 0, 10*sc, col, 0.40f, 20);
}

static void GAnim_Waveform(float sc, float p, float t, uint32_t col)
{
    float f  = p / 100.0f;
    int   n  = 24;
    float bw = 8*sc, sp = 13*sc;
    float tw = n * sp;

    float rr = 0x40/255.0f + (((col>>16)&0xFF)/255.0f - 0x40/255.0f)*f;
    float gg = 0x60/255.0f + (((col>> 8)&0xFF)/255.0f - 0x60/255.0f)*f;
    float bb = 0xCC/255.0f + ((col&0xFF)/255.0f - 0xCC/255.0f)*f;
    uint32_t barRgb = (uint32_t(rr*255)<<16)|(uint32_t(gg*255)<<8)|uint32_t(bb*255);
    float barAlpha = A255(200);

    for (int i = 0; i < n; i++) {
        float bx  = -tw/2 + (i + 0.5f)*sp;
        float env = 1.0f - fabsf((i / float(n-1)) * 2.0f - 1.0f);
        float w1  = sinf(i * 0.6f - t * 0.15f) * 0.5f + 0.5f;
        float w2  = sinf(i * 1.1f + t * 0.08f) * 0.25f + 0.25f;
        float bh  = (w1 + w2) * env * 70*sc * f + 4*sc;
        GFillRect(bx-bw/2, -bh, bx+bw/2, bh, barRgb, barAlpha);
        GLine(bx-bw/2, -bh, bx+bw/2, -bh, 0xFFFFFF, A255(80), 1.0f);
    }
    GLine(-tw/2, 0, tw/2, 0, col, 0.28f, 1.0f);
}

static void GAnim_PixelBlocks(float sc, float p, float /*t*/, uint32_t col)
{
    if (s_blockOrder.empty()) {
        s_blockOrder.resize(64);
        for (int i = 0; i < 64; i++) s_blockOrder[i] = i;
        for (int i = 63; i > 0; i--) { int j = rand()%(i+1); std::swap(s_blockOrder[i], s_blockOrder[j]); }
    }
    int   litN = static_cast<int>(p / 100.0f * 64);
    float bs   = 18*sc, gap = 3*sc, step = bs + gap;
    float ox   = -4*step + gap/2, oy = -4*step + gap/2;

    for (int idx = 0; idx < 64; idx++) {
        int   cell  = s_blockOrder[idx];
        float bx    = ox + (cell % 8) * step;
        float by    = oy + (cell / 8) * step;
        bool  isLit = (idx < litN);
        GFillRect(bx, by, bx+bs, by+bs, col, isLit ? 0.85f : 0.38f);
        GStrokeRect(bx, by, bx+bs, by+bs, isLit ? col : 0x334455u, isLit ? 0.60f : 0.22f, 1.0f);
        if (isLit && idx == litN-1)
            GFillRect(bx+2, by+2, bx+bs-2, by+bs-2, 0xFFFFFF, A255(55));
    }
}

static void GAnim_OrbitDots(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    int   n    = 12;
    float r    = 70*sc;
    int   litN = static_cast<int>(f * n);
    float curs = t * 2.5f * kDeg;

    for (int i = 0; i < n; i++) {
        float a   = (i / float(n)) * 2.0f * kPi - kPi/2;
        bool  act = (i < litN);
        if (act) {
            for (int tr = 1; tr <= 5; tr++) {
                float ta = (i/float(n))*2.0f*kPi - kPi/2 - tr*4.0f*kDeg;
                GFillCircle(cosf(ta)*r, sinf(ta)*r, (4.0f-tr*0.5f)*sc, col, (15-tr*2)/100.0f, 10);
            }
        }
        GFillCircle(cosf(a)*r, sinf(a)*r, (act ? 7.0f : 4.0f)*sc, col, act ? 0.90f : 0.22f, 16);
    }
    GFillCircle(cosf(curs)*r, sinf(curs)*r, 5*sc, 0xFFFFFF, A255(204), 14);
    GStrokeCircle(0, 0, r, col, 0.18f, 1.0f, 48);
    GStrokeCircle(0, 0, 14*sc, col, 0.40f, 2.0f, 24);
}

static void GAnim_CompassRose(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    int   revN = static_cast<int>(f * 8);
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);

    for (int i = 0; i < 8; i++) {
        float ba  = (i / 8.0f) * 2.0f * kPi - kPi/2;
        bool  card = (i % 2 == 0);
        float len  = (card ? 90.0f : 55.0f) * sc;
        float wid  = (card ? 14.0f :  9.0f) * sc;
        bool  rev  = (i < revN);
        float tipX = cosf(ba)*len, tipY = sinf(ba)*len;
        float sA   = ba + kPi/2, sB = ba - kPi/2;
        float bx2  = cosf(ba)*15*sc, by2 = sinf(ba)*15*sc;
        float mx   = cosf(ba)*(len*0.45f), my = sinf(ba)*(len*0.45f);
        float px[7] = { bx2+cosf(sA)*wid, mx+cosf(sA)*wid*0.4f, tipX, mx+cosf(sB)*wid*0.4f,
                        bx2+cosf(sB)*wid, bx2, bx2+cosf(sA)*wid };
        float py[7] = { by2+sinf(sA)*wid, my+sinf(sA)*wid*0.4f, tipY, my+sinf(sB)*wid*0.4f,
                        by2+sinf(sB)*wid, by2, by2+sinf(sA)*wid };
        GFillPoly(px, py, 6, col, rev ? 0.80f : 0.28f);
        GStrokePoly(px, py, 7, false, col, rev ? 0.90f : 0.35f, 1.0f);
    }
    GStrokeCircle(0, 0, 16*sc, col, 0.60f + 0.20f*glow, 2.0f, 24);
    GFillCircle(0, 0, 12*sc, col, 0.50f, 20);
}

static void GAnim_Helix(float sc, float p, float t, uint32_t col)
{
    float f      = p / 100.0f;
    int   segs   = 48;
    float height = 90.0f * f * sc;
    float twist  = t * 0.05f;

    for (int i = 0; i <= segs; i++) {
        float frac   = i / float(segs);
        float hy     = -height + frac * height * 2.0f;
        float angle1 = frac * 4.0f * kPi + twist;
        float angle2 = angle1 + kPi;
        float amp    = (35.0f + 10.0f * sinf(frac * kPi)) * sc;
        float hx1 = cosf(angle1)*amp, hx2 = cosf(angle2)*amp;
        float d1  = sinf(angle1),     d2  = sinf(angle2);
        GFillCircle(hx1, hy, 3*sc, col, 0.30f + 0.55f*((d1+1)/2), 8);
        GFillCircle(hx2, hy, 3*sc, 0x5599CC, 0.30f + 0.55f*((d2+1)/2), 8);
        if (i % 4 == 0)
            GLine(hx1, hy, hx2, hy, col, 0.20f + 0.20f*fabsf(d1), 1.0f);
    }
    GLine(0, -height, 0, height, col, 0.20f, 1.0f);
}

static void GAnim_Snowflake(float sc, float p, float t, uint32_t /*col*/)
{
    float f      = p / 100.0f;
    float armLen = 80.0f * f * sc;

    for (int arm = 0; arm < 6; arm++) {
        float ba  = (arm / 6.0f) * 2.0f * kPi;
        float tipX = cosf(ba)*armLen, tipY = sinf(ba)*armLen;
        GLine(0, 0, tipX, tipY, 0x88CCFF, A255(204), 2.0f);
        for (int br = 1; br <= 3; br++) {
            float bf  = br / 4.0f;
            float bl  = (1.0f - bf) * 28.0f * f * sc;
            float bbx = cosf(ba)*(armLen*bf), bby = sinf(ba)*(armLen*bf);
            float a1  = ba + 60*kDeg, a2 = ba - 60*kDeg;
            GLine(bbx, bby, bbx+cosf(a1)*bl, bby+sinf(a1)*bl, 0xAADDFF, A255(178), 1.5f);
            GLine(bbx, bby, bbx+cosf(a2)*bl, bby+sinf(a2)*bl, 0xAADDFF, A255(178), 1.5f);
        }
    }
    if (f > 0.85f) {
        for (int sa = 0; sa < 6; sa++) {
            float spa   = (sa/6.0f)*2.0f*kPi;
            float flash = fabsf(sinf(t*0.2f + sa));
            GFillCircle(cosf(spa)*armLen, sinf(spa)*armLen, 4*sc, 0xFFFFFF, flash*A255(150), 10);
        }
    }
    GStrokeCircle(0, 0, 8*sc, 0x88CCFF, A255(178), 2.0f, 16);
    GFillCircle(0, 0, 5*sc, 0xCCEEFF, A255(153), 12);
}

static void GAnim_LinearBar(float sc, float p, float t, uint32_t col)
{
    float f  = p / 100.0f;
    float bh = 20.0f * sc;

    float availSpace = (std::min)(s_animCX, s_stageW - s_animCX);
    float bw = std::clamp(availSpace * 1.6f, 80.0f*sc, 700.0f*sc);
    float decoPad = 14.0f * sc;
    float bx;
    if (s_animCX < s_stageW * 0.45f)
        bx = 0;
    else if (s_animCX > s_stageW * 0.55f)
        bx = -bw;
    else
        bx = -bw * 0.5f;
    bx = std::clamp(bx, -s_animCX + decoPad, s_stageW - s_animCX - bw - decoPad);

    float by = -bh * 0.5f;
    GFillRect(bx-2, by-2, bx+bw+2, by+bh+2, 0x0D0D1A, A255(230));
    GStrokeRect(bx-2, by-2, bx+bw+2, by+bh+2, 0x444455, 0.80f, 1.0f);

    if (f > 0.001f) {
        float fw = bw * f;
        GFillRect(bx, by, bx+fw, by+bh, col, 0.85f);
        int stripes = static_cast<int>(fw / 4);
        for (int si = 0; si < stripes; si++)
            GFillRect(bx+si*4.0f, by, bx+si*4.0f+2.0f, by+bh, 0x000000, A255(45));
        float flash = 0.35f + 0.25f * sinf(t * 0.15f);
        GFillRect(bx+fw-4, by, bx+fw, by+bh, 0xFFFFFF, flash);
    }

    auto Diamond = [&](float dcx, float dcy, float ds) {
        float dx[4] = {dcx, dcx+ds, dcx, dcx-ds};
        float dy[4] = {dcy-ds, dcy, dcy+ds, dcy};
        GFillPoly(dx, dy, 4, col, 0.25f);
        GStrokePoly(dx, dy, 4, true, col, 0.70f, 1.5f);
    };
    Diamond(bx - 10*sc, 0, 10*sc);
    Diamond(bx + bw + 10*sc, 0, 10*sc);
}

static void GAnim_SoulGem(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f*sinf(t * 0.08f);

    float rimW = 34.0f*sc, belW = 52.0f*sc, baseW = 20.0f*sc;
    float tY   = -48.0f*sc, shY = -20.0f*sc, midY = 10.0f*sc, bY = 46.0f*sc;
    float legBot = bY + 14.0f*sc, flameBot = legBot + 2.0f*sc;

    float poolA = 0.5f + 0.5f*sinf(t*0.19f);
    GFillEllipse(0, flameBot+sc, 44.0f*sc, 5.5f*sc, col, 0.10f + 0.06f*poolA, 16);

    for (int fi = 0; fi < 5; fi++) {
        float n1 = sinf(t*0.17f+fi*1.31f), n2 = sinf(t*0.37f+fi*0.83f), n3 = sinf(t*0.61f+fi*2.07f);
        float noise = n1*0.45f + n2*0.35f + n3*0.20f;
        float hs    = 0.60f + 0.40f*noise;
        float baseFX = (fi-2)*12.0f*sc;
        float sway   = sinf(t*0.29f + fi*1.73f) * 1.8f*sc;
        float fh     = (8.5f + 6.0f*sinf(fi*1.57f))*sc * hs;
        float fw     = (5.0f + (fi%3)*1.9f)*sc;
        GFillTri(baseFX+sway,      flameBot-fh*1.10f, baseFX-fw*1.5f, flameBot, baseFX+fw*1.5f, flameBot, col, 0.10f);
        GFillTri(baseFX+sway*0.7f, flameBot-fh,       baseFX-fw,      flameBot, baseFX+fw,      flameBot, col, 0.38f);
        GFillTri(baseFX+sway*0.4f, flameBot-fh*0.55f, baseFX-fw*0.42f, flameBot-sc, baseFX+fw*0.42f, flameBot-sc, col, 0.68f);
        GFillTri(baseFX+sway*0.2f, flameBot-fh*0.22f, baseFX-fw*0.16f, flameBot-fh*0.08f, baseFX+fw*0.16f, flameBot-fh*0.08f, col, 0.92f);
    }

    float bxs[8] = {-rimW, rimW, belW, belW, baseW, -baseW, -belW, -belW};
    float bys[8] = {tY, tY, shY, midY, bY, bY, midY, shY};
    GFillPoly(bxs, bys, 8, 0x12151A, A255(252));

    if (f > 0.01f) {
        float liquidTop = bY - (bY-tY)*f;
        liquidTop = (std::max)(liquidTop, tY + 4.0f*sc);
        float liqW;
        if      (liquidTop >= midY) liqW = baseW+(belW-baseW)*(1.0f-(liquidTop-midY)/(bY-midY))-sc;
        else if (liquidTop >= shY)  liqW = belW - sc;
        else                        liqW = rimW+(belW-rimW)*(liquidTop-tY)/(shY-tY)-sc;
        liqW = std::clamp(liqW, 0.0f, belW);

        float lqx[10], lqy[10]; int ln = 0;
        lqx[ln]=liqW;     lqy[ln]=liquidTop; ln++;
        if (liquidTop < shY)  { lqx[ln]=belW-sc; lqy[ln]=shY;  ln++; lqx[ln]=belW-sc; lqy[ln]=midY; ln++; }
        else if (liquidTop < midY) { lqx[ln]=belW-sc; lqy[ln]=midY; ln++; }
        lqx[ln]=baseW-sc;    lqy[ln]=bY-sc;    ln++;
        lqx[ln]=-(baseW-sc); lqy[ln]=bY-sc;    ln++;
        if (liquidTop < midY) { lqx[ln]=-(belW-sc); lqy[ln]=midY; ln++; }
        if (liquidTop < shY)  { lqx[ln]=-(belW-sc); lqy[ln]=shY;  ln++; }
        lqx[ln]=-liqW; lqy[ln]=liquidTop; ln++;
        GFillPoly(lqx, lqy, ln, col, 0.36f+0.14f*glow);
        GLine(-liqW, liquidTop, liqW, liquidTop, col, 0.52f+0.26f*glow, 1.5f);

        if (f > 0.78f) {
            float bf = (f-0.78f)/0.22f;
            for (int si = 0; si < 5; si++) {
                float ph = fmodf(t*0.032f+si*0.22f, 1.0f);
                float sx = (si-2)*rimW*0.45f + sinf(ph*kPi*2.0f+si)*5.0f*sc;
                float sy = tY - ph*38.0f*sc;
                float sa = (ph<0.20f?ph/0.20f:(1.0f-ph)/0.80f)*0.28f*bf;
                GFillCircle(sx, sy, (2.5f+ph*8.0f)*sc, col, sa, 8);
            }
            for (int di = 0; di < 4; di++) {
                float ph  = fmodf(t*0.048f+di*0.27f, 1.0f);
                float dx2 = (di-1.5f)*rimW*0.55f;
                float arc = sinf(ph*kPi);
                float da  = arc*0.75f*bf;
                GFillCircle(dx2, tY - arc*18.0f*sc, (1.2f+bf*0.8f)*sc, col, da, 5);
            }
        }
        if (f > 0.88f) {
            float sf = (f-0.88f)/0.12f;
            for (int si = 0; si < 7; si++) {
                float ph  = fmodf(t*0.060f+si*0.145f, 1.0f);
                float ang = kPi*0.85f + si*kPi*0.26f;
                float sp2 = ph*(22.0f+si*5.0f)*sc*sf;
                GFillCircle(cosf(ang)*sp2, tY - fabsf(sinf(ang))*sp2*0.6f - ph*10.0f*sc, 1.4f*sc, col, (1.0f-ph)*0.80f*sf, 4);
            }
        }
    }

    GStrokePoly(bxs, bys, 8, true, col, 0.38f+0.22f*f, 2.0f);
    GFillEllipse(0, tY, rimW, 5.5f*sc, 0x101317, A255(248), 16);
    GStrokeEllipse(0, tY, rimW+sc, 6.0f*sc, col, 0.50f+0.28f*f, 2.0f, 16);
    for (int side = -1; side <= 1; side += 2) {
        float hx = side*(rimW+9.0f*sc);
        GStrokeCircle(hx, tY+3.0f*sc, 6.0f*sc, col, 0.32f+0.15f*f, 1.6f, 12);
        GLine(side*rimW, tY, hx-side*6.0f*sc, tY+3.0f*sc, col, 0.25f, 1.3f);
    }
    for (int li = -1; li <= 1; li++) {
        float lx = li*14.0f*sc;
        GLine(lx, bY, lx+li*6.0f*sc, legBot, col, 0.38f, 2.5f);
    }
}

static void GAnim_DwemerCogs(float sc, float p, float t, uint32_t col)
{
    float f     = p / 100.0f;
    float rot   = t * 0.020f;
    float alpha = 0.25f + 0.65f * f;
    const int   N1 = 12, N2 = 7, N3 = 5;
    const float R1 = 42.0f*sc, R2 = 25.0f*sc, R3 = 16.0f*sc;
    float g1x = -20.0f*sc, g1y = 5.0f*sc;
    float g2x = g1x + R1 + R2, g2y = g1y;
    float g3x = g2x, g3y = g2y - (R2+R3);
    float rot1 = rot;
    float rot2 = -(rot1 * float(N1)/float(N2)) + kPi/float(N2);
    float rot3 = -(rot2 * float(N2)/float(N3)) + kPi/float(N3);
    GfxGear(g3x, g3y, R3, rot3, N3, col, alpha * 0.65f);
    GfxGear(g2x, g2y, R2, rot2, N2, col, alpha * 0.82f);
    GfxGear(g1x, g1y, R1, rot1, N1, col, alpha);
    float arcR = R1 + 13.0f*sc;
    if (f > 0.01f) GStrokeArcDeg(g1x, g1y, arcR, -90.0f, -90.0f + 360.0f*f, col, 0.82f, 3.0f);
    GStrokeArcDeg(g1x, g1y, arcR, -90.0f + 360.0f*f, 270.0f, col, 0.14f, 1.2f);
}

static void GAnim_Shout(float sc, float p, float t, uint32_t col)
{
    float f = p / 100.0f;
    float R = 90.0f * sc;

    static const float starX[] = {-0.82f,-0.45f, 0.15f, 0.62f,-0.22f, 0.42f,-0.68f, 0.80f, 0.00f,-0.55f};
    static const float starY[] = {-0.80f,-0.55f,-0.90f,-0.65f,-0.38f,-0.78f,-0.20f,-0.45f,-0.70f,-0.92f};
    for (int si = 0; si < 10; si++) {
        float puls = 0.35f + 0.40f*sinf(t*0.05f + si*0.88f);
        float sr   = (0.9f + si%2) * sc;
        GFillCircle(starX[si]*R, starY[si]*R, sr, 0xEBF0FF, puls*(0.20f + 0.70f*f), 4);
    }

    struct Band { float yOff,amp,freq,spd; int r,g,b; float minF; };
    static const Band bands[] = {
        {-0.58f,0.07f,0.75f,0.85f, 48,255,120,0.15f},
        {-0.34f,0.09f,1.05f,1.18f, 28,220,195,0.40f},
        {-0.12f,0.06f,1.30f,0.62f, 70,155,255,0.62f},
        { 0.08f,0.05f,0.65f,1.50f,155, 75,255,0.80f},
    };
    float W = R * 2.30f;
    for (auto& b : bands) {
        float bF = std::clamp((f - b.minF) / 0.28f, 0.0f, 1.0f);
        if (bF < 0.01f) continue;
        float shimmer = 0.5f + 0.5f*sinf(t*0.06f + b.yOff*3.0f);
        float alp     = bF * (0.38f + 0.30f*shimmer);
        float baseY   = b.yOff * R;
        uint32_t bRgb = (uint32_t(b.r)<<16)|(uint32_t(b.g)<<8)|uint32_t(b.b);
        const int NPTS = 18;
        float px[NPTS], py[NPTS];
        for (int pi = 0; pi < NPTS; pi++) {
            float nx  = -1.0f + 2.0f*pi/float(NPTS-1);
            float wave = sinf(b.freq*nx*kPi + t*b.spd*0.05f) * b.amp * R;
            px[pi] = nx*W*0.5f; py[pi] = baseY + wave;
        }
        GStrokePoly(px, py, NPTS, false, bRgb, alp*0.28f, 20.0f*sc);
        GStrokePoly(px, py, NPTS, false, bRgb, alp*0.62f,  7.0f*sc);
        GStrokePoly(px, py, NPTS, false, bRgb, alp*0.88f,  2.2f*sc);
    }

    float horizY = 0.32f * R;
    GFillRect(-W*0.5f, horizY, W*0.5f, horizY+28.0f*sc, 0x04060A, A255(230));

    unsigned mState = static_cast<unsigned>(s_mountainSeed);
    auto mLcg = [](unsigned& s) -> float {
        s = s * 1664525u + 1013904223u;
        return float(s >> 1) / float(0x7FFFFFFFu);
    };
    float mxBase[7] = {-0.80f,-0.54f,-0.28f,-0.02f,0.25f,0.52f,0.78f};
    for (int mi = 0; mi < 7; mi++) {
        float mxj = mxBase[mi] + (mLcg(mState) - 0.5f)*0.14f;
        float mh  = (10.0f + mLcg(mState)*28.0f) * sc;
        float mw  = mh * (0.60f + mLcg(mState)*0.50f);
        float mx2 = mxj * R;
        GFillTri(mx2-mw, horizY, mx2, horizY-mh, mx2+mw, horizY, 0x05070D, A255(238));
        GStrokeTri(mx2-mw, horizY, mx2, horizY-mh, mx2+mw, horizY, col, 0.11f + 0.09f*f, 0.7f);
    }
}

static void GAnim_Constellation(float sc, float p, float t, uint32_t col)
{
    float f = p / 100.0f;
    float R = 82.0f * sc;
    struct Star { float x, y; };
    static const Star stars[11] = {
        { 0.00f,-1.00f},{-0.58f,-0.55f},{ 0.58f,-0.55f},{-0.95f, 0.10f},{ 0.95f, 0.10f},
        {-0.40f, 0.65f},{ 0.40f, 0.65f},{ 0.00f, 1.00f},{-0.20f,-0.12f},{ 0.20f,-0.12f},{ 0.00f, 0.30f}
    };
    struct Edge { int a, b; };
    static const Edge patterns[6][13] = {
        {{0,1},{0,2},{1,3},{2,4},{3,5},{4,6},{5,7},{6,7},{1,8},{2,9},{8,9},{8,10},{9,10}},
        {{0,2},{2,4},{4,6},{6,7},{7,5},{5,3},{3,1},{1,0},{1,9},{9,10},{10,8},{8,5},{0,9}},
        {{0,7},{1,6},{2,5},{3,4},{0,10},{10,7},{8,4},{9,3},{8,10},{9,10},{1,2},{0,8},{0,9}},
        {{0,3},{0,4},{3,5},{4,6},{5,6},{1,5},{2,6},{7,10},{8,10},{9,10},{0,8},{0,9},{1,2}},
        {{0,1},{1,5},{5,7},{7,6},{6,2},{2,0},{0,10},{10,7},{3,8},{4,9},{1,8},{2,9},{3,4}},
        {{0,4},{4,2},{2,10},{10,3},{3,1},{1,9},{9,7},{7,8},{8,6},{6,5},{5,0},{0,6},{1,10}},
    };
    const Edge* edges = patterns[s_constPattern % 6];
    int litStars = static_cast<int>(f * 11);
    int litEdges = static_cast<int>(f * 13);

    for (int i = 0; i < litEdges; i++) {
        if (edges[i].a < litStars && edges[i].b < litStars)
            GLine(stars[edges[i].a].x*R, stars[edges[i].a].y*R,
                  stars[edges[i].b].x*R, stars[edges[i].b].y*R, col, 0.25f, 1.2f);
    }
    for (int i = 0; i < 11; i++) {
        float spx  = stars[i].x * R, spy = stars[i].y * R;
        bool  lit  = (i < litStars);
        float puls = lit ? (0.6f + 0.4f*sinf(t*0.08f + i*1.37f)) : 0.0f;
        float sr   = (lit ? (4.2f + 2.0f*puls) : 1.8f) * sc;
        if (lit) {
            GFillCircle(spx, spy, sr*2.8f, col, 0.10f*puls, 10);
            for (int ray = 0; ray < 4; ray++) {
                float ra = ray * kPi * 0.5f;
                GLine(spx, spy, spx+cosf(ra)*sr*2.5f, spy+sinf(ra)*sr*2.5f, col, 0.40f*puls, 1.0f);
            }
        }
        GFillCircle(spx, spy, sr, col, lit ? 0.90f : 0.15f, 10);
    }
}

static void GAnim_DragonScales(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);
    float sh = 36.0f*sc, tw2 = sh*0.32f, mw = sh*0.52f, bw2 = sh*0.30f;
    float rowStep = sh * 0.68f, colStep = mw * 2.10f;
    const int NROW = 4;
    const int rowCols[4] = { 5, 4, 5, 4 };
    float y0 = -rowStep * 1.5f;
    int total = 18, litCount = int(f * total);

    for (int row = NROW - 1; row >= 0; row--) {
        int   nc     = rowCols[row];
        float cy2    = y0 + row * rowStep;
        float xStart = -colStep * (nc - 1) * 0.5f;
        int   rowStart = 0;
        for (int r2 = 0; r2 < row; r2++) rowStart += rowCols[r2];

        for (int ci = 0; ci < nc; ci++) {
            int   idx   = rowStart + ci;
            bool  lit   = (idx < litCount);
            float cx2   = xStart + ci * colStep;
            float alpha = lit ? (0.52f + 0.30f*glow) : 0.08f;
            float px[7] = { cx2-tw2, cx2+tw2, cx2+mw, cx2+bw2, cx2, cx2-bw2, cx2-mw };
            float py[7] = { cy2, cy2, cy2+sh*0.40f, cy2+sh*0.74f, cy2+sh, cy2+sh*0.74f, cy2+sh*0.40f };
            GFillPoly(px, py, 7, lit ? col : 0x111820u, lit ? alpha*0.30f : 0.28f);
            if (lit) {
                float sx[4] = {px[0], px[1], px[2], px[6]};
                float sy[4] = {py[0], py[1], py[2], py[6]};
                GFillPoly(sx, sy, 4, col, alpha*0.20f);
            }
            GStrokePoly(px, py, 7, true, col, lit ? alpha : 0.11f, lit ? 1.6f : 0.9f);
            if (lit)
                GLine(cx2, cy2+sh*0.05f, cx2, cy2+sh*0.88f, col, alpha*0.48f, 1.1f);
        }
    }
}

static void GAnim_Enchantment(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.09f);
    struct Ring { float r, spd, thick, minF; };
    Ring rings[] = {
        {28.0f, 3.5f, 3.0f, 0.00f}, {50.0f,-2.0f, 2.5f, 0.25f},
        {72.0f, 1.3f, 2.0f, 0.50f}, {95.0f,-0.8f, 1.5f, 0.75f},
    };
    for (int ri = 0; ri < 4; ri++) {
        auto& r = rings[ri];
        float alpha = std::clamp((f - r.minF) / 0.25f, 0.0f, 1.0f) * (0.28f + 0.42f*glow);
        if (alpha < 0.01f) continue;
        float rotOff = t * r.spd;
        float arcDeg = (std::min)(360.0f * (f - r.minF) / (1.0f - r.minF + 0.01f), 360.0f);
        float segArc = arcDeg / 3 * 0.78f;
        for (int s = 0; s < 3; s++) {
            float startR = rotOff + (s / 3.0f) * 2.0f * kPi;
            float startD = startR / kDeg;
            GStrokeArcDeg(0, 0, r.r*sc, startD, startD + segArc, col, alpha, r.thick*sc);
        }
    }
    int runeCount = 1 + static_cast<int>(f * 6);
    for (int i = 0; i < runeCount && i < 7; i++) {
        float angle  = t * (0.025f + i * 0.006f) + i * 0.90f;
        float orbitR = (32.0f + i * 11.0f) * sc;
        float rpx = cosf(angle)*orbitR, rpy = sinf(angle)*orbitR;
        float rs  = (2.5f + (i % 3)) * sc;
        float rx[4] = {rpx, rpx+rs, rpx, rpx-rs};
        float ry[4] = {rpy-rs*1.6f, rpy, rpy+rs*1.6f, rpy};
        GFillPoly(rx, ry, 4, col, 0.55f);
        GStrokePoly(rx, ry, 4, true, 0xFFFFFF, A255(100), 1.0f);
    }
    float coreR = (7.0f + 11.0f*f + 4.0f*glow) * sc;
    GFillCircle(0, 0, coreR, col, 0.35f + 0.28f*glow, 24);
    GStrokeCircle(0, 0, coreR, 0xFFFFFF, (0.45f+0.45f*glow), 2.0f, 24);
}

static void GAnim_WordWall(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.06f);
    float wW = 270.0f*sc, wH = 102.0f*sc, bdr = 7.0f*sc;
    float x0 = -wW*0.5f, y0 = -wH*0.5f, x1 = wW*0.5f, y1 = wH*0.5f;

    GFillRect(x0, y0, x1, y1, 0x18202C, 0.92f);
    for (int gi = 1; gi < 3; gi++) {
        float gy = y0 + wH*gi/3.0f;
        GLine(x0+bdr, gy, x1-bdr, gy, 0x000000, 0.22f, 0.8f);
    }
    GStrokeRect(x0, y0, x1, y1, col, 0.48f+0.18f*f, 2.5f);
    GStrokeRect(x0+bdr, y0+bdr, x1-bdr, y1-bdr, col, 0.18f+0.10f*f, 1.0f);

    float cm = bdr * 0.85f;
    for (int cx2 = 0; cx2 < 2; cx2++) for (int cy2 = 0; cy2 < 2; cy2++) {
        float bx = cx2 ? x1-bdr : x0+bdr, by = cy2 ? y1-bdr : y0+bdr;
        float dx = cx2 ? -cm : cm,         dy = cy2 ? -cm : cm;
        GLine(bx, by, bx+dx, by, col, 0.35f, 1.0f);
        GLine(bx, by, bx, by+dy, col, 0.35f, 1.0f);
    }

    const int ROWS = 3, COLS = 9;
    float cellW = (wW - bdr*3.0f) / COLS, cellH = (wH - bdr*3.0f) / ROWS;
    float rx0   = x0 + bdr*1.5f, ry0 = y0 + bdr*1.5f;
    int   litN  = static_cast<int>(f * ROWS * COLS);

    for (int row = 0; row < ROWS; row++) {
        for (int ci = 0; ci < COLS; ci++) {
            int   idx = row * COLS + ci;
            bool  lit = (idx < litN);
            float mx  = rx0 + (ci + 0.5f) * cellW;
            float my  = ry0 + (row + 0.5f) * cellH;
            float gh  = cellH * 0.33f, gw = cellW * 0.26f;
            float glA = lit ? (0.52f + 0.22f*glow) : 0.09f;
            float lw  = lit ? 1.5f*sc : 1.1f*sc;
            int   tp  = static_cast<int>(
                (static_cast<unsigned>(s_wallSeed) ^ (static_cast<unsigned>(idx) * 2246822519u)) >> 17) % 8;
            switch (tp) {
            case 0: GLine(mx-gw,my-gh,mx+gw*0.35f,my+gh,col,glA,lw); GLine(mx-gw,my-gh,mx+gw*0.60f,my-gh,col,glA*0.85f,lw*0.90f); break;
            case 1: GLine(mx-gw*0.5f,my-gh,mx+gw*0.7f,my+gh,col,glA,lw); GLine(mx+gw*0.7f,my-gh,mx-gw*0.5f,my+gh,col,glA*0.80f,lw*0.88f); break;
            case 2: GLine(mx-gw*0.2f,my-gh,mx-gw*0.2f,my+gh,col,glA,lw); GLine(mx-gw*0.2f,my-gh*0.1f,mx+gw*0.8f,my-gh*0.6f,col,glA*0.85f,lw*0.90f); break;
            case 3: GLine(mx-gw*0.7f,my-gh,mx,my+gh*0.10f,col,glA,lw); GLine(mx+gw*0.7f,my-gh,mx,my+gh*0.10f,col,glA,lw); GLine(mx,my+gh*0.10f,mx,my+gh,col,glA*0.80f,lw*0.90f); break;
            case 4: GLine(mx+gw*0.5f,my-gh,mx-gw*0.5f,my+gh*0.1f,col,glA,lw); GLine(mx-gw*0.5f,my+gh*0.1f,mx+gw*0.3f,my+gh,col,glA*0.85f,lw); break;
            case 5: GLine(mx,my-gh,mx,my+gh,col,glA,lw); GLine(mx,my-gh*0.2f,mx-gw*0.8f,my-gh*0.7f,col,glA*0.85f,lw); GLine(mx,my-gh*0.2f,mx+gw*0.8f,my-gh*0.7f,col,glA*0.85f,lw); break;
            case 6: GLine(mx-gw*0.6f,my-gh*0.3f,mx+gw*0.6f,my-gh*0.3f,col,glA,lw); GLine(mx-gw*0.38f,my-gh*0.3f,mx-gw*0.38f,my+gh,col,glA*0.85f,lw); GLine(mx+gw*0.38f,my-gh*0.3f,mx+gw*0.38f,my+gh,col,glA*0.85f,lw); break;
            case 7: GLine(mx-gw*0.5f,my-gh,mx+gw*0.3f,my-gh,col,glA,lw); GLine(mx-gw*0.5f,my-gh,mx-gw*0.5f,my+gh,col,glA,lw); GLine(mx-gw*0.5f,my+gh,mx+gw*0.3f,my+gh,col,glA*0.85f,lw*0.90f); break;
            }
        }
    }
    if (litN > 0 && litN < ROWS*COLS) {
        int eidx = litN - 1;
        GStrokeCircle(rx0 + (eidx%COLS+0.5f)*cellW, ry0 + (eidx/COLS+0.5f)*cellH,
                      cellH*0.38f, col, 0.42f*glow, 1.5f, 12);
    }
}

static void GAnim_StandingStone(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);
    float stH = 92.0f*sc, stWb = 34.0f*sc, stWt = 24.0f*sc, peakH = 20.0f*sc;
    float topY = -stH * 0.42f, botY = stH * 0.58f;
    float apexY = topY - peakH;

    GFillEllipse(0, botY + 3.5f*sc, stWb*0.85f, 3.5f*sc, 0x000000, 0.32f, 14);

    float sx[5] = {0, stWt, stWb, -stWb, -stWt};
    float sy[5] = {apexY, topY, botY, botY, topY};
    GFillPoly(sx, sy, 5, 0x1C2626, 0.94f);
    GStrokePoly(sx, sy, 5, true, col, 0.28f + 0.20f*f, 2.0f);
    GLine(-stWt*0.80f, topY+5.0f*sc, stWt*0.80f, topY+5.0f*sc, col, 0.18f, 1.0f);

    float cw = stWt, ch = stH, cy0 = topY;
    unsigned crState = static_cast<unsigned>(s_crackSeed);
    auto lcgF = [](unsigned& s) -> float {
        s = s * 1664525u + 1013904223u;
        return float(s >> 1) / float(0x7FFFFFFFu);
    };
    struct CrPt { float x, y; };
    auto DrawCrack = [&](float threshold, const CrPt* pts, int n) {
        float cf = std::clamp((f - threshold) / 0.18f, 0.0f, 1.0f);
        if (cf < 0.01f) return;
        float bb  = f > 0.65f ? std::clamp((f-0.65f)/0.25f, 0.0f, 1.0f) : 0.0f;
        float crA = cf * (0.30f + 0.45f*bb + 0.18f*glow);
        for (int i = 0; i < n-1; i++) {
            float ax = pts[i].x*cw, ay = cy0 + pts[i].y*ch;
            float bx = pts[i+1].x*cw, by = cy0 + pts[i+1].y*ch;
            GLine(ax, ay, bx, by, col, crA*0.28f, 5.0f);
            GLine(ax, ay, bx, by, col, crA, 1.4f);
        }
    };
    for (int ci = 0; ci < 12; ci++) {
        float bandCtr = 0.04f + float(ci) * (0.90f / 11.0f);
        float bandHalf = 0.055f;
        float xBias = (ci%3==0) ? 0.18f : (ci%3==1) ? -0.18f : 0.0f;
        CrPt pts[5];
        pts[0].x = std::clamp(xBias+(lcgF(crState)-0.5f)*0.72f, -0.82f, 0.82f);
        pts[0].y = std::clamp(bandCtr-bandHalf+lcgF(crState)*bandHalf*2.0f, 0.01f, 0.95f);
        for (int k = 1; k < 5; k++) {
            pts[k].x = std::clamp(pts[k-1].x+(lcgF(crState)-0.5f)*0.65f, -0.82f, 0.82f);
            pts[k].y = (std::min)(pts[k-1].y+0.05f+lcgF(crState)*0.10f, 0.97f);
        }
        DrawCrack(0.04f + float(ci) * (0.48f / 11.0f), pts, 5);
    }

    if (f > 0.65f) {
        float bf    = std::clamp((f-0.65f)/0.23f, 0.0f, 1.0f);
        float beamH = bf * 85.0f * sc;
        float beamA = 0.65f + 0.28f*glow;
        GLine(0, apexY, 0, apexY-beamH, col, beamA*0.10f, 18.0f);
        GLine(0, apexY, 0, apexY-beamH, col, beamA*0.28f,  8.0f);
        GLine(0, apexY, 0, apexY-beamH, col, beamA,         2.5f);
    }
    if (f > 0.86f) {
        float bf = std::clamp((f-0.72f)/0.28f, 0.0f, 1.0f);
        for (int ri = 0; ri < 3; ri++) {
            float phase = fmodf(t*0.040f + ri*0.333f, 1.0f);
            float rr    = phase * 82.0f * sc;
            float ra    = (1.0f-phase) * bf * (0.55f + 0.22f*glow);
            if (ra > 0.02f) GStrokeCircle(0, apexY, rr, col, ra, 2.0f, 22);
        }
        for (int pi = 0; pi < 9; pi++) {
            float phase = fmodf(t*0.035f + pi*0.111f, 1.0f);
            float ang   = -kPi*0.5f + (pi-4.0f)*0.22f;
            float dist  = phase * (52.0f + pi*7.0f) * sc;
            GFillCircle(cosf(ang)*dist, apexY+sinf(ang)*dist, (1.4f+float(pi%3)*0.7f)*sc, col, (1.0f-phase)*bf*0.78f, 4);
        }
        GFillCircle(0, apexY, 7.0f*sc*bf, col, bf*(0.28f+0.18f*glow)*0.60f, 10);
    }
}

static void GAnim_TwinMoons(float sc, float p, float t, uint32_t col)
{
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.05f);
    float masserOrbitR = 52.0f*sc, secundaOrbitR = 82.0f*sc;
    float masserAngle  = t * 0.55f * kDeg, secundaAngle = t * 1.05f * kDeg + kPi * 0.42f;
    float masserX  = cosf(masserAngle)*masserOrbitR,  masserY  = sinf(masserAngle)*masserOrbitR*0.42f;
    float secundaX = cosf(secundaAngle)*secundaOrbitR, secundaY = sinf(secundaAngle)*secundaOrbitR*0.42f;
    float masserR = 25.0f*sc, secundaR = 14.0f*sc;

    GStrokeCircle(0, 0, masserOrbitR, col, 0.09f, 1.0f, 48);
    GStrokeEllipse(0, 0, secundaOrbitR, secundaOrbitR*0.42f, col, 0.09f, 1.0f, 48);

    static float starAngles[8] = {0.3f,1.1f,1.9f,2.7f,3.5f,4.2f,5.0f,5.8f};
    for (int i = 0; i < 8; i++) {
        float sr   = (55.0f + (i%4)*14.0f) * sc;
        float puls = 0.3f + 0.4f * sinf(t*0.04f + i*0.9f);
        GFillCircle(cosf(starAngles[i])*sr, sinf(starAngles[i])*sr*0.5f, 1.5f*sc, col, puls*f*0.8f, 4);
    }

    GFillCircle(masserX, masserY, masserR, 0x887755, 0.80f, 32);
    float masserLit = 0.35f + 0.65f*f;
    GStrokeArcDeg(masserX, masserY, masserR, -90.0f, -90.0f+360.0f*masserLit, col, 0.32f+0.25f*glow, masserR*0.75f);
    GStrokeCircle(masserX, masserY, masserR, col, 0.48f+0.28f*f, 2.0f, 32);
    for (int i = 0; i < 3; i++) {
        float ca  = (i/3.0f)*2.0f*kPi + 0.6f;
        GStrokeCircle(masserX+cosf(ca)*masserR*0.32f, masserY+sinf(ca)*masserR*0.32f, masserR*0.12f, 0x665544, 0.60f, 1.0f, 8);
    }

    GFillCircle(secundaX, secundaY, secundaR, 0x6688AA, 0.75f, 24);
    float secLit = 0.28f + 0.72f*f;
    GStrokeArcDeg(secundaX, secundaY, secundaR, -90.0f, -90.0f+360.0f*secLit, 0x99BBDD, 0.30f+0.22f*glow, secundaR*0.70f);
    GStrokeCircle(secundaX, secundaY, secundaR, 0x99BBDD, 0.58f+0.28f*f, 1.5f, 24);
}

static void GAnim_DaedricPortal(float sc, float p, float t, uint32_t col)
{
    float f = p / 100.0f;
    float R = 84.0f * sc;

    int nFlakes = (std::min)(int(6 + 36.0f*f), 28);
    for (int i = 0; i < nFlakes; i++) {
        float fi    = float(i) / float(nFlakes);
        float r     = R * (0.10f + 0.90f*fi);
        float theta = fi * 2.0f*kPi + t*(0.042f + fi*0.008f) * (f > 0.5f ? 1.35f : 1.0f);
        float puls  = 0.5f + 0.5f*sinf(t*0.10f + fi*2.8f);
        float alpha = (0.25f + 0.60f*f) * puls;
        float posX  = cosf(theta)*r, posY = sinf(theta)*r*0.70f;
        float fs    = (1.2f + i%3) * sc;
        GLineStyle(0.85f, col, alpha*0.75f);
        for (int arm = 0; arm < 6; arm++) {
            float aa = arm*(kPi/3.0f) + theta*0.15f;
            GMoveTo(posX, posY);
            GLineTo(posX + cosf(aa)*fs*2.8f, posY + sinf(aa)*fs*2.8f);
        }
        GFillCircle(posX, posY, fs, col, alpha, 4);
    }

    float eyeR = R * (0.18f - 0.06f*f);
    GFillCircle(0, 0, eyeR, 0x020408, 0.90f, 16);
    GStrokeCircle(0, 0, eyeR, col, 0.38f+0.28f*f, 1.8f, 16);

    if (f > 0.58f) {
        float sf = (f-0.58f)/0.42f;
        int nShards = int(sf * 14);
        for (int si = 0; si < nShards; si++) {
            float ang   = si*(2.0f*kPi/14.0f) + t*0.022f;
            float ph    = fmodf(t*0.048f + si*0.071f, 1.0f);
            float dist  = R*0.78f + ph*R*0.40f;
            GLine(cosf(ang)*(dist-13*sc), sinf(ang)*(dist-13*sc)*0.70f,
                  cosf(ang)*dist, sinf(ang)*dist*0.70f, col, (1.0f-ph)*sf*0.70f, 1.8f*sc);
        }
    }

    float arcR = R + 9.0f*sc;
    if (f > 0.01f) GStrokeArcDeg(0, 0, arcR, -90.0f, -90.0f+360.0f*f, col, 0.78f, 2.5f);
    GStrokeArcDeg(0, 0, arcR, -90.0f+360.0f*f, 270.0f, col, 0.12f, 1.0f);
}

// ── Dispatch table ────────────────────────────────────────────────────────────
using AnimFn = void(*)(float, float, float, uint32_t);
// Waveform and Helix Spiral are not lore-friendly (they're abstract, not
// Skyrim-themed) — kept last so they're easy to exclude from the random pool.
static const AnimFn kAnimFns[20] = {
    GAnim_CircleFill,   GAnim_DragonEye,    GAnim_NordicRunes,  GAnim_PixelBlocks,
    GAnim_OrbitDots,    GAnim_CompassRose,  GAnim_Snowflake,    GAnim_LinearBar,
    GAnim_SoulGem,      GAnim_DwemerCogs,   GAnim_Shout,        GAnim_Constellation,
    GAnim_DragonScales, GAnim_Enchantment,  GAnim_WordWall,     GAnim_StandingStone,
    GAnim_TwinMoons,    GAnim_DaedricPortal,GAnim_Waveform,     GAnim_Helix,
};
static constexpr int kLoreFriendlyAnimCount = 18;  // indices [0,18) are lore-friendly; 18,19 = Waveform, Helix

// Draws the next style from a shuffled bag of [0, poolSize) so every animation
// plays once before any repeat, instead of plain rand() which can repeat or
// skip entries. Reshuffles when the bag runs out or the pool size changes
// (e.g. the "include non-lore-friendly" toggle flips). Re-rolls a fresh
// shuffle if it would immediately repeat the last style shown.
static int NextRandomAnimStyle(int poolSize, int prevStyle)
{
    if (s_animBagPos >= s_animBag.size() || s_animBagPool != poolSize) {
        s_animBag.resize(poolSize);
        for (int i = 0; i < poolSize; i++) s_animBag[i] = i;
        for (int tries = 0; tries < 8; tries++) {
            std::shuffle(s_animBag.begin(), s_animBag.end(), std::default_random_engine(rand()));
            if (poolSize <= 1 || s_animBag[0] != prevStyle) break;
        }
        s_animBagPos  = 0;
        s_animBagPool = poolSize;
    }
    return s_animBag[s_animBagPos++];
}

// ── GFx init / update ─────────────────────────────────────────────────────────
static void GfxInit(RE::GFxMovieView* mv)
{
    // Defensive clear: wipe any stale drawing left on the cached movie.
    // The CLOSE-handler clear can be skipped (console-open early-return, s_mv null,
    // etc.). This runs before any draw so the 1-tick blank is imperceptible.
    mv->Invoke("_root.clear", nullptr, nullptr, 0);
    RE::GFxValue _tf, _r;
    mv->GetVariable(&_tf, "_root.slpT");
    if (_tf.IsObject()) _tf.Invoke("removeTextField", &_r, nullptr, 0);
    mv->GetVariable(&_tf, "_root.slpK");
    if (_tf.IsObject()) _tf.Invoke("removeTextField", &_r, nullptr, 0);

    auto& cfg = Settings::GetSingleton();

    {
        // Stage.width/Stage.height returns the SWF's declared art size (e.g. 550×400),
        // NOT Skyrim's GFx drawing coordinate space (1280×720).
        // Log it for diagnostics but use the correct effective dimensions.
        RE::GFxValue sw, sh;
        mv->GetVariable(&sw, "Stage.width");
        mv->GetVariable(&sh, "Stage.height");
        logger::info("ScaleformManager: SWF declared {}x{} (effective 1280x720)",
                     int(sw.GetNumber()), int(sh.GetNumber()));
        if (g_customSwf) {
            // Custom SWF: use its declared stage as-is.
            if (sw.GetNumber() > 100.0) s_stageW = float(sw.GetNumber());
            if (sh.GetNumber() > 100.0) s_stageH = float(sh.GetNumber());
        } else {
            // Vanilla loading screen: GFx viewport is always 1280×720.
            s_stageW = 1280.0f;
            s_stageH = 720.0f;
        }
    }

    float sc    = cfg.scale * s_stageH / 1080.0f;
    float animR = 100.0f * sc;

    // Skyrim pillarboxes loading screens using a uniform scale based on screen HEIGHT.
    // Both axes scale identically — no aspect-ratio correction is needed.
    s_xAspectCorr = 1.0f;

    int screenH = GetSystemMetrics(SM_CYSCREEN);
    if (screenH <= 0) screenH = 1080;
    float uniformScale = float(screenH) / s_stageH;  // screen pixels per Flash unit
    float screenRadius = animR * uniformScale;        // animation radius in screen pixels

    // Place the centre so the circle edge is gapPx screen-pixels from the viewport edge.
    float gapPx    = 50.0f;
    float edgeOffset = (screenRadius + gapPx) / uniformScale;  // in Flash units
    float edgeX    = edgeOffset;
    float edgeY    = edgeOffset;

    // The percent readout always renders underneath the animation (see GfxUpdate).
    // Bottom-anchored presets need extra headroom pushed upward so that text block
    // doesn't get pushed past the bottom of the screen.
    bool isBottomPos = (cfg.position <= 2);
    if (cfg.showPercent && isBottomPos) {
        float fontPx  = animR * 0.70f * cfg.textScale;
        float textGap = animR * 0.25f;
        edgeY += textGap + fontPx * 1.3f;
    }

    float cx, cy;
    switch (cfg.position) {
        case 0: cx = s_stageW - edgeX; cy = s_stageH - edgeY; break;  // bottom-right
        case 1: cx = edgeX;            cy = s_stageH - edgeY; break;  // bottom-left
        case 2: cx = s_stageW * 0.5f;  cy = s_stageH - edgeY; break;  // bottom-center
        case 3: cx = s_stageW - edgeX; cy = edgeY;            break;  // top-right
        case 4: cx = edgeX;            cy = edgeY;            break;  // top-left
        default: cx = s_stageW * 0.5f; cy = edgeY;            break;  // top-center
    }
    s_animCX = cx + static_cast<float>(cfg.offsetX);   // fine nudge from the preset
    s_animCY = cy + static_cast<float>(cfg.offsetY);

    g_gfxCreated = true;
    logger::info("ScaleformManager: GFx overlay init (cx={:.0f} cy={:.0f} sc={:.3f} R={:.0f}px uscale={:.2f})",
                 s_animCX, s_animCY, sc, screenRadius, uniformScale);
}

// ── Text rendering via Skyrim's global UI font ───────────────────────────────
// The game loads a shared font library, so "$EverywhereMediumFont" (the vanilla
// UI font used on the loading screen) is available inside the loading movie too.
static constexpr const char* kUiFont = "$EverywhereMediumFont";

// Create/update a center-aligned text field at stage point (cx, cy). The field is
// reused by name across frames; createTextField children survive _root.clear().
static void GDrawText(RE::GFxMovieView* mv, const char* field, double depth,
                      const char* text, double cx, double cy,
                      float sizePx, uint32_t rgb, float alpha)
{
    char path[32];
    snprintf(path, sizeof(path), "_root.%s", field);

    RE::GFxValue tf;
    mv->GetVariable(&tf, path);
    if (!tf.IsObject()) {
        RE::GFxValue root;
        mv->GetVariable(&root, "_root");
        if (!root.IsObject()) return;
        // createTextField(name, depth, x, y, width, height)
        RE::GFxValue a[6];
        a[0].SetString(field); a[1].SetNumber(depth);
        a[2].SetNumber(0.0);   a[3].SetNumber(0.0);
        a[4].SetNumber(800.0); a[5].SetNumber(200.0);
        root.Invoke("createTextField", nullptr, a, 6);
        mv->GetVariable(&tf, path);
        if (!tf.IsObject()) return;
        tf.SetMember("selectable", RE::GFxValue(false));
        tf.SetMember("embedFonts", RE::GFxValue(true));
        tf.SetMember("multiline",  RE::GFxValue(false));
        tf.SetMember("wordWrap",   RE::GFxValue(false));
    }

    const double W = 800.0;
    tf.SetMember("_visible", RE::GFxValue(true));
    tf.SetMember("_x", RE::GFxValue(cx - W * 0.5));
    tf.SetMember("_y", RE::GFxValue(cy - double(sizePx) * 0.85));
    tf.SetMember("_alpha", RE::GFxValue(double(std::clamp(alpha, 0.0f, 1.0f)) * 100.0));

    RE::GFxValue txt;
    mv->CreateString(&txt, text);
    tf.SetMember("text", txt);

    RE::GFxValue fmt;
    mv->CreateObject(&fmt, "TextFormat");
    if (fmt.IsObject()) {
        fmt.SetMember("font",  RE::GFxValue(kUiFont));
        fmt.SetMember("size",  RE::GFxValue(double(sizePx)));
        fmt.SetMember("color", RE::GFxValue(double(rgb & 0xFFFFFF)));
        fmt.SetMember("align", RE::GFxValue("center"));
        RE::GFxValue fa[1] = { fmt };
        tf.Invoke("setTextFormat", nullptr, fa, 1);
    }
}

static void GHideText(RE::GFxMovieView* mv, const char* field)
{
    char path[32];
    snprintf(path, sizeof(path), "_root.%s", field);
    RE::GFxValue tf;
    mv->GetVariable(&tf, path);
    if (tf.IsObject())
        tf.SetMember("_visible", RE::GFxValue(false));
}

static void GfxUpdate(RE::GFxMovieView* mv)
{
    auto& cfg = Settings::GetSingleton();
    s_mv = mv;
    s_ga = cfg.overlayAlpha;

    float sc    = cfg.scale * s_stageH / 1080.0f;
    float animR = 100.0f * sc;  // hoisted — shared by percent and prompt sizing
    GClear();

    if (cfg.showAnimation) {
        int style = std::clamp(s_activeStyle, 0, 19);
        kAnimFns[style](sc, s_display, float(s_tick), cfg.color & 0xFFFFFF);
    }

    if (cfg.showPercent) {
        const float fontPx   = animR * 0.70f * cfg.textScale;   // percentage text size
        const float gap      = animR * 0.25f;
        const float dy       = animR + gap;   // always underneath the animation
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (std::min)(100, int(s_display + 0.5f)));
        GDrawText(mv, "slpT", 10001.0, buf, s_animCX, s_animCY + dy,
                  fontPx, cfg.color & 0xFFFFFF, cfg.overlayAlpha);
    } else {
        GHideText(mv, "slpT");
    }

    if (s_awaitingKey && s_display >= 100.0f) {
        const float pulse = 0.55f + 0.45f * sinf(float(s_tick) * 0.08f);
        const float m     = 90.0f;
        double px, py;
        switch (cfg.promptPosition) {
            case 0: px = s_stageW - m * 2.2; py = s_stageH - m; break;
            case 1: px = m * 2.2;            py = s_stageH - m; break;
            case 2: px = s_stageW * 0.5;     py = s_stageH - m; break;
            case 3: px = s_stageW - m * 2.2; py = m;            break;
            case 4: px = m * 2.2;            py = m;            break;
            default: px = s_stageW * 0.5;    py = s_stageH * 0.5; break;
        }
        const float promptPx = animR * 0.70f * cfg.textScale;
        GDrawText(mv, "slpK", 10002.0, "Press any key to continue", px, py,
                  promptPx, cfg.color & 0xFFFFFF, cfg.overlayAlpha * pulse);
    } else {
        GHideText(mv, "slpK");
    }
}

// ── AdvanceMovie hook (vtbl[5]) ───────────────────────────────────────────────
using AdvanceMovieFn = void(*)(RE::IMenu*, float, std::uint32_t);
static AdvanceMovieFn s_origAM = nullptr;

static void HookAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
{
    s_origAM(a_menu, a_interval, a_currentTime);

    bool menuOpen = g_loadMenuCurrentlyOpen.load(std::memory_order_acquire);

    // Nothing to do when closed and no C++ clip exists and no custom SWF running.
    if (!menuOpen && !g_gfxCreated && !g_customSwf) return;

    auto* mv = a_menu->uiMovie.get();
    if (!mv) { g_gfxCreated = false; g_customSwf = false; return; }

    // Skip our overlay while the debug console is open — prevents GClear() from
    // wiping vanilla loading content and eliminates flicker on console toggle.
    if (menuOpen) {
        if (auto* ui2 = RE::UI::GetSingleton(); ui2 && ui2->IsMenuOpen(RE::BSFixedString("Console")))
            return;
    }

    if (menuOpen) {
        UpdateLoadingState();
        s_tick++;
        if (s_tick <= 5 || s_tick % 60 == 0)
            logger::info("AdvanceMovie tick={} loading={} draining={} display={:.0f}",
                         s_tick, s_loading, s_draining, s_display);

        // On first tick: detect custom SWF (has _root.progress_canvas MovieClip)
        // and read stage dimensions for position calculations.
        if (s_tick == 1) {
            RE::GFxValue canvasVal;
            mv->GetVariable(&canvasVal, "_root.progress_canvas");
            g_customSwf = (canvasVal.GetType() == RE::GFxValue::ValueType::kDisplayObject);
            logger::info("ScaleformManager: custom SWF: {}", g_customSwf);

            // s_stageW/s_stageH are set in GfxInit (always 1280×720 for vanilla SWF).
        }

        if (g_customSwf) {
            // Custom SWF path: ActionScript onEnterFrame handles all drawing.
            // Push progress and config via SetVariable so the SWF reads real values.
            auto& cfg = Settings::GetSingleton();

            mv->SetVariable("_root._loadProgress", RE::GFxValue(double(s_display)));
            mv->SetVariable("_root._animStyle",    RE::GFxValue(double(s_activeStyle)));
            mv->SetVariable("_root._showPct",      RE::GFxValue(cfg.showPercent));
            mv->SetVariable("_root._scale",        RE::GFxValue(double(cfg.scale)));
            mv->SetVariable("_root._textScale",    RE::GFxValue(double(cfg.textScale)));
            mv->SetVariable("_root._color",        RE::GFxValue(double(cfg.color & 0xFFFFFF)));

            // Canvas position: normalise margin to stage coords (stage=1280x720, screen=various)
            float sc     = cfg.scale * s_stageH / 720.0f;
            float margin = (100.0f * sc) + 40.0f;
            float cx, cy;
            switch (cfg.position) {
                case 0: cx = s_stageW - margin; cy = s_stageH - margin; break;
                case 1: cx = margin;             cy = s_stageH - margin; break;
                case 2: cx = s_stageW * 0.5f;   cy = s_stageH - margin; break;
                case 3: cx = s_stageW - margin;  cy = margin;            break;
                case 4: cx = margin;             cy = margin;            break;
                default: cx = s_stageW * 0.5f;  cy = margin;            break;
            }
            mv->SetVariable("_root._canvasX", RE::GFxValue(double(cx + cfg.offsetX)));
            mv->SetVariable("_root._canvasY", RE::GFxValue(double(cy + cfg.offsetY)));
        } else {
            // Vanilla SWF path: C++ GFx drawing via AdvanceMovie hook.
            bool shouldDraw = s_loading || s_draining || s_lingering || s_awaitingKey;
            if (!g_gfxCreated && shouldDraw) {
                GfxInit(mv);
            } else if (g_gfxCreated && shouldDraw) {
                GfxUpdate(mv);
            } else if (g_gfxCreated) {
                mv->Invoke("_root.clear", nullptr, nullptr, 0);
                g_gfxCreated = false;
            }
        }
    } else {
        // Menu closed. Clear root drawing once on GFx path so no frozen frame shows
        // during the loading screen's fade-out animation.
        if (g_gfxCreated) {
            mv->Invoke("_root.clear", nullptr, nullptr, 0);
            RE::GFxValue tf, r;
            mv->GetVariable(&tf, "_root.slpT"); tf.Invoke("removeTextField", &r, nullptr, 0);
            mv->GetVariable(&tf, "_root.slpK"); tf.Invoke("removeTextField", &r, nullptr, 0);
            g_gfxCreated = false;
        }
    }
}

// ── Menu event sink ───────────────────────────────────────────────────────────
class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuEventSink* GetSingleton() { static MenuEventSink inst; return &inst; }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;
        using namespace std::string_view_literals;

        // Quitting to the main menu shows a Loading Menu with no real destination
        // to "finish loading" into — kPostLoadGame never fires and the game may
        // never send the kHide we're blocking, so holdScreen/lingerSeconds can
        // get stuck waiting for a keypress the player has no reason to give,
        // leaving the animation floating over the main menu. Force a full
        // teardown the instant the Main Menu actually appears.
        if (a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) {
            g_holdActive.store(false, std::memory_order_release);
            s_loading = s_draining = s_lingering = s_awaitingKey = false;
            s_frozenAtComplete = false;
            s_display = 0.0f;
            if (g_gfxCreated && s_mv) {
                s_mv->Invoke("_root.clear", nullptr, nullptr, 0);
                RE::GFxValue tf, r;
                s_mv->GetVariable(&tf, "_root.slpT");
                if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
                s_mv->GetVariable(&tf, "_root.slpK");
                if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
            }
            g_gfxCreated = false;
            g_customSwf  = false;
            logger::info("MenuEventSink: Main Menu opened — forcing loading overlay teardown");
            return RE::BSEventNotifyControl::kContinue;
        }

        if (a_event->menuName != "Loading Menu"sv)
            return RE::BSEventNotifyControl::kContinue;

        if (a_event->opening) {
            Settings::GetSingleton().Load();  // pick up any MCM/INI changes since last load
            g_loadMenuCurrentlyOpen.store(true, std::memory_order_release);
            auto& cfg = Settings::GetSingleton();
            // Hold every load: the game's first kHide is our completion signal
            // for cell transitions, and the hold keeps the menu up for the
            // ~0.3s sweep to 100 so the bar visibly finishes on every load.
            g_holdActive.store(true, std::memory_order_release);
            ProgressTracker::GetSingleton().StartTracking();

            s_loadIndex++;
            s_activeStyle  = cfg.randomStyle
                ? NextRandomAnimStyle(cfg.includeNonLoreAnims ? 20 : kLoreFriendlyAnimCount, s_activeStyle)
                : std::clamp(cfg.animStyle, 0, 19);
            s_constPattern = rand() % 6;
            s_wallSeed     = rand();
            s_crackSeed    = rand();
            s_mountainSeed = rand();
            s_blockOrder.clear();

            g_gfxCreated     = false;
            g_customSwf      = false;  // re-detect on first AdvanceMovie tick
            s_tick           = 0;      // reset so s_tick==1 SWF-detection fires every load
            s_loading        = true;   // start drawing immediately; drain logic fires on completion
            s_draining       = false;
            s_lingering      = false;
            s_awaitingKey    = false;
            s_display        = 0.0f;
            s_prevRawBytes   = -1;
            // Consume any leftover kHide signal from the previous load.
            // If kHide fired but AdvanceMovie never ran before the CLOSE event,
            // s_kHideReceived stays true and would freeze Load B at 100% on its
            // very first tick — wiping all vanilla loading-screen content.
            s_kHideReceived.store(false, std::memory_order_relaxed);
            s_hidePending.store(false, std::memory_order_relaxed);
            s_frozenAtComplete   = false;
            s_completionReleased = false;
            s_completeViaEvent   = false;
            s_completionDetected = false;
            s_openSeen           = true;
            s_loadStart        = std::chrono::steady_clock::now();
            s_lastIoChange     = s_loadStart;
            logger::info("MenuEventSink: Loading Menu opened (style={})", s_activeStyle);
        } else {
            g_holdActive.store(false, std::memory_order_release);
            g_loadMenuCurrentlyOpen.store(false, std::memory_order_release);
            ProgressTracker::GetSingleton().Reset();
            // Explicitly clear the cached Scaleform movie right now.
            // Post-close AdvanceMovie may never fire for hidden menus, so we cannot
            // rely on it as the sole clear path — stale drawing would persist and
            // become visible behind the debug console and in subsequent loads.
            if (g_gfxCreated && s_mv) {
                s_mv->Invoke("_root.clear", nullptr, nullptr, 0);
                RE::GFxValue tf, r;
                s_mv->GetVariable(&tf, "_root.slpT");
                if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
                s_mv->GetVariable(&tf, "_root.slpK");
                if (tf.IsObject()) tf.Invoke("removeTextField", &r, nullptr, 0);
            }
            g_gfxCreated       = false;
            g_customSwf        = false;
            s_loading          = false;
            s_draining         = false;
            s_lingering        = false;
            s_awaitingKey      = false;
            s_frozenAtComplete = false;
            s_hidePending.store(false, std::memory_order_relaxed);
            s_display          = 0.0f;
            s_mv               = nullptr;

            auto& cfg2 = Settings::GetSingleton();
            if (cfg2.logLoadTimes && s_openSeen) {
                auto closeTime = std::chrono::steady_clock::now();
                float total   = std::chrono::duration<float>(closeTime - s_loadStart).count();
                float ioTime  = std::chrono::duration<float>(s_lastIoChange - s_loadStart).count();
                float cpuTime = std::chrono::duration<float>(closeTime - s_lastIoChange).count();
                if (ioTime  < 0.0f) ioTime  = 0.0f;
                if (cpuTime < 0.0f) cpuTime = 0.0f;

                auto& trk    = ProgressTracker::GetSingleton();
                LONGLONG bytes = trk.GetLastLoadBytes();
                bool isSave    = trk.WasSaveLoad();

                // Per-category bytes (MB)
                auto MB = [](LONGLONG b) { return (int)(b / (1024*1024)); };
                int texMB   = MB(trk.GetCategoryBytes(AssetCat::Texture));
                int meshMB  = MB(trk.GetCategoryBytes(AssetCat::Mesh));
                int audMB   = MB(trk.GetCategoryBytes(AssetCat::Audio));
                int animMB  = MB(trk.GetCategoryBytes(AssetCat::Animation));
                int scrMB   = MB(trk.GetCategoryBytes(AssetCat::Script));
                int plugMB  = MB(trk.GetCategoryBytes(AssetCat::Plugin));
                int savMB   = MB(trk.GetCategoryBytes(AssetCat::Save));
                int otherMB = MB(trk.GetCategoryBytes(AssetCat::Other));

                // Cell name (player is already in the new cell by CLOSE time)
                std::string cellName, worldName;
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* cell = player->GetParentCell()) {
                        if (const char* n = cell->GetName(); n && n[0]) cellName = n;
                        if (auto* ws = cell->GetRuntimeData().worldSpace)
                            if (const char* n = ws->GetName(); n && n[0]) worldName = n;
                    }
                }
                std::string location;
                if (!cellName.empty() && !worldName.empty())
                    location = cellName + " (" + worldName + ")";
                else if (!cellName.empty())
                    location = cellName;
                else if (!worldName.empty())
                    location = worldName;
                else
                    location = "Unknown";

                std::time_t now = std::time(nullptr);
                char ts[32];
                std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

                if (auto logDir = SKSE::log::log_directory()) {
                    std::ofstream log(*logDir / "SkyrimLoadingPercent_times.log",
                        std::ios::app);
                    if (log) {
                        log << std::fixed << std::setprecision(2);
                        log << "[" << ts << "] "
                            << (isSave ? "Save" : "Cell") << " | " << location << "\n";
                        log << "  " << total << "s total  |  "
                            << MB(bytes) << " MB read  |  I/O: " << ioTime
                            << "s  |  CPU/nav tail: " << cpuTime << "s\n";
                        log << "  save file: " << MB(trk.GetSaveBytesRead())
                            << "/" << MB(trk.GetSaveBytesTotal()) << " MB"
                            << "  |  refs attached: " << trk.GetRefsAttached()
                            << "/" << trk.GetRefsTotal() << "\n";
                        // Asset breakdown (only print categories that have data)
                        log << "  Assets: ";
                        bool first = true;
                        auto printCat = [&](const char* label, int mb) {
                            if (mb <= 0) return;
                            if (!first) log << "  |  ";
                            log << label << ": " << mb << " MB";
                            first = false;
                        };
                        printCat("Textures",   texMB);
                        printCat("Meshes",     meshMB);
                        printCat("Audio",      audMB);
                        printCat("Animations", animMB);
                        printCat("Scripts",    scrMB);
                        printCat("Plugins",    plugMB);
                        printCat("Save data",  savMB);
                        printCat("Other",      otherMB);
                        if (first) log << "(none tracked)";
                        log << "\n\n";
                    }
                }
            }

            s_openSeen = false;
            logger::info("MenuEventSink: Loading Menu closed");
        }
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    MenuEventSink() = default;
};

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────
void RegisterMenuSink() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) { logger::error("ScaleformManager: RE::UI not available"); return; }
    ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
    logger::info("ScaleformManager: menu event sink registered");
}

void InstallThreadHook() {
    auto vtbl   = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_BGSSaveLoadManager__Thread[0] };
    void* target = reinterpret_cast<void*>(vtbl.get()[2]);
    logger::info("ScaleformManager: Thread::Unk_02 target (vtbl[2]) = {:p}", target);
    MH_STATUS st = MH_CreateHook(target,
                                  reinterpret_cast<void*>(&HookThreadUnk02),
                                  reinterpret_cast<void**>(&s_origThreadFn));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
        logger::error("ScaleformManager: MH_CreateHook(Thread::Unk_02) failed ({})", static_cast<int>(st));
    else { MH_EnableHook(target); logger::info("ScaleformManager: Thread::Unk_02 hooked"); }

    // Always hook ProcessMessage: besides blocking kHide during hold, it performs the
    // deterministic _root clear when the loading menu hides (prevents a frozen frame).
    {
        auto pmVtbl   = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_LoadingMenu[0] };
        void* pmTarget = reinterpret_cast<void*>(pmVtbl.get()[4]);
        logger::info("ScaleformManager: ProcessMessage target (vtbl[4]) = {:p}", pmTarget);
        st = MH_CreateHook(pmTarget,
                           reinterpret_cast<void*>(&HookProcessMessage),
                           reinterpret_cast<void**>(&s_origPM));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
            logger::error("ScaleformManager: MH_CreateHook(ProcessMessage) failed ({})", static_cast<int>(st));
        else { MH_EnableHook(pmTarget); logger::info("ScaleformManager: ProcessMessage hooked"); }
    }

    {
        auto amVtbl   = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_LoadingMenu[0] };
        void* amTarget = reinterpret_cast<void*>(amVtbl.get()[5]);
        logger::info("ScaleformManager: AdvanceMovie target (vtbl[5]) = {:p}", amTarget);
        st = MH_CreateHook(amTarget,
                           reinterpret_cast<void*>(&HookAdvanceMovie),
                           reinterpret_cast<void**>(&s_origAM));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
            logger::error("ScaleformManager: MH_CreateHook(AdvanceMovie) failed ({})", static_cast<int>(st));
        else { MH_EnableHook(amTarget); logger::info("ScaleformManager: AdvanceMovie hooked"); }
    }
}

void WaitForHoldRelease() {
    if (!g_holdActive.load(std::memory_order_acquire)) return;
    logger::info("ScaleformManager: WaitForHoldRelease — blocking game thread");
    bool userHold = Settings::GetSingleton().holdScreen;
    for (int i = 0; g_holdActive.load(std::memory_order_acquire); ++i) {
        if (!userHold && i >= 1500) {  // 15 s failsafe — see HookThreadUnk02
            logger::warn("ScaleformManager: WaitForHoldRelease timed out — forcing release");
            g_holdActive.store(false, std::memory_order_release);
            break;
        }
        ::Sleep(10);
    }
    logger::info("ScaleformManager: WaitForHoldRelease — game thread unblocked");
}

void ReleaseLoadingMenuHold() {
    g_holdActive.store(false, std::memory_order_release);
    logger::info("ScaleformManager: ReleaseLoadingMenuHold called");
    if (g_loadMenuCurrentlyOpen.load(std::memory_order_acquire)) {
        if (auto* q = RE::UIMessageQueue::GetSingleton())
            q->AddMessage(RE::BSFixedString("Loading Menu"),
                          RE::UI_MESSAGE_TYPE::kHide, nullptr);
        logger::info("ScaleformManager: sent kHide to UIMessageQueue");
    }
}

} // namespace ScaleformManager

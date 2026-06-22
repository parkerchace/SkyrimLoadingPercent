#include "PCH.h"
#include "ScaleformManager.h"
#include "ProgressTracker.h"
#include "config/Settings.h"

namespace ScaleformManager {

namespace {

// ── Loading Menu ProcessMessage hook ────────────────────────────────────────
//
// When holdScreen or lingerSeconds > 0, we hook the Loading Menu's virtual
// ProcessMessage (vtable slot 4) so we can intercept the kHide/kForceHide
// messages that would dismiss the loading screen.  The hook blocks those
// messages and keeps the Loading Menu alive until ReleaseLoadingMenuHold() is
// called from the render thread.  We then send kHide ourselves via
// UIMessageQueue, which lets the menu close normally.

static void* s_hookTarget = nullptr;

using ProcessMsgFn = RE::UI_MESSAGE_RESULTS(*)(RE::IMenu*, RE::UIMessage&);
static ProcessMsgFn s_origProcessMsg = nullptr;

// Written from the render thread (DrawOverlay), read from the main thread
// (ProcessMessage hook) — must be atomic.
std::atomic<bool> g_releaseLoadingHold{ false };

static RE::UI_MESSAGE_RESULTS HookProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_message)
{
    auto type = a_message.type.get();
    if (type == RE::UI_MESSAGE_TYPE::kHide || type == RE::UI_MESSAGE_TYPE::kForceHide) {
        if (!g_releaseLoadingHold.load(std::memory_order_acquire)) {
            // Block the close.  For cell transitions (no kPostLoadGame event),
            // calling OnLoadComplete() here is the signal D3DOverlay uses to
            // start the drain animation.  Idempotent for save-game loads that
            // already fired kPostLoadGame.
            ProgressTracker::GetSingleton().OnLoadComplete();
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
    }
    return s_origProcessMsg(a_this, a_message);
}

static void InstallLoadingMenuHook()
{
    auto* ui  = RE::UI::GetSingleton();
    auto  menu = ui ? ui->GetMenu("Loading Menu") : RE::GPtr<RE::IMenu>{};
    if (!menu) {
        logger::warn("ScaleformManager: Loading Menu not found — cannot install hold hook");
        return;
    }

    // vtable[4] = IMenu::ProcessMessage (confirmed from CommonLibSSE-NG headers)
    void** vtable = *reinterpret_cast<void***>(menu.get());
    void*  target = vtable[4];

    if (MH_CreateHook(target,
                      reinterpret_cast<void*>(&HookProcessMessage),
                      reinterpret_cast<void**>(&s_origProcessMsg)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        s_hookTarget = target;
        logger::info("ScaleformManager: Loading Menu ProcessMessage hooked for hold");
    } else {
        logger::warn("ScaleformManager: failed to hook Loading Menu ProcessMessage");
        s_origProcessMsg = nullptr;
    }
}

static void RemoveLoadingMenuHook()
{
    if (!s_hookTarget) return;
    MH_DisableHook(s_hookTarget);
    MH_RemoveHook(s_hookTarget);
    s_hookTarget     = nullptr;
    s_origProcessMsg = nullptr;
    logger::info("ScaleformManager: Loading Menu hold hook removed");
}

// ── Menu event sink ──────────────────────────────────────────────────────────

class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuEventSink* GetSingleton() {
        static MenuEventSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;

        constexpr std::string_view kLoadingMenu{ "Loading Menu" };
        if (a_event->menuName != kLoadingMenu)
            return RE::BSEventNotifyControl::kContinue;

        if (a_event->opening) {
            g_releaseLoadingHold.store(false, std::memory_order_release);
            ProgressTracker::GetSingleton().StartTracking();

            auto& cfg = Settings::GetSingleton();
            if (cfg.holdScreen || cfg.lingerSeconds > 0)
                InstallLoadingMenuHook();

            logger::info("MenuEventSink: Loading Menu opened");
        } else {
            RemoveLoadingMenuHook();
            ProgressTracker::GetSingleton().Reset();
            logger::info("MenuEventSink: Loading Menu closed");
        }

        return RE::BSEventNotifyControl::kContinue;
    }

private:
    MenuEventSink() = default;
};

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────────

void RegisterMenuSink() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) {
        logger::error("ScaleformManager: RE::UI singleton not available");
        return;
    }
    ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
    logger::info("ScaleformManager: menu event sink registered");
}

void ReleaseLoadingMenuHold() {
    if (!s_hookTarget) return;  // hook not installed — settings were off, nothing to do
    g_releaseLoadingHold.store(true, std::memory_order_release);
    if (auto* q = RE::UIMessageQueue::GetSingleton())
        q->AddMessage(RE::BSFixedString("Loading Menu"), RE::UI_MESSAGE_TYPE::kHide, nullptr);
    logger::info("ScaleformManager: releasing Loading Menu hold");
}

} // namespace ScaleformManager

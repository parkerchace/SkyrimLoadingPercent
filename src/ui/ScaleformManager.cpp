#include "PCH.h"
#include "ScaleformManager.h"
#include "ProgressTracker.h"
#include "config/Settings.h"

namespace ScaleformManager {

std::atomic<bool> g_loadMenuCurrentlyOpen{ false };

namespace {

static std::atomic<bool> g_holdActive{ false };

// ── BGSSaveLoadManager::Thread::Unk_02 ──────────────────────────────────────
// Fires for some load types (e.g. new game). Blocks the load thread until the
// hold clears so the loading state remains alive on that path too.
using ThreadFn = void(*)(RE::BGSSaveLoadManager::Thread*);
static ThreadFn s_origThreadFn = nullptr;

void HookThreadUnk02(RE::BGSSaveLoadManager::Thread* a_this)
{
    s_origThreadFn(a_this);
    logger::info("ScaleformManager: Unk_02 done — waiting for hold release");
    while (g_holdActive.load(std::memory_order_acquire))
        ::Sleep(10);
    logger::info("ScaleformManager: Unk_02 hold released");
}

// ── LoadingMenu::ProcessMessage (vtbl[4]) ────────────────────────────────────
// Safety net: if kHide arrives from any path while hold is active, ignore it.
// Normally the game thread is blocked in WaitForHoldRelease so kHide can't be
// dispatched anyway, but UIMessageQueue can be written from other threads.
using ProcessMsgFn = RE::UI_MESSAGE_RESULTS(*)(RE::IMenu*, RE::UIMessage&);
static ProcessMsgFn s_origPM = nullptr;

static RE::UI_MESSAGE_RESULTS HookProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_msg)
{
    auto msgType = a_msg.type.get();
    if (msgType == RE::UI_MESSAGE_TYPE::kHide ||
        msgType == RE::UI_MESSAGE_TYPE::kForceHide) {
        if (g_holdActive.load(std::memory_order_acquire)) {
            logger::info("ProcessMessage kHide BLOCKED (hold active)");
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
        logger::info("ProcessMessage kHide forwarded");
    }
    return s_origPM(a_this, a_msg);
}

// ── Menu event sink ──────────────────────────────────────────────────────────
class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuEventSink* GetSingleton() { static MenuEventSink instance; return &instance; }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;
        using namespace std::string_view_literals;
        if (a_event->menuName != "Loading Menu"sv)
            return RE::BSEventNotifyControl::kContinue;

        if (a_event->opening) {
            g_loadMenuCurrentlyOpen.store(true, std::memory_order_release);
            auto& cfg = Settings::GetSingleton();
            bool hold = cfg.holdScreen || cfg.lingerSeconds > 0;
            g_holdActive.store(hold, std::memory_order_release);
            ProgressTracker::GetSingleton().StartTracking();
            logger::info("MenuEventSink: Loading Menu opened (hold={})", hold);
        } else {
            g_holdActive.store(false, std::memory_order_release);
            g_loadMenuCurrentlyOpen.store(false, std::memory_order_release);
            ProgressTracker::GetSingleton().Reset();
            logger::info("MenuEventSink: Loading Menu closed");
        }
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    MenuEventSink() = default;
};

} // anonymous namespace

void RegisterMenuSink() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) { logger::error("ScaleformManager: RE::UI not available"); return; }
    ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
    logger::info("ScaleformManager: menu event sink registered");
}

void InstallThreadHook() {
    // BGSSaveLoadManager::Thread::Unk_02 (vtbl[2])
    auto vtbl = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_BGSSaveLoadManager__Thread[0] };
    void* target = reinterpret_cast<void*>(vtbl.get()[2]);
    logger::info("ScaleformManager: Thread::Unk_02 target (vtbl[2]) = {:p}", target);
    MH_STATUS st = MH_CreateHook(target,
                                  reinterpret_cast<void*>(&HookThreadUnk02),
                                  reinterpret_cast<void**>(&s_origThreadFn));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        logger::error("ScaleformManager: MH_CreateHook(Thread::Unk_02) failed ({})", static_cast<int>(st));
    } else {
        MH_EnableHook(target);
        logger::info("ScaleformManager: Thread::Unk_02 hooked");
    }

    // LoadingMenu::ProcessMessage (VTABLE_LoadingMenu slot 4)
    auto pmVtbl = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_LoadingMenu[0] };
    void* pmTarget = reinterpret_cast<void*>(pmVtbl.get()[4]);
    logger::info("ScaleformManager: ProcessMessage target (vtbl[4]) = {:p}", pmTarget);
    st = MH_CreateHook(pmTarget,
                       reinterpret_cast<void*>(&HookProcessMessage),
                       reinterpret_cast<void**>(&s_origPM));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        logger::error("ScaleformManager: MH_CreateHook(ProcessMessage) failed ({})", static_cast<int>(st));
    } else {
        MH_EnableHook(pmTarget);
        logger::info("ScaleformManager: ProcessMessage hooked");
    }
}

void WaitForHoldRelease() {
    if (!g_holdActive.load(std::memory_order_acquire)) return;
    logger::info("ScaleformManager: WaitForHoldRelease — blocking game thread");
    while (g_holdActive.load(std::memory_order_acquire))
        ::Sleep(10);
    logger::info("ScaleformManager: WaitForHoldRelease — game thread unblocked");
}

void ReleaseLoadingMenuHold() {
    g_holdActive.store(false, std::memory_order_release);
    logger::info("ScaleformManager: ReleaseLoadingMenuHold called");
    // Game thread unblocks from WaitForHoldRelease and the game's natural
    // close sequence sends kHide. Send one explicitly too as a safety net.
    if (g_loadMenuCurrentlyOpen.load(std::memory_order_acquire)) {
        if (auto* q = RE::UIMessageQueue::GetSingleton())
            q->AddMessage(RE::BSFixedString("Loading Menu"),
                          RE::UI_MESSAGE_TYPE::kHide, nullptr);
        logger::info("ScaleformManager: sent kHide to UIMessageQueue");
    }
}

} // namespace ScaleformManager

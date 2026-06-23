#include "PCH.h"
#include "ScaleformManager.h"
#include "ProgressTracker.h"
#include "config/Settings.h"

namespace ScaleformManager {

std::atomic<bool> g_loadMenuCurrentlyOpen{ false };

namespace {

static std::atomic<bool> g_holdActive{ false };

using ThreadFn = void(*)(RE::BGSSaveLoadManager::Thread*);
static ThreadFn s_origThreadFn = nullptr;

using ProcessMsgFn = RE::UI_MESSAGE_RESULTS(*)(RE::IMenu*, RE::UIMessage&);
static ProcessMsgFn s_origPM = nullptr;

static RE::UI_MESSAGE_RESULTS HookProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_msg)
{
    auto msgType = a_msg.type.get();
    if (msgType == RE::UI_MESSAGE_TYPE::kHide ||
        msgType == RE::UI_MESSAGE_TYPE::kForceHide) {
        auto rva = reinterpret_cast<uintptr_t>(_ReturnAddress())
                   - REL::Module::get().base();
        if (g_holdActive.load(std::memory_order_acquire)) {
            // Engine wants to close the loading screen but we're still holding.
            // Return kIgnore so the engine moves on without closing or looping.
            logger::info("ProcessMessage kHide BLOCKED — RVA: {:#x}", rva);
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
        logger::info("ProcessMessage kHide forwarded — RVA: {:#x}", rva);
    }
    return s_origPM(a_this, a_msg);
}

// BGSSaveLoadManager::Thread vtable has exactly 3 slots (confirmed: vtbl[3] = MH_ERROR_NOT_EXECUTABLE):
//   vtbl[0] = ~Thread()    (// 00)
//   vtbl[1] = Unk_01       (// 01) — BSThread run loop or init function
//   vtbl[2] = Unk_02       (// 02) — per-task work function  <-- hook here
//
// Blocking inside Unk_02 keeps isBusy=true in the BSThread run loop, which
// prevents the engine from sending kHide to LoadingMenu.
// When g_holdActive clears, Unk_02 returns → run loop sets isBusy=false → normal teardown.
void HookThreadUnk02(RE::BGSSaveLoadManager::Thread* a_this)
{
    s_origThreadFn(a_this);

    // Loading data is ready. Signal the drain animation.
    ProgressTracker::GetSingleton().OnLoadComplete();

    // Block until DrawOverlay clears the hold. While we block, the BSThread run loop
    // has not yet set isBusy=false, so the engine keeps the loading screen fully alive.
    logger::info("ScaleformManager: Unk_02 done — holding (holdActive={})",
                 g_holdActive.load(std::memory_order_relaxed));
    while (g_holdActive.load(std::memory_order_acquire)) {
        ::Sleep(10);
    }
    logger::info("ScaleformManager: hold released — Unk_02 returning");
}

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
    auto vtbl = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_BGSSaveLoadManager__Thread[0] };
    void* target = reinterpret_cast<void*>(vtbl.get()[2]);  // slot 2 = Unk_02 (per-task)

    logger::info("ScaleformManager: Thread::Unk_02 target (vtbl[2]) = {:p}", target);

    MH_STATUS st = MH_CreateHook(target,
                                  reinterpret_cast<void*>(&HookThreadUnk02),
                                  reinterpret_cast<void**>(&s_origThreadFn));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        logger::error("ScaleformManager: MH_CreateHook(Thread::Unk_02) failed ({})",
                      static_cast<int>(st));
    } else {
        MH_EnableHook(target);
        logger::info("ScaleformManager: BGSSaveLoadManager::Thread::Unk_02 hooked");
    }

    // ProcessMessage diagnostic — read-only, no interception.
    // VTABLE_LoadingMenu[0] slot [4] = LoadingMenu::ProcessMessage (IMenu.h // 04).
    // _ReturnAddress() gives the exact game function that calls kHide.
    auto pmVtbl = REL::Relocation<std::uintptr_t*>{ RE::VTABLE_LoadingMenu[0] };
    void* pmTarget = reinterpret_cast<void*>(pmVtbl.get()[4]);
    logger::info("ScaleformManager: ProcessMessage target (vtbl[4]) = {:p}", pmTarget);
    st = MH_CreateHook(pmTarget,
                       reinterpret_cast<void*>(&HookProcessMessage),
                       reinterpret_cast<void**>(&s_origPM));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        logger::error("ScaleformManager: MH_CreateHook(ProcessMessage) failed ({})",
                      static_cast<int>(st));
    } else {
        MH_EnableHook(pmTarget);
        logger::info("ScaleformManager: LoadingMenu::ProcessMessage hooked (diagnostic)");
    }
}

void ReleaseLoadingMenuHold() {
    g_holdActive.store(false, std::memory_order_release);
    // If the thread hook kept the menu open (or bUserClosesLoadingMenu fallback did),
    // send kHide so the engine actually closes it after the thread unblocks.
    if (g_loadMenuCurrentlyOpen.load(std::memory_order_acquire)) {
        if (auto* q = RE::UIMessageQueue::GetSingleton())
            q->AddMessage(RE::BSFixedString("Loading Menu"),
                          RE::UI_MESSAGE_TYPE::kHide, nullptr);
        logger::info("ScaleformManager: sent kHide to UIMessageQueue");
    }
    logger::info("ScaleformManager: ReleaseLoadingMenuHold called");
}

} // namespace ScaleformManager

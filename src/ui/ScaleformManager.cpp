#include "PCH.h"
#include "ScaleformManager.h"
#include "ProgressTracker.h"

namespace ScaleformManager {

namespace {

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
            ProgressTracker::GetSingleton().StartTracking();
            logger::info("MenuEventSink: Loading Menu opened");
        } else {
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
    if (!ui) {
        logger::error("ScaleformManager: RE::UI singleton not available");
        return;
    }
    ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
    logger::info("ScaleformManager: menu event sink registered");
}

} // namespace ScaleformManager

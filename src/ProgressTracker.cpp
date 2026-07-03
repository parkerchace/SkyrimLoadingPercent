#include "PCH.h"
#include "ProgressTracker.h"

static constexpr std::wstring_view kDataExts[] = {
    L".bsa", L".ba2", L".esm", L".esp", L".esl", L".ess"
};

static bool HasExt(std::wstring_view path, std::wstring_view ext) noexcept {
    if (path.size() < ext.size()) return false;
    auto tail = path.substr(path.size() - ext.size());
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != ext[i]) return false;
    return true;
}

// Case-insensitive substring search in a wide path.
static bool ContainsW(std::wstring_view path, std::wstring_view sub) noexcept {
    if (sub.size() > path.size()) return false;
    for (std::size_t i = 0; i <= path.size() - sub.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < sub.size() && match; ++j)
            match = (std::tolower((unsigned char)path[i+j]) == sub[j]);
        if (match) return true;
    }
    return false;
}

bool ProgressTracker::IsDataFile(std::wstring_view path) noexcept {
    for (auto& ext : kDataExts)
        if (HasExt(path, ext)) return true;
    return false;
}

bool ProgressTracker::IsSaveFile(std::wstring_view path) noexcept {
    return HasExt(path, L".ess");
}

AssetCat ProgressTracker::ClassifyPath(std::wstring_view path) noexcept {
    if (HasExt(path, L".ess"))  return AssetCat::Save;
    if (HasExt(path, L".esm") ||
        HasExt(path, L".esp") ||
        HasExt(path, L".esl")) return AssetCat::Plugin;
    // Check BSA name for asset type hints (Skyrim SE naming conventions).
    if (ContainsW(path, L"texture"))   return AssetCat::Texture;
    if (ContainsW(path, L"mesh"))      return AssetCat::Mesh;
    if (ContainsW(path, L"sound") ||
        ContainsW(path, L"audio") ||
        ContainsW(path, L"voice"))     return AssetCat::Audio;
    if (ContainsW(path, L"anim"))      return AssetCat::Animation;
    if (ContainsW(path, L"script"))    return AssetCat::Script;
    return AssetCat::Other;
}

// ── Cell-attach event sink ────────────────────────────────────────────────────
// Fires on the game thread for every reference that attaches. The parent
// cell's `references` set gives a real denominator, discovered live as each
// cell begins attaching.
namespace {
class CellAttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellAttachDetachEvent* a_event,
        RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
    {
        if (a_event && a_event->attached && a_event->reference) {
            if (auto* cell = a_event->reference->GetParentCell()) {
                ProgressTracker::GetSingleton().NoteRefAttached(
                    cell->GetFormID(),
                    cell->GetRuntimeData().references.size());
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
} // anonymous namespace

void ProgressTracker::RegisterCellAttachSink() {
    static CellAttachSink sink;
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink<RE::TESCellAttachDetachEvent>(&sink);
        logger::info("ProgressTracker: cell-attach event sink registered");
    } else {
        logger::error("ProgressTracker: ScriptEventSourceHolder unavailable — no ref-attach signal");
    }
}

void ProgressTracker::NoteRefAttached(std::uint32_t cellID, std::uint64_t cellRefCount) {
    if (m_phase.load(std::memory_order_acquire) != LoadPhase::Tracking) return;
    {
        std::lock_guard lock(m_cellMutex);
        if (m_seenCells.insert(cellID).second)
            m_refsTotal.fetch_add(static_cast<LONGLONG>(cellRefCount), std::memory_order_relaxed);
    }
    LONGLONG attached = m_refsAttached.fetch_add(1, std::memory_order_relaxed) + 1;
    // The engine's reference set is still being filled in when we sample its size on
    // the first attach, so it can undercount the eventual total — never let the
    // attached count run ahead of what we think the total is.
    LONGLONG total = m_refsTotal.load(std::memory_order_relaxed);
    if (attached > total)
        m_refsTotal.compare_exchange_strong(total, attached, std::memory_order_relaxed);
}

void ProgressTracker::StartTracking() {
    m_sessionAtStart.store(m_sessionBytes.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    for (int i = 0; i < static_cast<int>(AssetCat::Count); ++i)
        m_catAtStart[i].store(m_catSessionBytes[i].load(std::memory_order_relaxed),
                              std::memory_order_relaxed);

    m_refsAttached.store(0, std::memory_order_relaxed);
    m_refsTotal.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_cellMutex);
        m_seenCells.clear();
    }

    // The .ess opens BEFORE the LoadingMenu appears. If that handle is still
    // open this is a save load already in progress — keep its counters and
    // flag it. Otherwise the counters are stale (e.g. the save-select menu
    // reading headers) and must not mislabel a plain cell transition.
    bool saveInFlight = false;
    {
        auto essHandle = reinterpret_cast<HANDLE>(m_essHandle.load(std::memory_order_relaxed));
        std::lock_guard lock(m_mutex);
        saveInFlight = essHandle && m_handles.contains(essHandle);
    }
    if (!saveInFlight) {
        m_essHandle.store(0, std::memory_order_relaxed);
        m_essTotal.store(0, std::memory_order_relaxed);
        m_essRead.store(0, std::memory_order_relaxed);
    }
    m_sawSaveFile.store(saveInFlight, std::memory_order_relaxed);

    m_phase.store(LoadPhase::Tracking, std::memory_order_release);
    logger::info("ProgressTracker: tracking started (save in flight: {}, ess {}/{} bytes)",
        saveInFlight, m_essRead.load(), m_essTotal.load());
}

void ProgressTracker::OnLoadComplete() {
    // Idempotent: only run once per load. CAS Tracking → Complete.
    LoadPhase expected = LoadPhase::Tracking;
    if (!m_phase.compare_exchange_strong(expected, LoadPhase::Complete,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
        return;  // already Complete or Idle

    LONGLONG delta = m_sessionBytes.load(std::memory_order_relaxed)
                   - m_sessionAtStart.load(std::memory_order_relaxed);
    m_lastLoadBytes.store(delta > 0 ? delta : 0, std::memory_order_relaxed);
    logger::info("ProgressTracker: load complete — {} bytes ({}), ess {}/{}, refs {}/{}",
        delta, m_sawSaveFile.load() ? "save" : "cell",
        m_essRead.load(), m_essTotal.load(),
        m_refsAttached.load(), m_refsTotal.load());
}

void ProgressTracker::Reset() {
    m_phase.store(LoadPhase::Idle, std::memory_order_release);
}

void ProgressTracker::OnFileOpened(HANDLE hFile, const wchar_t* rawPath, LONGLONG totalBytes) {
    if (!rawPath || !IsDataFile(rawPath)) return;
    if (totalBytes <= 0) return;

    if (IsSaveFile(rawPath)) {
        // Track the most recently opened save: during an actual load this is
        // the save being parsed. Header peeks from the save-select menu also
        // land here but read only a few KB and are cleared by StartTracking.
        m_essHandle.store(reinterpret_cast<ULONG_PTR>(hFile), std::memory_order_relaxed);
        m_essTotal.store(totalBytes, std::memory_order_relaxed);
        m_essRead.store(0, std::memory_order_relaxed);
        if (m_phase.load(std::memory_order_relaxed) == LoadPhase::Tracking)
            m_sawSaveFile.store(true, std::memory_order_relaxed);
    }

    AssetCat cat = ClassifyPath(rawPath);
    ULONG_PTR idx = reinterpret_cast<ULONG_PTR>(hFile) / 4;
    if (idx < kHandleSlots)
        m_handleCat[idx].store(static_cast<uint8_t>(cat) + 1, std::memory_order_relaxed);

    std::lock_guard lock(m_mutex);
    m_handles.emplace(hFile);
}

void ProgressTracker::OnBytesRead(HANDLE hFile, DWORD bytesRead) {
    if (bytesRead == 0) return;

    ULONG_PTR idx = reinterpret_cast<ULONG_PTR>(hFile) / 4;

    // Some file opens (notably the .ess) never reach our CreateFileW hook — the
    // engine appears to open them below the kernel32 layer we intercept. Classify
    // such handles the first time we see a read from them so save-fraction and
    // per-category totals stay real even for opens we never observed directly.
    if (idx < kHandleSlots && m_handleCat[idx].load(std::memory_order_relaxed) == 0) {
        AssetCat cat = AssetCat::Other;
        wchar_t buf[MAX_PATH];
        DWORD n = GetFinalPathNameByHandleW(hFile, buf, MAX_PATH, FILE_NAME_NORMALIZED);
        if (n > 0 && n < MAX_PATH) {
            std::wstring_view path(buf, n);
            if (IsDataFile(path)) cat = ClassifyPath(path);
            if (IsSaveFile(path) && m_essHandle.load(std::memory_order_relaxed) == 0) {
                LARGE_INTEGER sz{};
                if (GetFileSizeEx(hFile, &sz) && sz.QuadPart > 0) {
                    m_essHandle.store(reinterpret_cast<ULONG_PTR>(hFile), std::memory_order_relaxed);
                    m_essTotal.store(sz.QuadPart, std::memory_order_relaxed);
                    m_essRead.store(0, std::memory_order_relaxed);
                }
            }
        }
        m_handleCat[idx].store(static_cast<uint8_t>(cat) + 1, std::memory_order_relaxed);
        if (cat != AssetCat::Other) {
            std::lock_guard lock(m_mutex);
            m_handles.emplace(hFile);
        }
    }

    // Save-file reads count regardless of phase — the .ess parse begins
    // before the LoadingMenu opens and its fraction must not miss that head.
    if (reinterpret_cast<ULONG_PTR>(hFile) == m_essHandle.load(std::memory_order_relaxed)) {
        m_essRead.fetch_add(static_cast<LONGLONG>(bytesRead), std::memory_order_relaxed);
        if (m_phase.load(std::memory_order_relaxed) == LoadPhase::Tracking)
            m_sawSaveFile.store(true, std::memory_order_relaxed);
    }

    if (m_phase.load(std::memory_order_acquire) != LoadPhase::Tracking) return;
    m_sessionBytes.fetch_add(static_cast<LONGLONG>(bytesRead), std::memory_order_relaxed);

    // Lock-free per-category accumulation via the handle→category array.
    if (idx < kHandleSlots) {
        uint8_t entry = m_handleCat[idx].load(std::memory_order_relaxed);
        if (entry > 0) {
            int cat = entry - 1;
            m_catSessionBytes[cat].fetch_add(static_cast<LONGLONG>(bytesRead),
                                              std::memory_order_relaxed);
        }
    }
}

void ProgressTracker::OnFileClosed(HANDLE hFile) {
    // Stop attributing reads to the save file once its handle closes, so a
    // reused handle value can't pollute the fraction. Totals stay frozen.
    ULONG_PTR h = reinterpret_cast<ULONG_PTR>(hFile);
    ULONG_PTR expected = h;
    m_essHandle.compare_exchange_strong(expected, 0, std::memory_order_relaxed);

    ULONG_PTR idx = h / 4;
    if (idx < kHandleSlots)
        m_handleCat[idx].store(0, std::memory_order_relaxed);

    std::lock_guard lock(m_mutex);
    m_handles.erase(hFile);
}

double ProgressTracker::GetSaveFraction() const noexcept {
    LONGLONG total = m_essTotal.load(std::memory_order_relaxed);
    if (total <= 0 || !m_sawSaveFile.load(std::memory_order_relaxed)) return 0.0;
    double f = static_cast<double>(m_essRead.load(std::memory_order_relaxed))
             / static_cast<double>(total);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

double ProgressTracker::GetRefsFraction() const noexcept {
    LONGLONG total = m_refsTotal.load(std::memory_order_relaxed);
    if (total <= 0) return -1.0;
    double f = static_cast<double>(m_refsAttached.load(std::memory_order_relaxed))
             / static_cast<double>(total);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

LONGLONG ProgressTracker::GetBytesThisLoad() const noexcept {
    LONGLONG read = m_sessionBytes.load(std::memory_order_relaxed)
                  - m_sessionAtStart.load(std::memory_order_relaxed);
    return read > 0 ? read : 0;
}

LONGLONG ProgressTracker::GetCategoryBytes(AssetCat cat) const noexcept {
    int i = static_cast<int>(cat);
    LONGLONG total  = m_catSessionBytes[i].load(std::memory_order_relaxed);
    LONGLONG atStart = m_catAtStart[i].load(std::memory_order_relaxed);
    LONGLONG delta   = total - atStart;
    return delta > 0 ? delta : 0;
}

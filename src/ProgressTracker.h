#pragma once

#include <atomic>
#include <mutex>
#include <unordered_set>

enum class LoadPhase {
    Idle,        // No loading screen active
    Tracking,    // Loading screen open, counting bytes
    Complete,    // Load finished, locked at 100
};

enum class AssetCat : uint8_t {
    Other = 0,  // default / unknown
    Texture,
    Mesh,
    Audio,
    Animation,
    Script,
    Plugin,     // .esm / .esp / .esl
    Save,       // .ess
    Count
};

// All progress data is measured from the CURRENT load only — there is no
// cross-load history, learned estimate, or EMA anywhere. The two percentage
// signals both have real denominators:
//   * save parse: bytes read of the .ess ÷ its exact file size
//   * cell attach: refs attached ÷ total refs of the cells being attached
class ProgressTracker {
public:
    static ProgressTracker& GetSingleton() noexcept {
        static ProgressTracker instance;
        return instance;
    }

    // Called when the LoadingMenu opens
    void StartTracking();

    // Called when kPostLoadGame / kNewGame fires, or when the game first tries
    // to close the LoadingMenu (idempotent)
    void OnLoadComplete();

    // Called when the LoadingMenu closes
    void Reset();

    // Register the TESCellAttachDetachEvent sink. Call once at kDataLoaded.
    void RegisterCellAttachSink();

    // Called from the CreateFileW hook — register a data file handle
    void OnFileOpened(HANDLE hFile, const wchar_t* path, LONGLONG totalBytes);

    // Called from the NtReadFile hook
    void OnBytesRead(HANDLE hFile, DWORD bytesRead);

    // Called from the CloseHandle hook
    void OnFileClosed(HANDLE hFile);

    // Called from the attach-event sink (game thread) for every attached ref.
    void NoteRefAttached(std::uint32_t cellID, std::uint64_t cellRefCount);

    // Raw bytes read since tracking began for the current load (real, unnormalized).
    LONGLONG GetBytesThisLoad() const noexcept;

    LoadPhase GetPhase() const noexcept { return m_phase.load(std::memory_order_relaxed); }

    // Real save-parse fraction: .ess bytes read ÷ .ess file size, clamped [0,1].
    // 0 when no save file is involved in this load.
    double GetSaveFraction() const noexcept;

    // Real cell-attach fraction: refs attached ÷ total refs of every cell that
    // has started attaching, clamped [0,1]. Negative until the first attach.
    double GetRefsFraction() const noexcept;

    LONGLONG GetRefsAttached() const noexcept { return m_refsAttached.load(std::memory_order_relaxed); }
    LONGLONG GetRefsTotal()    const noexcept { return m_refsTotal.load(std::memory_order_relaxed); }

    // Save-file byte counters for diagnostics (real, current load).
    LONGLONG GetSaveBytesRead()  const noexcept { return m_essRead.load(std::memory_order_relaxed); }
    LONGLONG GetSaveBytesTotal() const noexcept { return m_essTotal.load(std::memory_order_relaxed); }

    // The actual bytes consumed by the most recent completed load (for the times log).
    LONGLONG GetLastLoadBytes() const noexcept { return m_lastLoadBytes.load(std::memory_order_relaxed); }

    // True if the current / most recent load involved reading a save file.
    bool WasSaveLoad() const noexcept { return m_sawSaveFile.load(std::memory_order_relaxed); }

    // Bytes attributed to a specific asset category for the current / last load.
    LONGLONG GetCategoryBytes(AssetCat cat) const noexcept;

private:
    ProgressTracker() = default;
    ProgressTracker(const ProgressTracker&) = delete;
    ProgressTracker& operator=(const ProgressTracker&) = delete;

    static bool IsDataFile(std::wstring_view path) noexcept;
    static bool IsSaveFile(std::wstring_view path) noexcept;
    static AssetCat ClassifyPath(std::wstring_view path) noexcept;

    std::atomic<LoadPhase> m_phase{ LoadPhase::Idle };

    // Set of data-file handles currently open; populated from game start.
    mutable std::mutex         m_mutex;
    std::unordered_set<HANDLE> m_handles;

    // Fast lock-free handle → category lookup.
    // Handles are small multiples of 4; index = (ULONG_PTR)hFile / 4.
    static constexpr ULONG_PTR kHandleSlots = 8192;
    std::array<std::atomic<uint8_t>, kHandleSlots> m_handleCat{};

    // Lifetime byte counters per category — never reset, snapshot on load start.
    std::array<std::atomic<LONGLONG>, static_cast<int>(AssetCat::Count)> m_catSessionBytes{};
    std::array<std::atomic<LONGLONG>, static_cast<int>(AssetCat::Count)> m_catAtStart{};

    // Lifetime byte counter — incremented for every ReadFile call seen.
    std::atomic<LONGLONG> m_sessionBytes{ 0 };

    // Snapshot of m_sessionBytes when tracking began for the current load.
    std::atomic<LONGLONG> m_sessionAtStart{ 0 };

    // Whether a .ess save file was read during the current load (→ save-game load).
    std::atomic<bool> m_sawSaveFile{ false };

    // Save-file progress. The .ess is opened BEFORE the LoadingMenu appears, so
    // these are owned by the file hooks and only conditionally cleared when
    // tracking starts (kept if the handle is still open, i.e. mid-read).
    std::atomic<ULONG_PTR> m_essHandle{ 0 };
    std::atomic<LONGLONG>  m_essTotal{ 0 };
    std::atomic<LONGLONG>  m_essRead{ 0 };

    // Cell-attach progress (game thread writes, UI thread reads).
    std::atomic<LONGLONG> m_refsAttached{ 0 };
    std::atomic<LONGLONG> m_refsTotal{ 0 };
    std::mutex                        m_cellMutex;
    std::unordered_set<std::uint32_t> m_seenCells;

    // Bytes consumed by the last completed load (times log only — never used
    // to predict anything).
    std::atomic<LONGLONG> m_lastLoadBytes{ 0 };
};

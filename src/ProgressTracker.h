#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

enum class LoadPhase {
    Idle,        // No loading screen active
    Tracking,    // Loading screen open, counting bytes
    Complete,    // Load finished, locked at 100
};

class ProgressTracker {
public:
    static ProgressTracker& GetSingleton() noexcept {
        static ProgressTracker instance;
        return instance;
    }

    // Called when the LoadingMenu opens
    void StartTracking();

    // Called when kPostLoadGame / kNewGame fires
    void OnLoadComplete();

    // Called when the LoadingMenu closes
    void Reset();

    // Called from the CreateFileW hook — register a data file handle
    void OnFileOpened(HANDLE hFile, const wchar_t* path, LONGLONG totalBytes);

    // Called from the ReadFile hook
    void OnBytesRead(HANDLE hFile, DWORD bytesRead);

    // Called from the CloseHandle hook
    void OnFileClosed(HANDLE hFile);

    // Thread-safe read; returns 0.0–100.0
    float GetProgress() const noexcept;

    LoadPhase GetPhase() const noexcept { return m_phase.load(std::memory_order_relaxed); }

    // Seed the expected-bytes denominator from a previously saved value.
    // Called once at startup with the INI-persisted lastStreamCount.
    void SeedEstimate(LONGLONG bytes) noexcept;

    // How many bytes the most recent completed load actually consumed.
    LONGLONG GetLastLoadBytes() const noexcept;

private:
    ProgressTracker() = default;
    ProgressTracker(const ProgressTracker&) = delete;
    ProgressTracker& operator=(const ProgressTracker&) = delete;

    struct FileEntry {
        std::wstring path;
        LONGLONG totalBytes{ 0 };
    };

    static bool IsDataFile(std::wstring_view path) noexcept;

    std::atomic<LoadPhase> m_phase{ LoadPhase::Idle };

    // Handles table: populated from game start, never cleared during tracking
    mutable std::mutex                    m_mutex;
    std::unordered_map<HANDLE, FileEntry> m_handles;

    // Lifetime byte counter — incremented for every tracked-handle ReadFile call
    std::atomic<LONGLONG> m_sessionBytes{ 0 };

    // Snapshot of m_sessionBytes when tracking began for the current load
    std::atomic<LONGLONG> m_sessionAtStart{ 0 };

    // How many bytes the previous load consumed (used as denominator)
    // Default 20 MB — updated after each completed load
    std::atomic<LONGLONG> m_lastLoadBytes{ 20LL * 1024 * 1024 };
};

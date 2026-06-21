#include "PCH.h"
#include "ProgressTracker.h"

static constexpr std::wstring_view kDataExts[] = {
    L".bsa", L".ba2", L".esm", L".esp", L".esl", L".ess"
};

bool ProgressTracker::IsDataFile(std::wstring_view path) noexcept {
    for (auto& ext : kDataExts) {
        if (path.size() >= ext.size()) {
            auto tail = path.substr(path.size() - ext.size());
            bool match = true;
            for (std::size_t i = 0; i < ext.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(tail[i])) != ext[i]) {
                    match = false; break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

void ProgressTracker::StartTracking() {
    // Snapshot the running byte counter so we can measure delta for this load
    m_sessionAtStart.store(m_sessionBytes.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    m_phase.store(LoadPhase::Tracking, std::memory_order_release);
    logger::info("ProgressTracker: tracking started (session bytes so far: {})",
        m_sessionAtStart.load());
}

void ProgressTracker::OnLoadComplete() {
    LONGLONG delta = m_sessionBytes.load(std::memory_order_relaxed)
                   - m_sessionAtStart.load(std::memory_order_relaxed);
    if (delta > 0) {
        m_lastLoadBytes.store(delta, std::memory_order_relaxed);
    }
    m_phase.store(LoadPhase::Complete, std::memory_order_release);
    logger::info("ProgressTracker: load complete — this load read {} bytes, next estimate {} bytes",
        delta, m_lastLoadBytes.load());
}

void ProgressTracker::Reset() {
    // Called when the loading menu closes; just idle — keep m_handles intact
    m_phase.store(LoadPhase::Idle, std::memory_order_release);
}

void ProgressTracker::OnFileOpened(HANDLE hFile, const wchar_t* rawPath, LONGLONG totalBytes) {
    // Track regardless of phase — BSA handles are opened at game startup
    if (!rawPath || !IsDataFile(rawPath)) return;
    if (totalBytes <= 0) return;

    std::lock_guard lock(m_mutex);
    FileEntry entry;
    entry.path       = rawPath;
    entry.totalBytes = totalBytes;
    m_handles.emplace(hFile, std::move(entry));
}

void ProgressTracker::OnBytesRead(HANDLE hFile, DWORD bytesRead) {
    if (bytesRead == 0) return;

    // Only count during an active tracking window
    if (m_phase.load(std::memory_order_acquire) != LoadPhase::Tracking) return;

    {
        std::lock_guard lock(m_mutex);
        if (m_handles.find(hFile) == m_handles.end()) return;
    }
    m_sessionBytes.fetch_add(static_cast<LONGLONG>(bytesRead), std::memory_order_relaxed);
}

void ProgressTracker::OnFileClosed(HANDLE hFile) {
    std::lock_guard lock(m_mutex);
    m_handles.erase(hFile);
}

void ProgressTracker::SeedEstimate(LONGLONG bytes) noexcept {
    if (bytes > 0)
        m_lastLoadBytes.store(bytes, std::memory_order_relaxed);
}

LONGLONG ProgressTracker::GetLastLoadBytes() const noexcept {
    return m_lastLoadBytes.load(std::memory_order_relaxed);
}

float ProgressTracker::GetProgress() const noexcept {
    LoadPhase phase = m_phase.load(std::memory_order_acquire);
    if (phase == LoadPhase::Idle)     return 0.0f;
    if (phase == LoadPhase::Complete) return 100.0f;

    LONGLONG read     = m_sessionBytes.load(std::memory_order_relaxed)
                      - m_sessionAtStart.load(std::memory_order_relaxed);
    LONGLONG expected = m_lastLoadBytes.load(std::memory_order_relaxed);

    if (expected <= 0 || read <= 0) return 0.0f;

    float pct = static_cast<float>(read) / static_cast<float>(expected) * 100.0f;
    return std::clamp(pct, 0.0f, 99.0f);
}

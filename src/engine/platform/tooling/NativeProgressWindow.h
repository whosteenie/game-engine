#pragma once

#include <chrono>
#include <string>

class NativeProgressWindow
{
public:
    static NativeProgressWindow& Instance();

    // Create the native progress window thread early so the first Begin() does not stall the UI.
    void WarmUp();
    // Associates the progress popup with the host application window without making it topmost
    // across the operating system.
    void SetOwnerWindow(void* nativeWindow);
    bool IsActive() const { return m_depth > 0; }

    void Begin(const std::string& title, const std::string& message);
    void SetMessage(const std::string& message);
    void SetProgress(float progress);
    // No-op when inactive. progress < 0 leaves the bar position unchanged.
    void Report(const std::string& message, float progress = -1.0f);
    void End();

    void Shutdown();

private:
    NativeProgressWindow() = default;
    ~NativeProgressWindow();

    NativeProgressWindow(const NativeProgressWindow&) = delete;
    NativeProgressWindow& operator=(const NativeProgressWindow&) = delete;

    int m_depth = 0;
    // A single outer operation owns the displayed range. Nested scopes may report detail, but
    // must not make a visible load operation move backward.
    float m_lastProgress = 0.0f;
    bool m_hasDeterminateProgress = false;
    std::string m_lastMessage;
    std::chrono::steady_clock::time_point m_lastMessageUpdate{};
};

class ScopedNativeProgress
{
public:
    ScopedNativeProgress(const std::string& title, const std::string& message);
    ~ScopedNativeProgress();

    void SetMessage(const std::string& message) const;
    void SetProgress(float progress) const;

    ScopedNativeProgress(const ScopedNativeProgress&) = delete;
    ScopedNativeProgress& operator=(const ScopedNativeProgress&) = delete;

private:
    bool m_active = false;
};

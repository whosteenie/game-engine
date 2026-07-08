#pragma once

#include <string>

class NativeProgressWindow
{
public:
    static NativeProgressWindow& Instance();

    // Create the native progress window thread early so the first Begin() does not stall the UI.
    void WarmUp();
    bool IsActive() const { return m_depth > 0; }

    void Begin(const std::string& title, const std::string& message);
    void SetMessage(const std::string& message);
    void SetProgress(float progress);
    void End();

    void Shutdown();

private:
    NativeProgressWindow() = default;
    ~NativeProgressWindow();

    NativeProgressWindow(const NativeProgressWindow&) = delete;
    NativeProgressWindow& operator=(const NativeProgressWindow&) = delete;

    int m_depth = 0;
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

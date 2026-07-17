#include "engine/platform/NativeProgressWindow.h"

#ifndef _WIN32

NativeProgressWindow& NativeProgressWindow::Instance()
{
    static NativeProgressWindow instance;
    return instance;
}

NativeProgressWindow::~NativeProgressWindow() = default;

void NativeProgressWindow::WarmUp() {}
void NativeProgressWindow::SetOwnerWindow(void*) {}
bool NativeProgressWindow::IsActive() const { return m_depth > 0; }
void NativeProgressWindow::Begin(const std::string&, const std::string&) {}
void NativeProgressWindow::SetMessage(const std::string&) {}
void NativeProgressWindow::SetProgress(float) {}
void NativeProgressWindow::Report(const std::string&, float) {}
void NativeProgressWindow::End() {}
void NativeProgressWindow::Shutdown() {}

ScopedNativeProgress::ScopedNativeProgress(const std::string&, const std::string&) {}
ScopedNativeProgress::~ScopedNativeProgress() = default;
void ScopedNativeProgress::SetMessage(const std::string&) const {}
void ScopedNativeProgress::SetProgress(float) const {}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr UINT WM_PROGRESS_SHOW = WM_APP + 1;
    constexpr UINT WM_PROGRESS_HIDE = WM_APP + 2;
    constexpr UINT WM_PROGRESS_SET_MESSAGE = WM_APP + 3;
    constexpr UINT WM_PROGRESS_SET_TITLE = WM_APP + 4;
    constexpr UINT WM_PROGRESS_SET_MODE = WM_APP + 5;
    constexpr UINT WM_PROGRESS_SHUTDOWN = WM_APP + 6;

    constexpr int kClientWidth = 460;
    constexpr int kClientHeight = 140;
    constexpr int kPadding = 16;
    constexpr int kControlGap = 12;
    constexpr int kPercentageHeight = 18;
    constexpr int kProgressHeight = 22;
    constexpr int kProgressRange = 1000;

    struct ProgressControls
    {
        HWND window = nullptr;
        HWND messageLabel = nullptr;
        HWND percentageLabel = nullptr;
        HWND progressBar = nullptr;
    };

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int requiredSize =
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (requiredSize <= 0)
        {
            return {};
        }

        std::wstring wide(static_cast<std::size_t>(requiredSize), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            wide.data(),
            requiredSize);
        return wide;
    }

    std::wstring* DuplicateWideString(const std::wstring& value)
    {
        return new std::wstring(value);
    }

    void SetMarqueeMode(HWND progressBar, bool enabled)
    {
        if (progressBar == nullptr)
        {
            return;
        }

        DWORD style = static_cast<DWORD>(GetWindowLongPtr(progressBar, GWL_STYLE));
        const bool marqueeEnabled = (style & PBS_MARQUEE) != 0;
        if (marqueeEnabled == enabled)
        {
            return;
        }

        if (enabled)
        {
            style |= PBS_MARQUEE;
            SetWindowLongPtr(progressBar, GWL_STYLE, style);
            SendMessage(progressBar, PBM_SETMARQUEE, TRUE, 30);
        }
        else
        {
            SendMessage(progressBar, PBM_SETMARQUEE, FALSE, 0);
            style &= ~PBS_MARQUEE;
            SetWindowLongPtr(progressBar, GWL_STYLE, style);
        }
    }

    void LayoutControls(ProgressControls* controls)
    {
        if (controls == nullptr || controls->window == nullptr)
        {
            return;
        }

        RECT clientRect = {};
        GetClientRect(controls->window, &clientRect);
        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;

        const int contentWidth = clientWidth > (kPadding * 2) ? clientWidth - (kPadding * 2) : 0;
        const int progressY = clientHeight - kPadding - kProgressHeight > kPadding
            ? clientHeight - kPadding - kProgressHeight
            : kPadding;
        const int percentageY = progressY - kControlGap - kPercentageHeight > kPadding
            ? progressY - kControlGap - kPercentageHeight
            : kPadding;
        const int messageY = kPadding;
        const int messageHeight = percentageY - kControlGap - messageY > 24
            ? percentageY - kControlGap - messageY : 24;

        if (controls->messageLabel != nullptr)
        {
            SetWindowPos(
                controls->messageLabel,
                nullptr,
                kPadding,
                messageY,
                contentWidth,
                messageHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (controls->percentageLabel != nullptr)
        {
            SetWindowPos(
                controls->percentageLabel,
                nullptr,
                kPadding,
                percentageY,
                contentWidth,
                kPercentageHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (controls->progressBar != nullptr)
        {
            SetWindowPos(
                controls->progressBar,
                nullptr,
                kPadding,
                progressY,
                contentWidth,
                kProgressHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    DWORD GetProgressWindowStyle()
    {
        return WS_CAPTION | WS_SYSMENU | WS_POPUPWINDOW;
    }

    DWORD GetProgressWindowExStyle()
    {
        return WS_EX_TOOLWINDOW;
    }

    void GetProgressOuterSize(int& outWidth, int& outHeight)
    {
        RECT windowRect = {0, 0, kClientWidth, kClientHeight};
        const DWORD style = GetProgressWindowStyle();
        const DWORD exStyle = GetProgressWindowExStyle();
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        outWidth = windowRect.right - windowRect.left;
        outHeight = windowRect.bottom - windowRect.top;
    }

    void CenterWindow(HWND window)
    {
        RECT windowRect = {};
        GetWindowRect(window, &windowRect);
        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;
        const HWND owner = GetWindow(window, GW_OWNER);
        RECT ownerRect = {};
        if (owner != nullptr && GetWindowRect(owner, &ownerRect))
        {
            const int x = ownerRect.left + (ownerRect.right - ownerRect.left - windowWidth) / 2;
            const int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - windowHeight) / 2;
            SetWindowPos(window, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }

        const int x = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;
        // Position only; keep the window hidden. Callers show it afterwards so it never appears
        // at its CW_USEDEFAULT spawn location first (the top-left "empty box" flash).
        SetWindowPos(window, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    LRESULT CALLBACK ProgressWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* controls = reinterpret_cast<ProgressControls*>(GetWindowLongPtr(window, GWLP_USERDATA));

        switch (message)
        {
        case WM_CREATE:
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
            controls = static_cast<ProgressControls*>(createStruct->lpCreateParams);
            SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controls));
            controls->window = window;

            const HFONT uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

            controls->messageLabel = CreateWindowExW(
                0,
                L"STATIC",
                L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0,
                0,
                0,
                0,
                window,
                nullptr,
                createStruct->hInstance,
                nullptr);
            SendMessage(controls->messageLabel, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);

            controls->percentageLabel = CreateWindowExW(
                0,
                L"STATIC",
                L"0%",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0,
                0,
                0,
                0,
                window,
                nullptr,
                createStruct->hInstance,
                nullptr);
            SendMessage(controls->percentageLabel, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);

            controls->progressBar = CreateWindowExW(
                0,
                PROGRESS_CLASSW,
                nullptr,
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                0,
                0,
                0,
                0,
                window,
                nullptr,
                createStruct->hInstance,
                nullptr);
            SendMessage(controls->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, kProgressRange));
            SetMarqueeMode(controls->progressBar, true);
            LayoutControls(controls);
            return 0;
        }
        case WM_SIZE:
            if (controls != nullptr)
            {
                LayoutControls(controls);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_PROGRESS_SET_TITLE:
        {
            auto* title = reinterpret_cast<std::wstring*>(lParam);
            if (title != nullptr)
            {
                SetWindowTextW(window, title->c_str());
                delete title;
            }
            return 0;
        }
        case WM_PROGRESS_SET_MESSAGE:
        {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text != nullptr && controls != nullptr && controls->messageLabel != nullptr)
            {
                SetWindowTextW(controls->messageLabel, text->c_str());
                delete text;
            }
            return 0;
        }
        case WM_PROGRESS_SET_MODE:
        {
            if (controls != nullptr && controls->progressBar != nullptr)
            {
                const bool indeterminate = wParam == 0;
                SetMarqueeMode(controls->progressBar, indeterminate);
                if (controls->percentageLabel != nullptr)
                {
                    if (indeterminate)
                    {
                        SetWindowTextW(controls->percentageLabel, L"Working...");
                    }
                    else
                    {
                        const int position = static_cast<int>(std::clamp<LPARAM>(lParam, 0, kProgressRange));
                        const int percentage = (position * 100 + kProgressRange / 2) / kProgressRange;
                        const std::wstring text = std::to_wstring(percentage) + L"%";
                        SetWindowTextW(controls->percentageLabel, text.c_str());
                    }
                }
                if (!indeterminate)
                {
                    const int position = static_cast<int>(std::clamp<LPARAM>(lParam, 0, kProgressRange));
                    SendMessage(controls->progressBar, PBM_SETPOS, position, 0);
                }
            }
            return 0;
        }
        case WM_PROGRESS_SHOW:
            // Center (and lay out children) while still hidden, then reveal fully formed so there
            // is no one-frame flash of an empty box at the default top-left spawn position.
            CenterWindow(window);
            LayoutControls(controls);
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
            SetForegroundWindow(window);
            return 0;
        case WM_PROGRESS_HIDE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            break;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    class Win32ProgressWindow
    {
    public:
        static Win32ProgressWindow& Get()
        {
            static Win32ProgressWindow instance;
            return instance;
        }

        void WarmUp()
        {
            EnsureThreadRunning();
        }

        void SetOwnerWindow(HWND ownerWindow)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ownerWindow = ownerWindow;
            if (m_window != nullptr)
            {
                SetWindowLongPtrW(m_window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerWindow));
            }
        }

        void Begin(const std::string& title, const std::string& message)
        {
            EnsureThreadRunning();
            // The caller starts CPU-heavy project work immediately after Begin. Send the initial
            // 0% state synchronously so the user sees it before subsequent queued updates can
            // advance the bar into GPU initialization or pipeline warm-up.
            SendStringMessage(WM_PROGRESS_SET_TITLE, title);
            SendStringMessage(WM_PROGRESS_SET_MESSAGE, message);
            SendModeMessage(false, 0);
            SendToWindow(WM_PROGRESS_SHOW, 0, 0);
        }

        void SetMessage(const std::string& message)
        {
            PostStringMessage(WM_PROGRESS_SET_MESSAGE, message);
        }

        void SetProgress(float progress)
        {
            if (progress < 0.0f)
            {
                PostModeMessage(true, 0);
                return;
            }

            const int position = static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * static_cast<float>(kProgressRange));
            PostModeMessage(false, position);
        }

        void End()
        {
            // Message/progress updates run on this window thread. Hide synchronously so the caller
            // cannot reveal the editor while the native progress window is still visible.
            SendToWindow(WM_PROGRESS_HIDE, 0, 0);
        }

        void Shutdown()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_threadRunning)
            {
                return;
            }

            if (m_window != nullptr)
            {
                PostMessage(m_window, WM_PROGRESS_SHUTDOWN, 0, 0);
            }

            if (m_thread.joinable())
            {
                m_thread.join();
            }

            if (m_readyEvent != nullptr)
            {
                CloseHandle(m_readyEvent);
                m_readyEvent = nullptr;
            }

            m_threadRunning = false;
            m_window = nullptr;
        }

    private:
        Win32ProgressWindow() = default;

        ~Win32ProgressWindow()
        {
            Shutdown();
        }

        void EnsureThreadRunning()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threadRunning)
            {
                return;
            }

            m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            m_thread = std::thread([this]() { ThreadMain(); });
            WaitForSingleObject(m_readyEvent, INFINITE);
            m_threadRunning = true;
        }

        void PostToWindow(UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (m_window != nullptr)
            {
                PostMessage(m_window, message, wParam, lParam);
            }
        }

        void SendToWindow(UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (m_window != nullptr)
            {
                SendMessage(m_window, message, wParam, lParam);
            }
        }

        void PostStringMessage(UINT message, const std::string& value)
        {
            PostToWindow(message, 0, reinterpret_cast<LPARAM>(DuplicateWideString(Utf8ToWide(value))));
        }

        void SendStringMessage(UINT message, const std::string& value)
        {
            SendToWindow(message, 0, reinterpret_cast<LPARAM>(DuplicateWideString(Utf8ToWide(value))));
        }

        void PostModeMessage(bool indeterminate, int position)
        {
            PostToWindow(WM_PROGRESS_SET_MODE, indeterminate ? 0 : 1, position);
        }

        void SendModeMessage(bool indeterminate, int position)
        {
            SendToWindow(WM_PROGRESS_SET_MODE, indeterminate ? 0 : 1, position);
        }

        void ThreadMain()
        {
            INITCOMMONCONTROLSEX commonControls = {};
            commonControls.dwSize = sizeof(commonControls);
            commonControls.dwICC = ICC_PROGRESS_CLASS;
            InitCommonControlsEx(&commonControls);

            HINSTANCE instance = GetModuleHandleW(nullptr);
            const wchar_t* className = L"GameEngineNativeProgressWindow";

            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = ProgressWindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            windowClass.lpszClassName = className;
            RegisterClassExW(&windowClass);

            ProgressControls controls;
            int outerWidth = 0;
            int outerHeight = 0;
            GetProgressOuterSize(outerWidth, outerHeight);

            HWND window = CreateWindowExW(
                GetProgressWindowExStyle(),
                className,
                L"Working...",
                GetProgressWindowStyle(),
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                outerWidth,
                outerHeight,
                m_ownerWindow,
                nullptr,
                instance,
                &controls);

            m_window = window;
            SetEvent(m_readyEvent);

            MSG msg = {};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (msg.message == WM_PROGRESS_SHUTDOWN)
                {
                    break;
                }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (IsWindow(window))
            {
                DestroyWindow(window);
            }

            UnregisterClassW(className, instance);
            m_window = nullptr;
        }

        std::mutex m_mutex;
        std::thread m_thread;
        HWND m_window = nullptr;
        HWND m_ownerWindow = nullptr;
        HANDLE m_readyEvent = nullptr;
        bool m_threadRunning = false;
    };
}

NativeProgressWindow& NativeProgressWindow::Instance()
{
    static NativeProgressWindow instance;
    return instance;
}

NativeProgressWindow::~NativeProgressWindow()
{
    Shutdown();
}

void NativeProgressWindow::WarmUp()
{
    Win32ProgressWindow::Get().WarmUp();
}

void NativeProgressWindow::Begin(const std::string& title, const std::string& message)
{
    ++m_depth;
    if (m_depth == 1)
    {
        m_lastProgress = 0.0f;
        m_hasDeterminateProgress = false;
        m_lastMessage = message;
        m_lastMessageUpdate = std::chrono::steady_clock::now();
        Win32ProgressWindow::Get().Begin(title, message);
    }
}

void NativeProgressWindow::SetMessage(const std::string& message)
{
    if (m_depth <= 0 || message == m_lastMessage)
    {
        return;
    }

    // Model import can produce a detail message for every texture/node. Publishing each one
    // floods the popup's Win32 message queue and makes the label unreadable, so publish changes
    // on a short cadence instead.
    constexpr auto kMinimumMessageInterval = std::chrono::milliseconds(80);
    const auto now = std::chrono::steady_clock::now();
    if (m_lastMessageUpdate.time_since_epoch().count() != 0
        && now - m_lastMessageUpdate < kMinimumMessageInterval)
    {
        return;
    }

    m_lastMessage = message;
    m_lastMessageUpdate = now;
    Win32ProgressWindow::Get().SetMessage(message);
}

void NativeProgressWindow::SetProgress(float progress)
{
    if (m_depth <= 0)
    {
        return;
    }

    if (progress < 0.0f)
    {
        // Once a load has a concrete position, keep it determinate and stable until it ends.
        if (!m_hasDeterminateProgress)
        {
            Win32ProgressWindow::Get().SetProgress(progress);
        }

        return;
    }

    const float clampedProgress = std::clamp(progress, 0.0f, 1.0f);
    if (m_hasDeterminateProgress && clampedProgress <= m_lastProgress)
    {
        return;
    }

    // 0.2% is below a visibly meaningful change in this small popup. Coalescing those updates
    // preserves smooth forward movement while preventing thousands of queued UI messages for a
    // large scene or imported model. Always publish completion.
    constexpr float kMinimumVisibleProgressDelta = 0.002f;
    if (m_hasDeterminateProgress && clampedProgress < 1.0f
        && clampedProgress - m_lastProgress < kMinimumVisibleProgressDelta)
    {
        return;
    }

    m_lastProgress = clampedProgress;
    m_hasDeterminateProgress = true;
    Win32ProgressWindow::Get().SetProgress(clampedProgress);
}

void NativeProgressWindow::SetOwnerWindow(void* nativeWindow)
{
    Win32ProgressWindow::Get().SetOwnerWindow(static_cast<HWND>(nativeWindow));
}

void NativeProgressWindow::Report(const std::string& message, float progress)
{
    if (m_depth <= 0)
    {
        return;
    }

    if (!message.empty())
    {
        SetMessage(message);
    }

    SetProgress(progress);
}

void NativeProgressWindow::End()
{
    if (m_depth <= 0)
    {
        return;
    }

    --m_depth;
    if (m_depth == 0)
    {
        Win32ProgressWindow::Get().End();
        m_lastProgress = 0.0f;
        m_hasDeterminateProgress = false;
        m_lastMessage.clear();
        m_lastMessageUpdate = {};
    }
}

void NativeProgressWindow::Shutdown()
{
    if (m_depth > 0)
    {
        m_depth = 0;
        Win32ProgressWindow::Get().End();
    }
    m_lastProgress = 0.0f;
    m_hasDeterminateProgress = false;
    m_lastMessage.clear();
    m_lastMessageUpdate = {};

    Win32ProgressWindow::Get().Shutdown();
}

ScopedNativeProgress::ScopedNativeProgress(const std::string& title, const std::string& message)
{
    NativeProgressWindow::Instance().Begin(title, message);
    m_active = true;
}

ScopedNativeProgress::~ScopedNativeProgress()
{
    if (m_active)
    {
        NativeProgressWindow::Instance().End();
    }
}

void ScopedNativeProgress::SetMessage(const std::string& message) const
{
    NativeProgressWindow::Instance().SetMessage(message);
}

void ScopedNativeProgress::SetProgress(float progress) const
{
    NativeProgressWindow::Instance().SetProgress(progress);
}

#endif

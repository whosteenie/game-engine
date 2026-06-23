#include "engine/NativeProgressWindow.h"

#ifndef _WIN32

NativeProgressWindow& NativeProgressWindow::Instance()
{
    static NativeProgressWindow instance;
    return instance;
}

NativeProgressWindow::~NativeProgressWindow() = default;

void NativeProgressWindow::Begin(const std::string&, const std::string&) {}
void NativeProgressWindow::SetMessage(const std::string&) {}
void NativeProgressWindow::SetProgress(float) {}
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
    constexpr int kClientHeight = 120;
    constexpr int kPadding = 16;
    constexpr int kControlGap = 12;
    constexpr int kProgressHeight = 22;
    constexpr int kProgressRange = 1000;

    struct ProgressControls
    {
        HWND window = nullptr;
        HWND messageLabel = nullptr;
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
            SendMessage(progressBar, PBM_SETPOS, 0, 0);
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
        const int messageY = kPadding;
        const int messageHeight = progressY - kControlGap - messageY > 24 ? progressY - kControlGap - messageY : 24;

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
        return WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
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
        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screenWidth - windowWidth) / 2;
        const int y = (screenHeight - windowHeight) / 2;
        SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
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
                if (!indeterminate)
                {
                    const int position = static_cast<int>(std::clamp<LPARAM>(lParam, 0, kProgressRange));
                    SendMessage(controls->progressBar, PBM_SETPOS, position, 0);
                }
            }
            return 0;
        }
        case WM_PROGRESS_SHOW:
            ShowWindow(window, SW_SHOW);
            CenterWindow(window);
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

        void Begin(const std::string& title, const std::string& message)
        {
            EnsureThreadRunning();
            PostStringMessage(WM_PROGRESS_SET_TITLE, title);
            PostStringMessage(WM_PROGRESS_SET_MESSAGE, message);
            PostModeMessage(false, 0);
            PostToWindow(WM_PROGRESS_SHOW, 0, 0);
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
            PostToWindow(WM_PROGRESS_HIDE, 0, 0);
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

        void PostStringMessage(UINT message, const std::string& value)
        {
            PostToWindow(message, 0, reinterpret_cast<LPARAM>(DuplicateWideString(Utf8ToWide(value))));
        }

        void PostModeMessage(bool indeterminate, int position)
        {
            PostToWindow(WM_PROGRESS_SET_MODE, indeterminate ? 0 : 1, position);
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
                nullptr,
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

void NativeProgressWindow::Begin(const std::string& title, const std::string& message)
{
    ++m_depth;
    if (m_depth == 1)
    {
        Win32ProgressWindow::Get().Begin(title, message);
    }
}

void NativeProgressWindow::SetMessage(const std::string& message)
{
    if (m_depth > 0)
    {
        Win32ProgressWindow::Get().SetMessage(message);
    }
}

void NativeProgressWindow::SetProgress(float progress)
{
    if (m_depth > 0)
    {
        Win32ProgressWindow::Get().SetProgress(progress);
    }
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
    }
}

void NativeProgressWindow::Shutdown()
{
    if (m_depth > 0)
    {
        m_depth = 0;
        Win32ProgressWindow::Get().End();
    }

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

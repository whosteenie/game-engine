#include "engine/platform/system/CrashHandler.h"

#ifdef _WIN32
#include <windows.h>

// dbghelp.h must follow windows.h.
#include <dbghelp.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
    std::once_flag g_installOnce;
    CrashHandler::ContextHook g_contextHook = nullptr;

    std::string ExceptionCodeName(const DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:
            return "IN_PAGE_ERROR";
        case 0xE06D7363: // MSVC C++ exception (thrown, uncaught)
            return "C++_EXCEPTION";
        default:
        {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(code));
            return buffer;
        }
        }
    }

    void WriteStackTrace(std::ostream& out, EXCEPTION_POINTERS* info)
    {
        const HANDLE process = ::GetCurrentProcess();
        const HANDLE thread = ::GetCurrentThread();

        ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        ::SymInitialize(process, nullptr, TRUE);

        // StackWalk64 mutates the context, so hand it a copy.
        CONTEXT context = *info->ContextRecord;

        STACKFRAME64 frame{};
        DWORD machineType = 0;
#if defined(_M_X64)
        machineType = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
        machineType = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = context.Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Esp;
        frame.AddrStack.Mode = AddrModeFlat;
#else
        out << "  (stack walk unsupported on this architecture)\n";
        ::SymCleanup(process);
        return;
#endif

        alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;

        for (int depth = 0; depth < 96; ++depth)
        {
            if (!::StackWalk64(
                    machineType,
                    process,
                    thread,
                    &frame,
                    &context,
                    nullptr,
                    ::SymFunctionTableAccess64,
                    ::SymGetModuleBase64,
                    nullptr))
            {
                break;
            }

            if (frame.AddrPC.Offset == 0)
            {
                break;
            }

            std::ostringstream line;
            line << "  #" << depth << " ";

            DWORD64 symbolDisplacement = 0;
            if (::SymFromAddr(process, frame.AddrPC.Offset, &symbolDisplacement, symbol))
            {
                line << symbol->Name;
            }
            else
            {
                char address[32]{};
                std::snprintf(
                    address,
                    sizeof(address),
                    "0x%llX",
                    static_cast<unsigned long long>(frame.AddrPC.Offset));
                line << address;
            }

            IMAGEHLP_LINE64 sourceLine{};
            sourceLine.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;
            if (::SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &sourceLine)
                && sourceLine.FileName != nullptr)
            {
                line << " (" << sourceLine.FileName << ":" << sourceLine.LineNumber << ")";
            }
            else
            {
                IMAGEHLP_MODULE64 moduleInfo{};
                moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
                if (::SymGetModuleInfo64(process, frame.AddrPC.Offset, &moduleInfo))
                {
                    line << " [" << moduleInfo.ModuleName << "]";
                }
            }

            out << line.str() << '\n';
        }

        ::SymCleanup(process);
    }

    void WriteReport(std::ostream& out, EXCEPTION_POINTERS* info)
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        char timeBuffer[32]{};
        std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", std::localtime(&nowTime));

        const EXCEPTION_RECORD* record = info->ExceptionRecord;

        out << timeBuffer << " [fatal] [crash] === unhandled exception ===\n";
        out << timeBuffer << " [fatal] [crash] code=" << ExceptionCodeName(record->ExceptionCode)
            << " at address 0x" << std::hex
            << reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) << std::dec << '\n';

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            && record->NumberParameters >= 2)
        {
            const ULONG_PTR operation = record->ExceptionInformation[0];
            const ULONG_PTR faultAddress = record->ExceptionInformation[1];
            const char* verb = operation == 1 ? "write to"
                : operation == 8            ? "execute at"
                                            : "read from";
            out << timeBuffer << " [fatal] [crash] access violation: " << verb << " 0x" << std::hex
                << static_cast<std::uintptr_t>(faultAddress) << std::dec << '\n';
        }

        out << timeBuffer << " [fatal] [crash] call stack:\n";
        WriteStackTrace(out, info);
        out << timeBuffer << " [fatal] [crash] === end crash report ===\n";
        out.flush();
    }

    LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info)
    {
        if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Let a subsystem flush its crash-time state first (e.g. the D3D12 debug-layer info queue),
        // so its messages land in the log immediately before the stack trace.
        if (g_contextHook != nullptr)
        {
            g_contextHook();
        }

        // Mirror to stderr (visible in the console) and append to the same diagnostics log the
        // breadcrumbs use, so the report sits right after the last breadcrumb.
        WriteReport(std::cerr, info);

        std::error_code error;
        std::filesystem::create_directories("diagnostics", error);
        std::ofstream logFile("diagnostics/engine.log", std::ios::app);
        if (logFile)
        {
            WriteReport(logFile, info);
        }

        // Let the OS finish default handling (WER / any attached debugger) after we have logged.
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
#endif // _WIN32

namespace CrashHandler
{
    void Install()
    {
#ifdef _WIN32
        std::call_once(g_installOnce, []() { ::SetUnhandledExceptionFilter(&UnhandledFilter); });
#endif
    }

    void SetContextHook(ContextHook hook)
    {
#ifdef _WIN32
        g_contextHook = hook;
#else
        (void)hook;
#endif
    }
}

#include "app/editor/TuningSectionState.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace
{
    constexpr const char* kHandlerType = "RendererTuningSections";

    std::map<std::string, bool> g_sectionOpen;
    bool g_handlerRegistered = false;

    // ImGui ini keys must be free of spaces and '='; derive a stable key from the label.
    std::string MakeKey(const char* label)
    {
        std::string key;
        for (const char* cursor = label; cursor != nullptr && *cursor != '\0'; ++cursor)
        {
            const unsigned char character = static_cast<unsigned char>(*cursor);
            if (std::isalnum(character) != 0)
            {
                key.push_back(static_cast<char>(std::tolower(character)));
            }
        }
        return key;
    }

    void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
    {
        // One logical entry ("Sections"); return a non-null sentinel so ReadLine is invoked.
        return name != nullptr ? reinterpret_cast<void*>(1) : nullptr;
    }

    void SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
    {
        if (line == nullptr)
        {
            return;
        }

        const char* equals = std::strchr(line, '=');
        if (equals == nullptr || equals == line)
        {
            return;
        }

        std::string key(line, static_cast<std::size_t>(equals - line));
        g_sectionOpen[key] = std::atoi(equals + 1) != 0;
    }

    void SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
    {
        buffer->appendf("[%s][Sections]\n", handler->TypeName);
        for (const auto& entry : g_sectionOpen)
        {
            buffer->appendf("%s=%d\n", entry.first.c_str(), entry.second ? 1 : 0);
        }
        buffer->append("\n");
    }
}

namespace TuningSectionState
{
    void RegisterImGuiSettingsHandler()
    {
        if (g_handlerRegistered || ImGui::GetCurrentContext() == nullptr)
        {
            return;
        }

        ImGuiSettingsHandler handler;
        handler.TypeName = kHandlerType;
        handler.TypeHash = ImHashStr(kHandlerType);
        handler.ReadOpenFn = SettingsReadOpen;
        handler.ReadLineFn = SettingsReadLine;
        handler.WriteAllFn = SettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
        g_handlerRegistered = true;
    }

    bool SectionHeader(const char* label, const bool defaultOpen)
    {
        const std::string key = MakeKey(label);

        bool seeded = defaultOpen;
        const auto persisted = g_sectionOpen.find(key);
        if (persisted != g_sectionOpen.end())
        {
            seeded = persisted->second;
        }

        // FirstUseEver applies the seed only when ImGui has no stored state (fresh session / after a
        // layout reset), so persisted values load on startup while in-session user toggles win.
        ImGui::SetNextItemOpen(seeded, ImGuiCond_FirstUseEver);
        const bool open = ImGui::CollapsingHeader(label);

        const auto current = g_sectionOpen.find(key);
        if (current == g_sectionOpen.end())
        {
            g_sectionOpen[key] = open;  // initial capture matches the seed; nothing to persist yet
        }
        else if (current->second != open)
        {
            current->second = open;
            ImGui::MarkIniSettingsDirty();
        }

        return open;
    }
}

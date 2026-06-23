#include "engine/ImGuiFonts.h"

#include "IconsFontAwesome6.h"

#include <imgui.h>

#include <filesystem>

namespace
{
    constexpr const char* kIconFontPath = "assets/fonts/fa-solid-900.ttf";
    constexpr const char* kTextFontPath = "assets/fonts/Inter-Regular.ttf";
    // Only used when Inter-Regular.ttf is present; default path keeps ImGui's crisp 13px bitmap font.
    constexpr float kInterFontSize = 14.0f;

    bool g_iconsAvailable = false;

    static ImWchar g_iconExcludeRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
}

namespace ImGuiFonts
{
    void LoadEditorFonts(ImGuiIO& io)
    {
        g_iconsAvailable = false;

        ImFontConfig textConfig;
        textConfig.GlyphExcludeRanges = g_iconExcludeRanges;

        ImFont* baseFont = nullptr;
        float explicitFontSize = 0.0f;
        if (std::filesystem::exists(kTextFontPath))
        {
            explicitFontSize = kInterFontSize;
            ImGui::GetStyle().FontSizeBase = kInterFontSize;
            baseFont = io.Fonts->AddFontFromFileTTF(kTextFontPath, kInterFontSize, &textConfig);
        }
        else
        {
            // ProggyClean bitmap at 13px — sharp at 1:1 scale. Do not raise FontSizeBase here;
            // AddFontDefault() would pick the vector font once FontSizeBase >= 15.
            baseFont = io.Fonts->AddFontDefaultBitmap(&textConfig);
        }

        if (baseFont == nullptr || !std::filesystem::exists(kIconFontPath))
        {
            return;
        }

        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;

        const bool implicitRefSize = (baseFont->Flags & ImFontFlags_ImplicitRefSize) != 0;
        const float iconFontSize = implicitRefSize ? 0.0f : explicitFontSize;
        if (!implicitRefSize)
        {
            iconConfig.GlyphMinAdvanceX = explicitFontSize;
        }

        if (io.Fonts->AddFontFromFileTTF(kIconFontPath, iconFontSize, &iconConfig) != nullptr)
        {
            g_iconsAvailable = true;
        }
    }

    bool IconsAvailable()
    {
        return g_iconsAvailable;
    }
}

#pragma once

#include <string_view>
#include <vector>

namespace SettingRegistry
{
    enum class ControlType { Checkbox, Slider, Dropdown };
    enum class PersistenceScope { SceneProject, GlobalEditor, SessionOnly };
    enum class UndoPolicy { Undoable, NotUndoable };

    struct Descriptor
    {
        std::string_view id;
        std::string_view label;
        std::string_view keywords;
        std::string_view panel;
        std::string_view section;
        ControlType controlType;
        PersistenceScope persistence;
        UndoPolicy undoPolicy;
        bool searchIndexed;
    };

    const std::vector<Descriptor>& GetAll();
    std::vector<const Descriptor*> FindSearchMatches(std::string_view query);
}

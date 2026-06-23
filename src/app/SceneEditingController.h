#pragma once

#include <memory>

class Scene;
class SceneEditor;
struct SceneEditorUpdateContext;

class SceneEditingController
{
public:
    SceneEditingController();
    ~SceneEditingController();

    SceneEditor& GetEditor();
    const SceneEditor& GetEditor() const;

    void Update(Scene& scene, const SceneEditorUpdateContext& context);
    void HandleEscapeKey(Scene& scene);

private:
    std::unique_ptr<SceneEditor> m_editor;
};

#pragma once

class Material;

void ApplyWoodTableMaterialMaps(Material& material);
void ApplyConcreteFloorMaterialMaps(Material& material);

// Store default texture paths without loading GPU resources (safe during project I/O).
void AssignWoodTableMaterialMapPaths(Material& material);
void AssignConcreteFloorMaterialMapPaths(Material& material);

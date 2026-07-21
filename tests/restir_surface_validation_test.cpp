#include "engine/raytracing/restir/RestirSurfaceValidation.h"

#include <iostream>

void RunRestirSurfaceValidationTests(int& failures)
{
    RestirSurfaceValidationRecord surface{};
    surface.linearDepth = 10.0f;
    surface.worldPosition = {0.0f, 0.0f, 0.0f};
    surface.geometricNormal = {0.0f, 1.0f, 0.0f};
    surface.shadingNormal = {0.0f, 1.0f, 0.0f};
    surface.roughness = 0.5f;
    surface.materialId = 7u;
    surface.valid = true;

    if (!AreRestirSurfacesCompatible(surface, surface))
    {
        std::cerr << "FAIL: identical ReSTIR surfaces must be compatible\n";
        ++failures;
    }

    auto changed = surface;
    changed.linearDepth = 10.21f;
    if (!AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: camera-relative depth change at the same world point must preserve history\n";
        ++failures;
    }

    changed = surface;
    changed.worldPosition = {0.0f, 0.2f, 0.0f};
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: world-space surface-plane mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.worldPosition = {0.2f, 0.0f, 0.0f};
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: excessive tangential reprojection mismatch must reject history\n";
        ++failures;
    }

    changed = surface;
    changed.instanceId = 1u;
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: instance mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.geometricNormal = {1.0f, 0.0f, 0.0f};
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: geometric-normal mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.shadingNormal = {1.0f, 0.0f, 0.0f};
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: normal mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.materialId++;
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: material mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.lobeFlags = 1u;
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: transmission/delta class mismatch must reject ReSTIR history\n";
        ++failures;
    }

    changed = surface;
    changed.valid = false;
    if (AreRestirSurfacesCompatible(surface, changed))
    {
        std::cerr << "FAIL: invalid ReSTIR surface must reject history\n";
        ++failures;
    }

}

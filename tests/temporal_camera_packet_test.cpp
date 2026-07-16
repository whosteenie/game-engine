#include "engine/raytracing/DxrPathTracerDispatch.h"
#include "engine/raytracing/DxrRootSignature.h"

#include <cmath>
#include <cstring>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace
{
    TemporalCameraState MakeCameraState(
        const glm::vec3& position,
        const glm::vec2& jitter)
    {
        const glm::mat4 view = glm::lookAtLH(
            position,
            position + glm::vec3(0.0f, 0.0f, -1.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveLH_ZO(
            glm::radians(55.0f),
            16.0f / 9.0f,
            0.1f,
            1000.0f);
        return TemporalCamera::MakeState(
            view,
            projection,
            glm::inverse(projection * view),
            position,
            jitter);
    }

    void Expect(const bool condition, int& failures)
    {
        if (!condition)
        {
            ++failures;
        }
    }
}

void RunTemporalCameraPacketTests(int& failures)
{
    TemporalCameraPacket packet{};
    packet.current = MakeCameraState(
        glm::vec3(4.0f, 2.0f, 7.0f),
        glm::vec2(0.25f / 1920.0f, -0.25f / 1080.0f));
    packet.previous = MakeCameraState(
        glm::vec3(3.5f, 2.0f, 7.0f),
        glm::vec2(-0.25f / 1920.0f, 0.25f / 1080.0f));
    Expect(TemporalCamera::IsComplete(packet.current), failures);
    Expect(TemporalCamera::IsComplete(packet.previous), failures);

    DxrPathTracerDispatch::CameraConstants cameraConstants{};
    Expect(
        DxrPathTracerDispatch::TryBuildCameraConstants(packet, true, cameraConstants),
        failures);
    Expect(cameraConstants.historyValid, failures);
    Expect(
        TemporalCamera::NearlyEqual(cameraConstants.view, packet.current.view),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.viewProjection,
            TemporalCamera::ApplyJitter(
                packet.current.projection,
                packet.current.jitterNdc) * packet.current.view),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.unjitteredViewProjection,
            packet.current.projection * packet.current.view),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.previousViewProjection,
            packet.previous.projection * packet.previous.view),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.previousReplayInverseViewProjection,
            glm::inverse(
                TemporalCamera::ApplyJitter(
                    packet.current.projection,
                    packet.current.jitterNdc) * packet.previous.view)),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.previousWorldPosition,
            packet.previous.worldPosition),
        failures);

    // Exercise the exact CPU-to-GPU byte copies used by DispatchIfEnabled. The layout offsets are
    // guarded by DxrRootSignature.h static_asserts; these checks guard the packet values copied in.
    DxrRootSignature::ReflectionDispatchConstants packed{};
    std::memcpy(
        packed.unjitteredViewProj,
        glm::value_ptr(cameraConstants.unjitteredViewProjection),
        sizeof(packed.unjitteredViewProj));
    std::memcpy(
        packed.prevViewProj,
        glm::value_ptr(cameraConstants.previousViewProjection),
        sizeof(packed.prevViewProj));
    std::memcpy(
        packed.prevInvViewProj,
        glm::value_ptr(cameraConstants.previousReplayInverseViewProjection),
        sizeof(packed.prevInvViewProj));
    packed.prevCameraPos[0] = cameraConstants.previousWorldPosition.x;
    packed.prevCameraPos[1] = cameraConstants.previousWorldPosition.y;
    packed.prevCameraPos[2] = cameraConstants.previousWorldPosition.z;
    Expect(
        std::memcmp(
            packed.unjitteredViewProj,
            glm::value_ptr(cameraConstants.unjitteredViewProjection),
            sizeof(packed.unjitteredViewProj)) == 0,
        failures);
    Expect(
        std::memcmp(
            packed.prevViewProj,
            glm::value_ptr(cameraConstants.previousViewProjection),
            sizeof(packed.prevViewProj)) == 0,
        failures);
    Expect(
        std::memcmp(
            packed.prevInvViewProj,
            glm::value_ptr(cameraConstants.previousReplayInverseViewProjection),
            sizeof(packed.prevInvViewProj)) == 0,
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            glm::vec3(
                packed.prevCameraPos[0],
                packed.prevCameraPos[1],
                packed.prevCameraPos[2]),
            packet.previous.worldPosition),
        failures);

    // Missing previous members invalidate history. Packing remains safe by copying the complete
    // current state, but temporal consumers receive historyValid=false and cannot consume it.
    TemporalCameraPacket incompletePrevious = packet;
    incompletePrevious.previous.inverseViewProjection[0][0] =
        std::numeric_limits<float>::quiet_NaN();
    Expect(
        DxrPathTracerDispatch::TryBuildCameraConstants(
            incompletePrevious,
            true,
            cameraConstants),
        failures);
    Expect(!cameraConstants.historyValid, failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.previousWorldPosition,
            packet.current.worldPosition),
        failures);
    Expect(
        TemporalCamera::NearlyEqual(
            cameraConstants.previousViewProjection,
            packet.current.projection * packet.current.view),
        failures);
    Expect(
        !TemporalCamera::NearlyEqual(
            cameraConstants.previousViewProjection,
            glm::mat4(1.0f)),
        failures);

    TemporalCameraPacket incompleteJitter = packet;
    incompleteJitter.previous.jitterNdc.x = std::numeric_limits<float>::infinity();
    Expect(
        DxrPathTracerDispatch::TryBuildCameraConstants(
            incompleteJitter,
            true,
            cameraConstants),
        failures);
    Expect(!cameraConstants.historyValid, failures);

    TemporalCameraPacket incompleteCurrent = packet;
    incompleteCurrent.current.valid = false;
    Expect(
        !DxrPathTracerDispatch::TryBuildCameraConstants(
            incompleteCurrent,
            true,
            cameraConstants),
        failures);
}

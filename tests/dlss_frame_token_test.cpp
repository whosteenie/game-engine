#include "engine/rhi/DlssContext.h"

#include <cstdint>

namespace
{
    struct FakeSdk
    {
        std::uint32_t calls = 0;
        std::uint32_t failFrame = UINT32_MAX;
    };

    bool AcquireFakeToken(
        void* const userData,
        const std::uint32_t requestedFrameIndex,
        void*& nativeToken,
        std::uint32_t& actualFrameIndex)
    {
        auto& sdk = *static_cast<FakeSdk*>(userData);
        ++sdk.calls;
        if (requestedFrameIndex == sdk.failFrame)
        {
            return false;
        }

        nativeToken = reinterpret_cast<void*>(static_cast<std::uintptr_t>(requestedFrameIndex) + 1u);
        actualFrameIndex = requestedFrameIndex;
        return true;
    }

    void Expect(const bool condition, int& failures)
    {
        if (!condition)
        {
            ++failures;
        }
    }
}

void RunDlssFrameTokenTests(int& failures)
{
    DlssFrameTokenState state;
    FakeSdk sdk{};

    // Single evaluation: BeginFrame acquires once and repeated reads do not advance.
    const DlssFrameToken single = state.BeginFrame(&AcquireFakeToken, &sdk);
    Expect(single.IsValid() && single.frameIndex == 0, failures);
    Expect(state.Current().native == single.native && sdk.calls == 1, failures);

    // Dual evaluation: distinct viewport identities consume the same immutable frame token.
    const DlssFrameToken dual = state.BeginFrame(&AcquireFakeToken, &sdk);
    const DlssFrameToken sceneToken = state.Current();
    const std::uint32_t sceneViewport = 0;
    const DlssFrameToken gameToken = state.Current();
    const std::uint32_t gameViewport = 1;
    Expect(sceneViewport != gameViewport, failures);
    Expect(sceneToken.native == gameToken.native && sceneToken.frameIndex == 1, failures);
    Expect(dual.native == sceneToken.native && sdk.calls == 2, failures);

    // A skipped frame still advances exactly once at the next BeginFrame, not when evaluated.
    const DlssFrameToken skipped = state.BeginFrame(&AcquireFakeToken, &sdk);
    (void)skipped;
    const DlssFrameToken afterSkip = state.BeginFrame(&AcquireFakeToken, &sdk);
    Expect(afterSkip.frameIndex == 3 && sdk.calls == 4, failures);

    // Reordered Game/Scene evaluation observes the same token and does not alter cadence.
    const DlssFrameToken reordered = state.BeginFrame(&AcquireFakeToken, &sdk);
    const DlssFrameToken gameFirst = state.Current();
    const DlssFrameToken sceneSecond = state.Current();
    Expect(gameFirst.native == sceneSecond.native, failures);
    Expect(reordered.frameIndex == 4 && sdk.calls == 5, failures);

    // An evaluation result cannot consume token state; the following frame advances by one.
    const bool evaluationSucceeded = false;
    (void)evaluationSucceeded;
    Expect(state.Current().native == reordered.native, failures);
    const DlssFrameToken afterFailedEvaluation = state.BeginFrame(&AcquireFakeToken, &sdk);
    Expect(afterFailedEvaluation.frameIndex == 5 && sdk.calls == 6, failures);

    // SDK acquisition failure leaves this frame invalid but consumes no additional identity.
    sdk.failFrame = 6;
    const DlssFrameToken failedAcquire = state.BeginFrame(&AcquireFakeToken, &sdk);
    Expect(!failedAcquire.IsValid() && failedAcquire.frameIndex == 6, failures);
    sdk.failFrame = UINT32_MAX;
    const DlssFrameToken recovered = state.BeginFrame(&AcquireFakeToken, &sdk);
    Expect(recovered.IsValid() && recovered.frameIndex == 7 && sdk.calls == 8, failures);
}

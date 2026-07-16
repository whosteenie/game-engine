#pragma once

#include <algorithm>
#include <cmath>

class ViewportResizeStabilizer
{
public:
    enum class Decision
    {
        Stable,
        Pending,
        Commit,
    };

    static constexpr double kSettleSeconds = 0.15;

    Decision Update(
        const int committedWidth,
        const int committedHeight,
        const int requestedWidth,
        const int requestedHeight,
        const double nowSeconds)
    {
        if (committedWidth <= 0 || committedHeight <= 0)
        {
            Reset();
            return Decision::Commit;
        }

        if (Near(requestedWidth, committedWidth) && Near(requestedHeight, committedHeight))
        {
            Reset();
            return Decision::Stable;
        }

        if (!m_pending
            || !Near(requestedWidth, m_pendingWidth)
            || !Near(requestedHeight, m_pendingHeight))
        {
            m_pending = true;
            m_pendingWidth = requestedWidth;
            m_pendingHeight = requestedHeight;
            m_pendingSinceSeconds = nowSeconds;
            return Decision::Pending;
        }

        if (std::max(0.0, nowSeconds - m_pendingSinceSeconds) < kSettleSeconds)
        {
            return Decision::Pending;
        }

        Reset();
        return Decision::Commit;
    }

    void Reset()
    {
        m_pending = false;
        m_pendingWidth = 0;
        m_pendingHeight = 0;
        m_pendingSinceSeconds = 0.0;
    }

    bool IsPending() const { return m_pending; }

private:
    static bool Near(const int lhs, const int rhs)
    {
        return std::abs(lhs - rhs) <= 1;
    }

    bool m_pending = false;
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    double m_pendingSinceSeconds = 0.0;
};

#include "core/M3AnimationPlayer.h"

#include <cmath>

bool M3AnimationPlayer::selectSequence(int index)
{
    if (!m_model || index < 0 || index >= m_model->sequences.size()) return false;
    m_sequenceIndex = index; m_normalizedTime = 0.0f; return true;
}
void M3AnimationPlayer::advance(float seconds)
{
    if (!m_model || m_sequenceIndex < 0 || seconds <= 0.0f) return;
    const M3AnimationSequence &s = m_model->sequences.at(m_sequenceIndex);
    const float duration = qMax(1u, s.endFrame - s.startFrame) / 60.0f;
    m_normalizedTime = std::fmod(m_normalizedTime + seconds / duration, 1.0f);
}

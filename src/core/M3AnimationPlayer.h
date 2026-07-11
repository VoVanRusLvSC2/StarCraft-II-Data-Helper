#pragma once

#include "core/M3Model.h"

class M3AnimationPlayer
{
public:
    void setModel(const M3Model *model) { m_model = model; }
    bool selectSequence(int index);
    void advance(float seconds);
    int sequenceIndex() const { return m_sequenceIndex; }
    float normalizedTime() const { return m_normalizedTime; }
private:
    const M3Model *m_model = nullptr;
    int m_sequenceIndex = -1;
    float m_normalizedTime = 0.0f;
};

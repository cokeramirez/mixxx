#pragma once

#include <QMap>
#include <algorithm>
#include <cmath>

#include "effects/backends/effectprocessor.h"
#include "engine/engine.h"
#include "util/class.h"
#include "util/samplebuffer.h"

enum class EchoOutPhase {
    Idle,
    Recording,
    EchoOut
};

class EchoOutGroupState : public EffectState {
  public:
    // 8 seconds supports 4 beats down to 30 BPM.
    static constexpr int kMaxDelaySeconds = 8;

    EchoOutGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        audioParametersChanged(engineParameters);
        clear();
    }
    ~EchoOutGroupState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters) {
        buffer = mixxx::SampleBuffer(kMaxDelaySeconds *
                engineParameters.sampleRate() *
                engineParameters.channelCount());
    }

    void clear() {
        buffer.clear();
        phase = EchoOutPhase::Idle;
        recorded_frames = 0;
        loop_read_pos = 0;
        current_gain = 0.0f;
        prev_decay_param = 0.0f;

        // Filter state memory (stereo)
        filter_lp_l = 0.0f;
        filter_lp_r = 0.0f;
    }

    mixxx::SampleBuffer buffer;
    EchoOutPhase phase;
    int recorded_frames;
    int loop_read_pos;
    CSAMPLE_GAIN current_gain;
    double prev_decay_param;

    // Filter memory
    CSAMPLE filter_lp_l;
    CSAMPLE filter_lp_r;
};

class EchoOutEffect : public EffectProcessorImpl<EchoOutGroupState> {
  public:
    EchoOutEffect() = default;
    ~EchoOutEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            EchoOutGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pDecayParameter;
    EngineEffectParameterPointer m_pSizeParameter;
    EngineEffectParameterPointer m_pFilterParameter;
    EngineEffectParameterPointer m_pQuantizeParameter;

    DISALLOW_COPY_AND_ASSIGN(EchoOutEffect);
};
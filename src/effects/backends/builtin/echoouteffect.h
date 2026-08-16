#pragma once

#include <QMap>
#include <algorithm>

#include "effects/backends/effectprocessor.h"
#include "engine/engine.h"
#include "util/class.h"
#include "util/samplebuffer.h"

class EchoOutGroupState : public EffectState {
  public:
    // 8 seconds max. Supports 4 beats down to 30 BPM.
    static constexpr int kMaxDelaySeconds = 8;

    EchoOutGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        audioParametersChanged(engineParameters);
        clear();
    }
    ~EchoOutGroupState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters) {
        delay_buf = mixxx::SampleBuffer(kMaxDelaySeconds *
                engineParameters.sampleRate() *
                engineParameters.channelCount());
    }

    void clear() {
        delay_buf.clear();
        prev_send = 1.0f;
        prev_dry = 1.0f;
        prev_feedback = 0.0f;
        prev_delay_samples = 0;
        write_position = 0;
        active = false;

        // Filter state memory
        filter_lp_l = 0.0f;
        filter_lp_r = 0.0f;
    }

    mixxx::SampleBuffer delay_buf;
    CSAMPLE_GAIN prev_send;
    CSAMPLE_GAIN prev_dry;
    CSAMPLE_GAIN prev_feedback;
    int prev_delay_samples;
    int write_position;
    bool active;

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

    EngineEffectParameterPointer m_pFeedbackParameter;
    EngineEffectParameterPointer m_pSizeParameter;
    EngineEffectParameterPointer m_pFilterParameter;
    EngineEffectParameterPointer m_pQuantizeParameter;

    DISALLOW_COPY_AND_ASSIGN(EchoOutEffect);
};
#pragma once

#include <QMap>
#include "effects/backends/effectprocessor.h"
#include "util/samplebuffer.h"
#include "util/class.h"

// ============================================================================
// HYBRID DJ LOOPER & ECHO OUT (v4)
// State machine + circular buffer looper with beat-sync'd loop size,
// feedback decay, and first-order DJ filter (HPF without Q)
// ============================================================================

struct HybridLooperGroupState : public EffectState {
  public:
    // 9 beat divisions: 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4, 8
    static constexpr double kBeatDivisions[] = {
        1.0/32.0, 1.0/16.0, 1.0/8.0, 1.0/4.0, 1.0/2.0,
        1.0, 2.0, 4.0, 8.0
    };
    static constexpr int kNumBeatDivisions = 9;
    
    // Maximum loop duration: 8 beats at 40 BPM @ 48kHz
    // = (60/40) * 8 * 48000 = 576,000 samples
    // With stereo (2 channels): ~2.3 MB per channel
    static constexpr int kMaxLoopBeats = 8;
    static constexpr int kMinBPM = 40;

    explicit HybridLooperGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        audioParametersChanged(engineParameters);
        clear();
    }
    
    ~HybridLooperGroupState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters) {
        // Allocate buffer for worst case: 8 beats @ 40 BPM
        int maxSamples = static_cast<int>(
            kMaxLoopBeats * (60.0 / kMinBPM) * 
            engineParameters.sampleRate() * 
            engineParameters.channelCount());
        loopBuffer = mixxx::SampleBuffer(maxSamples);
    }

    void clear() {
        loopBuffer.clear();
        writePtr = 0;
        readPtr = 0;
        targetSamples = 0;
        backgroundRecordingSamples = 0;
        state = STATE_IDLE;
        lastActivator = 0.0;
        hpfStateLeft = 0.0f;
        hpfStateRight = 0.0f;
        hpfPrevInputLeft = 0.0f;
        hpfPrevInputRight = 0.0f;
    }

    enum LooperState {
        STATE_IDLE,       // Bypass - no processing
        STATE_RECORDING,  // Recording loop to buffer
        STATE_LOOPING     // Playback with feedback decay
    };

    // Circular buffer for loop storage
    mixxx::SampleBuffer loopBuffer;
    int writePtr = 0;           // Current write position
    int readPtr = 0;            // Current read position
    int targetSamples = 0;      // Loop size in samples (determined by Time param)
    int backgroundRecordingSamples = 0; // Counter for background recording
    
    // State machine control
    LooperState state = STATE_IDLE;
    double lastActivator = 0.0; // Edge detection for state transitions
    
    // First-order IIR HPF state (DJ Filter without Q)
    CSAMPLE hpfStateLeft = 0.0f;
    CSAMPLE hpfStateRight = 0.0f;
    CSAMPLE hpfPrevInputLeft = 0.0f;
    CSAMPLE hpfPrevInputRight = 0.0f;
};

class HybridLooperEffect : public EffectProcessorImpl<HybridLooperGroupState> {
  public:
    HybridLooperEffect() = default;
    ~HybridLooperEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            HybridLooperGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    // Apply first-order IIR HPF (DJ Filter) to a single sample
    CSAMPLE applyDJFilter(
            CSAMPLE input,
            CSAMPLE& prevInput,
            CSAMPLE& filterState,
            double cutoffFreq,
            const mixxx::EngineParameters& engineParameters);

    // Effect parameters
    EngineEffectParameterPointer m_pTimeParam;       // 0.0-1.0 → beat divisions
    EngineEffectParameterPointer m_pFeedbackParam;   // 0.0-1.0 → loop decay
    EngineEffectParameterPointer m_pActivatorParam;  // 0.0-1.0 → state trigger
    EngineEffectParameterPointer m_pWetDryParam;     // 0.0-1.0 → Constant Power mix
    EngineEffectParameterPointer m_pFilterParam;     // 0.0-1.0 → DJ HPF cutoff (Hz)

    DISALLOW_COPY_AND_ASSIGN(HybridLooperEffect);
};

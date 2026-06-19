#include "effects/backends/builtin/hybridloopereffect.h"

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/math.h"
#include "util/sample.h"
#include <cmath>

namespace {
// DJ Filter range: 20 Hz - 20 kHz
constexpr double kMinFilterFreq = 20.0;
constexpr double kMaxFilterFreq = 20000.0;
} // anonymous namespace

// static
QString HybridLooperEffect::getId() {
    return "org.mixxx.effects.hybridlooper";
}

// static
EffectManifestPointer HybridLooperEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Hybrid Looper"));
    pManifest->setShortName(QObject::tr("HyLoop"));
    pManifest->setAuthor("Coke Ramirez");
    pManifest->setVersion("4.0");
    pManifest->setDescription(QObject::tr(
            "Hybrid DJ Looper with sync'd loop size, feedback control, and DJ filter. "
            "Activator >= 0.5 to start loop, < 0.5 to bypass."));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setAddDryToWet(true);

    // PARAMETER 1: TIME (0-1 → 9 beat divisions)
    EffectManifestParameterPointer time = pManifest->addParameter();
    time->setId("time");
    time->setName(QObject::tr("Time"));
    time->setShortName(QObject::tr("Time"));
    time->setDescription(QObject::tr(
            "Loop size in beats (synchronized to track BPM):\n"
            "1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4, 8"));
    time->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    time->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    time->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    time->setRange(0.0, 0.5, 1.0);

    // PARAMETER 2: FEEDBACK (0-1)
    EffectManifestParameterPointer feedback = pManifest->addParameter();
    feedback->setId("feedback");
    feedback->setName(QObject::tr("Feedback"));
    feedback->setShortName(QObject::tr("Fb"));
    feedback->setDescription(QObject::tr(
            "Loop decay per repetition:\n"
            "0.0 = Echo Out (single repeat)  |  1.0 = Infinite loop"));
    feedback->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    feedback->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    feedback->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    feedback->setRange(0.0, 0.7, 1.0);

    // PARAMETER 3: ACTIVATOR (0-1, threshold 0.5)
    EffectManifestParameterPointer activator = pManifest->addParameter();
    activator->setId("activator");
    activator->setName(QObject::tr("Activator"));
    activator->setShortName(QObject::tr("Act"));
    activator->setDescription(QObject::tr(
            "Loop trigger:\n"
            "< 0.5 = Bypass/Record mode  |  >= 0.5 = Freeze and Loop"));
    activator->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    activator->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    activator->setRange(0.0, 0.0, 1.0);

    // PARAMETER 4: WET/DRY (Constant Power Crossfade)
    EffectManifestParameterPointer wetdry = pManifest->addParameter();
    wetdry->setId("wetdry");
    wetdry->setName(QObject::tr("Wet/Dry"));
    wetdry->setShortName(QObject::tr("W/D"));
    wetdry->setDescription(QObject::tr(
            "Constant power mix between dry signal (left) and loop (right)"));
    wetdry->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    wetdry->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    wetdry->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    wetdry->setRange(0.0, 0.5, 1.0);

    // PARAMETER 5: DJ FILTER (HPF logarithmic without Q)
    EffectManifestParameterPointer filter = pManifest->addParameter();
    filter->setId("djfilter");
    filter->setName(QObject::tr("DJ Filter"));
    filter->setShortName(QObject::tr("Filter"));
    filter->setDescription(QObject::tr(
            "High-pass filter cutoff on looped signal (first-order, no resonance)"));
    filter->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    filter->setUnitsHint(EffectManifestParameter::UnitsHint::Hertz);
    filter->setRange(kMinFilterFreq, kMinFilterFreq, kMaxFilterFreq);

    return pManifest;
}

void HybridLooperEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pTimeParam = parameters.value("time");
    m_pFeedbackParam = parameters.value("feedback");
    m_pActivatorParam = parameters.value("activator");
    m_pWetDryParam = parameters.value("wetdry");
    m_pFilterParam = parameters.value("djfilter");
}

CSAMPLE HybridLooperEffect::applyDJFilter(
        CSAMPLE input,
        CSAMPLE& prevInput,
        CSAMPLE& filterState,
        double cutoffFreq,
        const mixxx::EngineParameters& engineParameters) {
    
    // Bypass filter if cutoff is at minimum
    if (cutoffFreq <= kMinFilterFreq) {
        prevInput = input;
        return input;
    }
    
    // Calculate IIR HPF coefficient
    // RC = 1 / (2π * fc)
    // α = RC / (RC + dt)
    double fs = engineParameters.sampleRate();
    double dt = 1.0 / fs;
    double RC = 1.0 / (2.0 * M_PI * cutoffFreq);
    double alpha = RC / (RC + dt);
    
    // Difference equation: y[n] = α * (y[n-1] + x[n] - x[n-1])
    CSAMPLE output = alpha * (filterState + input - prevInput);
    
    filterState = output;
    prevInput = input;
    
    return output;
}

void HybridLooperEffect::processChannel(
        HybridLooperGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {

    // ========================================================================
    // 1. GET PARAMETERS AND BPM (same pattern as EchoEffect)
    // ========================================================================
    
    double activator = m_pActivatorParam->value();
    double timeParam = m_pTimeParam->value();
    CSAMPLE_GAIN feedback = static_cast<CSAMPLE_GAIN>(m_pFeedbackParam->value());
    CSAMPLE_GAIN wetDry = static_cast<CSAMPLE_GAIN>(m_pWetDryParam->value());
    double filterFreq = m_pFilterParam->value();

    // Calculate target loop size in samples (sync'd to BPM like EchoEffect)
    int targetSamples = 0;
    if (groupFeatures.beat_length.has_value()) {
        // Convert timeParam (0-1) to beat division index (0-8)
        int beatDivisionIdx = static_cast<int>(timeParam * 9.0);
        beatDivisionIdx = std::min(beatDivisionIdx, 
                                   HybridLooperGroupState::kNumBeatDivisions - 1);
        
        double beats = HybridLooperGroupState::kBeatDivisions[beatDivisionIdx];
        double delaySecs = beats * groupFeatures.beat_length->seconds;
        
        targetSamples = static_cast<int>(delaySecs * engineParameters.sampleRate()) 
                       * engineParameters.channelCount();
    } else {
        // Fallback: no BPM detected, use time in seconds (0-2 sec)
        double timeSeconds = timeParam * 2.0;
        targetSamples = static_cast<int>(timeSeconds * engineParameters.sampleRate()) 
                       * engineParameters.channelCount();
    }
    
    // Validate bounds
    targetSamples = std::min(targetSamples, static_cast<int>(pState->loopBuffer.size()));
    targetSamples = std::max(targetSamples, engineParameters.channelCount());

    // ========================================================================
    // 2. STATE MACHINE (Edge detection on Activator)
    // ========================================================================
    
    // Rising edge: IDLE → RECORDING
    if (pState->lastActivator < 0.5 && activator >= 0.5) {
        pState->state = HybridLooperGroupState::STATE_RECORDING;
        pState->writePtr = 0;
        pState->readPtr = 0;
        pState->loopBuffer.clear();
        pState->targetSamples = targetSamples;
        pState->backgroundRecordingSamples = 0;
    } 
    // Falling edge: → IDLE
    else if (pState->lastActivator >= 0.5 && activator < 0.5) {
        pState->state = HybridLooperGroupState::STATE_IDLE;
        pState->loopBuffer.clear();
        pState->writePtr = 0;
        pState->readPtr = 0;
    }
    pState->lastActivator = activator;

    // Force IDLE if effect is disabling
    if (enableState == EffectEnableState::Disabling) {
        pState->state = HybridLooperGroupState::STATE_IDLE;
    }

    // ========================================================================
    // 3. AUDIO PROCESSING (per-sample state machine)
    // ========================================================================
    
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); 
         i += engineParameters.channelCount()) {
        
        CSAMPLE sampleL = pInput[i];
        CSAMPLE sampleR = pInput[i + 1];
        CSAMPLE outL = sampleL;
        CSAMPLE outR = sampleR;

        if (pState->state == HybridLooperGroupState::STATE_IDLE) {
            // ===== STATE_IDLE: Pure bypass =====
            outL = sampleL;
            outR = sampleR;
            
        } else if (pState->state == HybridLooperGroupState::STATE_RECORDING) {
            // ===== STATE_RECORDING: Record + output dry =====
            
            // Write to circular buffer
            if (pState->writePtr < static_cast<int>(pState->loopBuffer.size())) {
                pState->loopBuffer[pState->writePtr] = sampleL;
                pState->loopBuffer[pState->writePtr + 1] = sampleR;
                pState->writePtr += engineParameters.channelCount();
            }
            
            // Auto-transition to LOOPING when targetSamples reached
            if (pState->writePtr >= pState->targetSamples) {
                pState->state = HybridLooperGroupState::STATE_LOOPING;
                pState->readPtr = 0;
                pState->backgroundRecordingSamples = 0;
            }
            
            // Output: dry signal while recording
            outL = sampleL;
            outR = sampleR;
            
        } else if (pState->state == HybridLooperGroupState::STATE_LOOPING) {
            // ===== STATE_LOOPING: Playback with feedback decay =====
            
            // Read from circular buffer
            CSAMPLE loopL = pState->loopBuffer[pState->readPtr];
            CSAMPLE loopR = pState->loopBuffer[pState->readPtr + 1];
            
            // Advance read pointer
            pState->readPtr += engineParameters.channelCount();
            
            // Apply feedback attenuation when loop wraps around
            if (pState->readPtr >= pState->targetSamples) {
                pState->readPtr = 0;
                
                // Multiply entire loop by feedback gain
                for (int j = 0; j < pState->targetSamples; j++) {
                    pState->loopBuffer[j] *= feedback;
                }
            }
            
            // Background recording: capture input up to 8 beats
            int maxBackgroundSamples = 8 * engineParameters.sampleRate() 
                                      * engineParameters.channelCount() / 2;
            if (pState->backgroundRecordingSamples < maxBackgroundSamples) {
                int bgWriteIdx = pState->targetSamples + pState->backgroundRecordingSamples;
                if (bgWriteIdx < static_cast<int>(pState->loopBuffer.size())) {
                    pState->loopBuffer[bgWriteIdx] = sampleL;
                    pState->loopBuffer[bgWriteIdx + 1] = sampleR;
                    pState->backgroundRecordingSamples += engineParameters.channelCount();
                }
            }
            
            // Apply DJ filter (first-order HPF) to loop samples
            loopL = applyDJFilter(loopL, pState->hpfPrevInputLeft, 
                                  pState->hpfStateLeft, filterFreq, engineParameters);
            loopR = applyDJFilter(loopR, pState->hpfPrevInputRight, 
                                  pState->hpfStateRight, filterFreq, engineParameters);
            
            // Output is the processed loop
            outL = loopL;
            outR = loopR;
        }

        // ===== CONSTANT POWER WET/DRY MIX =====
        // If M < 0.5: Wet ramps 0→1, Dry stays 1
        // If M >= 0.5: Wet stays 1, Dry ramps 1→0
        
        CSAMPLE_GAIN wetGain, dryGain;
        if (wetDry < 0.5) {
            wetGain = wetDry * 2.0;
            dryGain = 1.0;
        } else {
            wetGain = 1.0;
            dryGain = (1.0 - wetDry) * 2.0;
        }
        
        pOutput[i] = sampleL * dryGain + outL * wetGain;
        pOutput[i + 1] = sampleR * dryGain + outR * wetGain;
    }

    // Handle disable/fade-out
    if (enableState == EffectEnableState::Disabling) {
        SampleUtil::applyRampingGain(pOutput, 1.0, 0.0, engineParameters.samplesPerBuffer());
        pState->clear();
    }
}

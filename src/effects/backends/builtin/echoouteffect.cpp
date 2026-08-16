#include "effects/backends/builtin/echoouteffect.h"

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/math.h"
#include "util/rampingvalue.h"
#include "util/sample.h"

constexpr int EchoOutGroupState::kMaxDelaySeconds;

namespace {

void incrementRing(int* pIndex, int increment, int length) {
    *pIndex = (*pIndex + increment) % length;
}

void decrementRing(int* pIndex, int decrement, int length) {
    *pIndex = (*pIndex + length - decrement) % length;
}

} // anonymous namespace

// static
QString EchoOutEffect::getId() {
    return "org.mixxx.effects.echoout";
}

// static
EffectManifestPointer EchoOutEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());

    // We mix Dry and Delay directly to perform smooth crossfades and dry mutes.
    pManifest->setAddDryToWet(false);
    pManifest->setEffectRampsFromDry(false);

    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Echo Out"));
    pManifest->setShortName(QObject::tr("Echo Out"));
    pManifest->setAuthor("Mixxx Community");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "Cuts the live input and releases a natural delay echo tail with dynamic feedback and DJ filter."));

    // 1. Feedback Parameter (Metaknob)
    EffectManifestParameterPointer feedback = pManifest->addParameter();
    feedback->setId("feedback_amount");
    feedback->setName(QObject::tr("Feedback"));
    feedback->setShortName(QObject::tr("Feedback"));
    feedback->setDescription(QObject::tr(
            "Controls echo decay and activates the effect.\n"
            "0 = Off / Bypass\n"
            "0.01 - 0.95 = Natural echo decay\n"
            "0.96 - 1.0 = Infinite Looper / Freeze"));
    feedback->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    feedback->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    feedback->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    // Range: Min: 0.0, Default: 0.70, Max: 1.0
    feedback->setRange(0.0, 0.70, 1.0);

    // 2. Loop Size Parameter
    EffectManifestParameterPointer size = pManifest->addParameter();
    size->setId("echo_size");
    size->setName(QObject::tr("Size"));
    size->setShortName(QObject::tr("Size"));
    size->setDescription(QObject::tr(
            "Echo delay time.\n"
            "Quantized: 1/4, 1/2, 1 (default), 2, 4 beats.\n"
            "Unquantized: 0.1s to 2.5s continuous."));
    size->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    size->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    // Range: 0.0 to 1.0 with default at 0.50 (1 Beat)
    size->setRange(0.0, 0.50, 1.0);

    // 3. DJ Filter Parameter (-1.0 Low-Pass <-> 0 Neutral <-> +1.0 High-Pass)
    EffectManifestParameterPointer filter = pManifest->addParameter();
    filter->setId("filter_sweep");
    filter->setName(QObject::tr("Filter"));
    filter->setShortName(QObject::tr("Filter"));
    filter->setDescription(QObject::tr(
            "DJ Filter in feedback loop. Left = Low-Pass Filter, Center = Off, Right = High-Pass Filter."));
    filter->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    filter->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    filter->setRange(-1.0, 0.0, 1.0);

    // 4. Quantize Toggle
    EffectManifestParameterPointer quantize = pManifest->addParameter();
    quantize->setId("quantize");
    quantize->setName(QObject::tr("Quantize"));
    quantize->setShortName(QObject::tr("Quantize"));
    quantize->setDescription(QObject::tr("Snap delay time to musical beat fractions."));
    quantize->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    quantize->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    quantize->setRange(0, 1, 1);

    return pManifest;
}

void EchoOutEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pFeedbackParameter = parameters.value("feedback_amount");
    m_pSizeParameter = parameters.value("echo_size");
    m_pFilterParameter = parameters.value("filter_sweep");
    m_pQuantizeParameter = parameters.value("quantize");
}

void EchoOutEffect::processChannel(
        EchoOutGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double feedback_param = m_pFeedbackParameter->value();
    const double size_param = m_pSizeParameter->value();
    const double filter_param = m_pFilterParameter->value();
    const int channels = engineParameters.channelCount();
    const double sample_rate = static_cast<double>(engineParameters.sampleRate());
    const int frames_per_buffer = engineParameters.framesPerBuffer();

    // 1. Check if the effect is active or bypassed
    const bool is_enabled = (enableState != EffectEnableState::Disabled &&
                             enableState != EffectEnableState::Disabling &&
                             feedback_param >= 0.01);

    // Target gains for micro-ramping
    CSAMPLE_GAIN target_send = 0.0f;
    CSAMPLE_GAIN target_dry = 0.0f;
    CSAMPLE_GAIN target_feedback = 0.0f;

    if (!is_enabled) {
        // Bypass mode: live input flows freely, no feedback, delay buffer records background
        target_send = 1.0f;
        target_dry = 1.0f;
        target_feedback = 0.0f;
        pState->active = false;
    } else {
        // Active Echo Out: Mute live input and cut send into delay; wet delay rings out
        target_send = 0.0f;
        target_dry = 0.0f;

        if (feedback_param >= 0.95) {
            target_feedback = 1.0f; // Infinite loop / Freeze
        } else {
            // Scale feedback musically (0.01 -> ~0.15, 0.94 -> ~0.92)
            target_feedback = static_cast<CSAMPLE_GAIN>(0.10 + (feedback_param / 0.95) * 0.82);
        }
        pState->active = true;
    }

    // 2. Calculate Delay Time in seconds
    double delay_seconds = 0.5;

    if (groupFeatures.beat_length.has_value()) {
        double beats = 1.0;
        if (m_pQuantizeParameter->toBool()) {
            // 5 Balanced physical sectors of 20% each
            // [0.0 - 0.2) = 1/4, [0.2 - 0.4) = 1/2, [0.4 - 0.6) = 1.0, [0.6 - 0.8) = 2.0, [0.8 - 1.0] = 4.0
            static const double kQuantizedBeats[] = {0.25, 0.50, 1.00, 2.00, 4.00};
            int sector = std::clamp(static_cast<int>(size_param * 5.0), 0, 4);
            beats = kQuantizedBeats[sector];
        } else {
            // Continuous beat sweep (0.25 to 4.0 beats)
            beats = 0.25 + size_param * (4.0 - 0.25);
        }
        delay_seconds = beats * groupFeatures.beat_length->seconds;
    } else {
        // Fallback without BPM: 0.10s to 2.50s
        delay_seconds = 0.10 + size_param * (2.50 - 0.10);
    }

    int delay_frames = static_cast<int>(delay_seconds * sample_rate);
    int delay_samples = delay_frames * channels;
    delay_samples = std::clamp(delay_samples, channels * 64, pState->delay_buf.size());

    if (pState->prev_delay_samples == 0) {
        pState->prev_delay_samples = delay_samples;
    }

    int prev_read_position = pState->write_position;
    decrementRing(&prev_read_position, pState->prev_delay_samples, pState->delay_buf.size());

    int read_position = pState->write_position;
    decrementRing(&read_position, delay_samples, pState->delay_buf.size());

    // 3. Smooth micro-ramping across this buffer (prevents clicks)
    RampingValue<CSAMPLE_GAIN> send(pState->prev_send, target_send, frames_per_buffer);
    RampingValue<CSAMPLE_GAIN> dry(pState->prev_dry, target_dry, frames_per_buffer);
    RampingValue<CSAMPLE_GAIN> feedback(pState->prev_feedback, target_feedback, frames_per_buffer);

    // 4. Setup DJ Filter coefficients
    const bool use_filter = (std::abs(filter_param) > 0.02);
    const bool is_hpf = (filter_param > 0.0);
    float alpha = 1.0f;

    if (use_filter) {
        double cutoff_hz = 20000.0;
        if (is_hpf) {
            // High-Pass: cutoff sweeps up as knob turns right (up to 4000 Hz)
            cutoff_hz = 20.0 + std::abs(filter_param) * 3980.0;
        } else {
            // Low-Pass: cutoff sweeps down as knob turns left (down to 250 Hz)
            cutoff_hz = 20000.0 - std::abs(filter_param) * 19750.0;
        }
        cutoff_hz = std::clamp(cutoff_hz, 20.0, 20000.0);
        double dt = 1.0 / sample_rate;
        double rc = 1.0 / (2.0 * M_PI * cutoff_hz);
        alpha = static_cast<float>(dt / (rc + dt));
    }

    // 5. DSP Loop (Sample-by-sample with continuous delay line)
    int ramp_index = 0;
    for (int i = 0; i < engineParameters.samplesPerBuffer(); i += channels) {
        const CSAMPLE_GAIN send_gain = send.getNth(ramp_index);
        const CSAMPLE_GAIN dry_gain = dry.getNth(ramp_index);
        const CSAMPLE_GAIN fb_gain = feedback.getNth(ramp_index);
        ++ramp_index;

        // Read from delay line with crossfade if delay time is changing dynamically
        CSAMPLE delay_sample_l = pState->delay_buf[read_position];
        CSAMPLE delay_sample_r = (channels > 1) ? pState->delay_buf[read_position + 1] : delay_sample_l;

        if (read_position != prev_read_position) {
            const CSAMPLE_GAIN frac = static_cast<CSAMPLE_GAIN>(i) /
                    static_cast<CSAMPLE_GAIN>(engineParameters.samplesPerBuffer());
            delay_sample_l *= frac;
            delay_sample_r *= frac;
            delay_sample_l += pState->delay_buf[prev_read_position] * (1.0f - frac);
            if (channels > 1) {
                delay_sample_r += pState->delay_buf[prev_read_position + 1] * (1.0f - frac);
            }
            incrementRing(&prev_read_position, channels, pState->delay_buf.size());
        }
        incrementRing(&read_position, channels, pState->delay_buf.size());

        // Apply DJ Filter in the feedback loop
        CSAMPLE filtered_l = delay_sample_l;
        CSAMPLE filtered_r = delay_sample_r;

        if (use_filter) {
            pState->filter_lp_l += alpha * (delay_sample_l - pState->filter_lp_l);
            pState->filter_lp_r += alpha * (delay_sample_r - pState->filter_lp_r);

            if (is_hpf) {
                filtered_l = delay_sample_l - pState->filter_lp_l;
                filtered_r = delay_sample_r - pState->filter_lp_r;
            } else {
                filtered_l = pState->filter_lp_l;
                filtered_r = pState->filter_lp_r;
            }
        }

        // Write new audio + feedback into delay line
        pState->delay_buf[pState->write_position] = SampleUtil::clampSample(
                pInput[i] * send_gain + filtered_l * fb_gain);

        if (channels > 1) {
            pState->delay_buf[pState->write_position + 1] = SampleUtil::clampSample(
                    pInput[i + 1] * send_gain + filtered_r * fb_gain);
        }

        // Output = Direct Live Dry + Delay Tail (Wet)
        pOutput[i] = SampleUtil::clampSample(pInput[i] * dry_gain + filtered_l);
        if (channels > 1) {
            pOutput[i + 1] = SampleUtil::clampSample(pInput[i + 1] * dry_gain + filtered_r);
        }

        incrementRing(&pState->write_position, channels, pState->delay_buf.size());
    }

    // Save states for next buffer's interpolation
    pState->prev_send = target_send;
    pState->prev_dry = target_dry;
    pState->prev_feedback = target_feedback;
    pState->prev_delay_samples = delay_samples;

    if (!is_enabled && enableState == EffectEnableState::Disabling) {
        pState->clear();
    }
}
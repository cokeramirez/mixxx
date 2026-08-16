#include "effects/backends/builtin/echoouteffect.h"

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/math.h"
#include "util/sample.h"

constexpr int EchoOutGroupState::kMaxDelaySeconds;

// static
QString EchoOutEffect::getId() {
    return "org.mixxx.effects.echoout";
}

// static
EffectManifestPointer EchoOutEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());

    // Echo Out handles the dry signal directly so that it can cleanly mute on trigger.
    pManifest->setAddDryToWet(false);
    pManifest->setEffectRampsFromDry(false);

    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Echo Out"));
    pManifest->setShortName(QObject::tr("Echo Out"));
    pManifest->setAuthor("Mixxx Community");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "Captures the input audio and loops it into an echo tail with decay and DJ filter, while muting the live track."));

    // Primary Metaknob Parameter: Decay Time / Duration & On-Off
    EffectManifestParameterPointer decay = pManifest->addParameter();
    decay->setId("decay_time");
    decay->setName(QObject::tr("Duration"));
    decay->setShortName(QObject::tr("Duration"));
    decay->setDescription(QObject::tr(
            "Controls decay time (1/4 to 16 beats) and activates the effect. At 0 the effect is Off/Bypass. At max it acts as an Infinite Looper."));
    decay->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    decay->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    decay->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    decay->setRange(0.0, 0.0, 1.0);

    // Loop Size Parameter
    EffectManifestParameterPointer size = pManifest->addParameter();
    size->setId("echo_size");
    size->setName(QObject::tr("Size"));
    size->setShortName(QObject::tr("Size"));
    size->setDescription(QObject::tr(
            "Loop size: 1/16 to 4 beats (or 30ms to 3.0s if tempo is not detected)."));
    size->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    size->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    size->setRange(0.0625, 1.0, 4.0);

    // DJ Filter Parameter (-1.0 Low-Pass <-> 0 Neutral <-> +1.0 High-Pass)
    EffectManifestParameterPointer filter = pManifest->addParameter();
    filter->setId("filter_sweep");
    filter->setName(QObject::tr("Filter"));
    filter->setShortName(QObject::tr("Filter"));
    filter->setDescription(QObject::tr(
            "DJ Filter sweep during decay. Left = Low-Pass Filter, Center = Off, Right = High-Pass Filter."));
    filter->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    filter->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    filter->setRange(-1.0, 0.0, 1.0);

    // Quantize Toggle
    EffectManifestParameterPointer quantize = pManifest->addParameter();
    quantize->setId("quantize");
    quantize->setName(QObject::tr("Quantize"));
    quantize->setShortName(QObject::tr("Quantize"));
    quantize->setDescription(QObject::tr(
            "Round loop size to musical beat fractions."));
    quantize->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    quantize->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    quantize->setRange(0, 1, 1);

    return pManifest;
}

void EchoOutEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pDecayParameter = parameters.value("decay_time");
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
    const double decay_param = m_pDecayParameter->value();
    const double filter_param = m_pFilterParameter->value();
    double size_param = m_pSizeParameter->value();
    const int channels = engineParameters.channelCount();
    const double sample_rate = static_cast<double>(engineParameters.sampleRate());

    // 1. OFF / BYPASS State when decay parameter is 0.0 or effect is disabled
    if (decay_param < 0.01 || enableState == EffectEnableState::Disabling || enableState == EffectEnableState::Disabled) {
        if (pState->phase != EchoOutPhase::Idle) {
            pState->clear();
        }
        SampleUtil::copy(pOutput, pInput, engineParameters.samplesPerBuffer());
        return;
    }

    // 2. Calculate Size in seconds and frames
    double size_seconds = 0.0;
    double max_capture_seconds = 0.0;

    if (groupFeatures.beat_length.has_value()) {
        if (m_pQuantizeParameter->toBool()) {
            if (size_param < 0.125) {
                size_param = 0.0625; // 1/16 beat
            } else if (size_param < 0.25) {
                size_param = 0.125;  // 1/8 beat
            } else if (size_param < 0.5) {
                size_param = 0.25;   // 1/4 beat
            } else if (size_param < 0.75) {
                size_param = 0.5;    // 1/2 beat
            } else if (size_param < 1.0) {
                size_param = 0.75;   // 3/4 beat
            } else if (size_param < 2.0) {
                size_param = 1.0;    // 1 beat
            } else if (size_param < 4.0) {
                size_param = 2.0;    // 2 beats
            } else {
                size_param = 4.0;    // 4 beats
            }
        }
        size_seconds = size_param * groupFeatures.beat_length->seconds;
        max_capture_seconds = 4.0 * groupFeatures.beat_length->seconds;
    } else {
        // Fallback without BPM: 0.03s (30ms) to 3.0s
        size_seconds = std::clamp(size_param * 0.75, 0.03, 3.0);
        max_capture_seconds = 3.0;
    }

    int size_frames = static_cast<int>(size_seconds * sample_rate);
    int max_capture_frames = static_cast<int>(max_capture_seconds * sample_rate);
    int max_buffer_frames = pState->buffer.size() / channels;

    size_frames = std::clamp(size_frames, 64, max_buffer_frames);
    max_capture_frames = std::clamp(max_capture_frames, size_frames, max_buffer_frames);

    // 3. Trigger / Start Transition
    if (pState->phase == EchoOutPhase::Idle) {
        pState->phase = EchoOutPhase::Recording;
        pState->recorded_frames = 0;
        pState->loop_read_pos = 0;
        pState->current_gain = 1.0f;
        pState->prev_decay_param = decay_param;
    }

    // 4. Calculate Alpha coefficient for the 1-pole DJ Filter
    // Filter sweep varies as gain drops: decay progress = (1.0 - current_gain)
    float decay_progress = 1.0f - pState->current_gain;
    if (decay_param >= 0.95) {
        decay_progress = 0.0f; // Static/Neutral in infinite loop
    }

    double cutoff_hz = 20000.0;
    bool is_hpf = false;
    if (filter_param > 0.01) {
        is_hpf = true;
        cutoff_hz = 20.0 + (filter_param * 4000.0 * decay_progress);
    } else if (filter_param < -0.01) {
        is_hpf = false;
        double target_min_hz = 200.0 + (1.0 + filter_param) * 800.0;
        cutoff_hz = 20000.0 - (20000.0 - target_min_hz) * decay_progress;
    }
    cutoff_hz = std::clamp(cutoff_hz, 20.0, 20000.0);

    double dt = 1.0 / sample_rate;
    double rc = 1.0 / (2.0 * M_PI * cutoff_hz);
    float alpha = static_cast<float>(dt / (rc + dt));

    // 5. Process audio buffer frame-by-frame
    int buffer_frame_count = engineParameters.framesPerBuffer();

    for (int frame = 0; frame < buffer_frame_count; ++frame) {
        int sample_idx = frame * channels;

        if (pState->phase == EchoOutPhase::Recording) {
            // Pass live audio dry while recording
            for (int ch = 0; ch < channels; ++ch) {
                pOutput[sample_idx + ch] = pInput[sample_idx + ch];
                if (pState->recorded_frames < max_capture_frames) {
                    pState->buffer[pState->recorded_frames * channels + ch] = pInput[sample_idx + ch];
                }
            }
            pState->recorded_frames++;

            // When selected size is reached, cut dry and switch to EchoOut loop
            if (pState->recorded_frames >= size_frames) {
                pState->phase = EchoOutPhase::EchoOut;
                pState->loop_read_pos = 0;
            }
        } else if (pState->phase == EchoOutPhase::EchoOut) {
            // Evaluate dynamic gain at the boundary of each loop cycle
            if (pState->loop_read_pos == 0) {
                if (decay_param >= 0.95) {
                    // Infinite looper: hold current gain
                } else {
                    // Relative decay calculation
                    double decay_beats = 0.25 + (decay_param / 0.95) * (16.0 - 0.25);
                    double total_loops = std::max(1.0, decay_beats / size_param);
                    float gain_step = pState->current_gain / static_cast<float>(total_loops);
                    pState->current_gain = std::max(0.0f, pState->current_gain - gain_step);
                }
            }

            if (pState->current_gain <= 0.0001f) {
                // Decay finished: output silence (dry remains cut)
                for (int ch = 0; ch < channels; ++ch) {
                    pOutput[sample_idx + ch] = 0.0f;
                }
            } else {
                int read_buf_idx = (pState->loop_read_pos % size_frames) * channels;
                CSAMPLE raw_l = pState->buffer[read_buf_idx] * pState->current_gain;
                CSAMPLE raw_r = pState->buffer[read_buf_idx + (channels > 1 ? 1 : 0)] * pState->current_gain;

                // Apply 1-pole Low-Pass / High-Pass DJ Filter
                pState->filter_lp_l += alpha * (raw_l - pState->filter_lp_l);
                pState->filter_lp_r += alpha * (raw_r - pState->filter_lp_r);

                if (is_hpf) {
                    pOutput[sample_idx] = SampleUtil::clampSample(raw_l - pState->filter_lp_l);
                    if (channels > 1) {
                        pOutput[sample_idx + 1] = SampleUtil::clampSample(raw_r - pState->filter_lp_r);
                    }
                } else {
                    pOutput[sample_idx] = SampleUtil::clampSample(pState->filter_lp_l);
                    if (channels > 1) {
                        pOutput[sample_idx + 1] = SampleUtil::clampSample(pState->filter_lp_r);
                    }
                }

                pState->loop_read_pos = (pState->loop_read_pos + 1) % size_frames;
            }
        }
    }
}
#include "effects/backends/builtin/echoouteffect.h"

#include <cmath>

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
            "Captures the input audio and loops it into an echo tail with dynamic decay and DJ filter, while muting the live track."));

    // 1. Primary Metaknob Parameter: Decay Time / Duration & On-Off
    EffectManifestParameterPointer decay = pManifest->addParameter();
    decay->setId("decay_time");
    decay->setName(QObject::tr("Duration"));
    decay->setShortName(QObject::tr("Duration"));
    decay->setDescription(QObject::tr(
            "Controls decay time (1/4 to 16 beats) and activates the effect.\n"
            "0 = Off / Bypass\n"
            "0.01 - 0.94 = Natural echo decay\n"
            "0.95 - 1.0 = Infinite Looper / Freeze"));
    decay->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    decay->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    decay->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    decay->setRange(0.0, 0.70, 1.0);

    // 2. Loop Size Parameter
    EffectManifestParameterPointer size = pManifest->addParameter();
    size->setId("echo_size");
    size->setName(QObject::tr("Size"));
    size->setShortName(QObject::tr("Size"));
    size->setDescription(QObject::tr(
            "Loop size.\n"
            "Quantized: 1/4, 1/2, 1 (default), 2, 4 beats.\n"
            "Unquantized: 0.10s to 2.50s continuous."));
    size->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    size->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    // Range 0.0 to 1.0 with default at 0.50 (Center = 1 Beat)
    size->setRange(0.0, 0.50, 1.0);

    // 3. DJ Filter Parameter (-1.0 Low-Pass <-> 0 Neutral <-> +1.0 High-Pass)
    EffectManifestParameterPointer filter = pManifest->addParameter();
    filter->setId("filter_sweep");
    filter->setName(QObject::tr("Filter"));
    filter->setShortName(QObject::tr("Filter"));
    filter->setDescription(QObject::tr(
            "DJ Filter sweep during decay. Left = Low-Pass Filter, Center = Neutral, Right = High-Pass Filter."));
    filter->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    filter->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    filter->setRange(-1.0, 0.0, 1.0);

    // 4. Quantize Toggle
    EffectManifestParameterPointer quantize = pManifest->addParameter();
    quantize->setId("quantize");
    quantize->setName(QObject::tr("Quantize"));
    quantize->setShortName(QObject::tr("Quantize"));
    quantize->setDescription(QObject::tr(
            "Snap loop size to musical beat fractions."));
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
    const double size_param = m_pSizeParameter->value();
    const int channels = engineParameters.channelCount();
    const double sample_rate = static_cast<double>(engineParameters.sampleRate());
    const int frames_per_buffer = engineParameters.framesPerBuffer();

    // 1. OFF / BYPASS State: Live audio passes cleanly through
    if (decay_param < 0.01 || enableState == EffectEnableState::Disabling || enableState == EffectEnableState::Disabled) {
        if (pState->phase != EchoOutPhase::Idle) {
            pState->clear();
        }
        SampleUtil::copy(pOutput, pInput, engineParameters.samplesPerBuffer());
        return;
    }

    // 2. Calculate Size in seconds and beats
    double size_seconds = 0.5;
    double effective_beats = 1.0;

    if (groupFeatures.beat_length.has_value()) {
        if (m_pQuantizeParameter->toBool()) {
            // 5 Balanced physical sectors: 1/4, 1/2, 1, 2, 4 beats
            static const double kQuantizedBeats[] = {0.25, 0.50, 1.00, 2.00, 4.00};
            int sector = std::clamp(static_cast<int>(size_param * 5.0), 0, 4);
            effective_beats = kQuantizedBeats[sector];
        } else {
            effective_beats = 0.25 + size_param * (4.0 - 0.25);
        }
        size_seconds = effective_beats * groupFeatures.beat_length->seconds;
    } else {
        // Fallback without BPM: 0.10s to 2.50s
        size_seconds = 0.10 + size_param * (2.50 - 0.10);
        effective_beats = size_seconds;
    }

    int size_frames = static_cast<int>(size_seconds * sample_rate);
    int max_buffer_frames = pState->buffer.size() / channels;
    size_frames = std::clamp(size_frames, 64, max_buffer_frames);

    // 3. Trigger / Transition from Idle to Recording
    if (pState->phase == EchoOutPhase::Idle) {
        pState->phase = EchoOutPhase::Recording;
        pState->recorded_frames = 0;
        pState->loop_read_pos = 0;
        pState->current_gain = 1.0f;
        pState->prev_decay_param = decay_param;
    }

    // 4. Calculate Dynamic Filter Parameters
    // Filter sweep varies as gain drops: decay_progress goes 0.0 -> 1.0
    float decay_progress = 1.0f - pState->current_gain;
    if (decay_param >= 0.95) {
        decay_progress = 0.0f; // Static/neutral during infinite freeze
    }

    const bool use_filter = (std::abs(filter_param) > 0.02);
    const bool is_hpf = (filter_param > 0.0);
    float alpha = 1.0f;

    if (use_filter) {
        double cutoff_hz = 20000.0;
        if (is_hpf) {
            // High-Pass: sweeps up into air/treble as the echo dies out
            cutoff_hz = 20.0 + (filter_param * 4000.0 * decay_progress);
        } else {
            // Low-Pass: sweeps down into dub/bass as the echo dies out
            double target_min_hz = 200.0 + (1.0 + filter_param) * 800.0;
            cutoff_hz = 20000.0 - (20000.0 - target_min_hz) * decay_progress;
        }
        cutoff_hz = std::clamp(cutoff_hz, 20.0, 20000.0);
        alpha = static_cast<float>(1.0 - std::exp(-2.0 * M_PI * cutoff_hz / sample_rate));
    }

    // 5. Audio Loop Processing
    for (int frame = 0; frame < frames_per_buffer; ++frame) {
        int sample_idx = frame * channels;

        if (pState->phase == EchoOutPhase::Recording) {
            // Live audio dry output while recording the phrase
            for (int ch = 0; ch < channels; ++ch) {
                pOutput[sample_idx + ch] = pInput[sample_idx + ch];
                pState->buffer[pState->recorded_frames * channels + ch] = pInput[sample_idx + ch];
            }
            pState->recorded_frames++;

            // When selected capture size is reached, cut live track and switch to EchoOut loop
            if (pState->recorded_frames >= size_frames) {
                // Micro-crossfade seam smoothing at buffer boundary to guarantee zero click
                const int kCrossfadeFrames = std::min(64, size_frames / 4);
                for (int f = 0; f < kCrossfadeFrames; ++f) {
                    float head_gain = static_cast<float>(f) / static_cast<float>(kCrossfadeFrames);
                    float tail_gain = 1.0f - head_gain;
                    int tail_sample = (size_frames - kCrossfadeFrames + f) * channels;
                    int head_sample = f * channels;
                    for (int ch = 0; ch < channels; ++ch) {
                        pState->buffer[tail_sample + ch] =
                                pState->buffer[tail_sample + ch] * tail_gain +
                                pState->buffer[head_sample + ch] * head_gain;
                    }
                }

                pState->phase = EchoOutPhase::EchoOut;
                pState->loop_read_pos = 0;
            }
        } else if (pState->phase == EchoOutPhase::EchoOut) {
            // Decay update at the boundary of each loop cycle
            if (pState->loop_read_pos == 0) {
                if (decay_param >= 0.95) {
                    // Infinite looper mode: hold full gain
                    pState->current_gain = 1.0f;
                } else {
                    // Option 2: Acoustic / logarithmic musical decay
                    double decay_beats = 0.25 + (decay_param / 0.95) * (16.0 - 0.25);
                    double total_loops = std::max(1.0, decay_beats / effective_beats);

                    // Drop by factor reaching -40dB (0.01) at the target loop count
                    float decay_factor = std::pow(0.01f, 1.0f / static_cast<float>(total_loops));
                    pState->current_gain *= decay_factor;

                    if (pState->current_gain < 0.005f) {
                        pState->current_gain = 0.0f; // Clean, complete silence
                    }
                }
            }

            if (pState->current_gain <= 0.0f) {
                // Decay finished: full silence (live deck stays cleanly muted)
                for (int ch = 0; ch < channels; ++ch) {
                    pOutput[sample_idx + ch] = 0.0f;
                }
            } else {
                int read_buf_idx = (pState->loop_read_pos % size_frames) * channels;
                CSAMPLE raw_l = pState->buffer[read_buf_idx] * pState->current_gain;
                CSAMPLE raw_r = pState->buffer[read_buf_idx + (channels > 1 ? 1 : 0)] * pState->current_gain;

                CSAMPLE filtered_l = raw_l;
                CSAMPLE filtered_r = raw_r;

                if (use_filter) {
                    pState->filter_lp_l += alpha * (raw_l - pState->filter_lp_l);
                    pState->filter_lp_r += alpha * (raw_r - pState->filter_lp_r);

                    if (is_hpf) {
                        filtered_l = raw_l - pState->filter_lp_l;
                        filtered_r = raw_r - pState->filter_lp_r;
                    } else {
                        filtered_l = pState->filter_lp_l;
                        filtered_r = pState->filter_lp_r;
                    }
                } else {
                    // Keep filter state updated to avoid pops when turned on
                    pState->filter_lp_l = raw_l;
                    pState->filter_lp_r = raw_r;
                }

                pOutput[sample_idx] = SampleUtil::clampSample(filtered_l);
                if (channels > 1) {
                    pOutput[sample_idx + 1] = SampleUtil::clampSample(filtered_r);
                }

                pState->loop_read_pos = (pState->loop_read_pos + 1) % size_frames;
            }
        }
    }
}
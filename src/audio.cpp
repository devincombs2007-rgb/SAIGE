#include "saige/audio.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace saige {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr std::uint32_t N = AudioMixer::chunk_frames;

bool fin(float x) { return std::isfinite(x); }
bool fin(Vec3 v) { return fin(v.x) && fin(v.y) && fin(v.z); }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 scaled(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

void check_voice_scalars(const VoiceParams& p) {
    if (!fin(p.gain) || p.gain < 0.0f) throw std::invalid_argument("audio: voice gain must be finite and non-negative");
    if (!fin(p.pitch) || !(p.pitch > 0.0f)) throw std::invalid_argument("audio: voice pitch must be finite and positive");
    if (!fin(p.position) || !fin(p.velocity)) throw std::invalid_argument("audio: voice position and velocity must be finite");
    if (!fin(p.min_distance) || !(p.min_distance > 0.0f) || !fin(p.max_distance) || !(p.max_distance >= p.min_distance)) {
        throw std::invalid_argument("audio: voice distances must be finite with 0 < min_distance <= max_distance");
    }
    if (!fin(p.rolloff) || p.rolloff < 0.0f) throw std::invalid_argument("audio: voice rolloff must be finite and non-negative");
    if (!fin(p.occlusion) || p.occlusion < 0.0f || p.occlusion > 1.0f) throw std::invalid_argument("audio: voice occlusion must be within 0..1");
    if (!fin(p.reverb_send) || p.reverb_send < 0.0f || p.reverb_send > 1.0f) throw std::invalid_argument("audio: voice reverb send must be within 0..1");
}

float distance_gain(const VoiceParams& p, float dist) {
    const float d = std::clamp(dist, p.min_distance, p.max_distance);
    switch (p.model) {
        case Rolloff::Inverse: return p.min_distance / (p.min_distance + p.rolloff * (d - p.min_distance));
        case Rolloff::Linear: {
            const float span = p.max_distance - p.min_distance;
            return span > 0.0f ? std::max(0.0f, 1.0f - p.rolloff * (d - p.min_distance) / span) : 1.0f;
        }
        case Rolloff::Exponential: return std::pow(d / p.min_distance, -p.rolloff);
    }
    return 1.0f;
}

// Freeverb (Jezar at Dreampoint, public domain): eight lowpass-feedback combs in parallel and four allpass filters in series per channel.
class Reverb {
public:
    explicit Reverb(std::uint32_t sample_rate) {
        const float k = static_cast<float>(sample_rate) / 44100.0f;
        static constexpr int comb_tunings[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr int allpass_tunings[4] = {556, 441, 341, 225};
        const int spread = static_cast<int>(std::lround(23.0f * k));
        for (int i = 0; i < 8; ++i) {
            const int n = std::max(1, static_cast<int>(std::lround(static_cast<float>(comb_tunings[i]) * k)));
            comb_[0][static_cast<std::size_t>(i)].buf.assign(static_cast<std::size_t>(n), 0.0f);
            comb_[1][static_cast<std::size_t>(i)].buf.assign(static_cast<std::size_t>(n + spread), 0.0f);
        }
        for (int i = 0; i < 4; ++i) {
            const int n = std::max(1, static_cast<int>(std::lround(static_cast<float>(allpass_tunings[i]) * k)));
            allpass_[0][static_cast<std::size_t>(i)].buf.assign(static_cast<std::size_t>(n), 0.0f);
            allpass_[1][static_cast<std::size_t>(i)].buf.assign(static_cast<std::size_t>(n + spread), 0.0f);
        }
        set(0.5f, 0.5f);
    }
    void set(float room, float damping) {
        feedback_ = room * 0.28f + 0.7f;
        damp_ = damping * 0.4f;
    }
    void process(float in, float& left, float& right) {
        const float x = in * 0.015f;
        float out[2] = {0.0f, 0.0f};
        for (std::size_t c = 0; c < 2; ++c) {
            for (auto& cb : comb_[c]) {
                const float o = cb.buf[cb.i];
                cb.store = o * (1.0f - damp_) + cb.store * damp_;
                cb.buf[cb.i] = x + cb.store * feedback_;
                if (++cb.i == cb.buf.size()) cb.i = 0;
                out[c] += o;
            }
            for (auto& ap : allpass_[c]) {
                const float b = ap.buf[ap.i];
                const float o = -out[c] + b;
                ap.buf[ap.i] = out[c] + b * 0.5f;
                if (++ap.i == ap.buf.size()) ap.i = 0;
                out[c] = o;
            }
        }
        left = out[0] * 3.0f;
        right = out[1] * 3.0f;
    }

private:
    struct Comb {
        std::vector<float> buf;
        std::size_t i = 0;
        float store = 0.0f;
    };
    struct Allpass {
        std::vector<float> buf;
        std::size_t i = 0;
    };
    std::array<std::array<Comb, 8>, 2> comb_;
    std::array<std::array<Allpass, 4>, 2> allpass_;
    float feedback_ = 0.84f;
    float damp_ = 0.2f;
};

struct Voice {
    bool alive = false;
    std::uint32_t generation = 0;
    VoiceParams p;
    double pos = 0.0;  // in source frames
    bool ramped_once = false;
    float lp = 0.0f, lp2 = 0.0f;  // two cascaded one-pole lowpasses: 12 dB per octave, the way a wall muffles
    float gl = 0.0f, gr = 0.0f, send = 0.0f;
    std::uint64_t order = 0;
    float audibility = 0.0f;
};

struct DuckState {
    DuckRule rule;
    float depth = 0.0f;  // 0..1, how far the rule has pulled the target down
};

} // namespace

struct AudioMixer::Impl {
    AudioMixerConfig cfg;
    std::vector<AudioClip> clips;
    std::vector<Voice> voices;
    std::uint64_t order = 0;
    Listener listener;
    Vec3 right{1, 0, 0};
    std::array<float, kAudioBusCount> bus_gain;
    std::array<float, kAudioBusCount> bus_applied;  // the effective gain at the end of the last chunk (bus gain times ducking)
    std::array<float, kAudioBusCount> bus_level{};
    std::array<float, kAudioBusCount> bus_duck;
    float master = 1.0f;
    float master_applied = 1.0f;
    ReverbSettings reverb_settings;
    Reverb reverb;
    std::vector<DuckState> ducks;
    std::array<float, 2 * N> chunk{};
    std::uint32_t chunk_pos = N;  // frames of `chunk` already handed out; N means it is spent
    AudioStats stats;

    explicit Impl(const AudioMixerConfig& c) : cfg(c), reverb(c.sample_rate) {
        voices.resize(c.max_voices);
        bus_gain.fill(1.0f);
        bus_applied.fill(1.0f);
        bus_duck.fill(1.0f);
    }

    Voice* find(VoiceId id) {
        if (id.generation == 0 || id.index == 0 || id.index > voices.size()) return nullptr;
        Voice& v = voices[id.index - 1];
        return v.alive && v.generation == id.generation ? &v : nullptr;
    }
    const Voice* find(VoiceId id) const { return const_cast<Impl*>(this)->find(id); }

    float estimate_audibility(const VoiceParams& p) const {
        if (!p.spatial) return p.gain;
        return p.gain * distance_gain(p, length(sub(p.position, listener.position))) * (1.0f - p.occlusion * (1.0f - cfg.occluded_gain));
    }

    void process_chunk() {
        std::array<std::array<float, 2 * N>, kAudioBusCount> acc{};
        std::array<float, N> send_in{};
        const float sr = static_cast<float>(cfg.sample_rate);
        for (Voice& v : voices) {
            if (!v.alive) continue;
            const VoiceParams& p = v.p;
            const AudioClip& clip = clips[p.clip.index - 1];
            float gain = p.gain;
            float pan = 0.0f;
            float cutoff = 20000.0f;
            float pitch = p.pitch;
            if (p.spatial) {
                const Vec3 d = sub(p.position, listener.position);
                const float dist = length(d);
                gain *= distance_gain(p, dist);
                if (dist > 1e-4f) {
                    const Vec3 u = scaled(d, 1.0f / dist);
                    pan = std::clamp(dot(u, right), -1.0f, 1.0f) * std::min(1.0f, dist / p.min_distance);
                    if (cfg.doppler_scale > 0.0f) {
                        const float c = cfg.speed_of_sound;
                        const float vs = std::clamp(dot(p.velocity, u), -0.9f * c, 0.9f * c);
                        const float vl = std::clamp(dot(listener.velocity, u), -0.9f * c, 0.9f * c);
                        const float shift = (c + vl) / (c + vs);
                        pitch *= std::clamp(1.0f + (shift - 1.0f) * cfg.doppler_scale, 0.5f, 2.0f);
                    }
                }
                if (cfg.air_absorption_distance > 0.0f) cutoff = 20000.0f / (1.0f + dist / cfg.air_absorption_distance);
                if (p.occlusion > 0.0f) {
                    gain *= 1.0f - p.occlusion * (1.0f - cfg.occluded_gain);
                    cutoff = std::min(cutoff, 20000.0f * std::pow(cfg.occluded_cutoff_hz / 20000.0f, p.occlusion));
                }
            }
            const float ang = (pan + 1.0f) * (kPi / 4.0f);
            const float tl = gain * std::cos(ang), tr = gain * std::sin(ang);
            const float ts = p.reverb_send;
            if (!v.ramped_once) {
                v.gl = tl;
                v.gr = tr;
                v.send = ts;
                v.ramped_once = true;
            }
            const float lp_a = cutoff >= 0.49f * sr ? 1.0f : 1.0f - std::exp(-2.0f * kPi * cutoff / sr);
            const double step = static_cast<double>(pitch) * static_cast<double>(clip.sample_rate) / static_cast<double>(cfg.sample_rate);
            const std::size_t n = clip.samples.size();
            auto& bus = acc[static_cast<std::size_t>(p.bus)];
            for (std::uint32_t i = 0; i < N; ++i) {
                std::size_t i0 = static_cast<std::size_t>(v.pos);
                if (i0 >= n) {
                    if (!p.loop) break;
                    v.pos -= static_cast<double>(n) * std::floor(v.pos / static_cast<double>(n));
                    i0 = static_cast<std::size_t>(v.pos);
                }
                const float frac = static_cast<float>(v.pos - static_cast<double>(i0));
                std::size_t i1 = i0 + 1;
                float s1;
                if (i1 >= n) s1 = p.loop ? clip.samples[i1 - n] : 0.0f;
                else s1 = clip.samples[i1];
                const float s = clip.samples[i0] + (s1 - clip.samples[i0]) * frac;
                v.lp += lp_a * (s - v.lp);
                v.lp2 += lp_a * (v.lp - v.lp2);
                const float t = static_cast<float>(i + 1) / static_cast<float>(N);
                const float gl = v.gl + (tl - v.gl) * t, gr = v.gr + (tr - v.gr) * t, gs = v.send + (ts - v.send) * t;
                bus[2 * i] += v.lp2 * gl;
                bus[2 * i + 1] += v.lp2 * gr;
                send_in[i] += v.lp2 * gs * gain;
                v.pos += step;
            }
            v.gl = tl;
            v.gr = tr;
            v.send = ts;
            if (!p.loop && static_cast<std::size_t>(v.pos) >= n) {
                v.alive = false;
                if (stats.active_voices > 0) --stats.active_voices;
            }
        }

        // Bus levels (before their gain), then ducking, then the gains.
        for (std::size_t b = 0; b < kAudioBusCount; ++b) {
            float e = 0.0f;
            for (std::uint32_t i = 0; i < 2 * N; ++i) e += acc[b][i] * acc[b][i];
            bus_level[b] = std::sqrt(e / static_cast<float>(2 * N));
        }
        const float dt = static_cast<float>(N) / sr;
        bus_duck.fill(1.0f);
        for (DuckState& d : ducks) {
            const float heard = bus_level[static_cast<std::size_t>(d.rule.source)] * bus_gain[static_cast<std::size_t>(d.rule.source)];
            const float target = heard > d.rule.threshold ? 1.0f : 0.0f;
            const float tau = target > d.depth ? d.rule.attack : d.rule.release;
            d.depth += (target - d.depth) * (1.0f - std::exp(-dt / std::max(tau, 1e-4f)));
            bus_duck[static_cast<std::size_t>(d.rule.target)] *= 1.0f - d.rule.amount * d.depth;
        }

        std::array<float, 2 * N> mix{};
        for (std::size_t b = 0; b < kAudioBusCount; ++b) {
            const float target = bus_gain[b] * bus_duck[b];
            for (std::uint32_t i = 0; i < N; ++i) {
                const float g = bus_applied[b] + (target - bus_applied[b]) * (static_cast<float>(i + 1) / static_cast<float>(N));
                mix[2 * i] += acc[b][2 * i] * g;
                mix[2 * i + 1] += acc[b][2 * i + 1] * g;
            }
            bus_applied[b] = target;
        }
        const float wet = reverb_settings.wet;
        for (std::uint32_t i = 0; i < N; ++i) {
            const float m = master_applied + (master - master_applied) * (static_cast<float>(i + 1) / static_cast<float>(N));
            float rl = 0.0f, rr = 0.0f;
            reverb.process(send_in[i], rl, rr);
            float l = mix[2 * i] + rl * wet, r = mix[2 * i + 1] + rr * wet;
            l *= m;
            r *= m;
            if (std::fabs(l) > 1.0f) {
                ++stats.clipped_samples;
                l = std::clamp(l, -1.0f, 1.0f);
            }
            if (std::fabs(r) > 1.0f) {
                ++stats.clipped_samples;
                r = std::clamp(r, -1.0f, 1.0f);
            }
            chunk[2 * i] = l;
            chunk[2 * i + 1] = r;
        }
        master_applied = master;
        chunk_pos = 0;
    }
};

AudioMixer::AudioMixer(const AudioMixerConfig& config) {
    if (config.sample_rate < 8000 || config.sample_rate > 384000) throw std::invalid_argument("AudioMixer: the sample rate must be within 8000..384000");
    if (config.max_voices == 0) throw std::invalid_argument("AudioMixer: at least one voice is needed");
    if (!fin(config.speed_of_sound) || !(config.speed_of_sound > 0.0f) || !fin(config.doppler_scale) || config.doppler_scale < 0.0f || !fin(config.occluded_gain) || config.occluded_gain < 0.0f ||
        config.occluded_gain > 1.0f || !fin(config.occluded_cutoff_hz) || !(config.occluded_cutoff_hz > 20.0f) || config.occluded_cutoff_hz > 20000.0f || !fin(config.air_absorption_distance) ||
        config.air_absorption_distance < 0.0f) {
        throw std::invalid_argument("AudioMixer: invalid physical configuration");
    }
    impl_ = std::make_unique<Impl>(config);
}
AudioMixer::~AudioMixer() = default;

ClipId AudioMixer::add_clip(AudioClip clip) {
    if (clip.samples.empty()) throw std::invalid_argument("add_clip: the clip is empty");
    if (clip.sample_rate == 0) throw std::invalid_argument("add_clip: the sample rate is zero");
    for (const float s : clip.samples) {
        if (!fin(s)) throw std::invalid_argument("add_clip: a sample is not finite");
    }
    impl_->clips.push_back(std::move(clip));
    return ClipId{static_cast<std::uint32_t>(impl_->clips.size())};
}

bool AudioMixer::has_clip(ClipId id) const { return id.index != 0 && id.index <= impl_->clips.size(); }

VoiceId AudioMixer::play(const VoiceParams& params) {
    if (!has_clip(params.clip)) throw std::invalid_argument("play: unknown clip");
    check_voice_scalars(params);
    Impl& m = *impl_;
    std::size_t slot = m.voices.size();
    for (std::size_t i = 0; i < m.voices.size(); ++i) {
        if (!m.voices[i].alive) {
            slot = i;
            break;
        }
    }
    const float audibility = m.estimate_audibility(params);
    if (slot == m.voices.size()) {
        // Full: the victim is the least important voice (lowest priority, then quietest, then oldest). The new voice replaces it only if it is more important.
        std::size_t victim = 0;
        for (std::size_t i = 1; i < m.voices.size(); ++i) {
            const Voice& a = m.voices[i];
            const Voice& b = m.voices[victim];
            if (a.p.priority != b.p.priority ? a.p.priority < b.p.priority : (a.audibility != b.audibility ? a.audibility < b.audibility : a.order < b.order)) victim = i;
        }
        const Voice& v = m.voices[victim];
        const bool better = params.priority != v.p.priority ? params.priority > v.p.priority : audibility > v.audibility;
        if (!better) {
            ++m.stats.voices_rejected;
            return {};
        }
        ++m.stats.voices_stolen;
        m.voices[victim].alive = false;
        --m.stats.active_voices;
        slot = victim;
    }
    Voice& v = m.voices[slot];
    v.alive = true;
    ++v.generation;
    if (v.generation == 0) v.generation = 1;
    v.p = params;
    v.pos = 0.0;
    v.ramped_once = false;
    v.lp = 0.0f;
    v.lp2 = 0.0f;
    v.order = ++m.order;
    v.audibility = audibility;
    ++m.stats.active_voices;
    ++m.stats.voices_started;
    return VoiceId{static_cast<std::uint32_t>(slot + 1), v.generation};
}

bool AudioMixer::stop(VoiceId id) {
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->alive = false;
    --impl_->stats.active_voices;
    return true;
}
bool AudioMixer::playing(VoiceId id) const { return impl_->find(id) != nullptr; }

bool AudioMixer::set_voice_gain(VoiceId id, float gain) {
    if (!fin(gain) || gain < 0.0f) throw std::invalid_argument("set_voice_gain: the gain must be finite and non-negative");
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->p.gain = gain;
    v->audibility = impl_->estimate_audibility(v->p);
    return true;
}
bool AudioMixer::set_voice_pitch(VoiceId id, float pitch) {
    if (!fin(pitch) || !(pitch > 0.0f)) throw std::invalid_argument("set_voice_pitch: the pitch must be finite and positive");
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->p.pitch = pitch;
    return true;
}
bool AudioMixer::set_voice_transform(VoiceId id, Vec3 position, Vec3 velocity) {
    if (!fin(position) || !fin(velocity)) throw std::invalid_argument("set_voice_transform: position and velocity must be finite");
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->p.position = position;
    v->p.velocity = velocity;
    v->audibility = impl_->estimate_audibility(v->p);
    return true;
}
bool AudioMixer::set_voice_occlusion(VoiceId id, float occlusion) {
    if (!fin(occlusion) || occlusion < 0.0f || occlusion > 1.0f) throw std::invalid_argument("set_voice_occlusion: the occlusion must be within 0..1");
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->p.occlusion = occlusion;
    v->audibility = impl_->estimate_audibility(v->p);
    return true;
}
bool AudioMixer::set_voice_reverb_send(VoiceId id, float send) {
    if (!fin(send) || send < 0.0f || send > 1.0f) throw std::invalid_argument("set_voice_reverb_send: the send must be within 0..1");
    Voice* v = impl_->find(id);
    if (!v) return false;
    v->p.reverb_send = send;
    return true;
}

void AudioMixer::set_listener(const Listener& l) {
    if (!fin(l.position) || !fin(l.forward) || !fin(l.up) || !fin(l.velocity)) throw std::invalid_argument("set_listener: values must be finite");
    const float fl = length(l.forward), ul = length(l.up);
    if (!(fl > 1e-6f) || !(ul > 1e-6f)) throw std::invalid_argument("set_listener: forward and up must be non-zero");
    const Vec3 f = scaled(l.forward, 1.0f / fl), u = scaled(l.up, 1.0f / ul);
    const Vec3 r = cross(f, u);
    const float rl = length(r);
    if (!(rl > 1e-4f)) throw std::invalid_argument("set_listener: forward and up must not be parallel");
    impl_->listener = l;
    impl_->right = scaled(r, 1.0f / rl);
}

void AudioMixer::set_bus_gain(AudioBus bus, float gain) {
    if (!fin(gain) || gain < 0.0f) throw std::invalid_argument("set_bus_gain: the gain must be finite and non-negative");
    impl_->bus_gain[static_cast<std::size_t>(bus)] = gain;
}
void AudioMixer::set_master_gain(float gain) {
    if (!fin(gain) || gain < 0.0f) throw std::invalid_argument("set_master_gain: the gain must be finite and non-negative");
    impl_->master = gain;
}
void AudioMixer::set_reverb(const ReverbSettings& r) {
    if (!fin(r.room_size) || r.room_size < 0.0f || r.room_size > 1.0f || !fin(r.damping) || r.damping < 0.0f || r.damping > 1.0f || !fin(r.wet) || r.wet < 0.0f || r.wet > 1.0f) {
        throw std::invalid_argument("set_reverb: room size, damping and wet must be within 0..1");
    }
    impl_->reverb_settings = r;
    impl_->reverb.set(r.room_size, r.damping);
}
void AudioMixer::clear_ducking() { impl_->ducks.clear(); }
void AudioMixer::add_ducking(const DuckRule& rule) {
    if (rule.source == rule.target) throw std::invalid_argument("add_ducking: a bus cannot duck itself");
    if (!fin(rule.threshold) || rule.threshold < 0.0f || !fin(rule.amount) || rule.amount < 0.0f || rule.amount > 1.0f || !fin(rule.attack) || !(rule.attack > 0.0f) || !fin(rule.release) ||
        !(rule.release > 0.0f)) {
        throw std::invalid_argument("add_ducking: threshold >= 0, amount within 0..1 and positive attack and release are required");
    }
    impl_->ducks.push_back(DuckState{rule, 0.0f});
}

void AudioMixer::render(float* out, std::uint32_t frames) {
    Impl& m = *impl_;
    std::uint32_t done = 0;
    while (done < frames) {
        if (m.chunk_pos >= N) m.process_chunk();
        const std::uint32_t take = std::min(frames - done, N - m.chunk_pos);
        for (std::uint32_t i = 0; i < take; ++i) {
            out[2 * (done + i)] = m.chunk[2 * (m.chunk_pos + i)];
            out[2 * (done + i) + 1] = m.chunk[2 * (m.chunk_pos + i) + 1];
        }
        m.chunk_pos += take;
        done += take;
    }
    m.stats.frames_rendered += frames;
}

AudioStats AudioMixer::stats() const { return impl_->stats; }
const AudioMixerConfig& AudioMixer::config() const { return impl_->cfg; }
float AudioMixer::bus_level(AudioBus bus) const { return impl_->bus_level[static_cast<std::size_t>(bus)]; }
float AudioMixer::duck_gain(AudioBus bus) const { return impl_->bus_duck[static_cast<std::size_t>(bus)]; }

AudioRuntime::AudioRuntime(std::unique_ptr<AudioOutput> output, const AudioMixerConfig& config) : output_(std::move(output)), mixer_(config) {
    if (!output_) throw std::invalid_argument("AudioRuntime: the output is null");
}
AudioRuntime::~AudioRuntime() { stop(); }

bool AudioRuntime::start() {
    if (running_) return true;
    error_.clear();
    if (!output_->start(mixer_.config().sample_rate, &AudioRuntime::callback, this)) {
        error_ = output_->error();
        return false;
    }
    if (output_->sample_rate() != mixer_.config().sample_rate) {
        error_ = "the device runs at " + std::to_string(output_->sample_rate()) + " Hz, not the mixer's " + std::to_string(mixer_.config().sample_rate) + " Hz";
        output_->stop();
        return false;
    }
    running_ = true;
    return true;
}

void AudioRuntime::stop() {
    if (!running_) return;
    output_->stop();  // returns once the device thread is done: no callback runs after this
    running_ = false;
}

AudioRuntime::Access AudioRuntime::access() { return Access{std::unique_lock<std::mutex>(mutex_), mixer_}; }

void AudioRuntime::callback(void* self, float* out, std::uint32_t frames) {
    auto* rt = static_cast<AudioRuntime*>(self);
    std::unique_lock<std::mutex> lock(rt->mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::fill(out, out + 2 * static_cast<std::size_t>(frames), 0.0f);
        rt->skipped_.fetch_add(1);
        return;
    }
    rt->mixer_.render(out, frames);
    rt->rendered_.fetch_add(1);
}

} // namespace saige

// The audio mixer: distance attenuation, panning, doppler, occlusion, reverb, ducking, voice limits and stealing, lifetimes, and determinism independent of how rendering is split. Everything is rendered
// offline and measured (RMS, zero-crossing frequency, tail energy), so these run on any machine without a sound device.
#include "saige/audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

constexpr float kPi = 3.14159265358979f;
constexpr std::uint32_t kRate = 48000;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

AudioClip sine(float hz, float seconds, float amp = 0.5f) {
    AudioClip c;
    c.sample_rate = kRate;
    c.samples.resize(static_cast<std::size_t>(seconds * static_cast<float>(kRate)));
    for (std::size_t i = 0; i < c.samples.size(); ++i) c.samples[i] = amp * std::sin(2.0f * kPi * hz * static_cast<float>(i) / static_cast<float>(kRate));
    return c;
}

AudioMixerConfig dry_config() {
    AudioMixerConfig c;
    c.sample_rate = kRate;
    c.air_absorption_distance = 0.0f;  // isolate the effect under test
    return c;
}

struct Rendered {
    std::vector<float> lr;  // interleaved
    [[nodiscard]] std::size_t frames() const { return lr.size() / 2; }
};

Rendered render(AudioMixer& m, float seconds) {
    Rendered r;
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds * static_cast<float>(kRate));
    r.lr.assign(2 * static_cast<std::size_t>(frames), 0.0f);
    m.render(r.lr.data(), frames);
    return r;
}

// RMS of one channel (0 left, 1 right, 2 both) over [from, to) seconds.
double rms(const Rendered& r, int channel, float from, float to) {
    const std::size_t a = static_cast<std::size_t>(from * static_cast<float>(kRate)), b = std::min(r.frames(), static_cast<std::size_t>(to * static_cast<float>(kRate)));
    double e = 0.0;
    std::size_t n = 0;
    for (std::size_t i = a; i < b; ++i) {
        if (channel != 1) {
            e += static_cast<double>(r.lr[2 * i]) * static_cast<double>(r.lr[2 * i]);
            ++n;
        }
        if (channel != 0) {
            e += static_cast<double>(r.lr[2 * i + 1]) * static_cast<double>(r.lr[2 * i + 1]);
            ++n;
        }
    }
    return n ? std::sqrt(e / static_cast<double>(n)) : 0.0;
}

double frequency(const Rendered& r, float from, float to) {  // rising zero crossings of the left channel (the source is a pure tone)
    const std::size_t a = static_cast<std::size_t>(from * static_cast<float>(kRate)), b = std::min(r.frames(), static_cast<std::size_t>(to * static_cast<float>(kRate)));
    std::size_t first = 0, last = 0;
    int count = 0;
    for (std::size_t i = a + 1; i < b; ++i) {
        const float p = r.lr[2 * (i - 1)], q = r.lr[2 * i];
        if (p < 0.0f && q >= 0.0f) {
            const double t = static_cast<double>(i - 1) + static_cast<double>(-p / (q - p));
            if (count == 0) first = static_cast<std::size_t>(t * 1024.0);
            last = static_cast<std::size_t>(t * 1024.0);
            ++count;
        }
    }
    require(count > 10, "no tone to measure");
    return static_cast<double>(count - 1) / (static_cast<double>(last - first) / 1024.0 / static_cast<double>(kRate));
}

VoiceParams tone_at(ClipId clip, Vec3 pos) {
    VoiceParams p;
    p.clip = clip;
    p.loop = true;
    p.position = pos;
    return p;
}

void test_attenuation() {
    auto level_at = [](Rolloff model, float rolloff, float dist) {
        AudioMixer m(dry_config());
        const ClipId c = m.add_clip(sine(440.0f, 1.0f));
        VoiceParams p = tone_at(c, {0, 0, -dist});
        p.model = model;
        p.rolloff = rolloff;
        p.max_distance = 50.0f;
        (void)m.play(p);
        return rms(render(m, 0.3f), 2, 0.1f, 0.3f);
    };
    const double d1 = level_at(Rolloff::Inverse, 1.0f, 1.0f), d2 = level_at(Rolloff::Inverse, 1.0f, 2.0f), d4 = level_at(Rolloff::Inverse, 1.0f, 4.0f);
    std::printf("inverse: %.4f %.4f %.4f\n", d1, d2, d4);
    require(std::fabs(d2 / d1 - 0.5) < 0.02 && std::fabs(d4 / d1 - 0.25) < 0.02, "the inverse model loses 6 dB per doubling of distance");
    // Inside the minimum distance the gain does not rise further; beyond the maximum it does not fall further (inverse) or is silent (linear).
    require(std::fabs(level_at(Rolloff::Inverse, 1.0f, 0.2f) / d1 - 1.0) < 0.02, "no gain boost inside the minimum distance");
    require(std::fabs(level_at(Rolloff::Inverse, 1.0f, 50.0f) / level_at(Rolloff::Inverse, 1.0f, 200.0f) - 1.0) < 0.02, "no further attenuation beyond the maximum distance");
    require(level_at(Rolloff::Linear, 1.0f, 50.0f) < 1e-6 && std::fabs(level_at(Rolloff::Linear, 1.0f, 25.5f) / d1 - 0.5) < 0.02, "linear: silent at the maximum, half way at half");
    require(std::fabs(level_at(Rolloff::Exponential, 2.0f, 2.0f) / d1 - 0.25) < 0.02, "exponential with exponent 2: a quarter at double the distance");
    // The rolloff factor steepens the curve: at factor 2 and double the distance the inverse model gives 1 / (1 + 2) of the level at the minimum distance.
    require(std::fabs(level_at(Rolloff::Inverse, 2.0f, 2.0f) / d1 - 1.0 / 3.0) < 0.02 && std::fabs(level_at(Rolloff::Inverse, 0.0f, 20.0f) / d1 - 1.0) < 0.02, "the rolloff factor scales the inverse curve (0 means none)");
    // A non-spatial voice does not care where it "is".
    AudioMixer m(dry_config());
    VoiceParams p = tone_at(m.add_clip(sine(440.0f, 1.0f)), {0, 0, -500.0f});
    p.spatial = false;
    (void)m.play(p);
    const Rendered r = render(m, 0.3f);
    require(std::fabs(rms(r, 0, 0.1f, 0.3f) - rms(r, 1, 0.1f, 0.3f)) < 1e-4 && rms(r, 2, 0.1f, 0.3f) > 0.2, "a non-spatial voice is centred at full level");
}

void test_panning() {
    // Equal power across a half circle in front of the listener: left^2 + right^2 stays constant, left dominates on the left and right on the right.
    double total_min = 1e9, total_max = 0.0;
    for (int deg = -90; deg <= 90; deg += 15) {
        AudioMixer m(dry_config());
        const float a = static_cast<float>(deg) * kPi / 180.0f;
        VoiceParams p = tone_at(m.add_clip(sine(440.0f, 1.0f)), {4.0f * std::sin(a), 0, -4.0f * std::cos(a)});
        (void)m.play(p);
        const Rendered r = render(m, 0.3f);
        const double l = rms(r, 0, 0.1f, 0.3f), rr = rms(r, 1, 0.1f, 0.3f);
        const double total = l * l + rr * rr;
        total_min = std::min(total_min, total);
        total_max = std::max(total_max, total);
        if (deg == -90) require(l > 20.0 * rr, "a source at the listener's left is heard on the left only");
        if (deg == 90) require(rr > 20.0 * l, "a source at the listener's right is heard on the right only");
        if (deg == 0) require(std::fabs(l - rr) < 1e-4, "straight ahead is centred");
    }
    require(total_max / total_min < 1.02, "equal-power panning: the total power does not change across the arc");
    // Turning the listener turns the scene: a source on the +X axis is centred when the listener faces +X and on the left when it faces +Z.
    auto balance = [](Vec3 forward) {
        AudioMixer m(dry_config());
        Listener l;
        l.forward = forward;
        m.set_listener(l);
        (void)m.play(tone_at(m.add_clip(sine(440.0f, 1.0f)), {5, 0, 0}));
        const Rendered r = render(m, 0.3f);
        return std::pair{rms(r, 0, 0.1f, 0.3f), rms(r, 1, 0.1f, 0.3f)};
    };
    const auto ahead = balance({1, 0, 0});
    require(std::fabs(ahead.first - ahead.second) < 1e-4, "facing the source centres it");
    const auto facing_back = balance({0, 0, 1});
    require(facing_back.first > 20.0 * facing_back.second, "facing +Z puts +X on the left");
    // A source nearer than its minimum distance is not hard-panned (it is "on" the listener).
    AudioMixer m(dry_config());
    VoiceParams p = tone_at(m.add_clip(sine(440.0f, 1.0f)), {0.1f, 0, 0});
    (void)m.play(p);
    const Rendered r = render(m, 0.3f);
    require(rms(r, 0, 0.1f, 0.3f) > 0.5 * rms(r, 1, 0.1f, 0.3f), "a source inside the minimum distance is nearly centred");
}

void test_doppler() {
    auto measure = [](float speed_towards, float doppler_scale) {
        AudioMixerConfig cfg = dry_config();
        cfg.doppler_scale = doppler_scale;
        AudioMixer m(cfg);
        VoiceParams p = tone_at(m.add_clip(sine(1000.0f, 2.0f)), {0, 0, -30});
        p.velocity = {0, 0, speed_towards};  // +Z is towards a listener at the origin
        (void)m.play(p);
        const Rendered r = render(m, 0.6f);
        return frequency(r, 0.1f, 0.6f);
    };
    const double still = measure(0.0f, 1.0f), towards = measure(34.3f, 1.0f), away = measure(-34.3f, 1.0f), off = measure(34.3f, 0.0f);
    std::printf("doppler: still %.1f, towards %.1f (expect %.1f), away %.1f (expect %.1f), off %.1f\n", still, towards, 1000.0 * 343.0 / (343.0 - 34.3), away, 1000.0 * 343.0 / (343.0 + 34.3), off);
    require(std::fabs(still - 1000.0) < 5.0, "a still source is at its own pitch");
    require(std::fabs(towards - 1000.0 * 343.0 / (343.0 - 34.3)) < 8.0, "a source approaching at a tenth of the speed of sound is 11% sharp");
    require(std::fabs(away - 1000.0 * 343.0 / (343.0 + 34.3)) < 8.0, "a receding source is flat");
    require(std::fabs(off - 1000.0) < 5.0, "doppler off");
    // A moving listener shifts it too.
    AudioMixer m(dry_config());
    Listener l;
    l.velocity = {0, 0, -34.3f};  // towards the source at z = -30
    m.set_listener(l);
    (void)m.play(tone_at(m.add_clip(sine(1000.0f, 2.0f)), {0, 0, -30}));
    require(std::fabs(frequency(render(m, 0.6f), 0.1f, 0.6f) - 1000.0 * (343.0 + 34.3) / 343.0) < 8.0, "a listener moving towards a source hears it sharp");
    // Pitch control: playback rate 2 is an octave up.
    AudioMixer m2(dry_config());
    VoiceParams p = tone_at(m2.add_clip(sine(500.0f, 2.0f)), {0, 0, -2});
    p.pitch = 2.0f;
    (void)m2.play(p);
    require(std::fabs(frequency(render(m2, 0.6f), 0.1f, 0.6f) - 1000.0) < 5.0, "pitch 2 plays an octave up");
}

void test_occlusion() {
    auto level = [](float hz, float occlusion) {
        AudioMixer m(dry_config());
        VoiceParams p = tone_at(m.add_clip(sine(hz, 1.0f)), {0, 0, -1});
        p.occlusion = occlusion;
        (void)m.play(p);
        return rms(render(m, 0.4f), 2, 0.2f, 0.4f);
    };
    const double low_open = level(200.0f, 0.0f), low_blocked = level(200.0f, 1.0f), high_open = level(8000.0f, 0.0f), high_blocked = level(8000.0f, 1.0f), high_half = level(8000.0f, 0.5f);
    std::printf("occlusion: 200 Hz %.3f -> %.3f, 8 kHz %.3f -> %.3f (half %.3f)\n", low_open, low_blocked, high_open, high_blocked, high_half);
    require(low_blocked / low_open > 0.30 && low_blocked / low_open < 0.37, "a blocked low tone keeps about the occluded gain (0.35)");
    require(high_blocked / high_open < 0.01, "a blocked high tone is almost gone (12 dB per octave lowpass at 800 Hz: 0.35 x 1/101)");
    require(high_half < high_open && high_half > high_blocked, "half occlusion is half way");
    // Occlusion changes smoothly when it is updated live (no jump in one chunk beyond the ramp).
    AudioMixer m(dry_config());
    VoiceParams p = tone_at(m.add_clip(sine(200.0f, 2.0f)), {0, 0, -1});
    const VoiceId v = m.play(p);
    (void)render(m, 0.1f);
    require(m.set_voice_occlusion(v, 1.0f), "live occlusion update");
    const Rendered r = render(m, 0.1f);
    double worst = 0.0;
    for (std::size_t i = 1; i < r.frames(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(r.lr[2 * i] - r.lr[2 * (i - 1)])));
    require(worst < 0.05, "no click when occlusion changes");
}

void test_reverb() {
    auto tail = [](float send, float room, float wet, float from, float to) {
        AudioMixer m(dry_config());
        AudioClip click;
        click.sample_rate = kRate;
        click.samples.assign(48, 0.0f);
        click.samples[0] = 1.0f;
        VoiceParams p;
        p.clip = m.add_clip(click);
        p.position = {0, 0, -2};
        p.reverb_send = send;
        ReverbSettings rv;
        rv.room_size = room;
        rv.wet = wet;
        m.set_reverb(rv);
        (void)m.play(p);
        return rms(render(m, 2.5f), 2, from, to);
    };
    const double dry = tail(0.0f, 0.8f, 1.0f, 0.3f, 2.0f), wet_off = tail(1.0f, 0.8f, 0.0f, 0.3f, 2.0f);
    require(dry == 0.0 && wet_off == 0.0, "no send, or no return, leaves silence after the click");
    const double early = tail(1.0f, 0.8f, 1.0f, 0.1f, 0.4f), late = tail(1.0f, 0.8f, 1.0f, 1.5f, 2.0f);
    const double small_late = tail(1.0f, 0.1f, 1.0f, 1.5f, 2.0f);
    std::printf("reverb: early %.5f late %.5f, small room late %.5f\n", early, late, small_late);
    require(early > 1e-4, "the click leaves a reverberant tail");
    require(late < early && late > 0.0, "the tail decays");
    require(small_late < 0.5 * late, "a small room's tail dies sooner than a big room's");
    // The send scales the tail.
    require(std::fabs(tail(0.5f, 0.8f, 1.0f, 0.1f, 0.4f) / early - 0.5) < 0.05, "half the send is half the tail");
}

void test_ducking_and_buses() {
    AudioMixer m(dry_config());
    DuckRule rule;  // dialogue ducks ambience by 60%
    m.add_ducking(rule);
    VoiceParams amb = tone_at(m.add_clip(sine(200.0f, 1.0f)), {0, 0, -1});
    amb.bus = AudioBus::Ambience;
    (void)m.play(amb);
    VoiceParams talk = tone_at(m.add_clip(sine(800.0f, 1.0f)), {0, 0, -1});
    talk.bus = AudioBus::Dialogue;
    talk.loop = false;
    talk.spatial = false;
    (void)render(m, 0.3f);
    require(m.duck_gain(AudioBus::Ambience) == 1.0f, "ambience is not ducked while nobody speaks");
    const VoiceId t = m.play(talk);
    (void)render(m, 0.02f);
    require(m.duck_gain(AudioBus::Ambience) < 1.0f && m.duck_gain(AudioBus::Ambience) > 0.8f, "ducking begins within the attack time, not instantly");
    (void)render(m, 0.58f);
    std::printf("duck gain while speaking: %.3f\n", static_cast<double>(m.duck_gain(AudioBus::Ambience)));
    require(m.duck_gain(AudioBus::Ambience) < 0.45f, "ambience is pulled down by 60% while dialogue plays");
    require(m.duck_gain(AudioBus::Dialogue) == 1.0f, "the speaker is not ducked");
    require(m.stop(t), "stop the dialogue");
    (void)render(m, 0.1f);
    require(m.duck_gain(AudioBus::Ambience) < 0.7f, "ducking releases over the release time, not instantly");
    (void)render(m, 2.0f);
    require(m.duck_gain(AudioBus::Ambience) > 0.97f, "ambience recovers when the dialogue ends");

    // Bus and master gains.
    AudioMixer b(dry_config());
    VoiceParams a = tone_at(b.add_clip(sine(440.0f, 1.0f)), {0, 0, -1});
    a.bus = AudioBus::Water;
    (void)b.play(a);
    const double full = rms(render(b, 0.3f), 2, 0.1f, 0.3f);
    b.set_bus_gain(AudioBus::Water, 0.5f);
    (void)render(b, 0.05f);
    require(std::fabs(rms(render(b, 0.3f), 2, 0.1f, 0.3f) / full - 0.5) < 0.02, "a bus at half gain halves its voices");
    b.set_bus_gain(AudioBus::Water, 0.0f);
    (void)render(b, 0.05f);
    require(rms(render(b, 0.2f), 2, 0.0f, 0.2f) < 1e-6, "a muted bus is silent");
    b.set_bus_gain(AudioBus::Water, 1.0f);
    b.set_master_gain(0.25f);
    (void)render(b, 0.05f);
    require(std::fabs(rms(render(b, 0.3f), 2, 0.1f, 0.3f) / full - 0.25) < 0.02, "the master gain scales everything");
    // A gain change is ramped across the chunk: from full to nothing in one call is a ramp of 64 frames, never a step.
    AudioMixer rm(dry_config());
    AudioClip dc;
    dc.sample_rate = kRate;
    dc.samples.assign(48000, 0.5f);
    VoiceParams dcp = tone_at(rm.add_clip(dc), {});
    dcp.spatial = false;
    const VoiceId dcv = rm.play(dcp);
    (void)render(rm, 0.05f);
    require(rm.set_voice_gain(dcv, 0.0f), "gain to zero");
    const Rendered ramp = render(rm, 0.05f);
    float step = 0.0f;
    for (std::size_t i = 1; i < ramp.frames(); ++i) step = std::max(step, std::fabs(ramp.lr[2 * i] - ramp.lr[2 * (i - 1)]));
    require(step < 0.012f && std::fabs(ramp.lr[2 * 200]) < 1e-4f, "a gain change ramps over a chunk (no click) and reaches its target");
    // Clipping is counted and the output is limited to [-1, 1].
    AudioMixer c(dry_config());
    VoiceParams loud = tone_at(c.add_clip(sine(440.0f, 1.0f, 1.0f)), {0, 0, -1});
    loud.gain = 4.0f;
    (void)c.play(loud);
    const Rendered r = render(c, 0.2f);
    float peak = 0.0f;
    for (const float s : r.lr) peak = std::max(peak, std::fabs(s));
    std::uint64_t at_limit = 0;
    for (const float s : r.lr) at_limit += std::fabs(s) >= 1.0f ? 1u : 0u;
    require(peak <= 1.0f && at_limit > 100 && c.stats().clipped_samples == at_limit, "output is clamped and every clipped sample (both channels) is counted");
}

void test_voices() {
    AudioMixerConfig cfg = dry_config();
    cfg.max_voices = 4;
    AudioMixer m(cfg);
    const ClipId c = m.add_clip(sine(300.0f, 5.0f));
    VoiceParams base = tone_at(c, {0, 0, -5});
    base.priority = 10;
    VoiceId low[4];
    for (VoiceId& v : low) {
        v = m.play(base);
        require(static_cast<bool>(v), "the first four voices start");
    }
    VoiceParams weaker = base;
    weaker.priority = 5;
    require(!m.play(weaker), "a less important voice is refused when full");
    require(m.stats().voices_rejected == 1, "the refusal is counted");
    VoiceParams equal_but_far = base;
    equal_but_far.position = {0, 0, -90};
    require(!m.play(equal_but_far), "an equally important but quieter voice is refused");
    VoiceParams equal_but_near = base;
    equal_but_near.position = {0, 0, -1};
    // Make the existing ones distinct in loudness so the victim is the quietest.
    require(m.set_voice_transform(low[2], {0, 0, -40}, {}), "move a voice away");
    const VoiceId near_id = m.play(equal_but_near);
    require(static_cast<bool>(near_id) && !m.playing(low[2]) && m.playing(low[0]) && m.playing(low[1]) && m.playing(low[3]), "an equal-priority nearer voice replaces the quietest one");
    VoiceParams boss = base;
    boss.priority = 200;
    boss.position = {0, 0, -99};
    const VoiceId boss_id = m.play(boss);
    require(static_cast<bool>(boss_id), "a higher priority wins even when quieter");
    require(m.stats().voices_stolen == 2 && m.stats().active_voices == 4, "two voices were stolen and four are active");
    // Priority outranks loudness when choosing the victim: with two important quiet voices and two unimportant loud ones, the newcomer takes an unimportant one.
    AudioMixer pm(cfg);
    const ClipId pc = pm.add_clip(sine(300.0f, 5.0f));
    VoiceParams pa = tone_at(pc, {0, 0, -1});
    pa.priority = 10;  // unimportant but close and loud
    VoiceParams pb = tone_at(pc, {0, 0, -50});
    pb.priority = 50;  // important but far and quiet
    const VoiceId ids[4] = {pm.play(pa), pm.play(pb), pm.play(pa), pm.play(pb)};
    VoiceParams pn = tone_at(pc, {0, 0, -90});
    pn.priority = 60;
    require(static_cast<bool>(pm.play(pn)), "a priority 60 voice starts");
    require((!pm.playing(ids[0]) || !pm.playing(ids[2])) && pm.playing(ids[1]) && pm.playing(ids[3]), "the victim is an unimportant voice, not a quiet important one");
    // A stolen voice's id is dead for every call.
    require(!m.stop(low[2]) && !m.set_voice_gain(low[2], 1.0f) && !m.set_voice_pitch(low[2], 1.0f) && !m.set_voice_transform(low[2], {}, {}) && !m.set_voice_occlusion(low[2], 0.0f) &&
                !m.set_voice_reverb_send(low[2], 0.0f),
            "a stolen id is refused everywhere");
    // One-shots free themselves, and the slot's next voice gets a new generation.
    AudioMixer s(dry_config());
    VoiceParams shot;
    shot.clip = s.add_clip(sine(440.0f, 0.02f));
    shot.position = {0, 0, -1};
    const VoiceId first = s.play(shot);
    require(s.playing(first), "playing");
    (void)render(s, 0.05f);
    require(!s.playing(first) && s.stats().active_voices == 0, "a finished one-shot is gone");
    const VoiceId second = s.play(shot);
    require(second.index == first.index && second.generation != first.generation && !s.playing(first) && s.playing(second), "slot reuse gives a new generation and the old id stays dead");
    // A pitch of 2 finishes twice as fast.
    AudioMixer p(dry_config());
    VoiceParams fast = shot;
    fast.clip = p.add_clip(sine(440.0f, 0.1f));
    fast.pitch = 2.0f;
    const VoiceId f = p.play(fast);
    (void)render(p, 0.04f);
    require(p.playing(f), "still playing at 0.04 s");
    (void)render(p, 0.02f);
    require(!p.playing(f), "finished by 0.06 s at double speed");
    // Looping is seamless: a looped whole-cycle tone is as steady as the original.
    AudioMixer l(dry_config());
    AudioClip cycle;
    cycle.sample_rate = kRate;
    for (int i = 0; i < 480; ++i) cycle.samples.push_back(0.5f * std::sin(2.0f * kPi * static_cast<float>(i) / 480.0f));
    VoiceParams lp = tone_at(l.add_clip(cycle), {0, 0, -1});
    lp.spatial = false;
    (void)l.play(lp);
    const Rendered r = render(l, 0.5f);
    float worst = 0.0f;
    for (std::size_t i = 1; i < r.frames(); ++i) worst = std::max(worst, std::fabs(r.lr[2 * i] - r.lr[2 * (i - 1)]));
    require(worst < 0.5f * 2.0f * kPi / 480.0f * 0.7072f * 1.02f, "the loop point is seamless");
}

void test_determinism() {
    auto scene = [](std::vector<std::uint32_t> splits) {
        AudioMixerConfig cfg = dry_config();
        cfg.air_absorption_distance = 300.0f;
        AudioMixer m(cfg);
        ReverbSettings rv;
        rv.wet = 0.3f;
        m.set_reverb(rv);
        m.add_ducking(DuckRule{});
        const ClipId a = m.add_clip(sine(220.0f, 1.0f)), b = m.add_clip(sine(990.0f, 0.4f));
        VoiceParams v1 = tone_at(a, {5, 0, -8});
        v1.velocity = {-3, 0, 4};
        v1.reverb_send = 0.4f;
        v1.occlusion = 0.3f;
        (void)m.play(v1);
        VoiceParams v2 = tone_at(b, {-2, 1, -3});
        v2.loop = false;
        v2.bus = AudioBus::Dialogue;
        (void)m.play(v2);
        std::vector<float> out(2 * 20000);
        std::size_t at = 0;
        std::size_t k = 0;
        while (at < 20000) {
            const std::uint32_t n = std::min<std::uint32_t>(splits[k++ % splits.size()], static_cast<std::uint32_t>(20000 - at));
            m.render(out.data() + 2 * at, n);
            at += n;
        }
        return out;
    };
    const auto whole = scene({20000}), chunks = scene({64}), odd = scene({7, 129, 1, 1000, 63}), again = scene({20000});
    require(std::memcmp(whole.data(), again.data(), whole.size() * sizeof(float)) == 0, "the same scene renders to the same bits twice");
    require(std::memcmp(whole.data(), chunks.data(), whole.size() * sizeof(float)) == 0, "splitting the render at chunk size changes nothing");
    require(std::memcmp(whole.data(), odd.data(), whole.size() * sizeof(float)) == 0, "splitting the render at odd sizes changes nothing");
    double e = 0.0;
    for (const float s : whole) e += static_cast<double>(s) * static_cast<double>(s);
    require(e > 1.0, "and the scene is not silent");
}

void test_validation() {
    auto throws = [](auto&& fn) {
        try {
            fn();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    AudioMixer m(dry_config());
    const ClipId c = m.add_clip(sine(440.0f, 0.1f));
    VoiceParams ok = tone_at(c, {});
    require(throws([] { AudioMixerConfig bad; bad.sample_rate = 100; AudioMixer x(bad); }), "tiny sample rate");
    require(throws([] { AudioMixerConfig bad; bad.max_voices = 0; AudioMixer x(bad); }), "zero voices");
    require(throws([&] { (void)m.add_clip(AudioClip{}); }), "empty clip");
    require(throws([&] { AudioClip x = sine(1.0f, 0.01f); x.samples[3] = NAN; (void)m.add_clip(x); }), "NaN sample");
    require(throws([&] { VoiceParams p = ok; p.clip = ClipId{99}; (void)m.play(p); }), "unknown clip");
    require(throws([&] { VoiceParams p = ok; p.gain = -1.0f; (void)m.play(p); }), "negative gain");
    require(throws([&] { VoiceParams p = ok; p.pitch = 0.0f; (void)m.play(p); }), "zero pitch");
    require(throws([&] { VoiceParams p = ok; p.position.x = INFINITY; (void)m.play(p); }), "infinite position");
    require(throws([&] { VoiceParams p = ok; p.max_distance = 0.5f; (void)m.play(p); }), "max below min distance");
    require(throws([&] { VoiceParams p = ok; p.occlusion = 1.5f; (void)m.play(p); }), "occlusion above 1");
    require(throws([&] { Listener l; l.up = {0, 0, -1}; m.set_listener(l); }), "up parallel to forward");
    require(throws([&] { Listener l; l.forward = {}; m.set_listener(l); }), "zero forward");
    require(throws([&] { m.set_bus_gain(AudioBus::Music, -1.0f); }), "negative bus gain");
    require(throws([&] { ReverbSettings r; r.wet = 2.0f; m.set_reverb(r); }), "reverb out of range");
    require(throws([&] { DuckRule r; r.target = r.source; m.add_ducking(r); }), "a bus ducking itself");
    require(throws([&] { const VoiceId v = m.play(ok); (void)m.set_voice_gain(v, NAN); }), "NaN voice gain");
    // A rejected play leaves the mixer untouched.
    require(m.stats().active_voices == 1, "only the one valid voice started");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"attenuation", test_attenuation}, {"panning", test_panning}, {"doppler", test_doppler}, {"occlusion", test_occlusion},
                           {"reverb", test_reverb},           {"ducking", test_ducking_and_buses}, {"voices", test_voices}, {"determinism", test_determinism}, {"validation", test_validation}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "audio tests passed\n";
    return failures == 0 ? 0 : 1;
}

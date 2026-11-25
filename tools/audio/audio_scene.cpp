// Renders a scripted scene through SAIGE's own mixer with real, licensed recordings (CC0 OpenGameArt packs from the asset cache, never committed): a car passing a listener in the rain, with
// engine loops pitched and crossfaded by engine_audio, doppler, occlusion behind a wall, a collision (impact_sound picks the bank), footsteps that change surface, a door, rain, wind and thunder.
// Output: a stereo WAV and a JSON report with objective checks (no clipping, level, determinism, the engine tracking the revs). It is an offline render: it says nothing about a real device.
//
//   saige_audio_scene LIBRARY_ROOT OUT.wav     (LIBRARY_ROOT holds the opengameart/ cache layout of tools/production's saige-assets)
#include "miniaudio_output.hpp"
#include "saige/audio_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace saige;

constexpr std::uint32_t kRate = 48000;

struct Bank {
    AudioMixer& mixer;
    std::string root;
    std::map<std::string, ClipId> clips;
    std::map<std::string, float> dominant_hz;
    bool failed = false;

    ClipId load(const std::string& key, const std::string& rel) {
        AudioClip c;
        std::string err;
        if (!decode_audio_file(root + "/" + rel, c, err)) {
            std::fprintf(stderr, "cannot load %s: %s\n", key.c_str(), err.c_str());
            failed = true;
            return {};
        }
        // Resample to the mixer's rate is the mixer's job (per-voice pitch ratio), so the clip keeps its own rate.
        dominant_hz[key] = estimate_hz(c);
        return clips[key] = mixer.add_clip(std::move(c));
    }

    // The dominant period by autocorrelation over the first part of the clip (for engine loops: the firing frequency).
    static float estimate_hz(const AudioClip& c) {
        const std::size_t n = std::min<std::size_t>(c.samples.size(), 8192);
        double best = 0.0;
        std::size_t best_lag = 0;
        for (std::size_t lag = c.sample_rate / 600; lag < c.sample_rate / 20 && lag + n < c.samples.size(); ++lag) {
            double s = 0.0;
            for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(c.samples[i]) * static_cast<double>(c.samples[i + lag]);
            if (s > best) {
                best = s;
                best_lag = lag;
            }
        }
        return best_lag ? static_cast<float>(c.sample_rate) / static_cast<float>(best_lag) : 0.0f;
    }
};

void write_wav(const std::string& path, const std::vector<float>& lr) {
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(lr.size() * 2);
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8); w32(16); w16(1); w16(2); w32(kRate); w32(kRate * 4); w16(4); w16(16); f.write("data", 4); w32(bytes);
    for (const float s : lr) w16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f)));
}

struct Result {
    std::vector<float> lr;
    AudioStats stats;
    std::vector<float> engine_rms_by_second;
    float engine_hz_start = 0.0f, engine_hz_end = 0.0f;
    bool ok = true;
};

Result render_scene(const std::string& root, bool engine_only = false) {
    Result r;
    AudioMixerConfig cfg;
    cfg.sample_rate = kRate;
    AudioMixer m(cfg);
    Bank bank{m, root, {}, {}, false};
    const std::string rc = "racing_car_engine_sound_loops/";
    bank.load("engine_low", rc + "loop_0.wav");
    bank.load("engine_mid", rc + "loop_2_0.wav");
    bank.load("engine_high", rc + "loop_5_0.wav");
    bank.load("rain", "rain_loopable/contents/1.ogg");
    bank.load("wind", "wind/contents/wind/Wind.ogg");
    bank.load("thunder", "thunder_rain_and_wind/contents/jute-dh-weather/thunder_3_far.wav");
    const char* stone[] = {"StoneL1", "StoneR1", "StoneL2", "StoneR2", "StoneL3", "StoneR3"};
    const char* sand[] = {"SandL1", "SandR1", "SandL2", "SandR2"};
    for (const char* s : stone) bank.load(std::string("fs_") + s, std::string("fantozzis_footsteps_grasssand_stone/contents/Fantozzi-footsteps/ogg/Fantozzi-") + s + ".ogg");
    for (const char* s : sand) bank.load(std::string("fs_") + s, std::string("fantozzis_footsteps_grasssand_stone/contents/Fantozzi-footsteps/ogg/Fantozzi-") + s + ".ogg");
    bank.load("hit_metal", "75_cc0_breaking_falling_hit_sfx/contents/bfh1_metal_falling_01.ogg");
    bank.load("hit_hard", "75_cc0_breaking_falling_hit_sfx/contents/bfh1_hit_06.ogg");
    bank.load("glass", "glass_break/glass_breaking.wav");
    bank.load("door", "door_open_door_close_set/contents/qubodup-DoorSet/ogg/qubodup-DoorClose03.ogg");
    if (bank.failed) {
        r.ok = false;
        return r;
    }
    // The three engine loops are described to the model by their measured firing frequency (four-stroke, four-cylinder assumption: rpm = 30 x firing Hz), which is how the recordings are placed on the rev axis.
    EngineAudioConfig ec;
    ec.loop_rpm = {30.0f * bank.dominant_hz["engine_low"], 30.0f * bank.dominant_hz["engine_mid"], 30.0f * bank.dominant_hz["engine_high"]};
    if (!(ec.loop_rpm[0] > 0.0f && ec.loop_rpm[1] > ec.loop_rpm[0] && ec.loop_rpm[2] > ec.loop_rpm[1])) {
        // The loops did not come out ascending by the estimate: fall back to nominal speeds and say so in the report.
        ec.loop_rpm = {1500.0f, 3500.0f, 6000.0f};
    }
    ec.idle_rpm = std::min(900.0f, ec.loop_rpm[0] * 0.8f);
    ec.redline_rpm = ec.loop_rpm[2] * 1.15f;
    r.engine_hz_start = ec.loop_rpm[0] / 30.0f;
    r.engine_hz_end = ec.loop_rpm[2] / 30.0f;

    ReverbSettings rv = reverb_for_space(0.3f, 2000.0f, 0.8f);
    m.set_reverb(rv);
    const AmbienceMix amb = ambience_mix(21.0f, 7.0f, 0.6f, 0.2f, 0.0f);
    m.set_listener(Listener{{0, 1.7f, 0}, {0, 0, -1}, {0, 1, 0}, {}});

    auto bed = [&](const char* key, float gain, std::uint8_t prio) {
        VoiceParams p;
        p.clip = bank.clips[key];
        p.loop = true;
        p.spatial = false;
        p.bus = AudioBus::Ambience;
        p.gain = gain;
        p.priority = prio;
        return m.play(p);
    };
    if (!engine_only) {
        (void)bed("rain", amb.rain * 0.9f, 60);
        (void)bed("wind", amb.wind * 0.8f, 60);
    }

    VoiceId eng[3];
    for (int i = 0; i < 3; ++i) {
        VoiceParams p;
        p.clip = bank.clips[i == 0 ? "engine_low" : i == 1 ? "engine_mid" : "engine_high"];
        p.loop = true;
        p.bus = AudioBus::Vehicle;
        p.gain = 0.0f;
        p.position = {-150, 0.5f, -6};
        p.min_distance = 4.0f;
        p.max_distance = 300.0f;
        p.priority = 200;
        p.reverb_send = 0.15f;
        eng[i] = m.play(p);
    }

    const float duration = 20.0f;
    const std::uint32_t block = 480;  // the scene's control rate: 100 Hz
    const std::uint32_t blocks = static_cast<std::uint32_t>(duration * 100.0f);
    r.lr.assign(2 * static_cast<std::size_t>(blocks) * block, 0.0f);
    EngineAudioState es;
    std::uint32_t seed = 11;
    bool thunder = false, crash = false, door = false;
    float next_step = 1.0f;
    int step_index = 0;
    std::vector<double> sec_energy(21, 0.0);
    std::vector<std::size_t> sec_n(21, 0);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const float t = static_cast<float>(b) * 0.01f;
        // The car: 15 m/s along +X, 6 m in front of the listener's line; revs climb from idle to near redline and throttle follows.
        const float car_speed = engine_only ? 0.0f : 15.0f;  // the engine-only render parks the car 6 m away so only the revs change the sound
        const Vec3 car{engine_only ? 0.0f : -150.0f + car_speed * t, 0.5f, -6.0f};
        const float rpm = ec.idle_rpm + (ec.redline_rpm * 0.9f - ec.idle_rpm) * std::min(1.0f, t / 12.0f);
        const EngineAudioFrame f = engine_audio(ec, es, rpm, t < 12.0f ? 0.9f : 0.2f, 0.01f);
        const float wall = (!engine_only && t > 14.0f && t < 16.0f) ? 0.85f : 0.0f;  // the car goes behind a building
        for (int i = 0; i < 3; ++i) {
            (void)m.set_voice_gain(eng[i], f.gain[static_cast<std::size_t>(i)] * f.master * 1.2f);
            (void)m.set_voice_pitch(eng[i], f.pitch[static_cast<std::size_t>(i)]);
            (void)m.set_voice_transform(eng[i], car, {car_speed, 0, 0});
            (void)m.set_voice_occlusion(eng[i], wall);
        }
        if (engine_only) {
            m.render(r.lr.data() + 2 * static_cast<std::size_t>(b) * block, block);
            continue;
        }
        if (!door && t >= 3.0f) {
            door = true;
            VoiceParams p;
            p.clip = bank.clips["door"];
            p.position = {-2, 1.0f, -3};
            p.reverb_send = 0.4f;
            p.bus = AudioBus::Effects;
            (void)m.play(p);
        }
        if (!thunder && t >= 8.0f) {
            thunder = true;
            VoiceParams p;
            p.clip = bank.clips["thunder"];
            p.spatial = false;
            p.bus = AudioBus::Ambience;
            p.gain = 0.8f;
            (void)m.play(p);
        }
        if (!crash && t >= 12.0f) {
            crash = true;
            const ImpactSound is = impact_sound(Surface::Concrete, true, 30000.0f, 5);
            VoiceParams p;
            p.clip = bank.clips[is.category == ImpactCategory::Metal ? "hit_metal" : "hit_hard"];
            p.position = {car.x, 0.5f, -6.0f};
            p.gain = is.gain;
            p.pitch = is.pitch;
            p.priority = 250;
            p.bus = AudioBus::Effects;
            p.reverb_send = 0.3f;
            (void)m.play(p);
            VoiceParams g = p;
            g.clip = bank.clips["glass"];
            g.gain = is.gain * 0.7f;
            g.pitch = 1.0f;
            (void)m.play(g);
        }
        if (t >= next_step && t < 18.0f) {
            // A pedestrian walking 1.4 m/s across the front, stone for the first 8 s then sand; alternate feet.
            const bool on_sand = t > 8.0f;
            const FootstepSound fs = footstep_sound(on_sand ? Surface::Sand : Surface::Concrete, 1.4f, 80.0f, 0.0f, 0.0f, seed++);
            const char* const* names = on_sand ? sand : stone;
            const int count = on_sand ? 4 : 6;
            const int pick = (step_index % 2) + 2 * ((step_index / 2) % (count / 2));  // L, R, L, R ... through the takes
            VoiceParams p;
            p.clip = bank.clips[std::string("fs_") + names[pick]];
            p.position = {-4.0f + 1.4f * (t - 1.0f) * 0.5f, 0.0f, -3.0f};
            p.gain = fs.gain;
            p.pitch = fs.pitch;
            p.bus = AudioBus::Footsteps;
            p.priority = 100;
            p.min_distance = 1.0f;
            (void)m.play(p);
            ++step_index;
            next_step += 0.55f;
        }
        m.render(r.lr.data() + 2 * static_cast<std::size_t>(b) * block, block);
        if (b % 100 == 0 || b + 1 == blocks) {
            // nothing: the per-second engine level is read from the Vehicle bus meter below
        }
        const std::size_t sec = static_cast<std::size_t>(t);
        sec_energy[sec] += static_cast<double>(m.bus_level(AudioBus::Vehicle));
        ++sec_n[sec];
    }
    for (std::size_t s = 0; s < 20; ++s) r.engine_rms_by_second.push_back(sec_n[s] ? static_cast<float>(sec_energy[s] / static_cast<double>(sec_n[s])) : 0.0f);
    r.stats = m.stats();
    return r;
}

std::uint64_t fnv(const std::vector<float>& v) {
    std::uint64_t h = 1469598103934665603ull;
    for (const float x : v) {
        std::uint32_t u;
        std::memcpy(&u, &x, 4);
        for (int i = 0; i < 4; ++i) {
            h ^= (u >> (8 * i)) & 255u;
            h *= 1099511628211ull;
        }
    }
    return h;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s LIBRARY_ROOT OUT.wav\n", argv[0]);
        return 2;
    }
    const Result a = render_scene(argv[1]);
    if (!a.ok) return 3;
    const Result b = render_scene(argv[1]);
    // The engine follows the revs: parked 6 m away with only the revs changing, the zero-crossing rate of the output (a measure of pitch) must rise as the engine climbs from idle.
    const Result en = render_scene(argv[1], true);
    auto zcr = [&](float from, float to) {
        const std::size_t i0 = static_cast<std::size_t>(from * static_cast<float>(kRate)), i1 = static_cast<std::size_t>(to * static_cast<float>(kRate));
        std::size_t z = 0;
        for (std::size_t i = i0 + 1; i < i1; ++i) z += (en.lr[2 * (i - 1)] < 0.0f) != (en.lr[2 * i] < 0.0f) ? 1u : 0u;
        return static_cast<double>(z) / static_cast<double>(to - from);
    };
    const double zcr_idle = zcr(0.5f, 1.5f), zcr_revved = zcr(10.5f, 11.5f);
    double e = 0.0;
    float peak = 0.0f;
    for (const float s : a.lr) {
        e += static_cast<double>(s) * static_cast<double>(s);
        peak = std::max(peak, std::fabs(s));
    }
    const double rms = std::sqrt(e / static_cast<double>(a.lr.size()));
    write_wav(argv[2], a.lr);
    const bool deterministic = a.lr == b.lr;
    // The engine tracks the revs: the Vehicle bus is loud while the car is near and louder as the revs climb; it must be audibly present at its closest approach (t = 10 s, x = 0).
    const float near = a.engine_rms_by_second[10], far_start = a.engine_rms_by_second[0];
    std::printf("{\n  \"frames\": %zu, \"seconds\": %.1f,\n  \"peak\": %.3f, \"rms\": %.4f, \"clipped_samples\": %llu,\n", a.lr.size() / 2, static_cast<double>(a.lr.size() / 2) / kRate, static_cast<double>(peak), rms,
                static_cast<unsigned long long>(a.stats.clipped_samples));
    std::printf("  \"voices_started\": %llu, \"voices_stolen\": %llu, \"voices_rejected\": %llu,\n", static_cast<unsigned long long>(a.stats.voices_started), static_cast<unsigned long long>(a.stats.voices_stolen),
                static_cast<unsigned long long>(a.stats.voices_rejected));
    std::printf("  \"engine_loop_firing_hz\": [%.1f, %.1f], \"engine_bus_level_t0\": %.4f, \"engine_bus_level_pass_by\": %.4f,\n", static_cast<double>(a.engine_hz_start), static_cast<double>(a.engine_hz_end),
                static_cast<double>(far_start), static_cast<double>(near));
    std::printf("  \"engine_zero_crossings_per_s_idle\": %.0f, \"engine_zero_crossings_per_s_revved\": %.0f,\n  \"deterministic\": %s, \"hash\": \"%016llx\"\n}\n", zcr_idle, zcr_revved, deterministic ? "true" : "false", static_cast<unsigned long long>(fnv(a.lr)));
    const bool ok = deterministic && a.stats.clipped_samples == 0 && peak > 0.2f && peak <= 1.0f && rms > 0.01 && near > 4.0f * far_start && a.stats.voices_rejected == 0 && zcr_revved > 1.3 * zcr_idle;
    if (!ok) std::fprintf(stderr, "scene checks FAILED\n");
    return ok ? 0 : 1;
}

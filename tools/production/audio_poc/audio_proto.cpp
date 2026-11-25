// The audio prototype of the engine's own model on miniaudio (CORE/AUD-1): the real `engine_audio` (rpm loops, equal-power crossfade, smoothed rpm) and `impact_sound` (material category, gain by energy, pitch by energy)
// from include/saige/audio_model.hpp drive miniaudio voices, rendered offline (no audio device) so that what comes out can be measured. The sounds themselves are PROCEDURAL STAND-INS (harmonic stacks, decaying
// partials, noise grains): they exist to make the mapping measurable, they are not recordings and not the intended quality. Replacing them with professionally recorded, licensed loops and one-shot banks changes
// the files, not the code (docs/assets/audio.md has the sources and the licence rule).
//
//   audio_proto OUT_DIR
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio/miniaudio.h"

#include "saige/audio_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr ma_uint32 sample_rate = 48000;
constexpr ma_uint32 block = 480;  // 10 ms
constexpr double pi = 3.14159265358979323846;

struct Offline {
    ma_engine engine;
    Offline() {
        ma_engine_config cfg = ma_engine_config_init();
        cfg.noDevice = MA_TRUE;
        cfg.channels = 2;
        cfg.sampleRate = sample_rate;
        if (ma_engine_init(&cfg, &engine) != MA_SUCCESS) std::exit(1);
    }
    ~Offline() { ma_engine_uninit(&engine); }
    void render(std::vector<float>& out, ma_uint32 frames) {
        std::vector<float> tmp(static_cast<std::size_t>(frames) * 2);
        ma_uint64 got = 0;
        ma_engine_read_pcm_frames(&engine, tmp.data(), frames, &got);
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got) * 2);
    }
};

void write_wav(const std::string& path, const std::vector<float>& interleaved) {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, sample_rate);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, interleaved.data(), interleaved.size() / 2, &written);
    ma_encoder_uninit(&enc);
}

std::uint32_t lcg_state = 1u;
float noise() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return static_cast<float>(lcg_state >> 8) / 8388608.0f - 1.0f;
}

// One second of an engine at `rpm` (four stroke, four cylinders: two firings per revolution): a harmonic stack on the firing frequency rounded to a whole hertz so the loop is seamless, a little roughness.
std::vector<float> engine_loop(float rpm) {
    const double f0 = std::round(static_cast<double>(rpm) / 30.0);
    std::vector<float> s(sample_rate * 2);
    for (std::size_t i = 0; i < s.size() / 2; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        double v = 0.0;
        for (int h = 1; h <= 14; ++h) v += std::sin(2.0 * pi * f0 * h * t + 0.7 * h) / std::pow(static_cast<double>(h), 0.9);
        const float x = static_cast<float>(v) * 0.12f;
        s[i * 2] = s[i * 2 + 1] = x;
    }
    return s;
}

struct Voice {
    ma_audio_buffer buffer;
    ma_sound sound;
    std::vector<float> data;
};

bool start_loop(Offline& o, Voice& v, std::vector<float> data) {
    v.data = std::move(data);
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, 2, v.data.size() / 2, v.data.data(), nullptr);
    if (ma_audio_buffer_init(&bc, &v.buffer) != MA_SUCCESS) return false;
    if (ma_sound_init_from_data_source(&o.engine, &v.buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &v.sound) != MA_SUCCESS) return false;
    ma_sound_set_looping(&v.sound, MA_TRUE);
    ma_sound_set_volume(&v.sound, 0.0f);
    ma_sound_start(&v.sound);
    return true;
}

// The one-shot sounds of each category: what a recorded bank would be, made of decaying partials and noise grains (0.8 s).
std::vector<float> impact_shot(saige::ImpactCategory c) {
    std::vector<float> s(static_cast<std::size_t>(sample_rate) * 8 / 10 * 2, 0.0f);
    const auto tone = [&](double f, double tau, double amp) {
        for (std::size_t i = 0; i < s.size() / 2; ++i) {
            const double t = static_cast<double>(i) / sample_rate;
            const float x = static_cast<float>(amp * std::sin(2.0 * pi * f * t) * std::exp(-t / tau));
            s[i * 2] += x;
            s[i * 2 + 1] += x;
        }
    };
    const auto grains = [&](int count, double tau, double amp, double bright) {
        for (int g = 0; g < count; ++g) {
            const std::size_t start = static_cast<std::size_t>((noise() * 0.5f + 0.5f) * static_cast<float>(sample_rate) * 0.25f);
            float lp = 0.0f;
            for (std::size_t i = 0; i < 2400 && start + i < s.size() / 2; ++i) {
                lp += static_cast<float>(bright) * (noise() - lp);
                const float x = lp * static_cast<float>(amp * std::exp(-static_cast<double>(i) / (tau * sample_rate)));
                s[(start + i) * 2] += x;
                s[(start + i) * 2 + 1] += x;
            }
        }
    };
    switch (c) {
    case saige::ImpactCategory::Soft: tone(85, 0.06, 0.7); grains(1, 0.01, 0.2, 0.05); break;
    case saige::ImpactCategory::Hard: tone(900, 0.03, 0.5); grains(2, 0.004, 0.5, 0.6); break;
    case saige::ImpactCategory::Granular: grains(40, 0.012, 0.35, 0.35); break;
    case saige::ImpactCategory::Metal: tone(520, 0.30, 0.3); tone(1370, 0.25, 0.25); tone(2210, 0.2, 0.2); tone(3300, 0.15, 0.15); grains(1, 0.004, 0.4, 0.8); break;
    case saige::ImpactCategory::Glassy: tone(2400, 0.2, 0.3); tone(4100, 0.15, 0.25); tone(6200, 0.1, 0.2); grains(1, 0.002, 0.4, 0.9); break;
    case saige::ImpactCategory::Dull: tone(140, 0.04, 0.6); grains(1, 0.02, 0.2, 0.08); break;
    }
    return s;
}

const char* category_name(saige::ImpactCategory c) {
    switch (c) {
    case saige::ImpactCategory::Soft: return "soft";
    case saige::ImpactCategory::Hard: return "hard";
    case saige::ImpactCategory::Granular: return "granular";
    case saige::ImpactCategory::Metal: return "metal";
    case saige::ImpactCategory::Glassy: return "glassy";
    case saige::ImpactCategory::Dull: return "dull";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: audio_proto OUT_DIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::printf("{\n");
    // 1. Engine: a sweep from idle to the redline and back (6 s up, 1 s hold, 6 s down), the three loops driven by engine_audio at 10 ms.
    {
        Offline o;
        const saige::EngineAudioConfig cfg;
        const std::array<float, 3> rpms = cfg.loop_rpm;
        Voice v[3];
        for (int k = 0; k < 3; ++k) {
            if (!start_loop(o, v[k], engine_loop(rpms[static_cast<std::size_t>(k)]))) return 3;
        }
        saige::EngineAudioState st;
        std::vector<float> pcm;
        std::printf(" \"engine\": {\"file\": \"engine.wav\", \"loop_rpm\": [%.0f, %.0f, %.0f], \"timeline\": [", static_cast<double>(rpms[0]), static_cast<double>(rpms[1]), static_cast<double>(rpms[2]));
        const int blocks = 1300;
        for (int i = 0; i < blocks; ++i) {
            const float t = static_cast<float>(i) * block / sample_rate;
            const float u = t < 6.0f ? t / 6.0f : (t < 7.0f ? 1.0f : std::max(0.0f, 1.0f - (t - 7.0f) / 6.0f));
            const float rpm = 900.0f + (6500.0f - 900.0f) * u;
            const float throttle = t < 6.0f ? 1.0f : (t < 7.0f ? 0.6f : 0.0f);
            const saige::EngineAudioFrame f = saige::engine_audio(cfg, st, rpm, throttle, static_cast<float>(block) / sample_rate);
            for (int k = 0; k < 3; ++k) {
                ma_sound_set_pitch(&v[k].sound, f.pitch[static_cast<std::size_t>(k)]);
                ma_sound_set_volume(&v[k].sound, f.gain[static_cast<std::size_t>(k)] * f.master);
            }
            if (i % 25 == 0) std::printf("%s{\"t\": %.2f, \"rpm\": %.1f, \"smoothed\": %.1f, \"master\": %.3f}", i ? "," : "", static_cast<double>(t), static_cast<double>(rpm), static_cast<double>(f.rpm), static_cast<double>(f.master));
            o.render(pcm, block);
        }
        std::printf("]},\n");
        write_wav(dir + "/engine.wav", pcm);
        for (Voice& x : v) {
            ma_sound_uninit(&x.sound);
            ma_audio_buffer_uninit(&x.buffer);
        }
    }
    // 2. Impacts: material by surface and object, loudness and pitch by energy.
    {
        struct Case {
            const char* name;
            saige::Surface surface;
            bool metal;
            float energy;
        };
        std::vector<Case> cases;
        for (const float e : {100.0f, 1000.0f, 10000.0f, 80000.0f}) cases.push_back({"car_on_asphalt", saige::Surface::Asphalt, true, e});
        cases.push_back({"car_on_grass", saige::Surface::Grass, true, 5000.0f});
        cases.push_back({"car_on_gravel", saige::Surface::Gravel, true, 5000.0f});
        cases.push_back({"car_on_ice", saige::Surface::Ice, true, 5000.0f});
        cases.push_back({"rock_on_ice", saige::Surface::Ice, false, 5000.0f});
        cases.push_back({"rock_on_concrete", saige::Surface::Concrete, false, 5000.0f});
        cases.push_back({"tire_on_rubber", saige::Surface::Rubber, false, 5000.0f});
        cases.push_back({"scrape", saige::Surface::Asphalt, true, 20.0f});
        std::printf(" \"impacts\": [");
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Case& c = cases[n];
            const saige::ImpactSound is = saige::impact_sound(c.surface, c.metal, c.energy, 7u);
            Offline o;
            Voice v;
            v.data = impact_shot(is.category);
            ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, 2, v.data.size() / 2, v.data.data(), nullptr);
            ma_audio_buffer_init(&bc, &v.buffer);
            ma_sound_init_from_data_source(&o.engine, &v.buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &v.sound);
            ma_sound_set_pitch(&v.sound, is.pitch);
            ma_sound_set_volume(&v.sound, is.gain);
            ma_sound_start(&v.sound);
            std::vector<float> pcm;
            for (int i = 0; i < 100; ++i) o.render(pcm, block);
            const std::string file = std::string("impact_") + std::to_string(n) + ".wav";
            write_wav(dir + "/" + file, pcm);
            std::printf("%s{\"name\": \"%s\", \"file\": \"%s\", \"category\": \"%s\", \"gain\": %.4f, \"pitch\": %.4f, \"energy\": %.1f}", n ? "," : "", c.name, file.c_str(), category_name(is.category), static_cast<double>(is.gain),
                        static_cast<double>(is.pitch), static_cast<double>(c.energy));
            ma_sound_uninit(&v.sound);
            ma_audio_buffer_uninit(&v.buffer);
        }
        std::printf("]\n}\n");
    }
    return 0;
}

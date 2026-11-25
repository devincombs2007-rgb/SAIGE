// A proof of concept for SAIGE's audio runtime on miniaudio (public domain / MIT-0): everything the engine needs from a mixer, rendered offline (no audio device) so that it is testable.
//   * 3D positioning, distance attenuation and the Doppler effect for a moving source, from engine-space positions and velocities (+X forward, +Y left, +Z up);
//   * obstruction/occlusion as a per-source low-pass filter plus gain, driven by a number the engine's raycasts would supply;
//   * environment reverb as a send through a reverb node;
//   * RPM-dependent engine audio: sample layers cross-faded by rpm and pitch-shifted, a pure function that the tests check.
// It is not the runtime (Codex owns the audio runtime architecture); it shows that the dependency can do the job and what the adapter looks like. Output: WAV files and a JSON summary.
//
//   audio_poc OUT_DIR TONE.wav
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio/miniaudio.h"
#include "third_party/miniaudio/extras/nodes/ma_reverb_node/ma_reverb_node.c"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr ma_uint32 sample_rate = 48000;
constexpr ma_uint32 block = 480;  // 10 ms

struct Vec3 {
    float x, y, z;
};
// SAIGE engine space (+X forward, +Y left, +Z up) to miniaudio's (right-handed, +X right, +Y up, -Z forward).
Vec3 to_ma(Vec3 v) { return {-v.y, v.z, -v.x}; }

// RPM-dependent engine audio: layers recorded at fixed rpm, blended by distance in log-rpm (equal power) and re-pitched to the actual rpm. Pure, so it can be tested without audio.
struct RpmLayer {
    float rpm;
    float gain;
    float pitch;
};
std::vector<RpmLayer> rpm_blend(const std::vector<float>& layer_rpm, float rpm)
{
    std::vector<RpmLayer> out;
    const std::size_t n = layer_rpm.size();
    rpm = std::clamp(rpm, layer_rpm.front() * 0.5f, layer_rpm.back() * 1.5f);
    for (std::size_t i = 0; i < n; ++i) {
        float w = 0.0f;
        if (rpm <= layer_rpm.front() && i == 0) w = 1.0f;
        else if (rpm >= layer_rpm.back() && i + 1 == n) w = 1.0f;
        else if (i + 1 < n && rpm >= layer_rpm[i] && rpm <= layer_rpm[i + 1]) w = std::cos(0.5f * 3.14159265f * (std::log(rpm / layer_rpm[i]) / std::log(layer_rpm[i + 1] / layer_rpm[i])));
        else if (i > 0 && rpm >= layer_rpm[i - 1] && rpm <= layer_rpm[i]) w = std::sin(0.5f * 3.14159265f * (std::log(rpm / layer_rpm[i - 1]) / std::log(layer_rpm[i] / layer_rpm[i - 1])));
        if (w > 1e-4f) out.push_back({layer_rpm[i], w, rpm / layer_rpm[i]});
    }
    return out;
}

void write_wav(const std::string& path, const std::vector<float>& interleaved, ma_uint32 channels)
{
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, channels, sample_rate);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, interleaved.data(), interleaved.size() / channels, &written);
    ma_encoder_uninit(&enc);
}

struct Offline {
    ma_engine engine;
    Offline()
    {
        ma_engine_config cfg = ma_engine_config_init();
        cfg.noDevice = MA_TRUE;
        cfg.channels = 2;
        cfg.sampleRate = sample_rate;
        if (ma_engine_init(&cfg, &engine) != MA_SUCCESS) std::exit(1);
    }
    ~Offline() { ma_engine_uninit(&engine); }
    void render(std::vector<float>& out, ma_uint32 frames)
    {
        std::vector<float> tmp(static_cast<std::size_t>(frames) * 2);
        ma_uint64 got = 0;
        ma_engine_read_pcm_frames(&engine, tmp.data(), frames, &got);
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got) * 2);
    }
};

void set_listener(ma_engine& e, Vec3 pos)
{
    const Vec3 p = to_ma(pos);
    ma_engine_listener_set_position(&e, 0, p.x, p.y, p.z);
    ma_engine_listener_set_direction(&e, 0, 0, 0, -1);  // facing engine +X
    ma_engine_listener_set_world_up(&e, 0, 0, 1, 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: audio_poc OUT_DIR TONE.wav\n");
        return 2;
    }
    const std::string dir = argv[1], tone = argv[2];
    std::printf("{\n");

    // 1. A car passing a listener at 40 m/s, 8 m to the side: pan, attenuation, Doppler.
    {
        Offline o;
        ma_sound s;
        if (ma_sound_init_from_file(&o.engine, tone.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &s) != MA_SUCCESS) return 3;
        ma_sound_set_looping(&s, MA_TRUE);
        ma_sound_set_attenuation_model(&s, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&s, 4.0f);
        ma_sound_set_max_distance(&s, 400.0f);
        ma_sound_set_rolloff(&s, 1.0f);
        ma_sound_set_doppler_factor(&s, 1.0f);
        set_listener(o.engine, {0, 0, 1.5f});
        ma_sound_start(&s);
        std::vector<float> pcm;
        const float v = 40.0f;
        const int blocks = 600;  // 6 s: x goes from -120 m to +120 m
        for (int i = 0; i < blocks; ++i) {
            const float t = static_cast<float>(i) * block / sample_rate;
            const Vec3 pos{-120.0f + v * t, 8.0f, 1.0f};  // 8 m to the left of the road axis
            const Vec3 m = to_ma(pos), mv = to_ma({v, 0, 0});
            ma_sound_set_position(&s, m.x, m.y, m.z);
            ma_sound_set_velocity(&s, mv.x, mv.y, mv.z);
            o.render(pcm, block);
        }
        write_wav(dir + "/passby.wav", pcm, 2);
        ma_sound_uninit(&s);
        std::printf(" \"passby\": {\"file\": \"passby.wav\", \"speed_mps\": %.1f, \"closest_m\": 8.0, \"speed_of_sound_mps\": 343.3},\n", static_cast<double>(v));
    }

    // 2. Occlusion: the same static source with and without a low-pass (what a wall does) and a gain drop.
    for (int variant = 0; variant < 2; ++variant) {
        Offline o;
        ma_sound s;
        ma_sound_init_from_file(&o.engine, tone.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &s);
        ma_sound_set_looping(&s, MA_TRUE);
        ma_lpf_node lpf;
        const float occlusion = variant == 0 ? 0.0f : 0.85f;  // 0 open air, 1 fully behind a wall
        const double cutoff = 20000.0 * std::pow(300.0 / 20000.0, static_cast<double>(occlusion));  // 20 kHz down to 300 Hz
        ma_lpf_node_config lc = ma_lpf_node_config_init(2, sample_rate, cutoff, 4);
        ma_lpf_node_init(ma_engine_get_node_graph(&o.engine), &lc, nullptr, &lpf);
        ma_node_attach_output_bus(&lpf, 0, ma_engine_get_endpoint(&o.engine), 0);
        ma_node_attach_output_bus(&s, 0, &lpf, 0);
        ma_sound_set_volume(&s, 1.0f - 0.5f * occlusion);
        ma_sound_start(&s);
        std::vector<float> pcm;
        for (int i = 0; i < 100; ++i) o.render(pcm, block);
        write_wav(dir + (variant == 0 ? "/open.wav" : "/occluded.wav"), pcm, 2);
        ma_sound_uninit(&s);
        ma_lpf_node_uninit(&lpf, nullptr);
    }
    std::printf(" \"occlusion\": {\"open\": \"open.wav\", \"occluded\": \"occluded.wav\", \"cutoff_hz_occluded\": %.1f},\n", 20000.0 * std::pow(300.0 / 20000.0, 0.85));

    // 3. Reverb: a decaying noise burst through the reverb node.
    {
        Offline o;
        std::vector<float> burst(sample_rate / 5 * 2);
        unsigned seed = 7;
        for (std::size_t i = 0; i < burst.size() / 2; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float n = (static_cast<float>(seed >> 9) / 4194304.0f - 1.0f) * std::exp(-static_cast<float>(i) / 600.0f);
            burst[i * 2] = burst[i * 2 + 1] = n;
        }
        ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, 2, burst.size() / 2, burst.data(), nullptr);
        ma_audio_buffer buf;
        ma_audio_buffer_init(&bc, &buf);
        ma_sound s;
        ma_sound_init_from_data_source(&o.engine, &buf, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &s);
        ma_reverb_node rv;
        ma_reverb_node_config rc = ma_reverb_node_config_init(2, sample_rate);
        rc.roomSize = 0.9f;
        rc.damping = 0.3f;
        rc.wetVolume = 0.6f;
        rc.dryVolume = 0.4f;
        ma_reverb_node_init(ma_engine_get_node_graph(&o.engine), &rc, nullptr, &rv);
        ma_node_attach_output_bus(&rv, 0, ma_engine_get_endpoint(&o.engine), 0);
        ma_node_attach_output_bus(&s, 0, &rv, 0);
        ma_sound_start(&s);
        std::vector<float> pcm;
        for (int i = 0; i < 200; ++i) o.render(pcm, block);
        write_wav(dir + "/reverb.wav", pcm, 2);
        ma_sound_uninit(&s);
        ma_reverb_node_uninit(&rv, nullptr);
        ma_audio_buffer_uninit(&buf);
        std::printf(" \"reverb\": {\"file\": \"reverb.wav\"},\n");
    }

    // 4. RPM blend (pure): the layers, gains and pitches at a few rpm.
    {
        const std::vector<float> layers{900, 2000, 3500, 5500};
        std::printf(" \"rpm_blend\": [");
        bool ok = true;
        const float samples[] = {700, 900, 1400, 2000, 2800, 4200, 5500, 6500};
        for (int k = 0; k < 8; ++k) {
            const auto b = rpm_blend(layers, samples[k]);
            float power = 0;
            for (const RpmLayer& l : b) power += l.gain * l.gain;
            ok = ok && std::fabs(power - 1.0f) < 1e-3f;  // equal power at every rpm
            std::printf("%s{\"rpm\":%.0f,\"layers\":[", k ? "," : "", static_cast<double>(samples[k]));
            for (std::size_t i = 0; i < b.size(); ++i) std::printf("%s{\"base\":%.0f,\"gain\":%.3f,\"pitch\":%.3f}", i ? "," : "", static_cast<double>(b[i].rpm), static_cast<double>(b[i].gain), static_cast<double>(b[i].pitch));
            std::printf("]}");
        }
        std::printf("],\n \"rpm_blend_equal_power\": %s\n}\n", ok ? "true" : "false");
        if (!ok) return 1;
    }
    return 0;
}

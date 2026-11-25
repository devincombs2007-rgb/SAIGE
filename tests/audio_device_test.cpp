// The miniaudio adapter: decoders on in-memory WAV data, and the AudioRuntime driven by miniaudio's null backend (no sound card needed, so it runs in CI). What it cannot say is whether a real
// device sounds right: that is a hardware check (docs/audio.md).
#include "miniaudio_output.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

void put16(std::vector<unsigned char>& v, unsigned x) { v.push_back(static_cast<unsigned char>(x & 255)); v.push_back(static_cast<unsigned char>((x >> 8) & 255)); }
void put32(std::vector<unsigned char>& v, unsigned x) { put16(v, x & 0xffff); put16(v, x >> 16); }
void tag(std::vector<unsigned char>& v, const char* t) { v.insert(v.end(), t, t + 4); }

// A 16-bit PCM WAV: `channels` interleaved values per frame from `value(frame, channel)`.
template <class F>
std::vector<unsigned char> wav(unsigned rate, unsigned channels, unsigned frames, F value) {
    std::vector<unsigned char> v;
    const unsigned data_bytes = frames * channels * 2;
    tag(v, "RIFF"); put32(v, 36 + data_bytes); tag(v, "WAVE");
    tag(v, "fmt "); put32(v, 16); put16(v, 1); put16(v, channels); put32(v, rate); put32(v, rate * channels * 2); put16(v, channels * 2); put16(v, 16);
    tag(v, "data"); put32(v, data_bytes);
    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) put16(v, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(value(i, c) * 32767.0f))));
    }
    return v;
}

void test_decode() {
    const auto stereo = wav(22050, 2, 2205, [](unsigned, unsigned c) { return c == 0 ? 0.5f : 0.25f; });
    AudioClip clip;
    std::string err;
    require(decode_audio_memory(stereo.data(), stereo.size(), clip, err), "decode: " + err);
    require(clip.sample_rate == 22050 && clip.samples.size() == 2205, "the file's own rate and length are kept (no resampling)");
    require(std::fabs(clip.samples[100] - 0.375f) < 1e-3f, "stereo is averaged to mono");
    const auto sine = wav(48000, 1, 4800, [](unsigned i, unsigned) { return 0.8f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 48000.0f); });
    AudioClip s;
    require(decode_audio_memory(sine.data(), sine.size(), s, err) && s.samples.size() == 4800 && std::fabs(s.samples[12] - 0.8f * std::sin(6.2831853f * 440.0f * 12.0f / 48000.0f)) < 1e-3f, "a mono sine decodes sample for sample");
    // The decoded clip is usable by the mixer.
    AudioMixer m;
    require(m.has_clip(m.add_clip(s)), "a decoded clip goes into the mixer");
    // Bad input fails with a message instead of throwing or crashing.
    const unsigned char junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
    AudioClip bad;
    require(!decode_audio_memory(junk, sizeof junk, bad, err) && !err.empty(), "junk is refused with a message");
    err.clear();
    require(!decode_audio_file("/nonexistent/none.wav", bad, err) && !err.empty(), "a missing file is refused with a message");
    std::vector<unsigned char> truncated(stereo.begin(), stereo.begin() + 30);
    require(!decode_audio_memory(truncated.data(), truncated.size(), bad, err), "a truncated header is refused");
    const auto empty = wav(48000, 1, 0, [](unsigned, unsigned) { return 0.0f; });
    require(!decode_audio_memory(empty.data(), empty.size(), bad, err), "a file with no audio is refused");
}

void test_runtime_null_device() {
    MiniaudioOutputConfig cfg;
    cfg.null_device = true;
    AudioRuntime rt(make_miniaudio_output(cfg));
    require(!rt.running(), "not running before start");
    require(rt.start(), std::string("the null device starts: ") + rt.error());
    require(rt.running() && rt.output().sample_rate() == 48000, "running at the mixer's rate");
    {
        auto a = rt.access();
        AudioClip c;
        c.sample_rate = 48000;
        c.samples.assign(48000, 0.25f);
        VoiceParams p;
        p.clip = a.mixer.add_clip(c);
        p.loop = true;
        p.spatial = false;
        (void)a.mixer.play(p);
    }
    // The device thread renders in real time while the game thread takes the lock in short bursts.
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t sections = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600)) {
        {
            auto a = rt.access();
            a.mixer.set_bus_gain(AudioBus::Effects, 1.0f);
            ++sections;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const std::uint64_t rendered = rt.rendered_blocks(), skipped = rt.skipped_blocks();
    std::printf("null device, 0.6 s: %llu blocks rendered, %llu skipped, %llu control sections\n", static_cast<unsigned long long>(rendered), static_cast<unsigned long long>(skipped),
                static_cast<unsigned long long>(sections));
    require(rendered >= 20, "the device thread rendered blocks while the game thread worked");
    require(skipped * 10 <= rendered + skipped, "no more than 10% of blocks were skipped for the lock");
    {
        auto a = rt.access();
        require(a.mixer.stats().frames_rendered >= rendered * 100, "the mixer rendered the frames the device asked for");
        require(a.mixer.stats().active_voices == 1, "the looping voice is still going");
    }
    rt.stop();
    require(!rt.running(), "stopped");
    const std::uint64_t after = rt.rendered_blocks();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(rt.rendered_blocks() == after, "no callback runs after stop");
    require(rt.start() && rt.running(), "it restarts");
    rt.stop();
    bool threw = false;
    try {
        AudioRuntime bad(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a null output is rejected");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"decode", test_decode}, {"runtime", test_runtime_null_device}};
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
    if (failures == 0) std::cout << "audio device tests passed\n";
    return failures == 0 ? 0 : 1;
}

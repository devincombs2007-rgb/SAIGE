// miniaudio (David Reid; public domain / MIT-0) behind SAIGE's AudioOutput. We use only its device layer and decoders: mixing, spatialisation, buses and reverb are SAIGE's own (src/audio.cpp),
// so the engine/resource-manager/node-graph parts are compiled out.
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENCODING
#define MA_NO_GENERATION
// Ogg Vorbis (the format most recorded effect libraries ship) is decoded by stb_vorbis (Sean Barrett; public domain / MIT), which miniaudio picks up when it is included before it.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"  // its implementation must follow miniaudio's

#include "miniaudio_output.hpp"

#include <atomic>
#include <cstring>

namespace saige {

namespace {

class MiniaudioOutput final : public AudioOutput {
public:
    explicit MiniaudioOutput(const MiniaudioOutputConfig& config) : config_(config) {}
    ~MiniaudioOutput() override { stop(); }

    bool start(std::uint32_t sample_rate, RenderCallback callback, void* user) override {
        stop();
        error_.clear();
        if (!callback || sample_rate < 8000 || sample_rate > 384000) {
            error_ = "start: a callback and a sample rate within 8000..384000 are required";
            return false;
        }
        callback_ = callback;
        user_ = user;
        ma_context_config cc = ma_context_config_init();
        const ma_backend null_backend[] = {ma_backend_null};
        context_ = std::make_unique<ma_context>();
        if (ma_context_init(config_.null_device ? null_backend : nullptr, config_.null_device ? 1u : 0u, &cc, context_.get()) != MA_SUCCESS) {
            error_ = "no audio backend could be initialised";
            context_.reset();
            return false;
        }
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.format = ma_format_f32;
        dc.playback.channels = 2;
        dc.sampleRate = sample_rate;
        dc.periodSizeInFrames = config_.period_frames;
        dc.dataCallback = &MiniaudioOutput::data_callback;
        dc.pUserData = this;
        device_ = std::make_unique<ma_device>();
        const ma_result r = ma_device_init(context_.get(), &dc, device_.get());
        if (r != MA_SUCCESS) {
            error_ = std::string("no playback device could be opened: ") + ma_result_description(r);
            device_.reset();
            ma_context_uninit(context_.get());
            context_.reset();
            return false;
        }
        if (ma_device_start(device_.get()) != MA_SUCCESS) {
            error_ = "the playback device would not start";
            ma_device_uninit(device_.get());
            device_.reset();
            ma_context_uninit(context_.get());
            context_.reset();
            return false;
        }
        rate_ = device_->sampleRate;
        name_ = device_->playback.name;
        return true;
    }

    void stop() override {
        if (device_) {
            ma_device_uninit(device_.get());  // stops the callback thread and waits for it
            device_.reset();
        }
        if (context_) {
            ma_context_uninit(context_.get());
            context_.reset();
        }
        rate_ = 0;
    }

    [[nodiscard]] std::uint32_t sample_rate() const override { return rate_; }
    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* error() const override { return error_.c_str(); }

private:
    static void data_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<MiniaudioOutput*>(device->pUserData);
        self->callback_(self->user_, static_cast<float*>(output), frames);
    }

    MiniaudioOutputConfig config_;
    RenderCallback callback_ = nullptr;
    void* user_ = nullptr;
    std::unique_ptr<ma_context> context_;
    std::unique_ptr<ma_device> device_;
    std::uint32_t rate_ = 0;
    std::string name_;
    std::string error_;
};

bool decode_with(ma_decoder& dec, AudioClip& clip, std::string& error) {
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) != MA_SUCCESS || total == 0) total = 0;
    const std::uint32_t channels = dec.outputChannels;
    std::vector<float> out;
    std::vector<float> block(4096 * channels);
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, block.data(), 4096, &got);
        for (ma_uint64 i = 0; i < got; ++i) {
            float s = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) s += block[i * channels + c];
            out.push_back(s / static_cast<float>(channels));
        }
        if (r != MA_SUCCESS || got == 0) break;
        if (out.size() > (1u << 28)) {
            error = "the clip is longer than 2^28 frames";
            return false;
        }
    }
    if (out.empty()) {
        error = "the file holds no audio";
        return false;
    }
    clip.samples = std::move(out);
    clip.sample_rate = dec.outputSampleRate;
    return true;
}

} // namespace

std::unique_ptr<AudioOutput> make_miniaudio_output(const MiniaudioOutputConfig& config) { return std::make_unique<MiniaudioOutput>(config); }

bool decode_audio_file(const std::string& path, AudioClip& clip, std::string& error) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);  // keep the file's channel count and rate
    ma_decoder dec;
    const ma_result r = ma_decoder_init_file(path.c_str(), &dc, &dec);
    if (r != MA_SUCCESS) {
        error = path + ": " + ma_result_description(r);
        return false;
    }
    const bool ok = decode_with(dec, clip, error);
    ma_decoder_uninit(&dec);
    return ok;
}

bool decode_audio_memory(const void* data, std::size_t size, AudioClip& clip, std::string& error) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder dec;
    const ma_result r = ma_decoder_init_memory(data, size, &dc, &dec);
    if (r != MA_SUCCESS) {
        error = std::string("the data could not be decoded: ") + ma_result_description(r);
        return false;
    }
    const bool ok = decode_with(dec, clip, error);
    ma_decoder_uninit(&dec);
    return ok;
}

} // namespace saige

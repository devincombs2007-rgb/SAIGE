#pragma once

// The miniaudio device adapter for SAIGE's AudioOutput (include/saige/audio.hpp). Everything miniaudio is in miniaudio_output.cpp; this header exposes only SAIGE types, so no game code includes
// miniaudio and another backend can replace this one. Optional: built with -DSAIGE_AUDIO_MINIAUDIO=ON and SAIGE_MINIAUDIO_ROOT (tools/audio/fetch_miniaudio.py).

#include "saige/audio.hpp"

#include <memory>
#include <string>

namespace saige {

struct MiniaudioOutputConfig {
    // The "null" backend opens no hardware: miniaudio runs the callback on a timer thread at the real rate. It is what CI uses (no sound card) and what a headless server wants.
    bool null_device = false;
    std::uint32_t period_frames = 480;  // callback size at the requested rate (10 ms at 48 kHz); a smaller one lowers latency and raises the chance of underruns
};

[[nodiscard]] std::unique_ptr<AudioOutput> make_miniaudio_output(const MiniaudioOutputConfig& config = {});

// Decodes a WAV, FLAC, MP3 or Vorbis file to mono float at the file's own sample rate (channels are averaged). Returns false with `error` set for a missing or undecodable file; never throws.
[[nodiscard]] bool decode_audio_file(const std::string& path, AudioClip& clip, std::string& error);
[[nodiscard]] bool decode_audio_memory(const void* data, std::size_t size, AudioClip& clip, std::string& error);

} // namespace saige

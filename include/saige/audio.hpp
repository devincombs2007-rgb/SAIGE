#pragma once

// The engine's audio runtime, owned by SAIGE and independent of any audio library (AUD-2). `AudioMixer` is a deterministic software mixer: it plays mono clips as voices, places them in the
// world around a listener (distance attenuation, panning, doppler, occlusion, a shared reverb), groups them on buses with ducking, decides which voices to keep when there are too many, and
// renders interleaved stereo float blocks. It never touches a device: `AudioOutput` is the interface a device adapter implements (engine/audio_miniaudio is the miniaudio one, optional and off by
// default), so the same mixer runs headless in tests and in CI, and a different backend can replace the device layer without changing a line of game code.
//
// Determinism: the same clips, the same calls between renders and the same sample rate give bit-identical output, and the output does not depend on how the render calls are split (the mixer works in
// internal chunks of `AudioMixer::chunk_frames` and parameters take effect at chunk boundaries, ramped across the chunk so a moving source does not click).
//
// Coordinates follow the renderer: +X right, +Y up, -Z forward for an unrotated listener. Units are metres and seconds.
//
// Threading: an AudioMixer is not thread-safe. Control calls and render() must be serialized by the owner (the device adapter holds a mutex around both; the critical sections are short and
// allocate nothing after the clips are loaded).

#include "saige/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace saige {

// Mono PCM, normalised to [-1, 1].
struct AudioClip {
    std::vector<float> samples;
    std::uint32_t sample_rate = 48000;
};

struct ClipId {
    std::uint32_t index = 0;  // 0 is invalid
    bool operator==(const ClipId&) const = default;
    [[nodiscard]] explicit operator bool() const { return index != 0; }
};

struct VoiceId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 is invalid; a stopped or finished voice's id is never valid again
    bool operator==(const VoiceId&) const = default;
    [[nodiscard]] explicit operator bool() const { return generation != 0; }
};

enum class AudioBus : std::uint8_t { Effects, Vehicle, Ambience, Footsteps, Water, Music, Dialogue };
inline constexpr std::size_t kAudioBusCount = 7;

enum class Rolloff : std::uint8_t {
    Inverse,      // gain = min / (min + rolloff * (d - min)), the physical-ish default (-6 dB per doubling at rolloff 1)
    Linear,       // 1 at min, 0 at max
    Exponential,  // (d / min) ^ -rolloff
};

struct Listener {
    Vec3 position{};
    Vec3 forward{0, 0, -1};
    Vec3 up{0, 1, 0};
    Vec3 velocity{};
};

struct VoiceParams {
    ClipId clip;
    bool loop = false;
    AudioBus bus = AudioBus::Effects;
    std::uint8_t priority = 128;  // higher is kept when the voice limit is reached
    float gain = 1.0f;            // linear, >= 0
    float pitch = 1.0f;           // playback rate, > 0
    bool spatial = true;          // false: heard centred at constant gain (UI, music, a cabin sound)
    Vec3 position{};
    Vec3 velocity{};
    float min_distance = 1.0f;    // full volume inside this distance
    float max_distance = 100.0f;  // no further attenuation beyond it (Linear: silent)
    float rolloff = 1.0f;
    Rolloff model = Rolloff::Inverse;
    float occlusion = 0.0f;       // 0 open air .. 1 fully blocked: set from the world (ray casts, portals)
    float reverb_send = 0.0f;     // 0..1 of the voice's (post-occlusion) signal sent to the shared reverb
};

struct ReverbSettings {
    float room_size = 0.5f;  // 0..1: decay time
    float damping = 0.5f;    // 0..1: how quickly high frequencies die in the tail
    float wet = 0.0f;        // 0..1: level of the reverb return in the mix (0 turns the reverb off)
};

// While the `source` bus is louder than `threshold` (RMS per chunk), the `target` bus is turned down by `amount` (0..1 of its level).
struct DuckRule {
    AudioBus source = AudioBus::Dialogue;
    AudioBus target = AudioBus::Ambience;
    float threshold = 0.02f;
    float amount = 0.6f;
    float attack = 0.05f;   // s
    float release = 0.4f;   // s
};

struct AudioMixerConfig {
    std::uint32_t sample_rate = 48000;
    std::uint32_t max_voices = 64;
    float speed_of_sound = 343.0f;
    float doppler_scale = 1.0f;          // 0 turns doppler off
    float occluded_gain = 0.35f;         // gain at occlusion 1 (the lowpass does the rest)
    float occluded_cutoff_hz = 800.0f;   // lowpass cutoff at occlusion 1
    float air_absorption_distance = 400.0f;  // m: the distance at which air has halved the lowpass cutoff's headroom; 0 turns it off
};

struct AudioStats {
    std::uint32_t active_voices = 0;
    std::uint64_t voices_started = 0;
    std::uint64_t voices_stolen = 0;
    std::uint64_t voices_rejected = 0;
    std::uint64_t frames_rendered = 0;
    std::uint64_t clipped_samples = 0;  // output samples beyond [-1, 1] (clamped)
};

class AudioMixer {
public:
    static constexpr std::uint32_t chunk_frames = 64;

    // Throws std::invalid_argument for a sample rate outside 8000..384000, no voices, or a non-positive or non-finite physical constant.
    explicit AudioMixer(const AudioMixerConfig& config = {});
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Clips are immutable once added. Throws std::invalid_argument for an empty clip, a zero sample rate or a non-finite sample.
    [[nodiscard]] ClipId add_clip(AudioClip clip);
    [[nodiscard]] bool has_clip(ClipId id) const;

    // Starts a voice. Returns an invalid id when the voice limit is reached and every playing voice is at least as important (priority, then loudness) as the new one. Throws
    // std::invalid_argument for an unknown clip, non-finite values, a negative gain, a non-positive pitch or distance, max_distance below min_distance, or an occlusion/send outside 0..1.
    [[nodiscard]] VoiceId play(const VoiceParams& params);
    bool stop(VoiceId id);  // false for a stale id
    [[nodiscard]] bool playing(VoiceId id) const;

    // Live control of a playing voice: all return false for a stale id and throw std::invalid_argument for invalid values.
    bool set_voice_gain(VoiceId id, float gain);
    bool set_voice_pitch(VoiceId id, float pitch);
    bool set_voice_transform(VoiceId id, Vec3 position, Vec3 velocity);
    bool set_voice_occlusion(VoiceId id, float occlusion);
    bool set_voice_reverb_send(VoiceId id, float send);

    // Throws std::invalid_argument for non-finite values or a forward/up pair that is zero or parallel.
    void set_listener(const Listener& listener);
    void set_bus_gain(AudioBus bus, float gain);  // >= 0, ramped across a chunk
    void set_master_gain(float gain);
    void set_reverb(const ReverbSettings& reverb);
    void clear_ducking();
    void add_ducking(const DuckRule& rule);

    // Renders `frames` frames of interleaved stereo into `out` (2 * frames floats).
    void render(float* out, std::uint32_t frames);

    [[nodiscard]] AudioStats stats() const;
    [[nodiscard]] const AudioMixerConfig& config() const;
    [[nodiscard]] float bus_level(AudioBus bus) const;  // the last chunk's RMS on the bus before its gain, for meters and tests
    [[nodiscard]] float duck_gain(AudioBus bus) const;  // the factor ducking currently applies to a bus (1 when not ducked)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A device adapter: pulls interleaved stereo float blocks from `render` on the device's thread. Implemented outside the core (engine/audio_miniaudio); nothing in the core links a device library.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    // Starts playback at the given sample rate. Returns false (and plays nothing) when no device could be opened; `error` then says why. The callback must not throw.
    using RenderCallback = void (*)(void* user, float* interleaved_stereo, std::uint32_t frames);
    virtual bool start(std::uint32_t sample_rate, RenderCallback callback, void* user) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::uint32_t sample_rate() const = 0;
    [[nodiscard]] virtual const char* name() const = 0;
    [[nodiscard]] virtual const char* error() const = 0;
};

} // namespace saige

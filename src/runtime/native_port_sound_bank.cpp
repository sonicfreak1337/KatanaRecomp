#include "katana/runtime/native_port_sound_bank.hpp"
#include "katana/runtime/native_port_content.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_output_sample_rate = 192'000u;
constexpr std::uint32_t maximum_render_block_frames = 16'384u;
constexpr std::uint32_t maximum_sound_bank_collections = 4'096u;
constexpr std::uint32_t maximum_sound_bank_sequences = 4'096u;
constexpr std::uint32_t maximum_sound_bank_voices = 4'096u;
constexpr std::uint32_t maximum_sound_bank_midi_ports = 4'096u;
constexpr std::uint32_t maximum_sound_bank_units = 65'536u;
constexpr std::uint32_t maximum_sound_bank_events = 16u * 1024u * 1024u;
constexpr std::uint32_t maximum_sound_bank_reference_depth = 64u;
constexpr std::uint64_t maximum_sound_bank_development_state_bytes =
    2ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t sound_bank_development_state_version = 1u;
constexpr std::array<std::uint8_t, 8u> sound_bank_development_state_magic{
    'K', 'S', 'B', 'S', 'T', 'A', 'T', 'E'};
constexpr std::uint32_t maximum_mpb_programs = 128u;
constexpr std::uint32_t maximum_mpb_layers = 4u;
constexpr std::uint32_t maximum_mpb_splits = 128u;
constexpr std::uint32_t maximum_mpb_velocity_curves = 31u;
constexpr std::uint32_t maximum_mlt_banks = 16u;
constexpr std::uint32_t maximum_midi_channels = 16u;
constexpr std::uint32_t maximum_effect_buses = 16u;
constexpr double pi = 3.1415926535897932384626433832795;
constexpr double aica_source_sample_rate = 44'100.0;
constexpr std::array<std::uint16_t, 16u> aica_send_attenuation{
    255u, 112u, 104u, 96u, 88u, 80u, 72u, 64u,
    56u, 48u,  40u,  32u, 24u, 16u, 8u,  0u};
constexpr std::array<std::int32_t, 32u> aica_filter_q{
    2048,  1536,  1024,   512,     0,  -256,  -512,  -768,
   -1024, -1280, -1536, -1792, -2048, -2176, -2304, -2432,
   -2560, -2688, -2816, -2944, -3072, -3136, -3200, -3264,
   -3328, -3392, -3456, -3520, -3584, -3648, -3712, -3776};

[[noreturn]] void fail_sound_bank(const NativePortSoundBankFailure failure,
                                  const std::string_view operation) {
    throw NativePortSoundBankError(failure, operation);
}

void saturating_add(std::uint64_t& destination,
                    const std::uint64_t value) noexcept {
    destination = value > std::numeric_limits<std::uint64_t>::max() - destination
                      ? std::numeric_limits<std::uint64_t>::max()
                      : destination + value;
}

[[nodiscard]] bool valid_gain_pan(const float gain, const float pan) noexcept {
    return std::isfinite(gain) && std::isfinite(pan) && gain >= 0.0f &&
           gain <= 4.0f && pan >= -1.0f && pan <= 1.0f;
}

[[nodiscard]] bool valid_playback_rate(const float value) noexcept {
    return std::isfinite(value) && value >= 0.125f && value <= 8.0f;
}

[[nodiscard]] double aica_attenuation_gain(const double attenuation) noexcept {
    if (!(attenuation < 255.0)) return 0.0;
    return std::exp2(-std::max(0.0, attenuation) / 16.0);
}

[[nodiscard]] const std::array<double, 256u>&
aica_attenuation_gain_table() noexcept {
    static const auto values = [] {
        std::array<double, 256u> result{};
        for (std::size_t index = 0u; index < result.size(); ++index)
            result[index] = aica_attenuation_gain(static_cast<double>(index));
        return result;
    }();
    return values;
}

[[nodiscard]] double aica_send_gain(const std::uint8_t level) noexcept {
    return aica_attenuation_gain_table()[aica_send_attenuation[level & 15u]];
}

[[nodiscard]] std::pair<double, double>
aica_pan_gains(const std::uint8_t raw_pan) noexcept {
    const auto amount = static_cast<std::uint8_t>(raw_pan & 15u);
    const auto attenuated = aica_send_gain(
        static_cast<std::uint8_t>(15u - amount));
    return (raw_pan & 0x10u) != 0u
               ? std::pair<double, double>{1.0, attenuated}
               : std::pair<double, double>{attenuated, 1.0};
}

void validate_config(const NativePortSoundBankConfig& config) {
    const auto& effect_provider = config.effect_kernel_provider;
    const auto valid_effect_provider =
        effect_provider.resolve == nullptr ||
        effect_provider.contract_version ==
            native_port_sound_effect_kernel_contract_version;
    if (config.output_sample_rate < 8'000u ||
        config.output_sample_rate > maximum_output_sample_rate ||
        config.render_block_frames == 0u ||
        config.render_block_frames > maximum_render_block_frames ||
        config.maximum_collections == 0u ||
        config.maximum_collections > maximum_sound_bank_collections ||
        config.maximum_collection_bytes < 32u ||
        config.maximum_total_collection_bytes <
            config.maximum_collection_bytes ||
        config.maximum_units_per_collection == 0u ||
        config.maximum_units_per_collection > maximum_sound_bank_units ||
        config.maximum_sequences_per_bank == 0u ||
        config.maximum_sequences_per_bank > maximum_sound_bank_sequences ||
        config.maximum_events_per_sequence == 0u ||
        config.maximum_events_per_sequence > maximum_sound_bank_events ||
        config.maximum_reference_depth == 0u ||
        config.maximum_reference_depth > maximum_sound_bank_reference_depth ||
        config.maximum_active_sequences == 0u ||
        config.maximum_active_sequences > maximum_sound_bank_sequences ||
        config.maximum_synth_voices == 0u ||
        config.maximum_synth_voices > maximum_sound_bank_voices ||
        config.maximum_midi_ports == 0u ||
        config.maximum_midi_ports > maximum_sound_bank_midi_ports ||
        config.maximum_decoded_sample_frames == 0u ||
        config.maximum_effect_state_bytes == 0u ||
        config.maximum_development_state_bytes <
            config.maximum_total_collection_bytes ||
        config.maximum_development_state_bytes >
            maximum_sound_bank_development_state_bytes ||
        !valid_effect_provider)
        fail_sound_bank(NativePortSoundBankFailure::InvalidConfig, "config");
}

class BoundedBytes final {
  public:
    explicit BoundedBytes(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    void extent(const std::uint64_t offset,
                const std::uint64_t size,
                const NativePortSoundBankFailure failure,
                const std::string_view operation) const {
        if (offset > bytes_.size() || size > bytes_.size() - offset)
            fail_sound_bank(failure, operation);
    }

    [[nodiscard]] std::uint8_t u8(
        const std::uint64_t offset,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        extent(offset, 1u, failure, operation);
        return bytes_[static_cast<std::size_t>(offset)];
    }

    [[nodiscard]] std::int8_t i8(
        const std::uint64_t offset,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        return std::bit_cast<std::int8_t>(u8(offset, failure, operation));
    }

    [[nodiscard]] std::uint16_t u16(
        const std::uint64_t offset,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        extent(offset, 2u, failure, operation);
        const auto index = static_cast<std::size_t>(offset);
        return static_cast<std::uint16_t>(bytes_[index]) |
               static_cast<std::uint16_t>(bytes_[index + 1u] << 8u);
    }

    [[nodiscard]] std::uint16_t be16(
        const std::uint64_t offset,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        extent(offset, 2u, failure, operation);
        const auto index = static_cast<std::size_t>(offset);
        return static_cast<std::uint16_t>(bytes_[index] << 8u) |
               static_cast<std::uint16_t>(bytes_[index + 1u]);
    }

    [[nodiscard]] std::uint32_t u32(
        const std::uint64_t offset,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        extent(offset, 4u, failure, operation);
        const auto index = static_cast<std::size_t>(offset);
        return static_cast<std::uint32_t>(bytes_[index]) |
               (static_cast<std::uint32_t>(bytes_[index + 1u]) << 8u) |
               (static_cast<std::uint32_t>(bytes_[index + 2u]) << 16u) |
               (static_cast<std::uint32_t>(bytes_[index + 3u]) << 24u);
    }

    [[nodiscard]] std::span<const std::uint8_t> span(
        const std::uint64_t offset,
        const std::uint64_t size,
        const NativePortSoundBankFailure failure,
        const std::string_view operation) const {
        extent(offset, size, failure, operation);
        return bytes_.subspan(static_cast<std::size_t>(offset),
                              static_cast<std::size_t>(size));
    }

    [[nodiscard]] bool magic(const std::uint64_t offset,
                             const char (&expected)[5]) const noexcept {
        if (offset > bytes_.size() || 4u > bytes_.size() - offset)
            return false;
        return std::memcmp(bytes_.data() + offset, expected, 4u) == 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  private:
    std::span<const std::uint8_t> bytes_;
};

[[nodiscard]] std::uint64_t sample_key(const std::uint32_t offset,
                                       const std::uint16_t frames,
                                       const std::uint8_t format,
                                       const std::uint8_t bank) noexcept {
    return static_cast<std::uint64_t>(offset) |
           (static_cast<std::uint64_t>(frames) << 32u) |
           (static_cast<std::uint64_t>(format) << 48u) |
           (static_cast<std::uint64_t>(bank) << 56u);
}

template <typename T, bool = std::is_enum_v<T>> struct SoundBankStateRaw {
    using type = T;
};
template <typename T> struct SoundBankStateRaw<T, true> {
    using type = std::underlying_type_t<T>;
};

class SoundBankStateWriter final {
  public:
    explicit SoundBankStateWriter(const std::uint64_t limit) : limit_(limit) {}

    template <typename T> void scalar(const T value) {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
        using Raw = typename SoundBankStateRaw<T>::type;
        using Unsigned = std::make_unsigned_t<Raw>;
        const auto bits = static_cast<Unsigned>(static_cast<Raw>(value));
        reserve(sizeof(T));
        for (std::size_t index = 0u; index < sizeof(T); ++index)
            bytes_.push_back(static_cast<std::uint8_t>(
                bits >> static_cast<unsigned>(index * 8u)));
    }

    void boolean(const bool value) { scalar<std::uint8_t>(value ? 1u : 0u); }
    void f32(const float value) {
        if (!std::isfinite(value)) invalid();
        scalar(std::bit_cast<std::uint32_t>(value));
    }
    void f64(const double value) {
        if (!std::isfinite(value)) invalid();
        scalar(std::bit_cast<std::uint64_t>(value));
    }
    void raw(const std::span<const std::uint8_t> value) {
        reserve(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void string(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) limit();
        scalar(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    template <typename Handle> void handle(const Handle value) {
        scalar(value.slot);
        scalar(value.generation);
    }
    void finalize() {
        const auto final_size = static_cast<std::uint64_t>(bytes_.size()) + 64u;
        if (final_size > limit_) limit();
        if (bytes_.size() < 20u) invalid();
        for (std::size_t index = 0u; index < 8u; ++index)
            bytes_[12u + index] = static_cast<std::uint8_t>(
                final_size >> static_cast<unsigned>(index * 8u));
        const auto digest = native_port_content_sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes_.data()), bytes_.size()));
        if (digest.size() != 64u) invalid();
        raw({reinterpret_cast<const std::uint8_t*>(digest.data()), digest.size()});
    }
    [[nodiscard]] std::vector<std::uint8_t> take() && {
        return std::move(bytes_);
    }

  private:
    [[noreturn]] static void invalid() {
        fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                        "state-encode");
    }
    [[noreturn]] static void limit() {
        fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                        "state-size");
    }
    void reserve(const std::uint64_t count) {
        if (count > limit_ || bytes_.size() > limit_ - count ||
            count > std::numeric_limits<std::size_t>::max())
            limit();
        const auto required = bytes_.size() + static_cast<std::size_t>(count);
        if (required > bytes_.capacity()) {
            const auto growth = std::max<std::uint64_t>(4096u,
                static_cast<std::uint64_t>(bytes_.capacity()) * 2u);
            bytes_.reserve(static_cast<std::size_t>(
                std::min(limit_, std::max<std::uint64_t>(required, growth))));
        }
    }
    std::uint64_t limit_;
    std::vector<std::uint8_t> bytes_;
};

class SoundBankStateReader final {
  public:
    SoundBankStateReader(const std::span<const std::uint8_t> bytes,
                         const std::uint64_t limit)
        : bytes_(bytes) {
        if (bytes.size() < 20u + 64u || bytes.size() > limit) invalid();
        const auto payload = bytes.first(bytes.size() - 64u);
        const auto digest = native_port_content_sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
        if (digest.size() != 64u ||
            std::memcmp(digest.data(), bytes.data() + payload.size(), 64u) != 0)
            invalid();
        bytes_ = payload;
    }

    template <typename T> [[nodiscard]] T scalar() {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
        using Raw = typename SoundBankStateRaw<T>::type;
        using Unsigned = std::make_unsigned_t<Raw>;
        extent(sizeof(T));
        Unsigned bits = 0u;
        for (std::size_t index = 0u; index < sizeof(T); ++index)
            bits |= static_cast<Unsigned>(bytes_[cursor_ + index])
                    << static_cast<unsigned>(index * 8u);
        cursor_ += sizeof(T);
        return static_cast<T>(static_cast<Raw>(bits));
    }
    [[nodiscard]] bool boolean() {
        const auto value = scalar<std::uint8_t>();
        if (value > 1u) invalid();
        return value != 0u;
    }
    [[nodiscard]] float f32() {
        const auto value = std::bit_cast<float>(scalar<std::uint32_t>());
        if (!std::isfinite(value)) invalid();
        return value;
    }
    [[nodiscard]] double f64() {
        const auto value = std::bit_cast<double>(scalar<std::uint64_t>());
        if (!std::isfinite(value)) invalid();
        return value;
    }
    [[nodiscard]] std::span<const std::uint8_t> raw(const std::uint64_t size) {
        extent(size);
        const auto result = bytes_.subspan(cursor_, static_cast<std::size_t>(size));
        cursor_ += static_cast<std::size_t>(size);
        return result;
    }
    [[nodiscard]] std::string string() {
        const auto value = raw(scalar<std::uint32_t>());
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    template <typename Handle> [[nodiscard]] Handle handle() {
        return {scalar<std::uint32_t>(), scalar<std::uint32_t>()};
    }
    void finish() const {
        if (cursor_ != bytes_.size()) invalid();
    }

  private:
    [[noreturn]] static void invalid() {
        fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                        "state-decode");
    }
    void extent(const std::uint64_t count) const {
        if (cursor_ > bytes_.size() || count > bytes_.size() - cursor_)
            invalid();
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

enum class SoundBankOpcode : std::uint16_t {
    Construct = 1u,
    Destroy,
    LoadCollection,
    UnloadCollection,
    HasProgram,
    HasSequence,
    HasCollectionUnit,
    BindPcmStreamRing,
    ReleasePcmStreamRing,
    PcmStreamRingSnapshot,
    PredecodeCollectionSamples,
    PlaySequence,
    PauseSequence,
    ResumeSequence,
    StopSequence,
    ReleaseSequence,
    SetSequenceGainPan,
    SetSequencePitchBend,
    SetSequencePlaybackRate,
    SetSequenceSendLevels,
    NoteOn,
    NoteOff,
    ReleaseVoice,
    SetVoiceGainPan,
    SetVoicePitchBend,
    SetVoiceSendLevels,
    OpenMidiPort,
    CloseMidiPort,
    SetMidiProgram,
    SetMidiGainPan,
    SetMidiPitchBend,
    SetMidiPlaybackRate,
    SetMidiSendLevels,
    MidiNoteOn,
    MidiNoteOff,
    MidiPlaySequence,
    MidiPause,
    MidiResume,
    MidiStop,
    CloseAllMidiPorts,
    ReleaseAllSequences,
    UnloadAllCollections,
    StopAll,
    Reset,
    Pump,
    SequenceSnapshot,
    MidiPortSnapshot,
    Snapshot,
    CaptureDevelopmentState,
    ValidateDevelopmentState,
    RestoreDevelopmentState,
};

struct SoundBankCommand final {
    NativePortSoundCollectionHandle collection{};
    NativePortSoundPcmStreamRingHandle ring{};
    NativePortSoundSequenceHandle sequence{};
    NativePortSoundVoiceHandle voice{};
    NativePortSoundMidiPortHandle port{};
    NativePortSoundPcmStreamRingConfig ring_config{};
    NativePortSoundSequenceConfig sequence_config{};
    NativePortSoundNoteConfig note_config{};
    NativePortSoundMidiPortConfig midi_config{};
    float value0 = 0.0f;
    float value1 = 0.0f;
    std::int16_t signed_value = 0;
    std::uint16_t value16 = 0u;
    std::uint8_t value8_0 = 0u;
    std::uint8_t value8_1 = 0u;
    std::uint8_t value8_2 = 0u;
    std::uint8_t value8_3 = 0u;
};

struct SoundBankLoadCommand final {
    std::uint64_t source_offset = 0u;
    std::uint64_t byte_size = 0u;
    std::uint32_t logical_id_size = 0u;
    std::uint32_t path_size = 0u;
    std::uint32_t byte_identity_size = 0u;
    std::uint32_t reserved = 0u;
};

struct SoundBankWireError final {
    NativePortSoundBankFailure failure =
        NativePortSoundBankFailure::WorkerFailure;
    std::array<std::uint8_t, 3u> reserved{};
};

static_assert(std::is_trivially_copyable_v<SoundBankCommand>);
static_assert(std::is_trivially_copyable_v<SoundBankLoadCommand>);
static_assert(std::is_trivially_copyable_v<SoundBankWireError>);

template <typename Value>
[[nodiscard]] std::span<const std::byte> sound_object_bytes(
    const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    return {reinterpret_cast<const std::byte*>(std::addressof(value)),
            sizeof(Value)};
}

template <typename Value>
[[nodiscard]] bool read_sound_object(
    const std::span<const std::byte> bytes,
    Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    if (bytes.size() != sizeof(Value)) return false;
    std::memcpy(std::addressof(value), bytes.data(), sizeof(Value));
    return true;
}

template <typename Value>
void write_sound_ack(NativePortAudioCommandAckResult& result,
                     const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    static_assert(sizeof(Value) <=
                  native_port_audio_command_queue_max_ack_result_bytes);
    result.status = NativePortAudioCommandAckStatus::Completed;
    result.result_size = sizeof(Value);
    std::memcpy(result.bytes.data(), std::addressof(value), sizeof(Value));
}

void write_sound_error(NativePortAudioCommandAckResult& result,
                       const NativePortSoundBankFailure failure) noexcept {
    const SoundBankWireError error{failure, {}};
    result.status = NativePortAudioCommandAckStatus::Failed;
    result.error_code = static_cast<std::uint32_t>(failure);
    result.result_size = sizeof(error);
    std::memcpy(result.bytes.data(), std::addressof(error), sizeof(error));
}

[[nodiscard]] std::vector<std::byte> materialize_sound_bank_content(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    const NativePortSoundBankConfig& config) {
    if (binding.byte_size < 32u ||
        binding.byte_size > config.maximum_collection_bytes ||
        binding.byte_size > std::numeric_limits<std::size_t>::max())
        fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                        "collection-size");
    std::unique_ptr<NativePortReadOnlyFile> file;
    try {
        file = platform.open_content_file(binding);
    } catch (...) {
        fail_sound_bank(NativePortSoundBankFailure::ContentOpen,
                        "content-open");
    }
    if (file == nullptr || file->byte_size() != binding.byte_size)
        fail_sound_bank(NativePortSoundBankFailure::ContentOpen,
                        "content-size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(binding.byte_size));
    try {
        file->read_at(0u, bytes);
    } catch (...) {
        fail_sound_bank(NativePortSoundBankFailure::ContentRead,
                        "content-read");
    }
    return bytes;
}

} // namespace

NativePortSoundBankError::NativePortSoundBankError(
    const NativePortSoundBankFailure failure,
    const std::string_view operation)
    : std::runtime_error("native-port-sound-bank-" + std::string(operation)),
      failure_(failure) {}

NativePortSoundBankFailure NativePortSoundBankError::failure() const noexcept {
    return failure_;
}

class NativePortSoundBankEngine::Core final {
    friend class NativePortSoundBankEngine::Impl;
  private:
    enum class SampleFormat : std::uint8_t { Pcm16, Pcm8, Adpcm4 };
    enum class EventKind : std::uint8_t {
        Note,
        Control,
        Program,
        Pressure,
        Pitch,
        Tempo,
        Loop,
        SysEx,
    };
    enum class EnvelopeStage : std::uint8_t {
        Attack,
        Decay1,
        Decay2,
        Release,
        Complete,
    };

    struct Split final {
        SampleFormat format = SampleFormat::Pcm16;
        std::uint32_t tone_offset = 0u;
        std::uint16_t loop_start = 0u;
        std::uint16_t loop_end = 0u;
        bool loop = false;
        std::uint8_t attack = 0u;
        std::uint8_t decay1 = 0u;
        std::uint8_t decay2 = 0u;
        std::uint8_t release = 0u;
        std::uint8_t decay_level = 0u;
        std::uint8_t key_rate_scaling = 0u;
        bool loop_start_link = false;
        std::uint16_t pitch_fns = 0u;
        std::int8_t pitch_octave = 0;
        std::uint8_t lfo_amp_depth = 0u;
        std::uint8_t lfo_amp_wave = 0u;
        std::uint8_t lfo_pitch_depth = 0u;
        std::uint8_t lfo_pitch_wave = 0u;
        std::uint8_t lfo_frequency = 0u;
        bool lfo_sync = false;
        std::uint8_t effect_bus = 0u;
        std::uint8_t effect_level = 0u;
        std::uint8_t direct_level = 0u;
        std::uint8_t pan = 0u;
        // Manatee stores the one's-complement of AICA TL. Keep the semantic
        // oscillator level here (255 is unattenuated) and convert it back to
        // logarithmic TL units only at the native mixer boundary.
        std::uint8_t oscillator_level = 255u;
        bool filter = false;
        bool voice_attenuation_off = false;
        std::uint8_t filter_resonance = 0u;
        std::array<std::uint16_t, 5u> filter_levels{};
        std::array<std::uint8_t, 4u> filter_rates{};
        std::uint8_t start_note = 0u;
        std::uint8_t end_note = 127u;
        std::uint8_t base_note = 60u;
        std::int8_t fine_tune = 0;
        std::uint8_t velocity_curve = 0u;
        std::uint8_t velocity_low = 0u;
        std::uint8_t velocity_high = 127u;
        bool drum = false;
        std::uint8_t drum_group = 0u;
    };

    struct Layer final {
        std::uint16_t delay = 0u;
        std::uint8_t bend_high = 0u;
        std::uint8_t bend_low = 0u;
        std::vector<Split> splits;
    };

    struct Program final { std::vector<Layer> layers; };

    struct ProgramBank final {
        std::vector<Program> programs;
        std::vector<std::array<std::uint8_t, 128u>> velocity_curves;
        // Keep collection-relative ownership coordinates, never a view into
        // Collection::bytes. Collections move when they are inserted into or
        // compacted within the slot table; a persisted span would then retain
        // the old vector allocation and turn the first later sample decode
        // into a use-after-move.
        std::uint32_t payload_offset = 0u;
        std::uint32_t payload_size = 0u;
        std::uint8_t bank = 0u;
    };

    struct PlayableProgram final {
        ProgramBank* bank = nullptr;
        const Program* program = nullptr;
    };

    struct Event final {
        EventKind kind = EventKind::Note;
        std::uint64_t tick = 0u;
        std::uint32_t gate = 0u;
        std::uint32_t step = 0u;
        std::uint16_t value = 0u;
        std::uint8_t channel = 0u;
        std::uint8_t data0 = 0u;
        std::uint8_t data1 = 0u;
    };

    struct Sequence final {
        std::uint32_t tpqn = 0u;
        std::uint32_t initial_tempo = 0u;
        std::vector<Event> events;
        std::uint64_t end_tick = 0u;
    };

    struct SequenceBank final { std::vector<std::optional<Sequence>> sequences; };

    struct EffectOutput final {
        std::uint8_t level = 0u;
        std::uint8_t pan = 0u;
    };

    struct EffectKernelState final {
        EffectKernelState() = default;
        EffectKernelState(const EffectKernelState&) = delete;
        EffectKernelState& operator=(const EffectKernelState&) = delete;
        EffectKernelState(EffectKernelState&& other) noexcept
            : kernel(std::exchange(other.kernel, nullptr)),
              storage(std::exchange(other.storage, nullptr)) {}
        EffectKernelState& operator=(EffectKernelState&& other) noexcept {
            if (this == &other) return *this;
            reset();
            kernel = std::exchange(other.kernel, nullptr);
            storage = std::exchange(other.storage, nullptr);
            return *this;
        }
        ~EffectKernelState() { reset(); }

        void reset() noexcept {
            if (storage == nullptr || kernel == nullptr) return;
            kernel->destroy(storage);
            ::operator delete(
                storage,
                std::align_val_t(static_cast<std::size_t>(
                    kernel->state_alignment)));
            storage = nullptr;
            kernel = nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return kernel != nullptr && storage != nullptr;
        }

        const NativePortSoundEffectKernel* kernel = nullptr;
        void* storage = nullptr;
    };

    struct Collection final {
        std::string logical_id;
        std::u8string content_relative_path;
        std::string byte_identity;
        std::uint64_t source_offset = 0u;
        std::vector<std::uint8_t> bytes;
        std::array<std::optional<ProgramBank>, maximum_mlt_banks> program_banks;
        std::array<std::optional<SequenceBank>, maximum_mlt_banks> sequence_banks;
        std::array<std::optional<NativePortSoundPcmStreamRingConfig>,
                   maximum_mlt_banks>
            authored_pcm_stream_rings;
        std::array<EffectOutput, maximum_effect_buses> effect_outputs{};
        bool effect_program_present = false;
        bool effect_output_present = false;
        bool effect_work_present = false;
        std::uint32_t effect_work_area_bytes = 0u;
        std::optional<std::uint8_t> effect_bank;
        std::vector<double> effect_sends;
        std::vector<std::int32_t> effect_kernel_inputs;
        std::vector<std::int32_t> effect_kernel_outputs;
        EffectKernelState effect_kernel_state;
        std::unordered_map<std::uint64_t,
                           std::shared_ptr<std::vector<std::int16_t>>>
            sample_cache;
        std::uint64_t decoded_sample_frames = 0u;
    };

    struct Channel final {
        std::uint8_t bank = 0u;
        std::uint8_t program = 0u;
        std::uint8_t volume = 127u;
        std::uint8_t expression = 127u;
        std::uint8_t pan = 64u;
        std::uint8_t modulation = 0u;
        std::uint8_t pressure = 127u;
        std::uint8_t effect_depth = 127u;
        std::uint8_t qsound_position = 64u;
        std::int8_t pitch = 0;
        std::optional<std::uint16_t> filter_level3;
        bool sustain = false;
    };

    struct VoiceControls final {
        float gain = 1.0f;
        float pan = 0.0f;
        std::int16_t pitch_bend = 0;
        std::uint8_t direct_level = 127u;
        std::uint8_t effect_level = 127u;
    };

    struct ActiveSequence final {
        NativePortSoundCollectionHandle collection;
        NativePortSoundSequenceConfig config;
        NativePortSoundSequenceState state = NativePortSoundSequenceState::Playing;
        std::array<Channel, maximum_midi_channels> channels{};
        std::size_t next_event = 0u;
        double tick = 0.0;
        std::uint32_t tempo = 0u;
        bool loop_marker_open = false;
        std::size_t loop_event = 0u;
        std::uint64_t loop_tick = 0u;
        std::uint32_t loop_tempo = 0u;
        std::uint64_t rendered_frames = 0u;
        std::uint64_t dispatched_events = 0u;
        std::uint64_t loop_count = 0u;
    };

    struct SynthVoice final {
        NativePortSoundCollectionHandle collection;
        std::optional<NativePortSoundSequenceHandle> sequence;
        std::shared_ptr<std::vector<std::int16_t>> sample;
        const Split* split = nullptr;
        std::uint8_t channel = 0u;
        std::uint8_t note = 0u;
        std::uint8_t velocity = 0u;
        double phase = 0.0;
        double base_step = 1.0;
        double lfo_step = 0.0;
        std::array<double, 256u> lfo_pitch_ratio{};
        std::int16_t cached_external_pitch_bend = 0;
        std::int8_t cached_channel_pitch = 0;
        std::uint8_t cached_modulation = 0u;
        bool pitch_cache_valid = false;
        bool lfo_pitch_cache_valid = false;
        std::uint8_t bend_high = 0u;
        std::uint8_t bend_low = 0u;
        // AICA amplitude attenuation is logarithmic: 0 is full scale and
        // 1023 is silence. Keeping that unit avoids a host-linear envelope
        // that changes the authored attack and decay shapes.
        double amplitude_attenuation = 640.0;
        double stage_step = 0.0;
        double decay_target = 0.0;
        EnvelopeStage envelope_stage = EnvelopeStage::Attack;
        double lfo_phase = 0.0;
        double filter_level = 0.0;
        double filter_target = 0.0;
        double filter_step = 0.0;
        EnvelopeStage filter_stage = EnvelopeStage::Attack;
        double filter_previous_1 = 0.0;
        double filter_previous_2 = 0.0;
        double filter_a0 = 0.0;
        double filter_b1 = 0.0;
        double filter_b2 = 0.0;
        std::uint32_t filter_coefficient_value = 0u;
        bool filter_coefficient_cache_valid = false;
        std::uint64_t release_tick = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t delayed_until_tick = 0u;
        float velocity_gain = 1.0f;
        std::shared_ptr<VoiceControls> controls;
        float cached_external_pan = 0.0f;
        double cached_pan_left = 1.0;
        double cached_pan_right = 1.0;
        std::uint8_t cached_channel_pan = 0u;
        std::uint8_t cached_qsound_position = 0u;
        bool pan_cache_valid = false;
        bool key_released = false;
        bool sustained = false;
        std::uint64_t start_serial = 0u;
    };

    struct VoiceGroup final {
        NativePortSoundCollectionHandle collection;
        std::shared_ptr<VoiceControls> controls;
        std::vector<NativePortSoundVoiceHandle> members;
    };

    struct MidiPort final {
        NativePortSoundCollectionHandle collection;
        NativePortSoundMidiPortConfig config;
        std::shared_ptr<VoiceControls> controls;
        std::array<std::optional<NativePortSoundVoiceHandle>, 128u> notes;
        std::optional<NativePortSoundSequenceHandle> sequence;
    };

    template <typename T> struct Slot final {
        std::uint32_t generation = 1u;
        std::optional<T> value;
    };

  public:
    Core(NativePortAudioEngine& audio,
         const NativePortSoundBankConfig& config)
        : audio_(audio), config_(config),
          owner_thread_(std::this_thread::get_id()) {
        validate_config(config_);
        sequences_.reserve(config_.maximum_active_sequences);
        active_sequence_indices_.reserve(config_.maximum_active_sequences);
        active_sequence_positions_.reserve(config_.maximum_active_sequences);
        voices_.reserve(config_.maximum_synth_voices);
        active_voice_indices_.reserve(config_.maximum_synth_voices);
        active_voice_positions_.reserve(config_.maximum_synth_voices);
        mix_.resize(static_cast<std::size_t>(config_.render_block_frames) * 2u);
    }

    ~Core() = default;

    [[nodiscard]] NativePortSoundCollectionHandle load_collection(
        const NativePortContentFileBinding& binding,
        std::vector<std::byte> immutable_bytes) {
        require_owner_thread();
        if (immutable_bytes.size() != binding.byte_size ||
            immutable_bytes.size() < 32u ||
            immutable_bytes.size() > config_.maximum_collection_bytes ||
            immutable_bytes.size() >
                config_.maximum_total_collection_bytes -
                    resident_collection_bytes_ ||
            immutable_bytes.size() >
                std::numeric_limits<std::size_t>::max())
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "collection-size");
        Collection collection;
        collection.logical_id = binding.logical_id;
        collection.content_relative_path =
            binding.content_relative_path.generic_u8string();
        collection.byte_identity = binding.byte_identity;
        collection.source_offset = binding.source_offset;
        collection.bytes.resize(immutable_bytes.size());
        std::memcpy(collection.bytes.data(), immutable_bytes.data(),
                    immutable_bytes.size());
        parse_collection(collection);
        const auto collection_bytes = collection.bytes.size();
        const auto handle = insert_collection(std::move(collection));
        resident_collection_bytes_ += collection_bytes;
        saturating_add(loaded_collections_, 1u);
        return handle;
    }

    void unload_collection(const NativePortSoundCollectionHandle handle) {
        require_owner_thread();
        auto& slot = require_collection_slot(handle);
        if (collection_in_use(handle))
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "collection-in-use");
        resident_collection_bytes_ -= slot.value->bytes.size();
        slot.value.reset();
        bump_generation(slot.generation);
        saturating_add(unloaded_collections_, 1u);
    }

    [[nodiscard]] bool has_program(
        const NativePortSoundCollectionHandle handle,
        const std::uint8_t bank,
        const std::uint8_t program) const {
        require_owner_thread();
        const auto& collection = require_collection(handle);
        return bank < maximum_mlt_banks &&
               collection.program_banks[bank].has_value() &&
               program < collection.program_banks[bank]->programs.size();
    }

    [[nodiscard]] bool has_sequence(
        const NativePortSoundCollectionHandle handle,
        const std::uint8_t bank,
        const std::uint16_t sequence) const {
        require_owner_thread();
        const auto& collection = require_collection(handle);
        return bank < maximum_mlt_banks &&
               collection.sequence_banks[bank].has_value() &&
               sequence < collection.sequence_banks[bank]->sequences.size() &&
               collection.sequence_banks[bank]->sequences[sequence].has_value();
    }

    [[nodiscard]] bool has_collection_unit(
        const NativePortSoundCollectionHandle handle,
        const NativePortSoundCollectionUnitKind kind,
        const std::uint8_t bank) const {
        require_owner_thread();
        const auto& collection = require_collection(handle);
        switch (kind) {
        case NativePortSoundCollectionUnitKind::ProgramBank:
            return bank < maximum_mlt_banks &&
                   collection.program_banks[bank].has_value();
        case NativePortSoundCollectionUnitKind::SequenceBank:
            return bank < maximum_mlt_banks &&
                   collection.sequence_banks[bank].has_value();
        case NativePortSoundCollectionUnitKind::OneShotBank:
            // SOSB decoding is deliberately not inferred from SMPB.  A
            // collection cannot advertise a usable native one-shot bank
            // until its distinct payload contract has been parsed.
            return false;
        case NativePortSoundCollectionUnitKind::PcmStreamRing:
            return bank < maximum_mlt_banks &&
                   collection.authored_pcm_stream_rings[bank].has_value();
        case NativePortSoundCollectionUnitKind::EffectProgramBank:
            return collection.effect_program_present &&
                   collection.effect_bank == bank;
        case NativePortSoundCollectionUnitKind::EffectOutputBank:
            return collection.effect_output_present &&
                   collection.effect_bank == bank;
        case NativePortSoundCollectionUnitKind::EffectProgramWork:
            return collection.effect_work_present &&
                   collection.effect_bank == bank;
        }
        return false;
    }

    [[nodiscard]] NativePortSoundPcmStreamRingHandle bind_pcm_stream_ring(
        const NativePortSoundPcmStreamRingConfig& config) {
        require_owner_thread();
        if (config.bank >= maximum_mlt_banks || config.byte_size == 0u ||
            (config.layout_offset & 3u) != 0u ||
            (config.byte_size & 3u) != 0u ||
            config.layout_offset > native_port_manatee_sound_layout_bytes ||
            config.byte_size >
                native_port_manatee_sound_layout_bytes - config.layout_offset)
            fail_sound_bank(NativePortSoundBankFailure::InvalidPcmStreamRing,
                            "pcm-ring-range");

        auto& slot = pcm_stream_rings_[config.bank];
        if (slot.value.has_value()) {
            const auto& current = *slot.value;
            if (current.bank == config.bank &&
                current.layout_offset == config.layout_offset &&
                current.byte_size == config.byte_size)
                return {config.bank, slot.generation};
            fail_sound_bank(NativePortSoundBankFailure::InvalidPcmStreamRing,
                            "pcm-ring-bank-bound");
        }

        const auto begin = static_cast<std::uint64_t>(config.layout_offset);
        const auto end = begin + config.byte_size;
        for (const auto& candidate : pcm_stream_rings_) {
            if (!candidate.value.has_value()) continue;
            const auto candidate_begin =
                static_cast<std::uint64_t>(candidate.value->layout_offset);
            const auto candidate_end =
                candidate_begin + candidate.value->byte_size;
            if (begin < candidate_end && candidate_begin < end)
                fail_sound_bank(
                    NativePortSoundBankFailure::InvalidPcmStreamRing,
                    "pcm-ring-overlap");
        }
        slot.value = config;
        return {config.bank, slot.generation};
    }

    void release_pcm_stream_ring(
        const NativePortSoundPcmStreamRingHandle handle) {
        require_owner_thread();
        auto& slot = require_pcm_stream_ring_slot(handle);
        slot.value.reset();
        bump_generation(slot.generation);
    }

    [[nodiscard]] NativePortSoundPcmStreamRingSnapshot
    pcm_stream_ring_snapshot(
        const NativePortSoundPcmStreamRingHandle handle) const {
        require_owner_thread();
        const auto& slot = require_pcm_stream_ring_slot(handle);
        return {*slot.value, true};
    }

    void predecode_collection_samples(
        const NativePortSoundCollectionHandle handle) {
        require_owner_thread();
        auto& collection = require_collection(handle);
        for (auto& optional_bank : collection.program_banks) {
            if (!optional_bank.has_value()) continue;
            auto& bank = *optional_bank;
            for (const auto& program : bank.programs)
                for (const auto& layer : program.layers)
                    for (const auto& split : layer.splits) {
                        if (split.tone_offset == 0u || split.loop_end == 0u)
                            continue;
                        static_cast<void>(decode_sample(
                            collection, bank, split));
                    }
        }
    }

  private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            fail_sound_bank(NativePortSoundBankFailure::ThreadViolation,
                            "thread");
    }

    static void bump_generation(std::uint32_t& generation) noexcept {
        ++generation;
        if (generation == 0u) generation = 1u;
    }

    static constexpr std::size_t invalid_active_position =
        std::numeric_limits<std::size_t>::max();

    static void activate_slot(std::vector<std::uint32_t>& active,
                              std::vector<std::size_t>& positions,
                              const std::size_t slot) {
        if (slot >= positions.size()) positions.resize(slot + 1u, invalid_active_position);
        if (positions[slot] != invalid_active_position) return;
        const auto insertion = std::lower_bound(
            active.begin(), active.end(), static_cast<std::uint32_t>(slot));
        const auto position = static_cast<std::size_t>(insertion - active.begin());
        active.insert(insertion, static_cast<std::uint32_t>(slot));
        for (std::size_t index = position; index < active.size(); ++index)
            positions[active[index]] = index;
    }

    static void deactivate_slot(std::vector<std::uint32_t>& active,
                                std::vector<std::size_t>& positions,
                                const std::size_t slot) noexcept {
        if (slot >= positions.size()) return;
        const auto position = positions[slot];
        if (position == invalid_active_position || position >= active.size()) return;
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(position));
        positions[slot] = invalid_active_position;
        for (std::size_t index = position; index < active.size(); ++index)
            positions[active[index]] = index;
    }

    void erase_sequence_slot(const std::size_t index) noexcept {
        if (index >= sequences_.size() || !sequences_[index].value.has_value()) return;
        sequences_[index].value.reset();
        bump_generation(sequences_[index].generation);
        deactivate_slot(active_sequence_indices_, active_sequence_positions_, index);
    }

    void erase_voice_slot(const std::size_t index) noexcept {
        if (index >= voices_.size() || !voices_[index].value.has_value()) return;
        voices_[index].value.reset();
        bump_generation(voices_[index].generation);
        deactivate_slot(active_voice_indices_, active_voice_positions_, index);
    }

    [[nodiscard]] NativePortSoundCollectionHandle insert_collection(
        Collection collection) {
        std::size_t index = collections_.size();
        for (std::size_t candidate = 0u; candidate < collections_.size();
             ++candidate) {
            if (!collections_[candidate].value.has_value()) {
                index = candidate;
                break;
            }
        }
        if (index == collections_.size()) {
            if (collections_.size() == config_.maximum_collections)
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "collection-limit");
            collections_.push_back({});
        }
        collections_[index].value.emplace(std::move(collection));
        return {static_cast<std::uint32_t>(index),
                collections_[index].generation};
    }

    [[nodiscard]] Slot<Collection>& require_collection_slot(
        const NativePortSoundCollectionHandle handle) {
        if (!handle || handle.slot >= collections_.size() ||
            collections_[handle.slot].generation != handle.generation ||
            !collections_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "collection-handle");
        return collections_[handle.slot];
    }

    [[nodiscard]] const Slot<Collection>& require_collection_slot(
        const NativePortSoundCollectionHandle handle) const {
        if (!handle || handle.slot >= collections_.size() ||
            collections_[handle.slot].generation != handle.generation ||
            !collections_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "collection-handle");
        return collections_[handle.slot];
    }

    [[nodiscard]] Collection& require_collection(
        const NativePortSoundCollectionHandle handle) {
        return *require_collection_slot(handle).value;
    }

    [[nodiscard]] const Collection& require_collection(
        const NativePortSoundCollectionHandle handle) const {
        return *require_collection_slot(handle).value;
    }

    [[nodiscard]] Slot<NativePortSoundPcmStreamRingConfig>&
    require_pcm_stream_ring_slot(
        const NativePortSoundPcmStreamRingHandle handle) {
        if (!handle || handle.slot >= pcm_stream_rings_.size() ||
            pcm_stream_rings_[handle.slot].generation != handle.generation ||
            !pcm_stream_rings_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "pcm-ring-handle");
        return pcm_stream_rings_[handle.slot];
    }

    [[nodiscard]] const Slot<NativePortSoundPcmStreamRingConfig>&
    require_pcm_stream_ring_slot(
        const NativePortSoundPcmStreamRingHandle handle) const {
        if (!handle || handle.slot >= pcm_stream_rings_.size() ||
            pcm_stream_rings_[handle.slot].generation != handle.generation ||
            !pcm_stream_rings_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "pcm-ring-handle");
        return pcm_stream_rings_[handle.slot];
    }

    [[nodiscard]] bool collection_in_use(
        const NativePortSoundCollectionHandle handle) const noexcept {
        for (const auto index : active_sequence_indices_)
            if (sequences_[index].value->collection.slot == handle.slot &&
                sequences_[index].value->collection.generation == handle.generation)
                return true;
        for (const auto index : active_voice_indices_)
            if (voices_[index].value->collection.slot == handle.slot &&
                voices_[index].value->collection.generation == handle.generation)
                return true;
        for (const auto& slot : groups_)
            if (slot.value.has_value() &&
                slot.value->collection.slot == handle.slot &&
                slot.value->collection.generation == handle.generation)
                return true;
        for (const auto& slot : ports_)
            if (slot.value.has_value() &&
                slot.value->collection.slot == handle.slot &&
                slot.value->collection.generation == handle.generation)
                return true;
        return false;
    }

    void parse_collection(Collection& collection) {
        const BoundedBytes bytes(collection.bytes);
        if (!bytes.magic(0u, "SMLT") || bytes.u32(
                4u, NativePortSoundBankFailure::InvalidMlt, "mlt-version") !=
                0x101u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidMlt,
                            "mlt-header");
        const auto unit_count = bytes.u32(
            8u, NativePortSoundBankFailure::InvalidMlt, "mlt-unit-count");
        if (unit_count > config_.maximum_units_per_collection)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "mlt-unit-limit");
        bytes.extent(32u,
                     static_cast<std::uint64_t>(unit_count) * 32u,
                     NativePortSoundBankFailure::InvalidMlt,
                     "mlt-unit-table");
        std::vector<std::pair<std::uint32_t, std::uint32_t>> unit_regions;
        unit_regions.reserve(unit_count);
        for (std::uint32_t unit = 0u; unit < unit_count; ++unit) {
            const auto record = 32u + static_cast<std::uint64_t>(unit) * 32u;
            const auto bank = bytes.u8(record + 4u,
                                       NativePortSoundBankFailure::InvalidMlt,
                                       "mlt-bank");
            const auto layout_offset = bytes.u32(
                record + 8u,
                NativePortSoundBankFailure::InvalidMlt,
                "mlt-layout-offset");
            const auto layout_size = bytes.u32(
                record + 12u,
                NativePortSoundBankFailure::InvalidMlt,
                "mlt-layout-size");
            const auto file_offset = bytes.u32(
                record + 16u,
                NativePortSoundBankFailure::InvalidMlt,
                "mlt-file-offset");
            const auto file_size = bytes.u32(
                record + 20u,
                NativePortSoundBankFailure::InvalidMlt,
                "mlt-file-size");
            const bool offset_present = file_offset != 0xFFFFFFFFu;
            const bool size_present = file_size != 0xFFFFFFFFu;
            if (offset_present != size_present)
                fail_sound_bank(NativePortSoundBankFailure::InvalidMlt,
                                "mlt-payload-presence");
            const bool has_payload = offset_present;
            const bool program = bytes.magic(record, "SMPB");
            const bool sequence = bytes.magic(record, "SMSB");
            const bool one_shot = bytes.magic(record, "SOSB");
            const bool stream_ring = bytes.magic(record, "SPSR");
            const bool effect_program = bytes.magic(record, "SFPB");
            const bool effect_output = bytes.magic(record, "SFOB");
            const bool effect_work = bytes.magic(record, "SFPW");
            if (!(program || sequence || one_shot || stream_ring ||
                  effect_program || effect_output || effect_work))
                fail_sound_bank(NativePortSoundBankFailure::UnsupportedUnit,
                                "mlt-unit-type");
            if (bank >= maximum_mlt_banks || layout_size == 0u ||
                (layout_offset & 3u) != 0u || (layout_size & 3u) != 0u ||
                layout_offset > native_port_manatee_sound_layout_bytes ||
                layout_size >
                    native_port_manatee_sound_layout_bytes - layout_offset)
                fail_sound_bank(NativePortSoundBankFailure::InvalidMlt,
                                "mlt-layout-range");
            const auto layout_end = layout_offset + layout_size;
            for (const auto [existing_begin, existing_end] : unit_regions)
                if (layout_offset < existing_end &&
                    existing_begin < layout_end)
                    fail_sound_bank(NativePortSoundBankFailure::InvalidMlt,
                                    "mlt-layout-overlap");
            unit_regions.emplace_back(layout_offset, layout_end);

            if (stream_ring) {
                if ((has_payload && file_size != 0u) ||
                    collection.authored_pcm_stream_rings[bank].has_value())
                    fail_sound_bank(
                        NativePortSoundBankFailure::InvalidPcmStreamRing,
                        "psr-unit");
                collection.authored_pcm_stream_rings[bank] =
                    NativePortSoundPcmStreamRingConfig{
                        bank, layout_offset, layout_size};
                continue;
            }
            if (effect_work) {
                if (has_payload && file_size != 0u)
                    fail_sound_bank(NativePortSoundBankFailure::UnsupportedUnit,
                                    "fpw-payload");
                bind_effect_unit(collection,
                                 bank,
                                 collection.effect_work_present,
                                 "fpw-duplicate");
                collection.effect_work_present = true;
                collection.effect_work_area_bytes = layout_size;
                continue;
            }
            if (!has_payload) continue;
            const auto payload = bytes.span(
                file_offset,
                file_size,
                NativePortSoundBankFailure::InvalidMlt,
                "mlt-payload");
            const BoundedBytes payload_bytes(payload);
            if (program) {
                if (collection.program_banks[bank].has_value())
                    fail_sound_bank(
                        NativePortSoundBankFailure::InvalidProgramBank,
                        "mpb-bank");
                collection.program_banks[bank] =
                    parse_program_bank(payload, file_offset, file_size, bank);
            } else if (sequence) {
                if (collection.sequence_banks[bank].has_value())
                    fail_sound_bank(
                        NativePortSoundBankFailure::InvalidSequenceBank,
                        "msb-bank");
                collection.sequence_banks[bank] = parse_sequence_bank(payload);
            } else if (effect_program) {
                bind_effect_unit(collection,
                                 bank,
                                 collection.effect_program_present,
                                 "fpb-duplicate");
                parse_effect_program(collection, payload_bytes);
                collection.effect_program_present = true;
            } else if (effect_output) {
                bind_effect_unit(collection,
                                 bank,
                                 collection.effect_output_present,
                                 "fob-duplicate");
                parse_effect_output(collection, payload_bytes);
                collection.effect_output_present = true;
            } else {
                // SOSB has a distinct one-shot payload ABI.  Accept its
                // payloadless layout reservation, but never reinterpret a
                // populated one as an SMPB program bank.
                fail_sound_bank(NativePortSoundBankFailure::UnsupportedUnit,
                                "osb-payload");
            }
        }
        const auto effect_unit_count =
            static_cast<std::uint32_t>(collection.effect_program_present) +
            static_cast<std::uint32_t>(collection.effect_output_present) +
            static_cast<std::uint32_t>(collection.effect_work_present);
        if (effect_unit_count != 0u && effect_unit_count != 3u)
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "effect-unit-family");
        if (collection.effect_kernel_state) {
            if (collection.effect_kernel_state.kernel
                    ->required_work_area_bytes !=
                collection.effect_work_area_bytes)
                fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                                "effect-work-area");
            collection.effect_sends.resize(
                static_cast<std::size_t>(config_.render_block_frames) *
                maximum_effect_buses);
            collection.effect_kernel_inputs.resize(
                static_cast<std::size_t>(config_.render_block_frames) *
                maximum_effect_buses);
            collection.effect_kernel_outputs.resize(
                static_cast<std::size_t>(config_.render_block_frames) *
                maximum_effect_buses);
        }
    }

    static void bind_effect_unit(Collection& collection,
                                 const std::uint8_t bank,
                                 const bool duplicate,
                                 const std::string_view operation) {
        if (bank >= maximum_mlt_banks || duplicate ||
            (collection.effect_bank.has_value() &&
             *collection.effect_bank != bank))
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            operation);
        collection.effect_bank = bank;
    }

    [[nodiscard]] ProgramBank parse_program_bank(
        const std::span<const std::uint8_t> payload,
        const std::uint32_t payload_offset,
        const std::uint32_t payload_size,
        const std::uint8_t bank) {
        const BoundedBytes bytes(payload);
        if (!bytes.magic(0u, "SMPB") ||
            bytes.u32(4u,
                      NativePortSoundBankFailure::InvalidProgramBank,
                      "mpb-version") != 1u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-header");
        const auto programs_offset = bytes.u32(
            16u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-program-offset");
        const auto program_count = bytes.u32(
            20u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-program-count");
        const auto velocities_offset = bytes.u32(
            24u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-velocity-offset");
        const auto velocity_count = bytes.u32(
            28u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-velocity-count");
        if (program_count > maximum_mpb_programs ||
            velocity_count > maximum_mpb_velocity_curves)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "mpb-table-limit");
        bytes.extent(programs_offset,
                     static_cast<std::uint64_t>(program_count) * 4u,
                     NativePortSoundBankFailure::InvalidProgramBank,
                     "mpb-program-table");
        bytes.extent(velocities_offset,
                     static_cast<std::uint64_t>(velocity_count) * 128u,
                     NativePortSoundBankFailure::InvalidProgramBank,
                     "mpb-velocity-table");
        ProgramBank result;
        result.payload_offset = payload_offset;
        result.payload_size = payload_size;
        result.bank = bank;
        result.programs.resize(program_count);
        result.velocity_curves.resize(velocity_count);
        for (std::uint32_t curve = 0u; curve < velocity_count; ++curve) {
            const auto source = bytes.span(
                velocities_offset + static_cast<std::uint64_t>(curve) * 128u,
                128u,
                NativePortSoundBankFailure::InvalidProgramBank,
                "mpb-velocity-curve");
            std::copy(source.begin(),
                      source.end(),
                      result.velocity_curves[curve].begin());
        }
        for (std::uint32_t program_index = 0u;
             program_index < program_count;
             ++program_index) {
            const auto program_offset = bytes.u32(
                programs_offset + static_cast<std::uint64_t>(program_index) * 4u,
                NativePortSoundBankFailure::InvalidProgramBank,
                "mpb-program-pointer");
            if (program_offset == 0u) continue;
            bytes.extent(program_offset,
                         maximum_mpb_layers * 4u,
                         NativePortSoundBankFailure::InvalidProgramBank,
                         "mpb-program");
            auto& program = result.programs[program_index];
            for (std::uint32_t layer_index = 0u;
                 layer_index < maximum_mpb_layers;
                 ++layer_index) {
                const auto layer_offset = bytes.u32(
                    program_offset + static_cast<std::uint64_t>(layer_index) * 4u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-layer-pointer");
                if (layer_offset == 0u) continue;
                bytes.extent(layer_offset,
                             16u,
                             NativePortSoundBankFailure::InvalidProgramBank,
                             "mpb-layer");
                const auto split_count = bytes.u32(
                    layer_offset,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-split-count");
                const auto split_offset = bytes.u32(
                    layer_offset + 4u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-split-pointer");
                if (split_count > maximum_mpb_splits)
                    fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                    "mpb-split-limit");
                bytes.extent(split_offset,
                             static_cast<std::uint64_t>(split_count) * 48u,
                             NativePortSoundBankFailure::InvalidProgramBank,
                             "mpb-splits");
                Layer layer;
                layer.delay = bytes.u16(
                    layer_offset + 8u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-layer-delay");
                const auto layer_unknown_1 = bytes.u16(
                    layer_offset + 10u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-layer-unknown-1");
                layer.bend_high = bytes.u8(
                    layer_offset + 12u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-bend-high");
                layer.bend_low = bytes.u8(
                    layer_offset + 13u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-bend-low");
                const auto layer_unknown_2 = bytes.u16(
                    layer_offset + 14u,
                    NativePortSoundBankFailure::InvalidProgramBank,
                    "mpb-layer-unknown-2");
                if (layer_unknown_1 != 0u || layer_unknown_2 != 0u)
                    fail_sound_bank(
                        NativePortSoundBankFailure::InvalidProgramBank,
                        "mpb-layer-unknown-contract");
                layer.splits.reserve(split_count);
                for (std::uint32_t split_index = 0u;
                     split_index < split_count;
                     ++split_index) {
                    layer.splits.push_back(parse_split(
                        bytes,
                        split_offset +
                            static_cast<std::uint64_t>(split_index) * 48u,
                        payload.size(),
                        velocity_count));
                    saturating_add(parsed_splits_, 1u);
                }
                program.layers.push_back(std::move(layer));
            }
            if (!program.layers.empty()) saturating_add(parsed_programs_, 1u);
        }
        return result;
    }

    [[nodiscard]] Split parse_split(const BoundedBytes& bytes,
                                    const std::uint64_t offset,
                                    const std::size_t payload_size,
                                    const std::uint32_t velocity_count) const {
        Split result;
        const auto jump = bytes.u8(
            offset,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-split-jump");
        const auto flags = bytes.u8(
            offset + 1u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-split-flags");
        if ((flags & 0xFCu) != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-split-flags-unknown");
        result.format = (flags & 1u) != 0u
                            ? SampleFormat::Adpcm4
                            : (jump & 0x80u) != 0u ? SampleFormat::Pcm8
                                                   : SampleFormat::Pcm16;
        result.tone_offset = bytes.u16(
                                 offset + 2u,
                                 NativePortSoundBankFailure::InvalidProgramBank,
                                 "mpb-tone-offset") |
                             (static_cast<std::uint32_t>(jump & 0x7Fu) << 16u);
        result.loop_start = bytes.u16(
            offset + 4u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-loop-start");
        result.loop_end = bytes.u16(
            offset + 6u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-loop-end");
        result.loop = (flags & 2u) != 0u;
        if (result.loop && result.loop_start >= result.loop_end)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSample,
                            "mpb-loop-range");
        const auto amp = bytes.u32(
            offset + 8u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-amp");
        if ((amp & 0x80000020u) != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-amp-reserved");
        result.attack = static_cast<std::uint8_t>(amp & 31u);
        result.decay1 = static_cast<std::uint8_t>((amp >> 6u) & 31u);
        result.decay2 = static_cast<std::uint8_t>((amp >> 11u) & 31u);
        result.release = static_cast<std::uint8_t>((amp >> 16u) & 31u);
        result.decay_level = static_cast<std::uint8_t>((amp >> 21u) & 31u);
        result.key_rate_scaling = static_cast<std::uint8_t>((amp >> 26u) & 15u);
        result.loop_start_link = ((amp >> 30u) & 1u) != 0u;
        const auto pitch = bytes.u16(
            offset + 12u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-pitch");
        if ((pitch & 0x0400u) != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-pitch-reserved");
        result.pitch_fns = static_cast<std::uint16_t>(pitch & 0x3FFu);
        auto octave = static_cast<std::int8_t>((pitch >> 11u) & 15u);
        if ((octave & 8) != 0) octave = static_cast<std::int8_t>(octave - 16);
        result.pitch_octave = octave;
        const auto lfo = bytes.u16(
            offset + 14u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-lfo");
        result.lfo_amp_depth = static_cast<std::uint8_t>(lfo & 7u);
        result.lfo_amp_wave = static_cast<std::uint8_t>((lfo >> 3u) & 3u);
        result.lfo_pitch_depth = static_cast<std::uint8_t>((lfo >> 5u) & 7u);
        result.lfo_pitch_wave = static_cast<std::uint8_t>((lfo >> 8u) & 3u);
        result.lfo_frequency = static_cast<std::uint8_t>((lfo >> 10u) & 31u);
        result.lfo_sync = ((lfo >> 15u) & 1u) != 0u;
        const auto effect = bytes.u8(
            offset + 16u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-effect");
        result.effect_bus = static_cast<std::uint8_t>(effect & 15u);
        result.effect_level = static_cast<std::uint8_t>(effect >> 4u);
        if (bytes.u8(offset + 17u,
                     NativePortSoundBankFailure::InvalidProgramBank,
                     "mpb-unknown-1") != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-unknown-1-contract");
        result.pan = bytes.u8(offset + 18u,
                              NativePortSoundBankFailure::InvalidProgramBank,
                              "mpb-pan");
        result.direct_level = bytes.u8(
            offset + 19u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-direct-level");
        const auto filter = bytes.u8(
            offset + 20u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-filter");
        result.filter_resonance = static_cast<std::uint8_t>(filter & 31u);
        result.filter = (filter & 0x20u) == 0u;
        result.voice_attenuation_off = (filter & 0x40u) != 0u;
        if ((filter & 0x80u) != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-filter-reserved");
        result.oscillator_level = static_cast<std::uint8_t>(~bytes.u8(
            offset + 21u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-oscillator-level"));
        for (std::size_t index = 0u; index < result.filter_levels.size(); ++index) {
            result.filter_levels[index] = bytes.u16(
                offset + 22u + index * 2u,
                NativePortSoundBankFailure::InvalidProgramBank,
                "mpb-filter-level");
            if (result.filter_levels[index] > 8184u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                                "mpb-filter-level-range");
        }
        for (std::size_t index = 0u; index < result.filter_rates.size(); ++index) {
            result.filter_rates[index] = bytes.u8(
                offset + 32u + index,
                NativePortSoundBankFailure::InvalidProgramBank,
                "mpb-filter-rate");
            if (result.filter_rates[index] > 31u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                                "mpb-filter-rate-range");
        }
        result.start_note = bytes.u8(
            offset + 36u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-start-note");
        result.end_note = bytes.u8(
            offset + 37u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-end-note");
        result.base_note = bytes.u8(
            offset + 38u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-base-note");
        result.fine_tune = bytes.i8(
            offset + 39u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-fine-tune");
        if (bytes.u16(offset + 40u,
                      NativePortSoundBankFailure::InvalidProgramBank,
                      "mpb-unknown-2") != 0xFFFFu)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-unknown-2-contract");
        result.velocity_curve = bytes.u8(
            offset + 42u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-velocity-curve");
        result.velocity_low = bytes.u8(
            offset + 43u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-velocity-low");
        result.velocity_high = bytes.u8(
            offset + 44u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-velocity-high");
        const auto drum = bytes.u8(offset + 45u,
                                   NativePortSoundBankFailure::InvalidProgramBank,
                                   "mpb-drum");
        if (drum > 1u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-drum-contract");
        result.drum = drum != 0u;
        result.drum_group = bytes.u8(
            offset + 46u,
            NativePortSoundBankFailure::InvalidProgramBank,
            "mpb-drum-group");
        if (bytes.u8(offset + 47u,
                     NativePortSoundBankFailure::InvalidProgramBank,
                     "mpb-unknown-3") != 0u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-unknown-3-contract");
        if (result.start_note > result.end_note || result.end_note > 127u ||
            result.base_note > 127u ||
            result.velocity_low > result.velocity_high ||
            result.velocity_high > 127u ||
            (velocity_count == 0u ? result.velocity_curve != 0u
                                  : result.velocity_curve >= velocity_count) ||
            result.pan > 31u || result.direct_level > 15u ||
            result.loop_start > result.loop_end)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "mpb-split-contract");
        if (result.tone_offset != 0u && result.loop_end != 0u) {
            const auto required = result.format == SampleFormat::Adpcm4
                                      ? (result.loop_end + 1u) / 2u
                                  : result.format == SampleFormat::Pcm8
                                      ? result.loop_end
                                      : static_cast<std::uint64_t>(result.loop_end) * 2u;
            if (result.tone_offset > payload_size ||
                required > payload_size - result.tone_offset)
                fail_sound_bank(NativePortSoundBankFailure::InvalidSample,
                                "mpb-tone-extent");
        }
        return result;
    }

    struct SequenceParser final {
        Core& owner;
        BoundedBytes bytes;
        std::uint64_t current_tick = 0u;
        std::uint32_t gate_extension = 0u;
        std::uint32_t step_extension = 0u;
        std::uint32_t event_budget = 0u;
        std::vector<Event> events;

        [[nodiscard]] std::pair<std::uint32_t, std::uint64_t> read_variable(
            const std::uint8_t first,
            const std::uint64_t cursor) const {
            if ((first & 0x80u) != 0u)
                return {bytes.be16(cursor,
                                   NativePortSoundBankFailure::InvalidSequence,
                                   "msd-variable16"),
                        cursor + 2u};
            return {bytes.u8(cursor,
                             NativePortSoundBankFailure::InvalidSequence,
                             "msd-variable8"),
                    cursor + 1u};
        }

        [[nodiscard]] std::pair<std::uint64_t, bool> parse_one(
            std::uint64_t cursor,
            const std::uint32_t depth,
            const bool reference) {
            if (event_budget == 0u ||
                depth > owner.config_.maximum_reference_depth)
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "msd-event-budget");
            --event_budget;
            const auto status_byte = bytes.u8(
                cursor++,
                NativePortSoundBankFailure::InvalidSequence,
                "msd-status");
            const auto status = static_cast<std::uint8_t>(status_byte & 0xF0u);
            const auto channel = static_cast<std::uint8_t>(status_byte & 15u);
            if (status <= 0x70u) {
                Event event;
                event.kind = EventKind::Note;
                event.tick = current_tick;
                event.channel = channel;
                event.data0 = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-note");
                event.data1 = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-velocity");
                const auto gate_bytes = static_cast<std::uint32_t>(status >> 5u) + 1u;
                std::uint32_t gate = 0u;
                for (std::uint32_t index = 0u; index < gate_bytes; ++index)
                    gate = (gate << 8u) |
                           bytes.u8(cursor++,
                                    NativePortSoundBankFailure::InvalidSequence,
                                    "msd-gate");
                std::uint32_t step = 0u;
                if ((status & 0x10u) != 0u) {
                    step = bytes.be16(
                        cursor,
                        NativePortSoundBankFailure::InvalidSequence,
                        "msd-note-step16");
                    cursor += 2u;
                } else {
                    step = bytes.u8(
                        cursor++,
                        NativePortSoundBankFailure::InvalidSequence,
                        "msd-note-step8");
                }
                event.gate = gate + gate_extension;
                event.step = step + step_extension;
                gate_extension = 0u;
                step_extension = 0u;
                append_event(event);
                return {cursor, true};
            }
            if (status == 0xB0u || status == 0xC0u || status == 0xD0u ||
                status == 0xE0u) {
                const auto first = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-channel-data");
                Event event;
                event.tick = current_tick;
                event.channel = channel;
                event.data0 = static_cast<std::uint8_t>(first & 0x7Fu);
                if (status == 0xB0u) {
                    event.kind = EventKind::Control;
                    event.data1 = bytes.u8(
                        cursor++,
                        NativePortSoundBankFailure::InvalidSequence,
                        "msd-controller-value");
                } else if (status == 0xC0u) {
                    event.kind = EventKind::Program;
                } else if (status == 0xD0u) {
                    event.kind = EventKind::Pressure;
                } else {
                    event.kind = EventKind::Pitch;
                }
                const auto [step, next] = read_variable(first, cursor);
                cursor = next;
                event.step = step + step_extension;
                step_extension = 0u;
                append_event(event);
                return {cursor, true};
            }
            if (status_byte == 0x81u) {
                const auto offset = bytes.be16(
                    cursor,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-reference-offset");
                const auto count = bytes.u8(
                    cursor + 2u,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-reference-count");
                cursor += 3u;
                if (count == 0u || offset >= bytes.size())
                    fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                    "msd-reference");
                std::uint64_t target = offset;
                for (std::uint32_t index = 0u; index < count; ++index) {
                    const auto [next, continued] =
                        parse_one(target, depth + 1u, true);
                    if (!continued)
                        fail_sound_bank(
                            NativePortSoundBankFailure::InvalidSequence,
                            "msd-reference-eot");
                    target = next;
                }
                return {cursor, true};
            }
            if (status_byte == 0x82u) {
                const auto first = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-loop-data");
                const auto [step, next] = read_variable(first, cursor);
                cursor = next;
                Event event;
                event.kind = EventKind::Loop;
                event.tick = current_tick;
                event.data0 = static_cast<std::uint8_t>(first & 0x7Fu);
                event.step = step + step_extension;
                step_extension = 0u;
                append_event(event);
                return {cursor, true};
            }
            if (status_byte == 0x83u) {
                if (reference)
                    fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                    "msd-reference-end");
                return {cursor, false};
            }
            if (status_byte == 0x84u) {
                Event event;
                event.kind = EventKind::Tempo;
                event.tick = current_tick;
                event.value = bytes.be16(
                    cursor,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-tempo");
                event.step = bytes.u8(
                                 cursor + 2u,
                                 NativePortSoundBankFailure::InvalidSequence,
                                 "msd-tempo-step") +
                             step_extension;
                cursor += 3u;
                step_extension = 0u;
                append_event(event);
                return {cursor, true};
            }
            if (status_byte == 0xF0u) {
                const auto first = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-sysex-step");
                const auto size = bytes.u8(
                    cursor++,
                    NativePortSoundBankFailure::InvalidSequence,
                    "msd-sysex-size");
                bytes.extent(cursor,
                             size,
                             NativePortSoundBankFailure::InvalidSequence,
                             "msd-sysex");
                cursor += size;
                const auto [step, next] = read_variable(first, cursor);
                cursor = next;
                Event event;
                event.kind = EventKind::SysEx;
                event.tick = current_tick;
                event.step = step + step_extension;
                step_extension = 0u;
                append_event(event);
                return {cursor, true};
            }
            if (status_byte >= 0x88u && status_byte <= 0x8Bu) {
                static constexpr std::array<std::uint32_t, 4u> gate_ext{
                    0x200u, 0x800u, 0x1000u, 0x2000u};
                gate_extension += gate_ext[status_byte & 3u];
                return parse_one(cursor, depth, reference);
            }
            if (status_byte >= 0x8Cu && status_byte <= 0x8Fu) {
                static constexpr std::array<std::uint32_t, 4u> step_ext{
                    0x100u, 0x200u, 0x800u, 0x1000u};
                step_extension += step_ext[status_byte & 3u];
                return parse_one(cursor, depth, reference);
            }
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "msd-unknown-status");
        }

        void append_event(const Event& event) {
            if (events.size() == owner.config_.maximum_events_per_sequence)
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "msd-event-limit");
            events.push_back(event);
            if (event.step > std::numeric_limits<std::uint64_t>::max() -
                                 current_tick)
                fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                "msd-tick-overflow");
            current_tick += event.step;
        }
    };

    [[nodiscard]] SequenceBank parse_sequence_bank(
        const std::span<const std::uint8_t> payload) {
        const BoundedBytes bytes(payload);
        if (!bytes.magic(0u, "SMSB") ||
            bytes.u32(4u,
                      NativePortSoundBankFailure::InvalidSequenceBank,
                      "msb-version") != 1u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequenceBank,
                            "msb-header");
        const auto count = bytes.u32(
            12u,
            NativePortSoundBankFailure::InvalidSequenceBank,
            "msb-count");
        if (count > config_.maximum_sequences_per_bank)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "msb-sequence-limit");
        bytes.extent(16u,
                     static_cast<std::uint64_t>(count) * 4u,
                     NativePortSoundBankFailure::InvalidSequenceBank,
                     "msb-pointer-table");
        std::vector<std::uint32_t> pointers(count);
        for (std::uint32_t index = 0u; index < count; ++index)
            pointers[index] = bytes.u32(
                16u + static_cast<std::uint64_t>(index) * 4u,
                NativePortSoundBankFailure::InvalidSequenceBank,
                "msb-sequence-pointer");
        SequenceBank result;
        result.sequences.resize(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            const auto start = pointers[index];
            if (start == 0u) continue;
            std::uint32_t end = static_cast<std::uint32_t>(payload.size());
            for (const auto candidate : pointers)
                if (candidate > start && candidate < end) end = candidate;
            if (end <= start)
                fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                "msb-sequence-range");
            const auto sequence_bytes = bytes.span(
                start,
                end - start,
                NativePortSoundBankFailure::InvalidSequence,
                "msb-sequence");
            const BoundedBytes sequence_reader(sequence_bytes);
            if (!sequence_reader.magic(0u, "SMSD"))
                fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                "msd-header");
            Sequence sequence;
            sequence.tpqn = sequence_reader.u32(
                4u,
                NativePortSoundBankFailure::InvalidSequence,
                "msd-tpqn");
            sequence.initial_tempo = sequence_reader.u32(
                8u,
                NativePortSoundBankFailure::InvalidSequence,
                "msd-initial-tempo");
            if (sequence.tpqn == 0u || sequence.initial_tempo == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                "msd-timing");
            SequenceParser parser{*this,
                                  BoundedBytes(sequence_bytes),
                                  0u,
                                  0u,
                                  0u,
                                  config_.maximum_events_per_sequence,
                                  {}};
            std::uint64_t cursor = 12u;
            for (;;) {
                const auto [next, continued] = parser.parse_one(cursor, 0u, false);
                cursor = next;
                if (!continued) break;
            }
            sequence.events = std::move(parser.events);
            sequence.end_tick = parser.current_tick;
            saturating_add(parsed_events_, sequence.events.size());
            saturating_add(parsed_sequences_, 1u);
            result.sequences[index] = std::move(sequence);
        }
        return result;
    }

    void parse_effect_program(Collection& collection,
                              const BoundedBytes& bytes) const {
        if (!bytes.magic(0u, "SFPB") ||
            bytes.u32(4u,
                      NativePortSoundBankFailure::UnsupportedEffect,
                      "fpb-version") != 1u ||
            bytes.u32(12u,
                      NativePortSoundBankFailure::UnsupportedEffect,
                      "fpb-count") != 1u)
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "fpb-header");
        const auto name_offset = bytes.u32(
            16u,
            NativePortSoundBankFailure::UnsupportedEffect,
            "fpb-name-offset");
        bytes.extent(name_offset,
                     32u,
                     NativePortSoundBankFailure::UnsupportedEffect,
                     "fpb-name");
        // The title-owned provider performs the complete immutable-program
        // identity and structural check. Public runtime never promotes a
        // program name into executable semantics and has no generic MPRO
        // interpreter fallback.
        const auto& provider = config_.effect_kernel_provider;
        if (provider.resolve == nullptr)
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "fpb-provider");
        const auto raw_program = bytes.span(
            0u, bytes.size(), NativePortSoundBankFailure::UnsupportedEffect,
            "fpb-program-bytes");
        const auto program = std::as_bytes(raw_program);
        const auto* const kernel = provider.resolve(provider.user, program);
        const auto valid_alignment =
            kernel != nullptr && kernel->state_alignment != 0u &&
            std::has_single_bit(kernel->state_alignment) &&
            kernel->state_alignment <= 4'096u;
        const auto callback_count =
            static_cast<unsigned>(kernel != nullptr &&
                                  kernel->capture_snapshot != nullptr) +
            static_cast<unsigned>(kernel != nullptr &&
                                  kernel->validate_snapshot != nullptr) +
            static_cast<unsigned>(kernel != nullptr &&
                                  kernel->restore_snapshot != nullptr);
        const auto valid_snapshot_callbacks =
            callback_count == 0u ||
            (callback_count == 3u && kernel->maximum_snapshot_bytes != 0u &&
             kernel->maximum_snapshot_bytes <=
                 config_.maximum_effect_state_bytes);
        if (kernel == nullptr ||
            kernel->contract_version !=
                native_port_sound_effect_kernel_contract_version ||
            kernel->input_bus_count != maximum_effect_buses ||
            kernel->output_bus_count != maximum_effect_buses ||
            kernel->required_work_area_bytes == 0u ||
            kernel->state_size == 0u ||
            kernel->state_size > config_.maximum_effect_state_bytes ||
            kernel->state_size > std::numeric_limits<std::size_t>::max() ||
            !valid_alignment || kernel->initialize == nullptr ||
            kernel->destroy == nullptr || kernel->render == nullptr ||
            !valid_snapshot_callbacks)
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "fpb-kernel");

        void* storage = nullptr;
        try {
            storage = ::operator new(
                static_cast<std::size_t>(kernel->state_size),
                std::align_val_t(
                    static_cast<std::size_t>(kernel->state_alignment)));
        } catch (const std::bad_alloc&) {
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "fpb-state");
        }
        if (!kernel->initialize(program, config_.output_sample_rate, storage)) {
            ::operator delete(
                storage,
                std::align_val_t(static_cast<std::size_t>(
                    kernel->state_alignment)));
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "fpb-initialize");
        }
        EffectKernelState state;
        state.kernel = kernel;
        state.storage = storage;
        collection.effect_kernel_state = std::move(state);
    }

    void parse_effect_output(Collection& collection,
                             const BoundedBytes& bytes) const {
        if (!bytes.magic(0u, "SFOB") ||
            bytes.u32(4u,
                      NativePortSoundBankFailure::UnsupportedEffect,
                      "fob-version") != 1u ||
            bytes.u32(12u,
                      NativePortSoundBankFailure::UnsupportedEffect,
                      "fob-count") != 1u)
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                            "fob-header");
        const auto output_offset = bytes.u32(
            16u,
            NativePortSoundBankFailure::UnsupportedEffect,
            "fob-output-offset");
        bytes.extent(output_offset,
                     maximum_effect_buses * 2u,
                     NativePortSoundBankFailure::UnsupportedEffect,
                     "fob-output-table");
        for (std::size_t index = 0u; index < maximum_effect_buses; ++index) {
            collection.effect_outputs[index] = {
                bytes.u8(output_offset + index * 2u,
                         NativePortSoundBankFailure::UnsupportedEffect,
                         "fob-level"),
                bytes.u8(output_offset + index * 2u + 1u,
                         NativePortSoundBankFailure::UnsupportedEffect,
                         "fob-pan")};
            if (collection.effect_outputs[index].level > 15u ||
                collection.effect_outputs[index].pan > 31u)
                fail_sound_bank(NativePortSoundBankFailure::UnsupportedEffect,
                                "fob-output-range");
        }
    }

  public:
    [[nodiscard]] NativePortSoundSequenceHandle play_sequence(
        const NativePortSoundCollectionHandle collection_handle,
        const NativePortSoundSequenceConfig& config) {
        require_owner_thread();
        return insert_sequence(prepare_sequence(collection_handle, config));
    }

    void pause_sequence(const NativePortSoundSequenceHandle handle) {
        require_owner_thread();
        auto& sequence = require_sequence(handle);
        if (sequence.state == NativePortSoundSequenceState::Playing)
            sequence.state = NativePortSoundSequenceState::Paused;
    }

    void resume_sequence(const NativePortSoundSequenceHandle handle) {
        require_owner_thread();
        auto& sequence = require_sequence(handle);
        if (sequence.state == NativePortSoundSequenceState::Paused)
            sequence.state = NativePortSoundSequenceState::Playing;
    }

    void stop_sequence(const NativePortSoundSequenceHandle handle) {
        require_owner_thread();
        auto& sequence = require_sequence(handle);
        sequence.state = NativePortSoundSequenceState::Stopped;
        remove_sequence_voices(handle, false);
    }

    void release_sequence(const NativePortSoundSequenceHandle handle) {
        require_owner_thread();
        static_cast<void>(require_sequence_slot(handle));
        remove_sequence_voices(handle, false);
        erase_sequence_slot(handle.slot);
    }

    void set_sequence_gain_pan(const NativePortSoundSequenceHandle handle,
                               const float gain,
                               const float pan) {
        require_owner_thread();
        if (!valid_gain_pan(gain, pan))
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-gain-pan");
        auto& active = require_sequence(handle);
        active.config.gain = gain;
        active.config.pan = pan;
    }

    void set_sequence_pitch_bend(const NativePortSoundSequenceHandle handle,
                                 const std::int16_t pitch_bend) {
        require_owner_thread();
        if (pitch_bend < -8192 || pitch_bend > 8191)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-pitch-bend");
        require_sequence(handle).config.pitch_bend = pitch_bend;
    }

    void set_sequence_playback_rate(
        const NativePortSoundSequenceHandle handle,
        const float playback_rate) {
        require_owner_thread();
        if (!valid_playback_rate(playback_rate))
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-playback-rate");
        require_sequence(handle).config.playback_rate = playback_rate;
    }

    void set_sequence_send_levels(const NativePortSoundSequenceHandle handle,
                                  const std::uint8_t direct_level,
                                  const std::uint8_t effect_level) {
        require_owner_thread();
        if (direct_level > 127u || effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-send-levels");
        auto& active = require_sequence(handle);
        active.config.direct_level = direct_level;
        active.config.effect_level = effect_level;
    }

    [[nodiscard]] NativePortSoundVoiceHandle note_on(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundNoteConfig& config) {
        require_owner_thread();
        if (config.program_bank >= maximum_mlt_banks ||
            config.program >= maximum_mpb_programs || config.note > 127u ||
            config.velocity > 127u || !valid_gain_pan(config.gain, config.pan) ||
            config.pitch_bend < -8192 || config.pitch_bend > 8191 ||
            config.direct_level > 127u || config.effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "note-config");
        VoiceGroup group;
        group.collection = collection;
        group.controls = std::make_shared<VoiceControls>();
        group.controls->gain = config.gain;
        group.controls->pan = config.pan;
        group.controls->pitch_bend = config.pitch_bend;
        group.controls->direct_level = config.direct_level;
        group.controls->effect_level = config.effect_level;
        group.members = spawn_program(collection,
                                      std::nullopt,
                                      config.program_bank,
                                      config.program,
                                      config.note,
                                      config.velocity,
                                      0u,
                                      group.controls,
                                      std::numeric_limits<std::uint64_t>::max(),
                                      0u);
        return insert_group(std::move(group));
    }

    void note_off(const NativePortSoundVoiceHandle handle) {
        require_owner_thread();
        auto& group = require_group(handle);
        for (const auto member : group.members) {
            if (auto* voice = find_voice(member)) key_off(*voice);
        }
    }

    void release_voice(const NativePortSoundVoiceHandle handle) {
        require_owner_thread();
        auto& slot = require_group_slot(handle);
        for (const auto member : slot.value->members) remove_voice(member);
        slot.value.reset();
        bump_generation(slot.generation);
    }

    void set_voice_gain_pan(const NativePortSoundVoiceHandle handle,
                            const float gain,
                            const float pan) {
        require_owner_thread();
        if (!valid_gain_pan(gain, pan))
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "voice-gain-pan");
        auto& group = require_group(handle);
        group.controls->gain = gain;
        group.controls->pan = pan;
    }

    void set_voice_pitch_bend(const NativePortSoundVoiceHandle handle,
                              const std::int16_t pitch_bend) {
        require_owner_thread();
        if (pitch_bend < -8192 || pitch_bend > 8191)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "voice-pitch-bend");
        require_group(handle).controls->pitch_bend = pitch_bend;
    }

    void set_voice_send_levels(const NativePortSoundVoiceHandle handle,
                               const std::uint8_t direct_level,
                               const std::uint8_t effect_level) {
        require_owner_thread();
        if (direct_level > 127u || effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "voice-send-levels");
        auto& controls = *require_group(handle).controls;
        controls.direct_level = direct_level;
        controls.effect_level = effect_level;
    }

    [[nodiscard]] NativePortSoundMidiPortHandle open_midi_port(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundMidiPortConfig& config) {
        require_owner_thread();
        if (!valid_midi_port_config(collection, config))
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "midi-port-config");
        MidiPort port;
        port.collection = collection;
        port.config = config;
        port.controls = std::make_shared<VoiceControls>();
        synchronize_port_controls(port);
        return insert_midi_port(std::move(port));
    }

    void close_midi_port(const NativePortSoundMidiPortHandle handle) {
        require_owner_thread();
        auto& slot = require_midi_port_slot(handle);
        const auto controls = slot.value->controls;
        if (slot.value->sequence.has_value()) {
            release_sequence_if_live(*slot.value->sequence);
            slot.value->sequence.reset();
        }
        for (auto& group_slot : groups_) {
            if (!group_slot.value.has_value() ||
                group_slot.value->controls != controls)
                continue;
            for (const auto member : group_slot.value->members)
                remove_voice(member);
            group_slot.value.reset();
            bump_generation(group_slot.generation);
        }
        slot.value.reset();
        bump_generation(slot.generation);
    }

    void set_midi_program(const NativePortSoundMidiPortHandle handle,
                          const std::uint8_t bank,
                          const std::uint8_t program) {
        require_owner_thread();
        auto& port = require_midi_port(handle);
        if (!has_program(port.collection, bank, program))
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "midi-program");
        port.config.program_bank = bank;
        port.config.program = program;
    }

    void set_midi_gain_pan(const NativePortSoundMidiPortHandle handle,
                           const float gain,
                           const float pan) {
        require_owner_thread();
        if (!valid_gain_pan(gain, pan))
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-gain-pan");
        auto& port = require_midi_port(handle);
        port.config.gain = gain;
        port.config.pan = pan;
        synchronize_port_controls(port);
        synchronize_port_sequence(port);
    }

    void set_midi_pitch_bend(const NativePortSoundMidiPortHandle handle,
                             const std::int16_t pitch_bend) {
        require_owner_thread();
        if (pitch_bend < -8192 || pitch_bend > 8191)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-pitch-bend");
        auto& port = require_midi_port(handle);
        port.config.pitch_bend = pitch_bend;
        synchronize_port_controls(port);
        synchronize_port_sequence(port);
    }

    void set_midi_playback_rate(const NativePortSoundMidiPortHandle handle,
                                const float playback_rate) {
        require_owner_thread();
        if (!valid_playback_rate(playback_rate))
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-playback-rate");
        auto& port = require_midi_port(handle);
        port.config.playback_rate = playback_rate;
        synchronize_port_sequence(port);
    }

    void set_midi_send_levels(const NativePortSoundMidiPortHandle handle,
                              const std::uint8_t direct_level,
                              const std::uint8_t effect_level) {
        require_owner_thread();
        if (direct_level > 127u || effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-send-levels");
        auto& port = require_midi_port(handle);
        port.config.direct_level = direct_level;
        port.config.effect_level = effect_level;
        synchronize_port_controls(port);
        synchronize_port_sequence(port);
    }

    [[nodiscard]] NativePortSoundVoiceHandle midi_note_on(
        const NativePortSoundMidiPortHandle handle,
        const std::uint8_t note,
        const std::uint8_t velocity) {
        require_owner_thread();
        if (note > 127u || velocity > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-note");
        auto& port = require_midi_port(handle);
        auto& collection = require_collection(port.collection);
        static_cast<void>(require_playable_program(
            collection,
            port.config.program_bank,
            port.config.program,
            note,
            velocity));
        if (port.notes[note].has_value()) {
            note_off_if_live(*port.notes[note]);
            port.notes[note].reset();
        }
        VoiceGroup group;
        group.collection = port.collection;
        group.controls = port.controls;
        group.members = spawn_program(port.collection,
                                      std::nullopt,
                                      port.config.program_bank,
                                      port.config.program,
                                      note,
                                      velocity,
                                      0u,
                                      port.controls,
                                      std::numeric_limits<std::uint64_t>::max(),
                                      0u);
        const auto voice = insert_group(std::move(group));
        port.notes[note] = voice;
        return voice;
    }

    void midi_note_off(const NativePortSoundMidiPortHandle handle,
                       const std::uint8_t note) {
        require_owner_thread();
        if (note > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-note");
        auto& port = require_midi_port(handle);
        if (!port.notes[note].has_value()) return;
        note_off_if_live(*port.notes[note]);
        port.notes[note].reset();
    }

    [[nodiscard]] NativePortSoundSequenceHandle midi_play_sequence(
        const NativePortSoundMidiPortHandle handle,
        const std::uint8_t sequence_bank,
        const std::uint16_t sequence,
        const bool enable_authored_loops) {
        require_owner_thread();
        auto& port = require_midi_port(handle);
        NativePortSoundSequenceConfig config;
        config.sequence_bank = sequence_bank;
        config.program_bank = port.config.program_bank;
        config.sequence = sequence;
        config.gain = port.config.gain;
        config.pan = port.config.pan;
        config.playback_rate = port.config.playback_rate;
        config.pitch_bend = port.config.pitch_bend;
        config.direct_level = port.config.direct_level;
        config.effect_level = port.config.effect_level;
        config.enable_authored_loops = enable_authored_loops;
        // Validate and prepare the replacement before retiring the old
        // sequence.  A live port replaces its own active slot in place: the
        // prepared state is nothrow-movable, so the previous handle remains
        // valid until every fallible validation step has completed and no
        // second capacity check can discard it after retirement.
        auto replacement = prepare_sequence(port.collection, config);
        if (port.sequence.has_value() && sequence_is_live(*port.sequence)) {
            static_assert(std::is_nothrow_move_assignable_v<ActiveSequence>);
            const auto previous = *port.sequence;
            auto& slot = require_sequence_slot(previous);
            remove_sequence_voices(previous, false);
            *slot.value = std::move(replacement);
            bump_generation(slot.generation);
            const NativePortSoundSequenceHandle result{
                previous.slot, slot.generation};
            port.sequence = result;
            return result;
        }
        // A non-live retained handle carries no recoverable playback state.
        // Clear it before the fallible insertion path so a failed fresh start
        // cannot leave the MIDI port advertising a stale sequence binding.
        port.sequence.reset();
        const auto result = insert_sequence(std::move(replacement));
        port.sequence = result;
        return result;
    }

    void midi_pause(const NativePortSoundMidiPortHandle handle) {
        require_owner_thread();
        auto& port = require_midi_port(handle);
        if (port.sequence.has_value() && sequence_is_live(*port.sequence))
            pause_sequence(*port.sequence);
    }

    void midi_resume(const NativePortSoundMidiPortHandle handle) {
        require_owner_thread();
        auto& port = require_midi_port(handle);
        if (port.sequence.has_value() && sequence_is_live(*port.sequence))
            resume_sequence(*port.sequence);
    }

    void midi_stop(const NativePortSoundMidiPortHandle handle) {
        require_owner_thread();
        auto& port = require_midi_port(handle);
        if (port.sequence.has_value()) {
            release_sequence_if_live(*port.sequence);
            port.sequence.reset();
        }
        for (auto& group_slot : groups_) {
            if (!group_slot.value.has_value() ||
                group_slot.value->controls != port.controls)
                continue;
            for (const auto member : group_slot.value->members)
                if (auto* voice = find_voice(member)) key_off(*voice);
        }
        for (auto& note : port.notes) note.reset();
    }

    void close_all_midi_ports() {
        require_owner_thread();
        for (std::size_t index = 0u; index < ports_.size(); ++index) {
            if (!ports_[index].value.has_value()) continue;
            close_midi_port({static_cast<std::uint32_t>(index),
                             ports_[index].generation});
        }
    }

    void release_all_sequences() {
        require_owner_thread();
        for (auto& port_slot : ports_) {
            if (!port_slot.value.has_value()) continue;
            port_slot.value->sequence.reset();
        }
        while (!active_sequence_indices_.empty()) {
            const auto index = active_sequence_indices_.back();
            release_sequence({index, sequences_[index].generation});
        }
    }

    void unload_all_collections() {
        require_owner_thread();
        // Validate the whole operation before mutating any slot. Callers
        // either unload every resident collection or retain every one.
        for (std::size_t index = 0u; index < collections_.size(); ++index) {
            if (!collections_[index].value.has_value()) continue;
            const NativePortSoundCollectionHandle handle{
                static_cast<std::uint32_t>(index),
                collections_[index].generation};
            if (collection_in_use(handle))
                fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                                "collection-in-use");
        }
        for (auto& slot : collections_) {
            if (!slot.value.has_value()) continue;
            resident_collection_bytes_ -= slot.value->bytes.size();
            slot.value.reset();
            bump_generation(slot.generation);
            saturating_add(unloaded_collections_, 1u);
        }
    }

    void stop_all() {
        require_owner_thread();
        for (const auto index : active_sequence_indices_)
            sequences_[index].value->state = NativePortSoundSequenceState::Stopped;
        while (!active_voice_indices_.empty())
            erase_voice_slot(active_voice_indices_.back());
        for (auto& slot : groups_) {
            if (!slot.value.has_value()) continue;
            slot.value.reset();
            bump_generation(slot.generation);
        }
        for (auto& slot : ports_) {
            if (!slot.value.has_value()) continue;
            slot.value->sequence.reset();
            for (auto& note : slot.value->notes) note.reset();
        }
    }

    void reset() {
        require_owner_thread();
        for (auto& slot : ports_) {
            if (!slot.value.has_value()) continue;
            slot.value.reset();
            bump_generation(slot.generation);
        }
        while (!active_sequence_indices_.empty())
            erase_sequence_slot(active_sequence_indices_.back());
        while (!active_voice_indices_.empty())
            erase_voice_slot(active_voice_indices_.back());
        for (auto& slot : groups_) {
            if (!slot.value.has_value()) continue;
            slot.value.reset();
            bump_generation(slot.generation);
        }
        for (auto& slot : pcm_stream_rings_) {
            if (!slot.value.has_value()) continue;
            slot.value.reset();
            bump_generation(slot.generation);
        }
        // No live handle can now retain a collection. Use the same validated
        // unload path so counters and generations cannot drift.
        unload_all_collections();
    }

    void pump() {
        require_owner_thread();
        collect_stale_groups_and_ports();
        // Compatibility entry point: the shared AudioEngine is the sole
        // output clock.  SoundBank samples are rendered directly into its
        // wide accumulator by render_into(), never through an S16 feed.
        audio_.pump();
        collect_stale_groups_and_ports();
    }

    void render_into(std::span<double> interleaved_stereo_accumulation,
                     const std::uint32_t frame_count) {
        require_owner_thread();
        const auto required_samples =
            static_cast<std::uint64_t>(frame_count) * 2u;
        if (required_samples > interleaved_stereo_accumulation.size())
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "mix-destination");

        std::uint32_t rendered = 0u;
        while (rendered < frame_count) {
            const auto frames = std::min(config_.render_block_frames,
                                         frame_count - rendered);
            render_block(frames);
            const auto sample_count = static_cast<std::size_t>(frames) * 2u;
            const auto destination = static_cast<std::size_t>(rendered) * 2u;
            for (std::size_t sample = 0u; sample < sample_count; ++sample)
                interleaved_stereo_accumulation[destination + sample] +=
                    mix_[sample];
            rendered += frames;
        }
    }

    [[nodiscard]] NativePortSoundSequenceSnapshot sequence_snapshot(
        const NativePortSoundSequenceHandle handle) const {
        require_owner_thread();
        const auto& sequence = require_sequence(handle);
        NativePortSoundSequenceSnapshot result;
        result.state = sequence.state;
        result.rendered_frames = sequence.rendered_frames;
        result.dispatched_events = sequence.dispatched_events;
        result.loop_count = sequence.loop_count;
        for (const auto index : active_voice_indices_)
            if (voices_[index].value->sequence.has_value() &&
                voices_[index].value->sequence->slot == handle.slot &&
                voices_[index].value->sequence->generation == handle.generation)
                ++result.active_voices;
        return result;
    }

    [[nodiscard]] NativePortSoundMidiPortSnapshot midi_port_snapshot(
        const NativePortSoundMidiPortHandle handle) const {
        require_owner_thread();
        const auto& port = require_midi_port(handle);
        NativePortSoundMidiPortSnapshot result;
        result.program_bank = port.config.program_bank;
        result.program = port.config.program;
        result.gain = port.config.gain;
        result.pan = port.config.pan;
        result.playback_rate = port.config.playback_rate;
        result.pitch_bend = port.config.pitch_bend;
        result.direct_level = port.config.direct_level;
        result.effect_level = port.config.effect_level;
        for (const auto note : port.notes)
            if (note.has_value() && group_has_live_voice(*note))
                ++result.active_notes;
        result.sequence_active =
            port.sequence.has_value() && sequence_is_live(*port.sequence);
        return result;
    }

    [[nodiscard]] NativePortSoundBankSnapshot snapshot() const {
        require_owner_thread();
        NativePortSoundBankSnapshot result;
        result.loaded_collections = loaded_collections_;
        result.unloaded_collections = unloaded_collections_;
        result.parsed_programs = parsed_programs_;
        result.parsed_splits = parsed_splits_;
        result.parsed_sequences = parsed_sequences_;
        result.parsed_events = parsed_events_;
        result.decoded_samples = decoded_samples_;
        result.decoded_pcm16_samples = decoded_pcm16_samples_;
        result.decoded_pcm8_samples = decoded_pcm8_samples_;
        result.decoded_adpcm_samples = decoded_adpcm_samples_;
        result.decoded_sample_frames = decoded_sample_frames_;
        result.resident_collection_bytes = resident_collection_bytes_;
        result.rendered_frames = rendered_frames_;
        result.effect_frames = effect_frames_;
        for (const auto& slot : collections_)
            result.active_collections += slot.value.has_value() ? 1u : 0u;
        for (const auto index : active_sequence_indices_)
            result.active_sequences +=
                sequences_[index].value->state ==
                            NativePortSoundSequenceState::Playing ||
                        sequences_[index].value->state ==
                            NativePortSoundSequenceState::Paused
                    ? 1u
                    : 0u;
        result.active_voices =
            static_cast<std::uint32_t>(active_voice_indices_.size());
        for (const auto& slot : pcm_stream_rings_) {
            if (!slot.value.has_value()) continue;
            ++result.active_pcm_stream_rings;
            saturating_add(result.reserved_pcm_stream_bytes,
                           slot.value->byte_size);
        }
        // Contract v8 has no intermediate SoundBank PCM feed.  The field is
        // retained as a compatibility counter and is therefore exactly zero.
        result.feed_buffered_frames = 0u;
        return result;
    }

  private:
    [[nodiscard]] ActiveSequence prepare_sequence(
        const NativePortSoundCollectionHandle collection_handle,
        const NativePortSoundSequenceConfig& config) {
        if (config.sequence_bank >= maximum_mlt_banks ||
            config.program_bank >= maximum_mlt_banks ||
            !valid_gain_pan(config.gain, config.pan) ||
            !valid_playback_rate(config.playback_rate) ||
            config.pitch_bend < -8192 || config.pitch_bend > 8191 ||
            config.direct_level > 127u || config.effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-config");
        const auto& collection = require_collection(collection_handle);
        const auto& bank = collection.sequence_banks[config.sequence_bank];
        if (!bank.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequenceBank,
                            "sequence-bank");
        if (config.sequence >= bank->sequences.size() ||
            !bank->sequences[config.sequence].has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-index");
        if (!collection.program_banks[config.program_bank].has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "sequence-program-bank");

        ActiveSequence active;
        active.collection = collection_handle;
        active.config = config;
        active.tempo = bank->sequences[config.sequence]->initial_tempo;
        for (auto& channel : active.channels)
            channel.bank = config.program_bank;
        return active;
    }

    [[nodiscard]] NativePortSoundSequenceHandle insert_sequence(
        ActiveSequence sequence) {
        std::size_t index = sequences_.size();
        for (std::size_t candidate = 0u; candidate < sequences_.size();
             ++candidate) {
            if (!sequences_[candidate].value.has_value() &&
                index == sequences_.size())
                index = candidate;
        }
        if (active_sequence_indices_.size() == config_.maximum_active_sequences)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "active-sequence-limit");
        if (index == sequences_.size()) {
            sequences_.push_back({});
            active_sequence_positions_.push_back(invalid_active_position);
        }
        sequences_[index].value.emplace(std::move(sequence));
        activate_slot(active_sequence_indices_, active_sequence_positions_, index);
        return {static_cast<std::uint32_t>(index), sequences_[index].generation};
    }

    [[nodiscard]] Slot<ActiveSequence>& require_sequence_slot(
        const NativePortSoundSequenceHandle handle) {
        if (!handle || handle.slot >= sequences_.size() ||
            sequences_[handle.slot].generation != handle.generation ||
            !sequences_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "sequence-handle");
        return sequences_[handle.slot];
    }

    [[nodiscard]] const Slot<ActiveSequence>& require_sequence_slot(
        const NativePortSoundSequenceHandle handle) const {
        if (!handle || handle.slot >= sequences_.size() ||
            sequences_[handle.slot].generation != handle.generation ||
            !sequences_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "sequence-handle");
        return sequences_[handle.slot];
    }

    [[nodiscard]] ActiveSequence& require_sequence(
        const NativePortSoundSequenceHandle handle) {
        return *require_sequence_slot(handle).value;
    }

    [[nodiscard]] const ActiveSequence& require_sequence(
        const NativePortSoundSequenceHandle handle) const {
        return *require_sequence_slot(handle).value;
    }

    [[nodiscard]] NativePortSoundVoiceHandle insert_group(VoiceGroup group) {
        std::size_t index = groups_.size();
        for (std::size_t candidate = 0u; candidate < groups_.size(); ++candidate)
            if (!groups_[candidate].value.has_value()) {
                index = candidate;
                break;
            }
        if (index == groups_.size()) groups_.push_back({});
        groups_[index].value.emplace(std::move(group));
        return {static_cast<std::uint32_t>(index), groups_[index].generation};
    }

    [[nodiscard]] Slot<VoiceGroup>& require_group_slot(
        const NativePortSoundVoiceHandle handle) {
        if (!handle || handle.slot >= groups_.size() ||
            groups_[handle.slot].generation != handle.generation ||
            !groups_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "voice-group-handle");
        return groups_[handle.slot];
    }

    [[nodiscard]] VoiceGroup& require_group(
        const NativePortSoundVoiceHandle handle) {
        return *require_group_slot(handle).value;
    }

    [[nodiscard]] const VoiceGroup& require_group(
        const NativePortSoundVoiceHandle handle) const {
        if (!handle || handle.slot >= groups_.size() ||
            groups_[handle.slot].generation != handle.generation ||
            !groups_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "voice-group-handle");
        return *groups_[handle.slot].value;
    }

    [[nodiscard]] bool group_has_live_voice(
        const NativePortSoundVoiceHandle handle) const noexcept {
        if (!handle || handle.slot >= groups_.size() ||
            groups_[handle.slot].generation != handle.generation ||
            !groups_[handle.slot].value.has_value())
            return false;
        return std::ranges::any_of(
            groups_[handle.slot].value->members, [&](const auto member) {
                return member && member.slot < voices_.size() &&
                       voices_[member.slot].generation == member.generation &&
                       voices_[member.slot].value.has_value();
            });
    }

    void note_off_if_live(const NativePortSoundVoiceHandle handle) {
        if (!handle || handle.slot >= groups_.size() ||
            groups_[handle.slot].generation != handle.generation ||
            !groups_[handle.slot].value.has_value())
            return;
        for (const auto member : groups_[handle.slot].value->members)
            if (auto* voice = find_voice(member)) key_off(*voice);
    }

    [[nodiscard]] bool sequence_is_live(
        const NativePortSoundSequenceHandle handle) const noexcept {
        return handle && handle.slot < sequences_.size() &&
               sequences_[handle.slot].generation == handle.generation &&
               sequences_[handle.slot].value.has_value();
    }

    void release_sequence_if_live(const NativePortSoundSequenceHandle handle) {
        if (!sequence_is_live(handle)) return;
        remove_sequence_voices(handle, false);
        erase_sequence_slot(handle.slot);
    }

    [[nodiscard]] bool valid_midi_port_config(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundMidiPortConfig& config) const {
        return valid_gain_pan(config.gain, config.pan) &&
               valid_playback_rate(config.playback_rate) &&
               config.pitch_bend >= -8192 && config.pitch_bend <= 8191 &&
               config.direct_level <= 127u && config.effect_level <= 127u &&
               has_program(collection, config.program_bank, config.program);
    }

    static void synchronize_port_controls(MidiPort& port) noexcept {
        port.controls->gain = port.config.gain;
        port.controls->pan = port.config.pan;
        port.controls->pitch_bend = port.config.pitch_bend;
        port.controls->direct_level = port.config.direct_level;
        port.controls->effect_level = port.config.effect_level;
    }

    void synchronize_port_sequence(MidiPort& port) {
        if (!port.sequence.has_value() || !sequence_is_live(*port.sequence)) {
            port.sequence.reset();
            return;
        }
        auto& config = require_sequence(*port.sequence).config;
        config.gain = port.config.gain;
        config.pan = port.config.pan;
        config.playback_rate = port.config.playback_rate;
        config.pitch_bend = port.config.pitch_bend;
        config.direct_level = port.config.direct_level;
        config.effect_level = port.config.effect_level;
    }

    [[nodiscard]] NativePortSoundMidiPortHandle insert_midi_port(
        MidiPort port) {
        std::size_t index = ports_.size();
        std::size_t active_count = 0u;
        for (std::size_t candidate = 0u; candidate < ports_.size();
             ++candidate) {
            if (ports_[candidate].value.has_value())
                ++active_count;
            else if (index == ports_.size())
                index = candidate;
        }
        if (active_count == config_.maximum_midi_ports)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "midi-port-limit");
        if (index == ports_.size()) ports_.push_back({});
        ports_[index].value.emplace(std::move(port));
        return {static_cast<std::uint32_t>(index), ports_[index].generation};
    }

    [[nodiscard]] Slot<MidiPort>& require_midi_port_slot(
        const NativePortSoundMidiPortHandle handle) {
        if (!handle || handle.slot >= ports_.size() ||
            ports_[handle.slot].generation != handle.generation ||
            !ports_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-port-handle");
        return ports_[handle.slot];
    }

    [[nodiscard]] const Slot<MidiPort>& require_midi_port_slot(
        const NativePortSoundMidiPortHandle handle) const {
        if (!handle || handle.slot >= ports_.size() ||
            ports_[handle.slot].generation != handle.generation ||
            !ports_[handle.slot].value.has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidHandle,
                            "midi-port-handle");
        return ports_[handle.slot];
    }

    [[nodiscard]] MidiPort& require_midi_port(
        const NativePortSoundMidiPortHandle handle) {
        return *require_midi_port_slot(handle).value;
    }

    [[nodiscard]] const MidiPort& require_midi_port(
        const NativePortSoundMidiPortHandle handle) const {
        return *require_midi_port_slot(handle).value;
    }

    [[nodiscard]] NativePortSoundVoiceHandle insert_voice(SynthVoice voice) {
        std::size_t index = voices_.size();
        for (std::size_t candidate = 0u; candidate < voices_.size(); ++candidate) {
            if (!voices_[candidate].value.has_value() && index == voices_.size())
                index = candidate;
        }
        if (active_voice_indices_.size() == config_.maximum_synth_voices) {
            // The Manatee driver targets a finite hardware voice pool and
            // steals an existing slot under pressure. Reproduce that semantic
            // lifecycle natively instead of turning legitimate polyphony into
            // a product-fatal resource error. Released voices are preferred;
            // ties and the fallback are deterministic oldest-first.
            std::optional<std::size_t> victim;
            for (const auto candidate : active_voice_indices_) {
                if (!victim.has_value() ||
                    (voices_[candidate].value->key_released &&
                     !voices_[*victim].value->key_released) ||
                    (voices_[candidate].value->key_released ==
                         voices_[*victim].value->key_released &&
                     voices_[candidate].value->start_serial <
                         voices_[*victim].value->start_serial))
                    victim = candidate;
            }
            if (!victim.has_value())
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "synth-voice-limit");
            index = *victim;
            erase_voice_slot(index);
        }
        if (index == voices_.size()) {
            voices_.push_back({});
            active_voice_positions_.push_back(invalid_active_position);
        }
        voices_[index].value.emplace(std::move(voice));
        activate_slot(active_voice_indices_, active_voice_positions_, index);
        return {static_cast<std::uint32_t>(index), voices_[index].generation};
    }

    [[nodiscard]] SynthVoice* find_voice(
        const NativePortSoundVoiceHandle handle) noexcept {
        if (!handle || handle.slot >= voices_.size() ||
            voices_[handle.slot].generation != handle.generation ||
            !voices_[handle.slot].value.has_value())
            return nullptr;
        return &*voices_[handle.slot].value;
    }

    void remove_voice(const NativePortSoundVoiceHandle handle) noexcept {
        if (!handle || handle.slot >= voices_.size() ||
            voices_[handle.slot].generation != handle.generation ||
            !voices_[handle.slot].value.has_value())
            return;
        erase_voice_slot(handle.slot);
    }

    void collect_stale_groups_and_ports() noexcept {
        for (std::size_t index = 0u; index < groups_.size(); ++index) {
            auto& slot = groups_[index];
            if (!slot.value.has_value()) continue;
            const NativePortSoundVoiceHandle handle{
                static_cast<std::uint32_t>(index), slot.generation};
            if (group_has_live_voice(handle)) continue;
            slot.value.reset();
            bump_generation(slot.generation);
        }
        for (auto& slot : ports_) {
            if (!slot.value.has_value()) continue;
            auto& port = *slot.value;
            if (port.sequence.has_value() &&
                !sequence_is_live(*port.sequence))
                port.sequence.reset();
            for (auto& note : port.notes)
                if (note.has_value() && !group_has_live_voice(*note))
                    note.reset();
        }
    }

    void remove_sequence_voices(const NativePortSoundSequenceHandle handle,
                                const bool release) {
        std::size_t active = 0u;
        while (active < active_voice_indices_.size()) {
            const auto index = active_voice_indices_[active];
            auto& voice = *voices_[index].value;
            if (!voice.sequence.has_value() ||
                voice.sequence->slot != handle.slot ||
                voice.sequence->generation != handle.generation) {
                ++active;
                continue;
            }
            if (release)
                key_off(voice);
            else
                erase_voice_slot(index);
            if (release) ++active;
        }
    }

    [[nodiscard]] std::vector<NativePortSoundVoiceHandle> spawn_program(
        const NativePortSoundCollectionHandle collection_handle,
        const std::optional<NativePortSoundSequenceHandle> sequence_handle,
        const std::uint8_t bank_index,
        const std::uint8_t program_index,
        const std::uint8_t note,
        const std::uint8_t velocity,
        const std::uint8_t channel,
        const std::shared_ptr<VoiceControls>& controls,
        const std::uint64_t release_tick,
        const std::uint64_t current_tick) {
        auto& collection = require_collection(collection_handle);
        const auto playable = require_playable_program(
            collection, bank_index, program_index, note, velocity);
        auto& bank = *playable.bank;
        const auto& program = *playable.program;
        std::array<bool, 256u> drum_groups{};
        for (const auto& layer : program.layers)
            for (const auto& split : layer.splits)
                if (split.drum && note >= split.start_note &&
                    note <= split.end_note && velocity >= split.velocity_low &&
                    velocity <= split.velocity_high && split.tone_offset != 0u &&
                    split.loop_end != 0u)
                    drum_groups[split.drum_group] = true;
        if (std::ranges::any_of(drum_groups, [](const bool value) {
                return value;
            })) {
            std::size_t active = 0u;
            while (active < active_voice_indices_.size()) {
                const auto index = active_voice_indices_[active];
                const auto& voice = *voices_[index].value;
                if (voice.collection.slot != collection_handle.slot ||
                    voice.collection.generation !=
                        collection_handle.generation ||
                    !voice.split->drum ||
                    !drum_groups[voice.split->drum_group]) {
                    ++active;
                    continue;
                }
                erase_voice_slot(index);
            }
        }
        std::vector<NativePortSoundVoiceHandle> created;
        try {
            for (const auto& layer : program.layers) {
                for (const auto& split : layer.splits) {
                    if (note < split.start_note || note > split.end_note ||
                        velocity < split.velocity_low ||
                        velocity > split.velocity_high ||
                        split.tone_offset == 0u || split.loop_end == 0u)
                        continue;
                    SynthVoice voice;
                    voice.collection = collection_handle;
                    voice.sequence = sequence_handle;
                    voice.sample = decode_sample(collection, bank, split);
                    voice.split = &split;
                    voice.channel = channel;
                    voice.note = note;
                    voice.velocity = velocity;
                    voice.bend_high = layer.bend_high;
                    voice.bend_low = layer.bend_low;
                    voice.release_tick = release_tick;
                    voice.delayed_until_tick = current_tick + layer.delay;
                    voice.velocity_gain = velocity_gain(bank, split, velocity);
                    voice.controls = controls;
                    voice.base_step = pitch_step(split, note, 0);
                    voice.lfo_step =
                        lfo_frequency(split.lfo_frequency) /
                        config_.output_sample_rate;
                    voice.lfo_phase =
                        split.lfo_sync
                            ? 0.0
                            : std::fmod(static_cast<double>(current_render_frame_) *
                                            voice.lfo_step,
                                        1.0);
                    voice.filter_level = split.filter_levels[0u];
                    ++voice_serial_;
                    if (voice_serial_ == 0u) ++voice_serial_;
                    voice.start_serial = voice_serial_;
                    configure_envelope(voice, EnvelopeStage::Attack);
                    configure_filter_envelope(voice, EnvelopeStage::Attack);
                    created.push_back(insert_voice(std::move(voice)));
                }
            }
        } catch (...) {
            for (const auto handle : created) remove_voice(handle);
            throw;
        }
        return created;
    }

    [[nodiscard]] PlayableProgram require_playable_program(
        Collection& collection,
        const std::uint8_t bank_index,
        const std::uint8_t program_index,
        const std::uint8_t note,
        const std::uint8_t velocity) {
        if (bank_index >= maximum_mlt_banks ||
            !collection.program_banks[bank_index].has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "note-program-bank");
        auto& bank = *collection.program_banks[bank_index];
        if (program_index >= bank.programs.size())
            fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                            "note-program");
        const auto& program = bank.programs[program_index];
        const auto playable = std::ranges::any_of(
            program.layers, [&](const auto& layer) {
                return std::ranges::any_of(
                    layer.splits, [&](const auto& split) {
                        return note >= split.start_note &&
                               note <= split.end_note &&
                               velocity >= split.velocity_low &&
                               velocity <= split.velocity_high &&
                               split.tone_offset != 0u &&
                               split.loop_end != 0u;
                    });
            });
        if (!playable)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSample,
                            "note-no-playable-split");
        return {&bank, &program};
    }

    [[nodiscard]] std::shared_ptr<std::vector<std::int16_t>> decode_sample(
        Collection& collection,
        const ProgramBank& bank,
        const Split& split) {
        const auto format = static_cast<std::uint8_t>(split.format);
        const auto key =
            sample_key(split.tone_offset, split.loop_end, format, bank.bank);
        if (const auto found = collection.sample_cache.find(key);
            found != collection.sample_cache.end())
            return found->second;
        if (split.loop_end > config_.maximum_decoded_sample_frames ||
            collection.decoded_sample_frames >
                config_.maximum_decoded_sample_frames - split.loop_end)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "decoded-sample-budget");
        auto decoded = std::make_shared<std::vector<std::int16_t>>();
        decoded->reserve(split.loop_end);
        if (bank.payload_offset > collection.bytes.size() ||
            bank.payload_size >
                collection.bytes.size() - bank.payload_offset)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSample,
                            "sample-bank-payload");
        const auto payload = std::span<const std::uint8_t>(collection.bytes)
                                 .subspan(bank.payload_offset,
                                          bank.payload_size);
        const BoundedBytes bytes(payload);
        if (split.format == SampleFormat::Pcm16) {
            for (std::uint32_t frame = 0u; frame < split.loop_end; ++frame) {
                const auto value = bytes.u16(
                    split.tone_offset + static_cast<std::uint64_t>(frame) * 2u,
                    NativePortSoundBankFailure::InvalidSample,
                    "pcm16");
                decoded->push_back(std::bit_cast<std::int16_t>(value));
            }
        } else if (split.format == SampleFormat::Pcm8) {
            for (std::uint32_t frame = 0u; frame < split.loop_end; ++frame) {
                const auto value = bytes.i8(
                    split.tone_offset + frame,
                    NativePortSoundBankFailure::InvalidSample,
                    "pcm8");
                decoded->push_back(static_cast<std::int16_t>(value) * 256);
            }
        } else {
            static constexpr std::array<std::int32_t, 8u> step_scale{
                230, 230, 230, 230, 307, 409, 512, 614};
            std::int32_t predictor = 0;
            std::int32_t step = 127;
            for (std::uint32_t frame = 0u; frame < split.loop_end; ++frame) {
                const auto packed = bytes.u8(
                    split.tone_offset + frame / 2u,
                    NativePortSoundBankFailure::InvalidSample,
                    "adpcm");
                const auto nibble = static_cast<std::uint8_t>(
                    (frame & 1u) == 0u ? packed & 15u : packed >> 4u);
                const auto magnitude = static_cast<std::int32_t>(nibble & 7u);
                const auto delta = std::min(
                    ((magnitude * 2 + 1) * step) >> 3, 0x7FFF);
                predictor += (nibble & 8u) != 0u ? -delta : delta;
                predictor = std::clamp(predictor, -32768, 32767);
                step = std::clamp(
                    (step * step_scale[static_cast<std::size_t>(magnitude)]) >>
                        8,
                    127,
                    24576);
                decoded->push_back(static_cast<std::int16_t>(predictor));
            }
        }
        collection.decoded_sample_frames += split.loop_end;
        saturating_add(decoded_samples_, 1u);
        switch (split.format) {
            case SampleFormat::Pcm16:
                saturating_add(decoded_pcm16_samples_, 1u);
                break;
            case SampleFormat::Pcm8:
                saturating_add(decoded_pcm8_samples_, 1u);
                break;
            case SampleFormat::Adpcm4:
                saturating_add(decoded_adpcm_samples_, 1u);
                break;
        }
        saturating_add(decoded_sample_frames_, split.loop_end);
        collection.sample_cache.emplace(key, decoded);
        return decoded;
    }

    [[nodiscard]] static float velocity_gain(const ProgramBank& bank,
                                             const Split& split,
                                             const std::uint8_t velocity) {
        if (bank.velocity_curves.empty())
            return static_cast<float>(velocity) / 127.0f;
        return static_cast<float>(
                   bank.velocity_curves[split.velocity_curve][velocity]) /
               127.0f;
    }

    [[nodiscard]] static double pitch_step(const Split& split,
                                           const std::uint8_t note,
                                           const double normalized_pitch_bend,
                                           const std::uint8_t bend_high = 2u,
                                           const std::uint8_t bend_low = 2u) {
        const auto bend = normalized_pitch_bend >= 0.0
                              ? normalized_pitch_bend * bend_high
                              : normalized_pitch_bend * bend_low;
        const auto semitones = static_cast<double>(
                                   static_cast<int>(note) -
                                   static_cast<int>(split.base_note)) +
                               static_cast<double>(split.fine_tune) / 256.0 +
                               static_cast<double>(split.pitch_octave) * 12.0 +
                               bend;
        return std::pow(2.0, semitones / 12.0) *
               (1.0 + static_cast<double>(split.pitch_fns) / 1024.0);
    }

    static constexpr std::array<double, 64u> attack_milliseconds_{
        -1, -1, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000,
        1700, 1500, 1200, 1000, 860, 760, 600, 500, 430, 380, 300, 250,
        220, 190, 150, 130, 110, 95, 76, 63, 55, 47, 38, 31, 27, 24, 19,
        15, 13, 12, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0,
        1.8, 1.6, 1.3, 1.1, .93, .85, .65, .53, .44, .40, .35, 0, 0};
    static constexpr std::array<double, 64u> decay_milliseconds_{
        -1, -1, 118200, 101300, 88600, 70900, 59100, 50700, 44300,
        35500, 29600, 25300, 22200, 17700, 14800, 12700, 11100, 8900,
        7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600,
        1400, 1100, 920, 790, 690, 550, 460, 390, 340, 270, 230, 200,
        170, 140, 110, 98, 85, 68, 57, 49, 43, 34, 28, 25, 22, 18, 14,
        12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};

    [[nodiscard]] static std::uint8_t effective_rate(
        const Split& split,
        const std::uint8_t note,
        const std::uint8_t rate) noexcept {
        auto effective = static_cast<int>(rate) * 2;
        if (split.key_rate_scaling < 15u) {
            const auto ratio = pitch_step(split, note, 0);
            if (!(ratio > 0.0) || !std::isfinite(ratio)) return 0u;
            auto octave = static_cast<int>(std::floor(std::log2(ratio)));
            auto fraction = ratio / std::exp2(static_cast<double>(octave));
            auto fns = static_cast<int>(std::llround((fraction - 1.0) * 1024.0));
            if (fns >= 1024) {
                fns = 0;
                ++octave;
            }
            effective += (fns >> 9) & 1;
            effective +=
                std::max(0, (static_cast<int>(split.key_rate_scaling) + octave) * 2);
        }
        return static_cast<std::uint8_t>(std::clamp(effective, 0, 63));
    }

    void configure_envelope(SynthVoice& voice,
                            const EnvelopeStage stage) const {
        voice.envelope_stage = stage;
        if (stage == EnvelopeStage::Complete) {
            voice.amplitude_attenuation = 1023.0;
            voice.stage_step = 0.0;
            return;
        }
        const auto& split = *voice.split;
        const auto rate = stage == EnvelopeStage::Attack
                              ? split.attack
                          : stage == EnvelopeStage::Decay1
                              ? split.decay1
                          : stage == EnvelopeStage::Decay2
                              ? split.decay2
                              : split.release;
        const auto effective = effective_rate(split, voice.note, rate);
        const auto milliseconds =
            stage == EnvelopeStage::Attack ? attack_milliseconds_[effective]
                                            : decay_milliseconds_[effective];
        if (stage == EnvelopeStage::Attack) {
            if (milliseconds <= 0.0) {
                if (milliseconds < 0.0) {
                    voice.stage_step = 0.0;
                    return;
                }
                voice.amplitude_attenuation = 0.0;
                if (split.loop_start_link) {
                    voice.stage_step = 0.0;
                    return;
                }
                configure_envelope(voice, EnvelopeStage::Decay1);
                return;
            }
            const auto samples =
                milliseconds * config_.output_sample_rate / 1000.0;
            voice.stage_step =
                1.0 / (1.0 - std::pow(640.0, -1.0 / samples));
        } else {
            voice.decay_target =
                stage == EnvelopeStage::Decay1
                    ? static_cast<double>(split.decay_level) * 32.0
                    : 1023.0;
            voice.stage_step = milliseconds < 0.0
                                   ? 0.0
                               : milliseconds == 0.0
                                   ? 1024.0
                                   : 1024.0 /
                                         (milliseconds *
                                          config_.output_sample_rate / 1000.0);
        }
    }

    void configure_filter_envelope(SynthVoice& voice,
                                   const EnvelopeStage stage) const {
        voice.filter_stage = stage;
        if (!voice.split->filter || stage == EnvelopeStage::Complete) {
            voice.filter_step = 0.0;
            return;
        }
        const auto& split = *voice.split;
        const auto rate_index = stage == EnvelopeStage::Attack
                                    ? 1u
                                : stage == EnvelopeStage::Decay1
                                    ? 0u
                                : stage == EnvelopeStage::Decay2
                                    ? 3u
                                    : 2u;
        const auto target_index = stage == EnvelopeStage::Attack
                                      ? 1u
                                  : stage == EnvelopeStage::Decay1
                                      ? 2u
                                  : stage == EnvelopeStage::Decay2
                                      ? 3u
                                      : 4u;
        voice.filter_target = split.filter_levels[target_index];
        const auto effective =
            effective_rate(split, voice.note, split.filter_rates[rate_index]);
        const auto milliseconds = decay_milliseconds_[effective];
        // FEG spans eight times the AEG attenuation range while using the
        // same step table, hence an authored full-range transition takes 8x
        // the table duration.
        voice.filter_step = milliseconds < 0.0
                                ? 0.0
                            : milliseconds == 0.0
                                ? 8192.0
                                : 1024.0 /
                                      (milliseconds *
                                       config_.output_sample_rate / 1000.0);
    }

    void key_off(SynthVoice& voice) {
        if (voice.key_released) return;
        voice.key_released = true;
        configure_envelope(voice, EnvelopeStage::Release);
        configure_filter_envelope(voice, EnvelopeStage::Release);
    }

    [[nodiscard]] bool advance_envelope(SynthVoice& voice) {
        switch (voice.envelope_stage) {
        case EnvelopeStage::Attack: {
            if (voice.stage_step <= 0.0) break;
            voice.amplitude_attenuation -=
                voice.amplitude_attenuation / voice.stage_step +
                1.0 / 65536.0;
            if (voice.amplitude_attenuation <= 0.0) {
                voice.amplitude_attenuation = 0.0;
                if (!voice.split->loop_start_link)
                    configure_envelope(voice, EnvelopeStage::Decay1);
            }
            break;
        }
        case EnvelopeStage::Decay1:
            // Loop-start-linked Attack may enter above the Decay1 target.
            // A decay step must not reduce attenuation at that boundary.
            voice.amplitude_attenuation += voice.stage_step;
            if (voice.amplitude_attenuation >= voice.decay_target)
                configure_envelope(voice, EnvelopeStage::Decay2);
            break;
        case EnvelopeStage::Decay2:
            voice.amplitude_attenuation = std::min(
                voice.decay_target,
                voice.amplitude_attenuation + voice.stage_step);
            if (voice.amplitude_attenuation >= 1023.0)
                configure_envelope(voice, EnvelopeStage::Release);
            break;
        case EnvelopeStage::Release:
            voice.amplitude_attenuation = std::min(
                voice.decay_target,
                voice.amplitude_attenuation + voice.stage_step);
            if (voice.amplitude_attenuation >= 1023.0)
                configure_envelope(voice, EnvelopeStage::Complete);
            break;
        case EnvelopeStage::Complete:
            return false;
        }
        return voice.envelope_stage != EnvelopeStage::Complete;
    }

    void advance_filter_envelope(SynthVoice& voice,
                                 const Channel& channel) const {
        if (!voice.split->filter || voice.filter_stage == EnvelopeStage::Complete)
            return;
        if (voice.filter_stage == EnvelopeStage::Decay2)
            voice.filter_target = channel.filter_level3.value_or(
                voice.split->filter_levels[3]);
        if (voice.filter_level < voice.filter_target)
            voice.filter_level =
                std::min(voice.filter_target,
                         voice.filter_level + voice.filter_step);
        else if (voice.filter_level > voice.filter_target)
            voice.filter_level =
                std::max(voice.filter_target,
                         voice.filter_level - voice.filter_step);
        if (voice.filter_level != voice.filter_target) return;
        if (voice.filter_stage == EnvelopeStage::Attack)
            configure_filter_envelope(voice, EnvelopeStage::Decay1);
        else if (voice.filter_stage == EnvelopeStage::Decay1)
            configure_filter_envelope(voice, EnvelopeStage::Decay2);
    }

    [[nodiscard]] static std::uint8_t lfo_wave(const std::uint8_t shape,
                                               const double phase) noexcept {
        const auto state = static_cast<std::uint8_t>(
            static_cast<std::uint32_t>(phase * 256.0) & 0xFFu);
        switch (shape) {
        case 0u:
            return state;
        case 1u:
            return (state & 0x80u) != 0u ? 255u : 0u;
        case 2u:
            return static_cast<std::uint8_t>(
                ((state & 0x7Fu) ^ ((state & 0x80u) != 0u ? 0x7Fu : 0u))
                << 1u);
        case 3u:
            return static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(state) * 0x41C64E6Du + 0x3039u) &
                0xFFu);
        }
        return 0u;
    }

    [[nodiscard]] static double lfo_frequency(
        const std::uint8_t value) noexcept {
        const auto shift = static_cast<std::uint32_t>(value >> 2u);
        const auto group = 128u >> shift;
        const auto multiplier = static_cast<std::uint32_t>((~value) & 3u);
        const auto counter = ((group - 1u) << 2u) +
                             group * (multiplier + 1u);
        return aica_source_sample_rate /
               (static_cast<double>(counter) * 256.0);
    }

    [[nodiscard]] bool sequence_matches(
        const SynthVoice& voice,
        const NativePortSoundSequenceHandle handle) const noexcept {
        return voice.sequence.has_value() && voice.sequence->slot == handle.slot &&
               voice.sequence->generation == handle.generation;
    }

    void release_sustained(const NativePortSoundSequenceHandle handle,
                           const std::uint8_t channel) {
        for (const auto index : active_voice_indices_) {
            auto& voice = *voices_[index].value;
            if (sequence_matches(voice, handle) && voice.channel == channel &&
                voice.sustained) {
                voice.sustained = false;
                key_off(voice);
            }
        }
    }

    [[nodiscard]] const Sequence& sequence_definition(
        const ActiveSequence& active) const {
        const auto& collection = require_collection(active.collection);
        const auto& bank = *collection.sequence_banks[active.config.sequence_bank];
        return *bank.sequences[active.config.sequence];
    }

    void dispatch_event(const NativePortSoundSequenceHandle handle,
                        ActiveSequence& active,
                        const Event& event,
                        bool& jumped) {
        auto& channel = active.channels[event.channel];
        switch (event.kind) {
        case EventKind::Note: {
            const auto release = event.tick >
                                         std::numeric_limits<std::uint64_t>::max() -
                                             event.gate
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : event.tick + event.gate;
            static_cast<void>(spawn_program(active.collection,
                                            handle,
                                            channel.bank,
                                            channel.program,
                                            event.data0,
                                            event.data1,
                                            event.channel,
                                            {},
                                            release,
                                            event.tick));
            break;
        }
        case EventKind::Control:
            dispatch_controller(handle,
                                active,
                                event.channel,
                                event.data0,
                                event.data1);
            break;
        case EventKind::Program:
            channel.program = event.data0;
            break;
        case EventKind::Pressure:
            channel.pressure = event.data0;
            break;
        case EventKind::Pitch:
            channel.pitch = static_cast<std::int8_t>(
                static_cast<int>(event.data0) - 64);
            break;
        case EventKind::Tempo:
            if (event.value == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                                "sequence-tempo");
            active.tempo = event.value;
            break;
        case EventKind::Loop:
            if (!active.loop_marker_open) {
                active.loop_marker_open = true;
                active.loop_event = active.next_event + 1u;
                active.loop_tick = event.tick;
                active.loop_tempo = active.tempo;
            } else {
                if (active.config.enable_authored_loops) {
                    if (event.tick <= active.loop_tick)
                        fail_sound_bank(
                            NativePortSoundBankFailure::InvalidSequence,
                            "sequence-zero-loop");
                    active.next_event = active.loop_event;
                    active.tick = static_cast<double>(active.loop_tick);
                    active.tempo = active.loop_tempo;
                    saturating_add(active.loop_count, 1u);
                    jumped = true;
                } else
                    active.loop_marker_open = false;
            }
            break;
        case EventKind::SysEx:
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedController,
                            "sequence-sysex");
        }
        saturating_add(active.dispatched_events, 1u);
    }

    void dispatch_controller(const NativePortSoundSequenceHandle handle,
                             ActiveSequence& active,
                             const std::uint8_t channel_index,
                             const std::uint8_t controller,
                             const std::uint8_t value) {
        if (value > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "controller-value");
        auto& channel = active.channels[channel_index];
        switch (controller) {
        case 1u:
            channel.modulation = value;
            break;
        case 7u:
            channel.volume = value;
            break;
        case 10u:
            channel.pan = value;
            break;
        case 11u:
            channel.expression = value;
            break;
        case 23u:
            channel.filter_level3 = static_cast<std::uint16_t>(value) << 6u;
            break;
        case 32u:
            if (value >= maximum_mlt_banks ||
                !require_collection(active.collection)
                     .program_banks[value]
                     .has_value())
                fail_sound_bank(NativePortSoundBankFailure::InvalidProgramBank,
                                "controller-bank");
            channel.bank = value;
            break;
        case 64u: {
            const bool sustain = value >= 64u;
            if (channel.sustain && !sustain)
                release_sustained(handle, channel_index);
            channel.sustain = sustain;
            break;
        }
        case 80u:
            channel.qsound_position = value;
            break;
        case 91u:
            channel.effect_depth = value;
            break;
        case 121u:
            if (channel.sustain) release_sustained(handle, channel_index);
            channel.expression = 127u;
            channel.modulation = 0u;
            channel.pressure = 127u;
            channel.effect_depth = 127u;
            channel.qsound_position = 64u;
            channel.pitch = 0;
            channel.filter_level3.reset();
            channel.sustain = false;
            break;
        default:
            fail_sound_bank(NativePortSoundBankFailure::UnsupportedController,
                            "controller");
        }
    }

    void process_sequence_events(const NativePortSoundSequenceHandle handle,
                                 ActiveSequence& active) {
        if (active.state != NativePortSoundSequenceState::Playing) return;
        const auto& definition = sequence_definition(active);
        std::uint32_t dispatch_budget = config_.maximum_events_per_sequence;
        while (active.next_event < definition.events.size() &&
               static_cast<double>(definition.events[active.next_event].tick) <=
                   active.tick + 1e-9) {
            if (dispatch_budget-- == 0u)
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "sequence-dispatch-budget");
            const auto event = definition.events[active.next_event];
            bool jumped = false;
            dispatch_event(handle, active, event, jumped);
            if (!jumped) ++active.next_event;
        }
        if (active.next_event >= definition.events.size() &&
            active.tick >= static_cast<double>(definition.end_tick)) {
            if (active.loop_marker_open &&
                active.config.enable_authored_loops &&
                definition.end_tick > active.loop_tick) {
                active.next_event = active.loop_event;
                active.tick = static_cast<double>(active.loop_tick);
                active.tempo = active.loop_tempo;
                saturating_add(active.loop_count, 1u);
            } else if (!has_sequence_voices(handle)) {
                active.state = NativePortSoundSequenceState::Completed;
            }
        }
    }

    [[nodiscard]] bool has_sequence_voices(
        const NativePortSoundSequenceHandle handle) const noexcept {
        return std::ranges::any_of(active_voice_indices_, [&](const auto index) {
            return sequence_matches(*voices_[index].value, handle);
        });
    }

    void advance_sequence_time(ActiveSequence& active) const {
        const auto& definition = sequence_definition(active);
        const auto denominator =
            static_cast<double>(config_.output_sample_rate) * active.tempo *
            definition.tpqn;
        if (!(denominator > 0.0) || !std::isfinite(denominator))
            fail_sound_bank(NativePortSoundBankFailure::InvalidSequence,
                            "sequence-clock");
        active.tick += 1000.0 * 65536.0 /
                       denominator * active.config.playback_rate;
        saturating_add(active.rendered_frames, 1u);
    }

    [[nodiscard]] static double combined_pan(const Split& split,
                                             const Channel& channel,
                                             const float external) noexcept {
        const auto split_pan = (split.pan & 0x10u) != 0u
                                   ? -static_cast<double>(split.pan & 15u) / 15.0
                                   : static_cast<double>(split.pan & 15u) / 15.0;
        const auto channel_pan =
            (static_cast<double>(channel.pan) - 64.0) / 63.0;
        const auto qsound_pan =
            (static_cast<double>(channel.qsound_position) - 64.0) / 63.0;
        return std::clamp(split_pan + channel_pan + qsound_pan * 0.5 +
                              static_cast<double>(external),
                          -1.0,
                          1.0);
    }

    [[nodiscard]] static double lfo_pitch_cents(
        const std::uint8_t depth) noexcept {
        static constexpr std::array<double, 8u> cents{
            0.0, 3.61, 7.22, 14.44, 28.88, 57.75, 115.5, 231.0};
        return cents[depth];
    }

    [[nodiscard]] double render_voice(SynthVoice& voice,
                                      const Channel& channel,
                                      const std::uint64_t sequence_tick,
                                      const float external_gain,
                                      const std::int16_t external_pitch_bend) {
        if (sequence_tick < voice.delayed_until_tick) return 0.0;
        if (!voice.key_released && sequence_tick >= voice.release_tick) {
            if (channel.sustain) {
                voice.sustained = true;
                voice.release_tick = std::numeric_limits<std::uint64_t>::max();
            } else {
                key_off(voice);
            }
        }
        if (!advance_envelope(voice)) return 0.0;
        const auto& split = *voice.split;
        const auto wave_pitch = lfo_wave(split.lfo_pitch_wave, voice.lfo_phase);
        const auto wave_amp = lfo_wave(split.lfo_amp_wave, voice.lfo_phase);
        if (!voice.lfo_pitch_cache_valid ||
            voice.cached_modulation != channel.modulation) {
            const auto modulation =
                static_cast<double>(channel.modulation) / 127.0;
            if (split.lfo_pitch_depth == 0u || channel.modulation == 0u) {
                voice.lfo_pitch_ratio.fill(1.0);
            } else {
                const auto depth = lfo_pitch_cents(split.lfo_pitch_depth);
                for (std::size_t index = 0u;
                     index < voice.lfo_pitch_ratio.size(); ++index) {
                    const auto cents =
                        (static_cast<double>(index) - 128.0) / 128.0 *
                        depth * modulation;
                    voice.lfo_pitch_ratio[index] =
                        std::pow(2.0, cents / 1200.0);
                }
            }
            voice.cached_modulation = channel.modulation;
            voice.lfo_pitch_cache_valid = true;
        }
        if (!voice.pitch_cache_valid ||
            voice.cached_channel_pitch != channel.pitch ||
            voice.cached_external_pitch_bend != external_pitch_bend) {
            const auto channel_bend =
                channel.pitch >= 0
                    ? static_cast<double>(channel.pitch) / 63.0
                    : static_cast<double>(channel.pitch) / 64.0;
            const auto external_bend =
                external_pitch_bend >= 0
                    ? static_cast<double>(external_pitch_bend) / 8191.0
                    : static_cast<double>(external_pitch_bend) / 8192.0;
            const auto combined_bend =
                std::clamp(channel_bend + external_bend, -1.0, 1.0);
            voice.base_step =
                pitch_step(split,
                           voice.note,
                           combined_bend,
                           voice.bend_high,
                           voice.bend_low) *
                aica_source_sample_rate / config_.output_sample_rate;
            voice.cached_channel_pitch = channel.pitch;
            voice.cached_external_pitch_bend = external_pitch_bend;
            voice.pitch_cache_valid = true;
        }
        const auto step = voice.base_step * voice.lfo_pitch_ratio[wave_pitch];
        if (!(step > 0.0) || !std::isfinite(step))
            fail_sound_bank(NativePortSoundBankFailure::InvalidSample,
                            "voice-pitch");
        auto position = static_cast<std::uint64_t>(voice.phase);
        if (split.loop_start_link &&
            voice.envelope_stage == EnvelopeStage::Attack &&
            position >= split.loop_start)
            configure_envelope(voice, EnvelopeStage::Decay1);
        if (position >= split.loop_end) {
            if (!split.loop) {
                configure_envelope(voice, EnvelopeStage::Complete);
                return 0.0;
            }
            const auto length = static_cast<std::uint64_t>(
                split.loop_end - split.loop_start);
            const auto whole = std::floor(voice.phase);
            const auto fraction = voice.phase - whole;
            position = split.loop_start +
                       (position - split.loop_start) % length;
            // Preserve the sub-sample accumulator across the authored loop.
            // Resetting to an integer phase-modulates short, bright SFX on
            // every wrap even though their pitch step remains unchanged.
            voice.phase = static_cast<double>(position) + fraction;
        }
        const auto next_position =
            position + 1u < split.loop_end
                ? position + 1u
                : split.loop ? split.loop_start : position;
        const auto fraction = voice.phase - std::floor(voice.phase);
        const auto first = static_cast<double>((*voice.sample)[position]);
        const auto second = static_cast<double>((*voice.sample)[next_position]);
        double sample = first + (second - first) * fraction;

        // Match the neutral FEG bypass using effective controller values.
        // Preserve both envelope and recursive history while bypassed.
        const bool filter_active = split.filter &&
            (split.filter_resonance != 4u ||
             split.filter_levels[0] < 8184u ||
             split.filter_levels[1] < 8184u ||
             split.filter_levels[2] < 8184u ||
             channel.filter_level3.value_or(split.filter_levels[3]) < 8184u ||
             split.filter_levels[4] < 8184u);
        if (filter_active) {
            advance_filter_envelope(voice, channel);
            const auto value = static_cast<std::uint32_t>(std::clamp(
                std::llround(voice.filter_level), 0ll, 8184ll));
            const auto exponent = value >> 9u;
            if (!voice.filter_coefficient_cache_valid ||
                voice.filter_coefficient_value != value) {
                const auto mantissa = (value & 0x1FFu) | 0x200u;
                std::uint64_t a0 =
                    (static_cast<std::uint64_t>(mantissa) << 30u) >>
                    ((15u - exponent) * 2u);
                a0 *= (mantissa - 1u) / 8u;
                a0 >>= 17u;
                std::int64_t frequency =
                    (static_cast<std::int64_t>(mantissa) << exponent) << 5u;
                frequency +=
                    static_cast<std::int64_t>(
                        aica_filter_q[split.filter_resonance]) *
                    frequency / 4096;
                voice.filter_a0 = static_cast<double>(a0);
                voice.filter_b1 =
                    2147483648.0 -
                    (static_cast<double>(frequency) + voice.filter_a0);
                voice.filter_b2 =
                    1073741824.0 - static_cast<double>(frequency);
                voice.filter_coefficient_value = value;
                voice.filter_coefficient_cache_valid = true;
            }
            if (exponent == 0u) {
                voice.filter_previous_1 = 0.0;
                voice.filter_previous_2 = 0.0;
            }
            constexpr double coefficient_scale = 1073741824.0;
            const auto filtered =
                (-voice.filter_a0 * sample +
                 voice.filter_b1 * voice.filter_previous_1 -
                 voice.filter_b2 * voice.filter_previous_2) /
                coefficient_scale;
            voice.filter_previous_2 = voice.filter_previous_1;
            voice.filter_previous_1 = std::clamp(filtered,
                                                 -524288.0,
                                                 524287.0);
            sample = voice.filter_previous_1;
        }

        const auto lfo_attenuation = static_cast<double>(
            static_cast<std::uint32_t>(wave_amp) >>
            (8u - split.lfo_amp_depth));
        const auto total_attenuation =
            split.voice_attenuation_off
                ? 0.0
                : static_cast<double>(255u - split.oscillator_level) +
                      std::floor(voice.amplitude_attenuation / 4.0) +
                      lfo_attenuation;
        const auto channel_gain =
            static_cast<double>(channel.volume) / 127.0 *
            static_cast<double>(channel.expression) / 127.0 *
            static_cast<double>(channel.pressure) / 127.0;
        const auto attenuation_gain =
            !(total_attenuation < 255.0)
                ? 0.0
                : aica_attenuation_gain_table()[
                      static_cast<std::uint8_t>(
                          std::max(0.0, total_attenuation))];
        sample *= attenuation_gain *
                  voice.velocity_gain * static_cast<double>(external_gain) *
                  channel_gain;
        voice.phase += step;
        voice.lfo_phase += voice.lfo_step;
        // Both the normalized starting phase and the bounded AICA LFO step
        // are below one, so at most one wrap is possible per output frame.
        if (voice.lfo_phase >= 1.0) voice.lfo_phase -= 1.0;
        return sample;
    }

    void render_block(const std::uint32_t frames) {
        const auto sample_count = static_cast<std::size_t>(frames) * 2u;
        std::fill_n(mix_.begin(), sample_count, 0.0);
        for (auto& collection : collections_)
            if (collection.value.has_value() &&
                collection.value->effect_kernel_state)
                std::fill_n(collection.value->effect_sends.begin(),
                            static_cast<std::size_t>(frames) *
                                maximum_effect_buses,
                            0.0);
        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            current_render_frame_ = rendered_frames_ + frame;
            for (const auto index : active_sequence_indices_) {
                auto& active = *sequences_[index].value;
                const NativePortSoundSequenceHandle handle{
                    index, sequences_[index].generation};
                process_sequence_events(handle, active);
            }

            std::size_t active_voice = 0u;
            while (active_voice < active_voice_indices_.size()) {
                const auto index = active_voice_indices_[active_voice];
                auto& slot = voices_[index];
                auto& voice = *slot.value;
                Channel direct_channel;
                const Channel* channel = &direct_channel;
                std::uint64_t tick = 0u;
                bool paused = false;
                float external_gain = 1.0f;
                float external_pan = 0.0f;
                std::int16_t external_pitch_bend = 0;
                std::uint8_t external_direct_level = 127u;
                std::uint8_t external_effect_level = 127u;
                if (voice.sequence.has_value()) {
                    if (voice.sequence->slot >= sequences_.size() ||
                        sequences_[voice.sequence->slot].generation !=
                            voice.sequence->generation ||
                        !sequences_[voice.sequence->slot].value.has_value()) {
                        erase_voice_slot(index);
                        continue;
                    }
                    const auto& sequence =
                        *sequences_[voice.sequence->slot].value;
                    paused = sequence.state == NativePortSoundSequenceState::Paused;
                    if (sequence.state == NativePortSoundSequenceState::Stopped) {
                        erase_voice_slot(index);
                        continue;
                    }
                    channel = &sequence.channels[voice.channel];
                    tick = static_cast<std::uint64_t>(sequence.tick);
                    external_gain = sequence.config.gain;
                    external_pan = sequence.config.pan;
                    external_pitch_bend = sequence.config.pitch_bend;
                    external_direct_level = sequence.config.direct_level;
                    external_effect_level = sequence.config.effect_level;
                } else if (voice.controls) {
                    external_gain = voice.controls->gain;
                    external_pan = voice.controls->pan;
                    external_pitch_bend = voice.controls->pitch_bend;
                    external_direct_level = voice.controls->direct_level;
                    external_effect_level = voice.controls->effect_level;
                }
                if (paused) {
                    ++active_voice;
                    continue;
                }
                const auto mono = render_voice(voice,
                                               *channel,
                                               tick,
                                               external_gain,
                                               external_pitch_bend);
                if (voice.envelope_stage == EnvelopeStage::Complete) {
                    erase_voice_slot(index);
                    continue;
                }
                if (!voice.pan_cache_valid ||
                    voice.cached_channel_pan != channel->pan ||
                    voice.cached_qsound_position != channel->qsound_position ||
                    voice.cached_external_pan != external_pan) {
                    const auto pan = combined_pan(*voice.split,
                                                  *channel,
                                                  external_pan);
                    const auto pan_amount = static_cast<std::uint8_t>(std::clamp(
                        std::llround(std::abs(pan) * 15.0), 0ll, 15ll));
                    const auto raw_pan = static_cast<std::uint8_t>(
                        pan < 0.0 ? 0x10u | pan_amount : pan_amount);
                    const auto [left, right] = aica_pan_gains(raw_pan);
                    voice.cached_pan_left = left;
                    voice.cached_pan_right = right;
                    voice.cached_channel_pan = channel->pan;
                    voice.cached_qsound_position = channel->qsound_position;
                    voice.cached_external_pan = external_pan;
                    voice.pan_cache_valid = true;
                }
                const auto direct =
                    aica_send_gain(voice.split->direct_level) *
                    static_cast<double>(external_direct_level) / 127.0;
                const auto destination = static_cast<std::size_t>(frame) * 2u;
                mix_[destination] += mono * direct * voice.cached_pan_left;
                mix_[destination + 1u] +=
                    mono * direct * voice.cached_pan_right;
                const auto send =
                    mono * aica_send_gain(voice.split->effect_level) *
                    static_cast<double>(channel->effect_depth) / 127.0 *
                    static_cast<double>(external_effect_level) / 127.0;
                if (send != 0.0) {
                    auto& collection = require_collection(voice.collection);
                    if (collection.effect_sends.empty())
                        fail_sound_bank(
                            NativePortSoundBankFailure::UnsupportedEffect,
                            "effect-send-without-program");
                    collection.effect_sends[
                        static_cast<std::size_t>(frame) *
                            maximum_effect_buses +
                        voice.split->effect_bus] += send;
                }
                ++active_voice;
            }

            for (const auto index : active_sequence_indices_) {
                auto& sequence = *sequences_[index].value;
                if (sequence.state != NativePortSoundSequenceState::Playing)
                    continue;
                advance_sequence_time(sequence);
            }
        }
        for (auto& collection : collections_)
            if (collection.value.has_value() &&
                collection.value->effect_kernel_state)
                process_effect_block(*collection.value, frames);
        // Keep the SoundBank contribution wide.  AudioEngine combines this
        // with codec/movie sources and performs the sole final S16 rounding
        // and saturation at the host-output boundary.
        saturating_add(rendered_frames_, frames);
        current_render_frame_ = rendered_frames_;
        collect_stale_groups_and_ports();
    }

    void process_effect_block(Collection& collection,
                              const std::uint32_t frames) {
        constexpr std::int64_t minimum_input = -(1ll << 19);
        constexpr std::int64_t maximum_input = (1ll << 19) - 1ll;
        const auto bus_values =
            static_cast<std::size_t>(frames) * maximum_effect_buses;
        for (std::size_t index = 0u; index < bus_values; ++index) {
            const auto value = collection.effect_sends[index];
            if (!std::isfinite(value))
                fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                                "effect-input");
            collection.effect_kernel_inputs[index] =
                static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    std::llround(value), minimum_input, maximum_input));
        }
        std::fill_n(collection.effect_kernel_outputs.begin(), bus_values, 0);
        const auto& state = collection.effect_kernel_state;
        if (!state.kernel->render(
                state.storage,
                std::span<const std::int32_t>(
                    collection.effect_kernel_inputs.data(), bus_values),
                std::span<std::int32_t>(
                    collection.effect_kernel_outputs.data(), bus_values),
                frames))
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "effect-render");

        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            const auto bus_base =
                static_cast<std::size_t>(frame) * maximum_effect_buses;
            const auto destination = static_cast<std::size_t>(frame) * 2u;
            for (std::size_t output = 0u; output < maximum_effect_buses;
                 ++output) {
                const auto value =
                    collection.effect_kernel_outputs[bus_base + output];
                if (value < std::numeric_limits<std::int16_t>::min() ||
                    value > std::numeric_limits<std::int16_t>::max())
                    fail_sound_bank(
                        NativePortSoundBankFailure::WorkerFailure,
                        "effect-output");
                const auto& route = collection.effect_outputs[output];
                const auto [left, right] = aica_pan_gains(route.pan);
                const auto level = aica_send_gain(route.level);
                mix_[destination] +=
                    static_cast<double>(value) * level * left;
                mix_[destination + 1u] +=
                    static_cast<double>(value) * level * right;
            }
        }
        saturating_add(effect_frames_, frames);
    }

    template <typename Handle>
    static void write_optional_handle(SoundBankStateWriter& writer,
                                      const std::optional<Handle>& value) {
        writer.boolean(value.has_value());
        if (value.has_value()) writer.handle(*value);
    }

    template <typename Handle>
    [[nodiscard]] static std::optional<Handle> read_optional_handle(
        SoundBankStateReader& reader) {
        if (!reader.boolean()) return std::nullopt;
        return reader.handle<Handle>();
    }

    static void write_voice_controls(SoundBankStateWriter& writer,
                                     const VoiceControls& value) {
        writer.f32(value.gain);
        writer.f32(value.pan);
        writer.scalar(value.pitch_bend);
        writer.scalar(value.direct_level);
        writer.scalar(value.effect_level);
    }

    [[nodiscard]] static VoiceControls read_voice_controls(
        SoundBankStateReader& reader) {
        VoiceControls value;
        value.gain = reader.f32();
        value.pan = reader.f32();
        value.pitch_bend = reader.scalar<std::int16_t>();
        value.direct_level = reader.scalar<std::uint8_t>();
        value.effect_level = reader.scalar<std::uint8_t>();
        if (!valid_gain_pan(value.gain, value.pan) ||
            value.pitch_bend < -8192 || value.pitch_bend > 8191 ||
            value.direct_level > 127u || value.effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-controls");
        return value;
    }

    static void write_sequence_config(SoundBankStateWriter& writer,
                                      const NativePortSoundSequenceConfig& value) {
        writer.scalar(value.sequence_bank);
        writer.scalar(value.program_bank);
        writer.scalar(value.sequence);
        writer.f32(value.gain);
        writer.f32(value.pan);
        writer.f32(value.playback_rate);
        writer.scalar(value.pitch_bend);
        writer.scalar(value.direct_level);
        writer.scalar(value.effect_level);
        writer.boolean(value.enable_authored_loops);
    }

    [[nodiscard]] static NativePortSoundSequenceConfig read_sequence_config(
        SoundBankStateReader& reader) {
        NativePortSoundSequenceConfig value;
        value.sequence_bank = reader.scalar<std::uint8_t>();
        value.program_bank = reader.scalar<std::uint8_t>();
        value.sequence = reader.scalar<std::uint16_t>();
        value.gain = reader.f32();
        value.pan = reader.f32();
        value.playback_rate = reader.f32();
        value.pitch_bend = reader.scalar<std::int16_t>();
        value.direct_level = reader.scalar<std::uint8_t>();
        value.effect_level = reader.scalar<std::uint8_t>();
        value.enable_authored_loops = reader.boolean();
        if (!valid_gain_pan(value.gain, value.pan) ||
            !valid_playback_rate(value.playback_rate) ||
            value.pitch_bend < -8192 || value.pitch_bend > 8191 ||
            value.direct_level > 127u || value.effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-sequence-config");
        return value;
    }

    static void write_midi_config(SoundBankStateWriter& writer,
                                  const NativePortSoundMidiPortConfig& value) {
        writer.scalar(value.program_bank);
        writer.scalar(value.program);
        writer.f32(value.gain);
        writer.f32(value.pan);
        writer.f32(value.playback_rate);
        writer.scalar(value.pitch_bend);
        writer.scalar(value.direct_level);
        writer.scalar(value.effect_level);
    }

    [[nodiscard]] static NativePortSoundMidiPortConfig read_midi_config(
        SoundBankStateReader& reader) {
        NativePortSoundMidiPortConfig value;
        value.program_bank = reader.scalar<std::uint8_t>();
        value.program = reader.scalar<std::uint8_t>();
        value.gain = reader.f32();
        value.pan = reader.f32();
        value.playback_rate = reader.f32();
        value.pitch_bend = reader.scalar<std::int16_t>();
        value.direct_level = reader.scalar<std::uint8_t>();
        value.effect_level = reader.scalar<std::uint8_t>();
        if (!valid_gain_pan(value.gain, value.pan) ||
            !valid_playback_rate(value.playback_rate) ||
            value.pitch_bend < -8192 || value.pitch_bend > 8191 ||
            value.direct_level > 127u || value.effect_level > 127u)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-midi-config");
        return value;
    }

    [[nodiscard]] std::array<std::uint32_t, 4u> split_location(
        const SynthVoice& voice) const {
        const auto& collection = require_collection(voice.collection);
        for (std::uint32_t bank_index = 0u; bank_index < maximum_mlt_banks;
             ++bank_index) {
            const auto& bank = collection.program_banks[bank_index];
            if (!bank.has_value()) continue;
            for (std::uint32_t program_index = 0u;
                 program_index < bank->programs.size(); ++program_index) {
                const auto& program = bank->programs[program_index];
                for (std::uint32_t layer_index = 0u;
                     layer_index < program.layers.size(); ++layer_index) {
                    const auto& layer = program.layers[layer_index];
                    for (std::uint32_t split_index = 0u;
                         split_index < layer.splits.size(); ++split_index)
                        if (&layer.splits[split_index] == voice.split)
                            return {bank_index, program_index, layer_index,
                                    split_index};
                }
            }
        }
        fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                        "state-split-reference");
    }

    [[nodiscard]] std::vector<std::uint8_t> capture_development_state() const {
        require_owner_thread();
        SoundBankStateWriter writer(config_.maximum_development_state_bytes);
        writer.raw(sound_bank_development_state_magic);
        writer.scalar(sound_bank_development_state_version);
        writer.scalar<std::uint64_t>(0u);
        writer.scalar(native_port_sound_bank_contract_version);
        writer.scalar(native_port_sound_effect_kernel_contract_version);
        writer.scalar(config_.output_sample_rate);
        writer.scalar(config_.render_block_frames);
        writer.scalar(config_.maximum_collections);
        writer.scalar(config_.maximum_collection_bytes);
        writer.scalar(config_.maximum_total_collection_bytes);
        writer.scalar(config_.maximum_units_per_collection);
        writer.scalar(config_.maximum_sequences_per_bank);
        writer.scalar(config_.maximum_events_per_sequence);
        writer.scalar(config_.maximum_reference_depth);
        writer.scalar(config_.maximum_active_sequences);
        writer.scalar(config_.maximum_synth_voices);
        writer.scalar(config_.maximum_midi_ports);
        writer.scalar(config_.maximum_decoded_sample_frames);
        writer.scalar(config_.maximum_effect_state_bytes);

        writer.scalar(current_render_frame_);
        writer.scalar(voice_serial_);
        writer.scalar(loaded_collections_);
        writer.scalar(unloaded_collections_);
        writer.scalar(parsed_programs_);
        writer.scalar(parsed_splits_);
        writer.scalar(parsed_sequences_);
        writer.scalar(parsed_events_);
        writer.scalar(decoded_samples_);
        writer.scalar(decoded_pcm16_samples_);
        writer.scalar(decoded_pcm8_samples_);
        writer.scalar(decoded_adpcm_samples_);
        writer.scalar(decoded_sample_frames_);
        writer.scalar(resident_collection_bytes_);
        writer.scalar(rendered_frames_);
        writer.scalar(effect_frames_);

        std::vector<std::shared_ptr<VoiceControls>> controls;
        std::unordered_map<const VoiceControls*, std::uint32_t> control_ids;
        const auto intern = [&](const std::shared_ptr<VoiceControls>& value) {
            if (!value) return std::numeric_limits<std::uint32_t>::max();
            const auto found = control_ids.find(value.get());
            if (found != control_ids.end()) return found->second;
            if (controls.size() == std::numeric_limits<std::uint32_t>::max())
                fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                                "state-control-count");
            const auto id = static_cast<std::uint32_t>(controls.size());
            controls.push_back(value);
            control_ids.emplace(value.get(), id);
            return id;
        };
        for (const auto& slot : groups_)
            if (slot.value.has_value()) static_cast<void>(intern(slot.value->controls));
        for (const auto& slot : ports_)
            if (slot.value.has_value()) static_cast<void>(intern(slot.value->controls));
        for (const auto& slot : voices_)
            if (slot.value.has_value()) static_cast<void>(intern(slot.value->controls));
        writer.scalar(static_cast<std::uint32_t>(controls.size()));
        for (const auto& value : controls) write_voice_controls(writer, *value);

        writer.scalar(static_cast<std::uint32_t>(collections_.size()));
        for (const auto& slot : collections_) {
            writer.scalar(slot.generation);
            writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            const auto& collection = *slot.value;
            writer.string(collection.logical_id);
            writer.string(std::string_view(
                reinterpret_cast<const char*>(collection.content_relative_path.data()),
                collection.content_relative_path.size()));
            writer.string(collection.byte_identity);
            writer.scalar(collection.source_offset);
            writer.scalar(static_cast<std::uint64_t>(collection.bytes.size()));
            writer.string(native_port_content_sha256(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(collection.bytes.data()),
                collection.bytes.size())));
            writer.raw(collection.bytes);
            std::vector<std::uint64_t> sample_keys;
            sample_keys.reserve(collection.sample_cache.size());
            for (const auto& [key, unused] : collection.sample_cache) {
                static_cast<void>(unused);
                sample_keys.push_back(key);
            }
            std::ranges::sort(sample_keys);
            writer.scalar(static_cast<std::uint32_t>(sample_keys.size()));
            for (const auto key : sample_keys) writer.scalar(key);
            writer.boolean(static_cast<bool>(collection.effect_kernel_state));
            if (collection.effect_kernel_state) {
                const auto* kernel = collection.effect_kernel_state.kernel;
                if (kernel->capture_snapshot == nullptr ||
                    kernel->validate_snapshot == nullptr ||
                    kernel->restore_snapshot == nullptr ||
                    kernel->maximum_snapshot_bytes == 0u ||
                    kernel->maximum_snapshot_bytes >
                        config_.maximum_effect_state_bytes)
                    fail_sound_bank(NativePortSoundBankFailure::StateUnavailable,
                                    "effect-state-capture");
                std::vector<std::uint8_t> state(
                    static_cast<std::size_t>(kernel->maximum_snapshot_bytes));
                std::uint64_t written = 0u;
                if (!kernel->capture_snapshot(collection.effect_kernel_state.storage,
                                              state, written) ||
                    written > state.size())
                    fail_sound_bank(NativePortSoundBankFailure::StateUnavailable,
                                    "effect-state-capture");
                state.resize(static_cast<std::size_t>(written));
                writer.scalar(written);
                writer.raw(state);
            }
        }

        writer.scalar(static_cast<std::uint32_t>(sequences_.size()));
        for (const auto& slot : sequences_) {
            writer.scalar(slot.generation);
            writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            writer.handle(value.collection);
            write_sequence_config(writer, value.config);
            writer.scalar(value.state);
            for (const auto& channel : value.channels) {
                writer.scalar(channel.bank); writer.scalar(channel.program);
                writer.scalar(channel.volume); writer.scalar(channel.expression);
                writer.scalar(channel.pan); writer.scalar(channel.modulation);
                writer.scalar(channel.pressure); writer.scalar(channel.effect_depth);
                writer.scalar(channel.qsound_position); writer.scalar(channel.pitch);
                writer.boolean(channel.filter_level3.has_value());
                if (channel.filter_level3.has_value())
                    writer.scalar(*channel.filter_level3);
                writer.boolean(channel.sustain);
            }
            writer.scalar(static_cast<std::uint64_t>(value.next_event));
            writer.f64(value.tick); writer.scalar(value.tempo);
            writer.boolean(value.loop_marker_open);
            writer.scalar(static_cast<std::uint64_t>(value.loop_event));
            writer.scalar(value.loop_tick); writer.scalar(value.loop_tempo);
            writer.scalar(value.rendered_frames);
            writer.scalar(value.dispatched_events); writer.scalar(value.loop_count);
        }

        writer.scalar(static_cast<std::uint32_t>(voices_.size()));
        for (const auto& slot : voices_) {
            writer.scalar(slot.generation);
            writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            writer.handle(value.collection);
            write_optional_handle(writer, value.sequence);
            for (const auto coordinate : split_location(value)) writer.scalar(coordinate);
            writer.scalar(value.channel); writer.scalar(value.note);
            writer.scalar(value.velocity); writer.f64(value.phase);
            writer.f64(value.base_step); writer.f64(value.lfo_step);
            for (const auto ratio : value.lfo_pitch_ratio) writer.f64(ratio);
            writer.scalar(value.cached_external_pitch_bend);
            writer.scalar(value.cached_channel_pitch);
            writer.scalar(value.cached_modulation);
            writer.boolean(value.pitch_cache_valid);
            writer.boolean(value.lfo_pitch_cache_valid);
            writer.scalar(value.bend_high); writer.scalar(value.bend_low);
            writer.f64(value.amplitude_attenuation); writer.f64(value.stage_step);
            writer.f64(value.decay_target); writer.scalar(value.envelope_stage);
            writer.f64(value.lfo_phase); writer.f64(value.filter_level);
            writer.f64(value.filter_target); writer.f64(value.filter_step);
            writer.scalar(value.filter_stage);
            writer.f64(value.filter_previous_1); writer.f64(value.filter_previous_2);
            writer.f64(value.filter_a0); writer.f64(value.filter_b1);
            writer.f64(value.filter_b2);
            writer.scalar(value.filter_coefficient_value);
            writer.boolean(value.filter_coefficient_cache_valid);
            writer.scalar(value.release_tick); writer.scalar(value.delayed_until_tick);
            writer.f32(value.velocity_gain);
            writer.scalar(intern(value.controls));
            writer.f32(value.cached_external_pan);
            writer.f64(value.cached_pan_left); writer.f64(value.cached_pan_right);
            writer.scalar(value.cached_channel_pan);
            writer.scalar(value.cached_qsound_position);
            writer.boolean(value.pan_cache_valid); writer.boolean(value.key_released);
            writer.boolean(value.sustained); writer.scalar(value.start_serial);
        }

        writer.scalar(static_cast<std::uint32_t>(groups_.size()));
        for (const auto& slot : groups_) {
            writer.scalar(slot.generation); writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            writer.handle(slot.value->collection);
            writer.scalar(intern(slot.value->controls));
            writer.scalar(static_cast<std::uint32_t>(slot.value->members.size()));
            for (const auto member : slot.value->members) writer.handle(member);
        }
        writer.scalar(static_cast<std::uint32_t>(ports_.size()));
        for (const auto& slot : ports_) {
            writer.scalar(slot.generation); writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            writer.handle(slot.value->collection);
            write_midi_config(writer, slot.value->config);
            writer.scalar(intern(slot.value->controls));
            for (const auto& note : slot.value->notes)
                write_optional_handle(writer, note);
            write_optional_handle(writer, slot.value->sequence);
        }
        for (const auto& slot : pcm_stream_rings_) {
            writer.scalar(slot.generation); writer.boolean(slot.value.has_value());
            if (!slot.value.has_value()) continue;
            writer.scalar(slot.value->bank);
            writer.scalar(slot.value->layout_offset);
            writer.scalar(slot.value->byte_size);
        }
        writer.finalize();
        return std::move(writer).take();
    }

    [[nodiscard]] std::pair<const ProgramBank*, const Split*>
    find_sample_split(Collection& collection, const std::uint64_t key) const {
        const auto bank_index = static_cast<std::uint8_t>(key >> 56u);
        const auto format = static_cast<std::uint8_t>(key >> 48u);
        const auto frames = static_cast<std::uint16_t>(key >> 32u);
        const auto offset = static_cast<std::uint32_t>(key);
        if (bank_index >= maximum_mlt_banks ||
            !collection.program_banks[bank_index].has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-sample-bank");
        const auto& bank = *collection.program_banks[bank_index];
        for (const auto& program : bank.programs)
            for (const auto& layer : program.layers)
                for (const auto& split : layer.splits)
                    if (split.tone_offset == offset && split.loop_end == frames &&
                        static_cast<std::uint8_t>(split.format) == format)
                        return {&bank, &split};
        fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                        "state-sample-reference");
    }

    [[nodiscard]] std::pair<const ProgramBank*, const Split*>
    split_at(Collection& collection,
             const std::array<std::uint32_t, 4u>& location) const {
        if (location[0] >= maximum_mlt_banks ||
            !collection.program_banks[location[0]].has_value())
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-split-bank");
        const auto& bank = *collection.program_banks[location[0]];
        if (location[1] >= bank.programs.size() ||
            location[2] >= bank.programs[location[1]].layers.size() ||
            location[3] >=
                bank.programs[location[1]].layers[location[2]].splits.size())
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-split-index");
        return {&bank,
                &bank.programs[location[1]].layers[location[2]].splits[location[3]]};
    }

    void restore_development_state(
        const std::span<const std::uint8_t> bytes) {
        require_owner_thread();
        SoundBankStateReader reader(bytes, config_.maximum_development_state_bytes);
        if (!std::ranges::equal(reader.raw(sound_bank_development_state_magic.size()),
                                sound_bank_development_state_magic) ||
            reader.scalar<std::uint32_t>() !=
                sound_bank_development_state_version ||
            reader.scalar<std::uint64_t>() != bytes.size() ||
            reader.scalar<std::uint32_t>() !=
                native_port_sound_bank_contract_version ||
            reader.scalar<std::uint32_t>() !=
                native_port_sound_effect_kernel_contract_version)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-header");
        const auto exact_u32 = [&](const std::uint32_t expected) {
            if (reader.scalar<std::uint32_t>() != expected)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-config");
        };
        const auto exact_u64 = [&](const std::uint64_t expected) {
            if (reader.scalar<std::uint64_t>() != expected)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-config");
        };
        exact_u32(config_.output_sample_rate);
        exact_u32(config_.render_block_frames);
        exact_u32(config_.maximum_collections);
        exact_u32(config_.maximum_collection_bytes);
        exact_u64(config_.maximum_total_collection_bytes);
        exact_u32(config_.maximum_units_per_collection);
        exact_u32(config_.maximum_sequences_per_bank);
        exact_u32(config_.maximum_events_per_sequence);
        exact_u32(config_.maximum_reference_depth);
        exact_u32(config_.maximum_active_sequences);
        exact_u32(config_.maximum_synth_voices);
        exact_u32(config_.maximum_midi_ports);
        exact_u32(config_.maximum_decoded_sample_frames);
        exact_u32(config_.maximum_effect_state_bytes);

        const auto saved_current_render_frame = reader.scalar<std::uint64_t>();
        const auto saved_voice_serial = reader.scalar<std::uint64_t>();
        const auto saved_loaded_collections = reader.scalar<std::uint64_t>();
        const auto saved_unloaded_collections = reader.scalar<std::uint64_t>();
        const auto saved_parsed_programs = reader.scalar<std::uint64_t>();
        const auto saved_parsed_splits = reader.scalar<std::uint64_t>();
        const auto saved_parsed_sequences = reader.scalar<std::uint64_t>();
        const auto saved_parsed_events = reader.scalar<std::uint64_t>();
        const auto saved_decoded_samples = reader.scalar<std::uint64_t>();
        const auto saved_decoded_pcm16_samples = reader.scalar<std::uint64_t>();
        const auto saved_decoded_pcm8_samples = reader.scalar<std::uint64_t>();
        const auto saved_decoded_adpcm_samples = reader.scalar<std::uint64_t>();
        const auto saved_decoded_sample_frames = reader.scalar<std::uint64_t>();
        const auto saved_resident_collection_bytes = reader.scalar<std::uint64_t>();
        const auto saved_rendered_frames = reader.scalar<std::uint64_t>();
        const auto saved_effect_frames = reader.scalar<std::uint64_t>();

        const auto control_count = reader.scalar<std::uint32_t>();
        if (control_count > config_.maximum_synth_voices +
                                config_.maximum_midi_ports)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-control-count");
        std::vector<std::shared_ptr<VoiceControls>> controls;
        controls.reserve(control_count);
        for (std::uint32_t index = 0u; index < control_count; ++index)
            controls.push_back(
                std::make_shared<VoiceControls>(read_voice_controls(reader)));
        const auto control = [&](const std::uint32_t id,
                                 const bool nullable) {
            if (id == std::numeric_limits<std::uint32_t>::max() && nullable)
                return std::shared_ptr<VoiceControls>{};
            if (id >= controls.size())
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-control-reference");
            return controls[id];
        };

        const auto collection_count = reader.scalar<std::uint32_t>();
        if (collection_count > config_.maximum_collections)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-collection-count");
        collections_.clear();
        collections_.resize(collection_count);
        std::uint64_t resident_bytes = 0u;
        for (auto& slot : collections_) {
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            if (!reader.boolean()) continue;
            Collection collection;
            collection.logical_id = reader.string();
            const auto path = reader.string();
            collection.content_relative_path.assign(
                reinterpret_cast<const char8_t*>(path.data()), path.size());
            collection.byte_identity = reader.string();
            collection.source_offset = reader.scalar<std::uint64_t>();
            const auto byte_count = reader.scalar<std::uint64_t>();
            if (byte_count < 32u || byte_count > config_.maximum_collection_bytes ||
                byte_count > config_.maximum_total_collection_bytes - resident_bytes)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-collection-size");
            const auto digest = reader.string();
            if (digest.size() != 64u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-collection-identity");
            const auto raw = reader.raw(byte_count);
            if (native_port_content_sha256(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size())) != digest)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-collection-identity");
            collection.bytes.assign(raw.begin(), raw.end());
            parse_collection(collection);
            resident_bytes += byte_count;

            const auto sample_count = reader.scalar<std::uint32_t>();
            if (sample_count > config_.maximum_decoded_sample_frames)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-sample-count");
            std::uint64_t last_key = 0u;
            bool first_key = true;
            for (std::uint32_t index = 0u; index < sample_count; ++index) {
                const auto key = reader.scalar<std::uint64_t>();
                if (!first_key && key <= last_key)
                    fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                    "state-sample-order");
                first_key = false;
                last_key = key;
                const auto [bank, split] = find_sample_split(collection, key);
                static_cast<void>(decode_sample(collection, *bank, *split));
            }
            const auto saved_effect = reader.boolean();
            if (saved_effect != static_cast<bool>(collection.effect_kernel_state))
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-effect-presence");
            if (saved_effect) {
                const auto effect_bytes = reader.raw(reader.scalar<std::uint64_t>());
                const auto* kernel = collection.effect_kernel_state.kernel;
                if (kernel->capture_snapshot == nullptr ||
                    kernel->validate_snapshot == nullptr ||
                    kernel->restore_snapshot == nullptr ||
                    effect_bytes.size() > kernel->maximum_snapshot_bytes)
                    fail_sound_bank(NativePortSoundBankFailure::StateUnavailable,
                                    "effect-state-restore");
                if (!kernel->validate_snapshot(effect_bytes))
                    fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                    "effect-state-validate");
                if (!kernel->restore_snapshot(collection.effect_kernel_state.storage,
                                              effect_bytes))
                    fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                    "effect-state-restore");
            }
            slot.value.emplace(std::move(collection));
        }

        const auto sequence_count = reader.scalar<std::uint32_t>();
        if (sequence_count > config_.maximum_active_sequences)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-sequence-count");
        sequences_.clear(); sequences_.resize(sequence_count);
        active_sequence_indices_.clear();
        active_sequence_positions_.assign(sequence_count, invalid_active_position);
        for (std::uint32_t index = 0u; index < sequence_count; ++index) {
            auto& slot = sequences_[index];
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            if (!reader.boolean()) continue;
            ActiveSequence value;
            value.collection = reader.handle<NativePortSoundCollectionHandle>();
            value.config = read_sequence_config(reader);
            value.state = reader.scalar<NativePortSoundSequenceState>();
            if (value.state > NativePortSoundSequenceState::Stopped)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-sequence-state");
            for (auto& channel : value.channels) {
                channel.bank = reader.scalar<std::uint8_t>();
                channel.program = reader.scalar<std::uint8_t>();
                channel.volume = reader.scalar<std::uint8_t>();
                channel.expression = reader.scalar<std::uint8_t>();
                channel.pan = reader.scalar<std::uint8_t>();
                channel.modulation = reader.scalar<std::uint8_t>();
                channel.pressure = reader.scalar<std::uint8_t>();
                channel.effect_depth = reader.scalar<std::uint8_t>();
                channel.qsound_position = reader.scalar<std::uint8_t>();
                channel.pitch = reader.scalar<std::int8_t>();
                if (reader.boolean())
                    channel.filter_level3 = reader.scalar<std::uint16_t>();
                channel.sustain = reader.boolean();
                if (channel.bank >= maximum_mlt_banks || channel.program > 127u ||
                    channel.volume > 127u || channel.expression > 127u ||
                    channel.pan > 127u || channel.modulation > 127u ||
                    channel.pressure > 127u || channel.effect_depth > 127u ||
                    channel.qsound_position > 127u || channel.pitch < -64 ||
                    channel.pitch > 63 ||
                    (channel.filter_level3.has_value() &&
                     *channel.filter_level3 > 8128u))
                    fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                    "state-channel");
            }
            const auto next_event = reader.scalar<std::uint64_t>();
            const auto loop_event = [&] {
                value.tick = reader.f64();
                value.tempo = reader.scalar<std::uint32_t>();
                value.loop_marker_open = reader.boolean();
                return reader.scalar<std::uint64_t>();
            }();
            if (next_event > std::numeric_limits<std::size_t>::max() ||
                loop_event > std::numeric_limits<std::size_t>::max() ||
                value.tick < 0.0 || value.tempo == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-sequence-cursor");
            value.next_event = static_cast<std::size_t>(next_event);
            value.loop_event = static_cast<std::size_t>(loop_event);
            value.loop_tick = reader.scalar<std::uint64_t>();
            value.loop_tempo = reader.scalar<std::uint32_t>();
            value.rendered_frames = reader.scalar<std::uint64_t>();
            value.dispatched_events = reader.scalar<std::uint64_t>();
            value.loop_count = reader.scalar<std::uint64_t>();
            slot.value.emplace(std::move(value));
            activate_slot(active_sequence_indices_, active_sequence_positions_, index);
        }

        const auto voice_count = reader.scalar<std::uint32_t>();
        if (voice_count > config_.maximum_synth_voices)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-voice-count");
        voices_.clear(); voices_.resize(voice_count);
        active_voice_indices_.clear();
        active_voice_positions_.assign(voice_count, invalid_active_position);
        for (std::uint32_t index = 0u; index < voice_count; ++index) {
            auto& slot = voices_[index];
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            if (!reader.boolean()) continue;
            SynthVoice value;
            value.collection = reader.handle<NativePortSoundCollectionHandle>();
            value.sequence = read_optional_handle<NativePortSoundSequenceHandle>(reader);
            std::array<std::uint32_t, 4u> location{};
            for (auto& coordinate : location)
                coordinate = reader.scalar<std::uint32_t>();
            auto& collection = require_collection(value.collection);
            const auto [bank, split] = split_at(collection, location);
            const auto key = sample_key(split->tone_offset, split->loop_end,
                                        static_cast<std::uint8_t>(split->format),
                                        bank->bank);
            const auto sample = collection.sample_cache.find(key);
            if (sample == collection.sample_cache.end())
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-voice-sample");
            value.sample = sample->second; value.split = split;
            value.channel = reader.scalar<std::uint8_t>();
            value.note = reader.scalar<std::uint8_t>();
            value.velocity = reader.scalar<std::uint8_t>();
            value.phase = reader.f64(); value.base_step = reader.f64();
            value.lfo_step = reader.f64();
            for (auto& ratio : value.lfo_pitch_ratio) ratio = reader.f64();
            value.cached_external_pitch_bend = reader.scalar<std::int16_t>();
            value.cached_channel_pitch = reader.scalar<std::int8_t>();
            value.cached_modulation = reader.scalar<std::uint8_t>();
            value.pitch_cache_valid = reader.boolean();
            value.lfo_pitch_cache_valid = reader.boolean();
            value.bend_high = reader.scalar<std::uint8_t>();
            value.bend_low = reader.scalar<std::uint8_t>();
            value.amplitude_attenuation = reader.f64();
            value.stage_step = reader.f64(); value.decay_target = reader.f64();
            value.envelope_stage = reader.scalar<EnvelopeStage>();
            value.lfo_phase = reader.f64(); value.filter_level = reader.f64();
            value.filter_target = reader.f64(); value.filter_step = reader.f64();
            value.filter_stage = reader.scalar<EnvelopeStage>();
            value.filter_previous_1 = reader.f64();
            value.filter_previous_2 = reader.f64();
            value.filter_a0 = reader.f64(); value.filter_b1 = reader.f64();
            value.filter_b2 = reader.f64();
            value.filter_coefficient_value = reader.scalar<std::uint32_t>();
            value.filter_coefficient_cache_valid = reader.boolean();
            value.release_tick = reader.scalar<std::uint64_t>();
            value.delayed_until_tick = reader.scalar<std::uint64_t>();
            value.velocity_gain = reader.f32();
            value.controls = control(reader.scalar<std::uint32_t>(), true);
            value.cached_external_pan = reader.f32();
            value.cached_pan_left = reader.f64();
            value.cached_pan_right = reader.f64();
            value.cached_channel_pan = reader.scalar<std::uint8_t>();
            value.cached_qsound_position = reader.scalar<std::uint8_t>();
            value.pan_cache_valid = reader.boolean();
            value.key_released = reader.boolean();
            value.sustained = reader.boolean();
            value.start_serial = reader.scalar<std::uint64_t>();
            // Capture runs after the final sample's phase increment. The next
            // render performs the loop/end transition, so retain one bounded
            // step of overshoot (including its fractional part) verbatim.
            // Bound both cached multipliers from the authored split instead
            // of letting corrupt finite caches inflate the permitted phase.
            const auto maximum_base_step = std::max(
                pitch_step(*split, value.note, 0.0),
                pitch_step(*split, value.note, 1.0,
                           value.bend_high, value.bend_low) *
                    aica_source_sample_rate / config_.output_sample_rate);
            const auto maximum_lfo_pitch_ratio = std::pow(
                2.0, ((127.0 / 128.0) *
                      lfo_pitch_cents(split->lfo_pitch_depth)) / 1200.0);
            const auto maximum_post_render_phase =
                static_cast<double>(value.sample->size()) +
                value.base_step * maximum_lfo_pitch_ratio;
            const auto invalid_cached_lfo_ratio =
                value.lfo_pitch_cache_valid &&
                std::ranges::any_of(value.lfo_pitch_ratio,
                    [&](const double ratio) {
                        return !(ratio > 0.0) ||
                               ratio > maximum_lfo_pitch_ratio;
                    });
            if (value.channel >= maximum_midi_channels || value.note > 127u ||
                value.velocity > 127u || value.phase < 0.0 ||
                value.base_step <= 0.0 || value.base_step > maximum_base_step ||
                !std::isfinite(maximum_post_render_phase) ||
                value.phase > maximum_post_render_phase ||
                invalid_cached_lfo_ratio ||
                value.lfo_step < 0.0 || value.lfo_phase < 0.0 ||
                value.lfo_phase >= 1.0 || value.bend_high > 24u ||
                value.bend_low > 24u ||
                value.envelope_stage > EnvelopeStage::Complete ||
                value.filter_stage > EnvelopeStage::Complete ||
                value.cached_external_pitch_bend < -8192 ||
                value.cached_external_pitch_bend > 8191 ||
                value.cached_channel_pitch < -64 ||
                value.cached_channel_pitch > 63 ||
                value.cached_modulation > 127u ||
                value.cached_external_pan < -1.0f ||
                value.cached_external_pan > 1.0f ||
                value.cached_channel_pan > 127u ||
                value.cached_qsound_position > 127u ||
                value.velocity_gain < 0.0f)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-voice");
            slot.value.emplace(std::move(value));
            activate_slot(active_voice_indices_, active_voice_positions_, index);
        }

        const auto group_count = reader.scalar<std::uint32_t>();
        if (group_count > config_.maximum_synth_voices)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-group-count");
        groups_.clear(); groups_.resize(group_count);
        for (auto& slot : groups_) {
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            if (!reader.boolean()) continue;
            VoiceGroup value;
            value.collection = reader.handle<NativePortSoundCollectionHandle>();
            value.controls = control(reader.scalar<std::uint32_t>(), false);
            const auto count = reader.scalar<std::uint32_t>();
            if (count > config_.maximum_synth_voices)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-group-members");
            value.members.reserve(count);
            for (std::uint32_t index = 0u; index < count; ++index)
                value.members.push_back(
                    reader.handle<NativePortSoundVoiceHandle>());
            slot.value.emplace(std::move(value));
        }

        const auto port_count = reader.scalar<std::uint32_t>();
        if (port_count > config_.maximum_midi_ports)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-port-count");
        ports_.clear(); ports_.resize(port_count);
        for (auto& slot : ports_) {
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            if (!reader.boolean()) continue;
            MidiPort value;
            value.collection = reader.handle<NativePortSoundCollectionHandle>();
            value.config = read_midi_config(reader);
            value.controls = control(reader.scalar<std::uint32_t>(), false);
            for (auto& note : value.notes)
                note = read_optional_handle<NativePortSoundVoiceHandle>(reader);
            value.sequence =
                read_optional_handle<NativePortSoundSequenceHandle>(reader);
            slot.value.emplace(std::move(value));
        }
        for (std::uint32_t index = 0u; index < maximum_mlt_banks; ++index) {
            auto& slot = pcm_stream_rings_[index];
            slot.generation = reader.scalar<std::uint32_t>();
            if (slot.generation == 0u)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-generation");
            slot.value.reset();
            if (!reader.boolean()) continue;
            NativePortSoundPcmStreamRingConfig value;
            value.bank = reader.scalar<std::uint8_t>();
            value.layout_offset = reader.scalar<std::uint32_t>();
            value.byte_size = reader.scalar<std::uint32_t>();
            if (value.bank != index || value.byte_size == 0u ||
                (value.layout_offset & 3u) != 0u ||
                (value.byte_size & 3u) != 0u ||
                value.layout_offset > native_port_manatee_sound_layout_bytes ||
                value.byte_size > native_port_manatee_sound_layout_bytes -
                                      value.layout_offset)
                fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                                "state-ring");
            slot.value = value;
        }
        reader.finish();
        validate_restored_state();
        if (resident_bytes != saved_resident_collection_bytes)
            fail_sound_bank(NativePortSoundBankFailure::InvalidState,
                            "state-resident-bytes");

        current_render_frame_ = saved_current_render_frame;
        voice_serial_ = saved_voice_serial;
        loaded_collections_ = saved_loaded_collections;
        unloaded_collections_ = saved_unloaded_collections;
        parsed_programs_ = saved_parsed_programs;
        parsed_splits_ = saved_parsed_splits;
        parsed_sequences_ = saved_parsed_sequences;
        parsed_events_ = saved_parsed_events;
        decoded_samples_ = saved_decoded_samples;
        decoded_pcm16_samples_ = saved_decoded_pcm16_samples;
        decoded_pcm8_samples_ = saved_decoded_pcm8_samples;
        decoded_adpcm_samples_ = saved_decoded_adpcm_samples;
        decoded_sample_frames_ = saved_decoded_sample_frames;
        resident_collection_bytes_ = saved_resident_collection_bytes;
        rendered_frames_ = saved_rendered_frames;
        effect_frames_ = saved_effect_frames;
    }

    void validate_restored_state() const {
        const auto invalid = [](const std::string_view operation) {
            fail_sound_bank(NativePortSoundBankFailure::InvalidState, operation);
        };
        const auto collection_live = [&](const NativePortSoundCollectionHandle h) {
            return h && h.slot < collections_.size() &&
                   collections_[h.slot].generation == h.generation &&
                   collections_[h.slot].value.has_value();
        };
        const auto sequence_live = [&](const NativePortSoundSequenceHandle h) {
            return h && h.slot < sequences_.size() &&
                   sequences_[h.slot].generation == h.generation &&
                   sequences_[h.slot].value.has_value();
        };
        const auto voice_live = [&](const NativePortSoundVoiceHandle h) {
            return h && h.slot < voices_.size() &&
                   voices_[h.slot].generation == h.generation &&
                   voices_[h.slot].value.has_value();
        };
        const auto group_live = [&](const NativePortSoundVoiceHandle h) {
            return h && h.slot < groups_.size() &&
                   groups_[h.slot].generation == h.generation &&
                   groups_[h.slot].value.has_value();
        };

        for (const auto& slot : sequences_) {
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            if (!collection_live(value.collection)) invalid("state-sequence-collection");
            const auto& collection = *collections_[value.collection.slot].value;
            if (value.config.sequence_bank >= maximum_mlt_banks ||
                !collection.sequence_banks[value.config.sequence_bank].has_value() ||
                value.config.sequence >=
                    collection.sequence_banks[value.config.sequence_bank]->sequences.size() ||
                !collection.sequence_banks[value.config.sequence_bank]
                     ->sequences[value.config.sequence].has_value() ||
                value.config.program_bank >= maximum_mlt_banks ||
                !collection.program_banks[value.config.program_bank].has_value())
                invalid("state-sequence-definition");
            const auto& definition = *collection
                .sequence_banks[value.config.sequence_bank]
                ->sequences[value.config.sequence];
            if (value.next_event > definition.events.size() ||
                value.loop_event > definition.events.size() ||
                (value.loop_marker_open && value.loop_tempo == 0u))
                invalid("state-sequence-cursor");
            for (const auto& channel : value.channels)
                if (channel.bank >= maximum_mlt_banks ||
                    !collection.program_banks[channel.bank].has_value() ||
                    channel.program >=
                        collection.program_banks[channel.bank]->programs.size())
                    invalid("state-channel-program");
        }
        for (const auto& slot : voices_) {
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            if (!collection_live(value.collection)) invalid("state-voice-collection");
            if (value.sequence.has_value() && !sequence_live(*value.sequence))
                invalid("state-voice-sequence");
            if (value.sample == nullptr || value.split == nullptr)
                invalid("state-voice-binding");
        }
        for (const auto& slot : groups_) {
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            if (!collection_live(value.collection) || value.controls == nullptr)
                invalid("state-group");
            for (const auto member : value.members) {
                if (!member || member.slot >= voices_.size())
                    invalid("state-group-member");
                if (!voice_live(member)) continue;
                const auto& voice = *voices_[member.slot].value;
                if (voice.collection.slot != value.collection.slot ||
                    voice.collection.generation != value.collection.generation ||
                    voice.controls != value.controls)
                    invalid("state-group-sharing");
            }
        }
        for (const auto& slot : ports_) {
            if (!slot.value.has_value()) continue;
            const auto& value = *slot.value;
            if (!collection_live(value.collection) || value.controls == nullptr)
                invalid("state-port");
            const auto& collection = *collections_[value.collection.slot].value;
            if (value.config.program_bank >= maximum_mlt_banks ||
                !collection.program_banks[value.config.program_bank].has_value() ||
                value.config.program >=
                    collection.program_banks[value.config.program_bank]->programs.size())
                invalid("state-port-program");
            for (const auto& note : value.notes) {
                if (!note.has_value()) continue;
                if (!*note || note->slot >= groups_.size())
                    invalid("state-port-note");
                if (group_live(*note) &&
                    groups_[note->slot].value->controls != value.controls)
                    invalid("state-port-note-sharing");
            }
            if (value.sequence.has_value()) {
                if (!*value.sequence || value.sequence->slot >= sequences_.size())
                    invalid("state-port-sequence");
                if (sequence_live(*value.sequence)) {
                    const auto& sequence = *sequences_[value.sequence->slot].value;
                    if (sequence.collection.slot != value.collection.slot ||
                        sequence.collection.generation !=
                            value.collection.generation)
                        invalid("state-port-sequence-sharing");
                }
            }
        }
        for (std::size_t index = 0u; index < pcm_stream_rings_.size(); ++index) {
            const auto& left = pcm_stream_rings_[index];
            if (!left.value.has_value()) continue;
            const auto left_begin =
                static_cast<std::uint64_t>(left.value->layout_offset);
            const auto left_end = left_begin + left.value->byte_size;
            for (std::size_t other = index + 1u;
                 other < pcm_stream_rings_.size(); ++other) {
                const auto& right = pcm_stream_rings_[other];
                if (!right.value.has_value()) continue;
                const auto right_begin =
                    static_cast<std::uint64_t>(right.value->layout_offset);
                const auto right_end = right_begin + right.value->byte_size;
                if (left_begin < right_end && right_begin < left_end)
                    invalid("state-ring-overlap");
            }
        }
    }

    [[nodiscard]] NativePortSoundBankDevelopmentStateInventory
    development_state_inventory() const {
        require_owner_thread();
        NativePortSoundBankDevelopmentStateInventory result;
        for (std::uint32_t index = 0u; index < collections_.size(); ++index)
            if (collections_[index].value.has_value())
                result.collections.push_back({index, collections_[index].generation});
        for (std::uint32_t index = 0u; index < ports_.size(); ++index)
            if (ports_[index].value.has_value())
                result.midi_ports.push_back({index, ports_[index].generation});
        for (std::uint32_t index = 0u; index < pcm_stream_rings_.size(); ++index)
            if (pcm_stream_rings_[index].value.has_value())
                result.pcm_stream_rings.push_back(
                    {index, pcm_stream_rings_[index].generation});
        return result;
    }

    NativePortAudioEngine& audio_;
    NativePortSoundBankConfig config_;
    std::thread::id owner_thread_;
    std::vector<Slot<Collection>> collections_;
    std::vector<Slot<ActiveSequence>> sequences_;
    std::vector<std::uint32_t> active_sequence_indices_;
    std::vector<std::size_t> active_sequence_positions_;
    std::vector<Slot<SynthVoice>> voices_;
    std::vector<std::uint32_t> active_voice_indices_;
    std::vector<std::size_t> active_voice_positions_;
    std::vector<Slot<VoiceGroup>> groups_;
    std::vector<Slot<MidiPort>> ports_;
    std::array<Slot<NativePortSoundPcmStreamRingConfig>, maximum_mlt_banks>
        pcm_stream_rings_{};
    std::vector<double> mix_;
    std::uint64_t current_render_frame_ = 0u;
    std::uint64_t voice_serial_ = 0u;
    std::uint64_t loaded_collections_ = 0u;
    std::uint64_t unloaded_collections_ = 0u;
    std::uint64_t parsed_programs_ = 0u;
    std::uint64_t parsed_splits_ = 0u;
    std::uint64_t parsed_sequences_ = 0u;
    std::uint64_t parsed_events_ = 0u;
    std::uint64_t decoded_samples_ = 0u;
    std::uint64_t decoded_pcm16_samples_ = 0u;
    std::uint64_t decoded_pcm8_samples_ = 0u;
    std::uint64_t decoded_adpcm_samples_ = 0u;
    std::uint64_t decoded_sample_frames_ = 0u;
    std::uint64_t resident_collection_bytes_ = 0u;
    std::uint64_t rendered_frames_ = 0u;
    std::uint64_t effect_frames_ = 0u;
};

class NativePortSoundBankEngine::Impl final {
  public:
    Impl(NativePortPlatformServices& platform,
         NativePortAudioEngine& audio,
         const NativePortSoundBankConfig& config)
        : platform_(platform), audio_(audio), config_(config),
          owner_thread_(std::this_thread::get_id()) {
        validate_config(config_);
        audio_.bind_sound_bank_target(
            this, &NativePortSoundBankEngine::execute_worker_command,
            &Impl::cleanup_worker_state,
            &Impl::mix_worker_audio);
        try {
            call_void(SoundBankOpcode::Construct);
        } catch (...) {
            audio_.unbind_sound_bank_target(this, false);
            throw;
        }
    }

    ~Impl() {
        bool destroyed = false;
        try {
            call_void(SoundBankOpcode::Destroy);
            destroyed = true;
        } catch (...) {
        }
        audio_.unbind_sound_bank_target(this, destroyed);
    }

    [[nodiscard]] NativePortSoundCollectionHandle load_collection(
        const NativePortContentFileBinding& binding) {
        require_owner_thread();
        const auto content =
            materialize_sound_bank_content(platform_, binding, config_);
        const auto logical_id = std::string(binding.logical_id);
        const auto path = binding.content_relative_path.generic_u8string();
        const auto identity = std::string(binding.byte_identity);
        std::uint64_t payload_size = sizeof(SoundBankLoadCommand);
        const auto add = [&](const std::uint64_t value) {
            if (value > std::numeric_limits<std::uint64_t>::max() - payload_size)
                return false;
            payload_size += value;
            return payload_size <= std::numeric_limits<std::uint32_t>::max();
        };
        if (!add(logical_id.size()) || !add(path.size()) ||
            !add(identity.size()) || !add(content.size()))
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "load-command-size");
        const SoundBankLoadCommand command{
            binding.source_offset,
            binding.byte_size,
            static_cast<std::uint32_t>(logical_id.size()),
            static_cast<std::uint32_t>(path.size()),
            static_cast<std::uint32_t>(identity.size()),
            0u};
        std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
        auto* cursor = payload.data();
        std::memcpy(cursor, &command, sizeof(command));
        cursor += sizeof(command);
        std::memcpy(cursor, logical_id.data(), logical_id.size());
        cursor += logical_id.size();
        std::memcpy(cursor, path.data(), path.size());
        cursor += path.size();
        std::memcpy(cursor, identity.data(), identity.size());
        cursor += identity.size();
        std::memcpy(cursor, content.data(), content.size());
        return call_payload<NativePortSoundCollectionHandle>(
            SoundBankOpcode::LoadCollection, payload);
    }

    void unload_collection(const NativePortSoundCollectionHandle value) {
        SoundBankCommand command;
        command.collection = value;
        call_void(SoundBankOpcode::UnloadCollection, command);
    }
    [[nodiscard]] bool has_program(
        const NativePortSoundCollectionHandle collection,
        const std::uint8_t bank,
        const std::uint8_t program) const {
        SoundBankCommand command;
        command.collection = collection;
        command.value8_0 = bank;
        command.value8_1 = program;
        return call<std::uint8_t>(SoundBankOpcode::HasProgram, command) != 0u;
    }
    [[nodiscard]] bool has_sequence(
        const NativePortSoundCollectionHandle collection,
        const std::uint8_t bank,
        const std::uint16_t sequence) const {
        SoundBankCommand command;
        command.collection = collection;
        command.value8_0 = bank;
        command.value16 = sequence;
        return call<std::uint8_t>(SoundBankOpcode::HasSequence, command) != 0u;
    }
    [[nodiscard]] bool has_collection_unit(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundCollectionUnitKind kind,
        const std::uint8_t bank) const {
        SoundBankCommand command;
        command.collection = collection;
        command.value8_0 = static_cast<std::uint8_t>(kind);
        command.value8_1 = bank;
        return call<std::uint8_t>(SoundBankOpcode::HasCollectionUnit,
                                  command) != 0u;
    }
    [[nodiscard]] NativePortSoundPcmStreamRingHandle bind_pcm_stream_ring(
        const NativePortSoundPcmStreamRingConfig& value) {
        SoundBankCommand command;
        command.ring_config = value;
        return call<NativePortSoundPcmStreamRingHandle>(
            SoundBankOpcode::BindPcmStreamRing, command);
    }
    void release_pcm_stream_ring(const NativePortSoundPcmStreamRingHandle value) {
        SoundBankCommand command;
        command.ring = value;
        call_void(SoundBankOpcode::ReleasePcmStreamRing, command);
    }
    [[nodiscard]] NativePortSoundPcmStreamRingSnapshot
    pcm_stream_ring_snapshot(const NativePortSoundPcmStreamRingHandle value) const {
        SoundBankCommand command;
        command.ring = value;
        return call<NativePortSoundPcmStreamRingSnapshot>(
            SoundBankOpcode::PcmStreamRingSnapshot, command);
    }
    void predecode_collection_samples(
        const NativePortSoundCollectionHandle value) {
        SoundBankCommand command;
        command.collection = value;
        call_void(SoundBankOpcode::PredecodeCollectionSamples, command);
    }
    [[nodiscard]] NativePortSoundSequenceHandle play_sequence(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundSequenceConfig& config) {
        SoundBankCommand command;
        command.collection = collection;
        command.sequence_config = config;
        return call<NativePortSoundSequenceHandle>(
            SoundBankOpcode::PlaySequence, command);
    }
    void pause_sequence(const NativePortSoundSequenceHandle value) {
        sequence_async(SoundBankOpcode::PauseSequence, value);
    }
    void resume_sequence(const NativePortSoundSequenceHandle value) {
        sequence_async(SoundBankOpcode::ResumeSequence, value);
    }
    void stop_sequence(const NativePortSoundSequenceHandle value) {
        sequence_void(SoundBankOpcode::StopSequence, value);
    }
    void release_sequence(const NativePortSoundSequenceHandle value) {
        sequence_void(SoundBankOpcode::ReleaseSequence, value);
    }
    void set_sequence_gain_pan(const NativePortSoundSequenceHandle value,
                               const float gain,
                               const float pan) {
        SoundBankCommand command;
        command.sequence = value;
        command.value0 = gain;
        command.value1 = pan;
        call_async(SoundBankOpcode::SetSequenceGainPan, command);
    }
    void set_sequence_pitch_bend(const NativePortSoundSequenceHandle value,
                                 const std::int16_t bend) {
        SoundBankCommand command;
        command.sequence = value;
        command.signed_value = bend;
        call_async(SoundBankOpcode::SetSequencePitchBend, command);
    }
    void set_sequence_playback_rate(const NativePortSoundSequenceHandle value,
                                    const float rate) {
        SoundBankCommand command;
        command.sequence = value;
        command.value0 = rate;
        call_async(SoundBankOpcode::SetSequencePlaybackRate, command);
    }
    void set_sequence_send_levels(const NativePortSoundSequenceHandle value,
                                  const std::uint8_t direct,
                                  const std::uint8_t effect) {
        SoundBankCommand command;
        command.sequence = value;
        command.value8_0 = direct;
        command.value8_1 = effect;
        call_async(SoundBankOpcode::SetSequenceSendLevels, command);
    }
    [[nodiscard]] NativePortSoundVoiceHandle note_on(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundNoteConfig& config) {
        SoundBankCommand command;
        command.collection = collection;
        command.note_config = config;
        return call<NativePortSoundVoiceHandle>(SoundBankOpcode::NoteOn,
                                                command);
    }
    void note_off(const NativePortSoundVoiceHandle value) {
        voice_async(SoundBankOpcode::NoteOff, value);
    }
    void release_voice(const NativePortSoundVoiceHandle value) {
        voice_void(SoundBankOpcode::ReleaseVoice, value);
    }
    void set_voice_gain_pan(const NativePortSoundVoiceHandle value,
                            const float gain,
                            const float pan) {
        SoundBankCommand command;
        command.voice = value;
        command.value0 = gain;
        command.value1 = pan;
        call_async(SoundBankOpcode::SetVoiceGainPan, command);
    }
    void set_voice_pitch_bend(const NativePortSoundVoiceHandle value,
                              const std::int16_t bend) {
        SoundBankCommand command;
        command.voice = value;
        command.signed_value = bend;
        call_async(SoundBankOpcode::SetVoicePitchBend, command);
    }
    void set_voice_send_levels(const NativePortSoundVoiceHandle value,
                               const std::uint8_t direct,
                               const std::uint8_t effect) {
        SoundBankCommand command;
        command.voice = value;
        command.value8_0 = direct;
        command.value8_1 = effect;
        call_async(SoundBankOpcode::SetVoiceSendLevels, command);
    }
    [[nodiscard]] NativePortSoundMidiPortHandle open_midi_port(
        const NativePortSoundCollectionHandle collection,
        const NativePortSoundMidiPortConfig& config) {
        SoundBankCommand command;
        command.collection = collection;
        command.midi_config = config;
        return call<NativePortSoundMidiPortHandle>(
            SoundBankOpcode::OpenMidiPort, command);
    }
    void close_midi_port(const NativePortSoundMidiPortHandle value) {
        midi_void(SoundBankOpcode::CloseMidiPort, value);
    }
    void set_midi_program(const NativePortSoundMidiPortHandle value,
                          const std::uint8_t bank,
                          const std::uint8_t program) {
        SoundBankCommand command;
        command.port = value;
        command.value8_0 = bank;
        command.value8_1 = program;
        call_async(SoundBankOpcode::SetMidiProgram, command);
    }
    void set_midi_gain_pan(const NativePortSoundMidiPortHandle value,
                           const float gain,
                           const float pan) {
        SoundBankCommand command;
        command.port = value;
        command.value0 = gain;
        command.value1 = pan;
        call_async(SoundBankOpcode::SetMidiGainPan, command);
    }
    void set_midi_pitch_bend(const NativePortSoundMidiPortHandle value,
                             const std::int16_t bend) {
        SoundBankCommand command;
        command.port = value;
        command.signed_value = bend;
        call_async(SoundBankOpcode::SetMidiPitchBend, command);
    }
    void set_midi_playback_rate(const NativePortSoundMidiPortHandle value,
                                const float rate) {
        SoundBankCommand command;
        command.port = value;
        command.value0 = rate;
        call_async(SoundBankOpcode::SetMidiPlaybackRate, command);
    }
    void set_midi_send_levels(const NativePortSoundMidiPortHandle value,
                              const std::uint8_t direct,
                              const std::uint8_t effect) {
        SoundBankCommand command;
        command.port = value;
        command.value8_0 = direct;
        command.value8_1 = effect;
        call_async(SoundBankOpcode::SetMidiSendLevels, command);
    }
    [[nodiscard]] NativePortSoundVoiceHandle midi_note_on(
        const NativePortSoundMidiPortHandle port,
        const std::uint8_t note,
        const std::uint8_t velocity) {
        SoundBankCommand command;
        command.port = port;
        command.value8_0 = note;
        command.value8_1 = velocity;
        return call<NativePortSoundVoiceHandle>(SoundBankOpcode::MidiNoteOn,
                                                command);
    }
    void midi_note_off(const NativePortSoundMidiPortHandle port,
                       const std::uint8_t note) {
        SoundBankCommand command;
        command.port = port;
        command.value8_0 = note;
        call_async(SoundBankOpcode::MidiNoteOff, command);
    }
    [[nodiscard]] NativePortSoundSequenceHandle midi_play_sequence(
        const NativePortSoundMidiPortHandle port,
        const std::uint8_t bank,
        const std::uint16_t sequence,
        const bool loops) {
        SoundBankCommand command;
        command.port = port;
        command.value8_0 = bank;
        command.value16 = sequence;
        command.value8_1 = static_cast<std::uint8_t>(loops ? 1u : 0u);
        return call<NativePortSoundSequenceHandle>(
            SoundBankOpcode::MidiPlaySequence, command);
    }
    void midi_pause(const NativePortSoundMidiPortHandle value) {
        midi_void(SoundBankOpcode::MidiPause, value);
    }
    void midi_resume(const NativePortSoundMidiPortHandle value) {
        midi_void(SoundBankOpcode::MidiResume, value);
    }
    void midi_stop(const NativePortSoundMidiPortHandle value) {
        midi_void(SoundBankOpcode::MidiStop, value);
    }
    void close_all_midi_ports() {
        call_void(SoundBankOpcode::CloseAllMidiPorts);
    }
    void release_all_sequences() {
        call_void(SoundBankOpcode::ReleaseAllSequences);
    }
    void unload_all_collections() {
        call_void(SoundBankOpcode::UnloadAllCollections);
    }
    void stop_all() { call_void(SoundBankOpcode::StopAll); }
    void reset() { call_void(SoundBankOpcode::Reset); }
    void pump() {
        require_owner_thread();
        audio_.dispatch_sound_bank_async(
            static_cast<std::uint16_t>(SoundBankOpcode::Pump), {});
    }
    [[nodiscard]] NativePortSoundSequenceSnapshot sequence_snapshot(
        const NativePortSoundSequenceHandle value) const {
        SoundBankCommand command;
        command.sequence = value;
        return call<NativePortSoundSequenceSnapshot>(
            SoundBankOpcode::SequenceSnapshot, command);
    }
    [[nodiscard]] NativePortSoundMidiPortSnapshot midi_port_snapshot(
        const NativePortSoundMidiPortHandle value) const {
        SoundBankCommand command;
        command.port = value;
        return call<NativePortSoundMidiPortSnapshot>(
            SoundBankOpcode::MidiPortSnapshot, command);
    }
    [[nodiscard]] NativePortSoundBankSnapshot snapshot() const {
        return call_payload<NativePortSoundBankSnapshot>(
            SoundBankOpcode::Snapshot, {});
    }
    [[nodiscard]] std::vector<std::uint8_t> capture_development_state() const {
        require_owner_thread();
        staged_development_state_.clear();
        try {
            call_void(SoundBankOpcode::CaptureDevelopmentState);
            return std::move(staged_development_state_);
        } catch (...) {
            staged_development_state_.clear();
            throw;
        }
    }
    [[nodiscard]] NativePortSoundBankDevelopmentStateInventory
    validate_development_state(
        const std::span<const std::uint8_t> state) const {
        stage_development_state(state);
        try {
            call_void(SoundBankOpcode::ValidateDevelopmentState);
            staged_development_state_.clear();
            return std::move(staged_development_state_inventory_);
        } catch (...) {
            staged_development_state_.clear();
            staged_development_state_inventory_ = {};
            throw;
        }
    }
    void restore_development_state(
        const std::span<const std::uint8_t> state) {
        stage_development_state(state);
        try {
            call_void(SoundBankOpcode::RestoreDevelopmentState);
            staged_development_state_.clear();
        } catch (...) {
            staged_development_state_.clear();
            throw;
        }
    }

    static void execute_worker_command(
        void* const target,
        const std::uint16_t raw_opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) noexcept {
        result = {};
        auto* const self = static_cast<Impl*>(target);
        if (self == nullptr) {
            write_sound_error(result, NativePortSoundBankFailure::WorkerFailure);
            return;
        }
        try {
            self->execute(static_cast<SoundBankOpcode>(raw_opcode),
                          payload,
                          result);
            self->audio_.request_consumer_service();
        } catch (const NativePortSoundBankError& error) {
            write_sound_error(result, error.failure());
        } catch (...) {
            write_sound_error(result, NativePortSoundBankFailure::WorkerFailure);
        }
    }

    static void cleanup_worker_state(void* const target) noexcept {
        auto& self = *static_cast<Impl*>(target);
        self.core_.reset();
    }

    static std::uint32_t mix_worker_audio(
        void* const target,
        const std::span<double> interleaved_stereo_accumulation,
        const std::uint32_t frame_count) noexcept {
        auto* const self = static_cast<Impl*>(target);
        if (self == nullptr || self->core_ == nullptr) return 0u;
        try {
            self->core_->render_into(interleaved_stereo_accumulation,
                                     frame_count);
            return 0u;
        } catch (const NativePortSoundBankError& error) {
            return static_cast<std::uint32_t>(error.failure());
        } catch (...) {
            return static_cast<std::uint32_t>(
                NativePortSoundBankFailure::WorkerFailure);
        }
    }

  private:
    void stage_development_state(
        const std::span<const std::uint8_t> state) const {
        require_owner_thread();
        if (state.size() > config_.maximum_development_state_bytes)
            fail_sound_bank(NativePortSoundBankFailure::ResourceLimit,
                            "state-size");
        staged_development_state_.assign(state.begin(), state.end());
        staged_development_state_inventory_ = {};
    }

    void require_owner_thread() const {
        if (!audio_.on_audio_thread() &&
            std::this_thread::get_id() != owner_thread_)
            fail_sound_bank(NativePortSoundBankFailure::ThreadViolation,
                            "producer-thread");
    }

    template <typename Value>
    [[nodiscard]] Value call(const SoundBankOpcode opcode,
                             const SoundBankCommand& command) const {
        return call_payload<Value>(opcode, sound_object_bytes(command));
    }

    template <typename Value>
    [[nodiscard]] Value call_payload(
        const SoundBankOpcode opcode,
        const std::span<const std::byte> payload) const {
        require_owner_thread();
        const auto ack = audio_.dispatch_sound_bank_sync(
            static_cast<std::uint16_t>(opcode), payload);
        require_success(ack);
        if (ack.result_size != sizeof(Value))
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "ack-size");
        Value result{};
        std::memcpy(&result, ack.bytes.data(), sizeof(result));
        return result;
    }

    void call_void(const SoundBankOpcode opcode) const {
        call_void_payload(opcode, {});
    }
    void call_void(const SoundBankOpcode opcode,
                   const SoundBankCommand& command) const {
        call_void_payload(opcode, sound_object_bytes(command));
    }
    void call_void_payload(const SoundBankOpcode opcode,
                           const std::span<const std::byte> payload) const {
        require_owner_thread();
        const auto ack = audio_.dispatch_sound_bank_sync(
            static_cast<std::uint16_t>(opcode), payload);
        require_success(ack);
        if (ack.result_size != 0u)
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "void-ack-size");
    }

    void call_async(const SoundBankOpcode opcode,
                    const SoundBankCommand& command) const {
        require_owner_thread();
        audio_.dispatch_sound_bank_async(
            static_cast<std::uint16_t>(opcode),
            sound_object_bytes(command));
    }

    static void require_success(const NativePortAudioCommandAck& ack) {
        if (ack.status == NativePortAudioCommandAckStatus::Completed) return;
        SoundBankWireError error;
        if (ack.result_size == sizeof(error)) {
            std::memcpy(&error, ack.bytes.data(), sizeof(error));
            throw NativePortSoundBankError(error.failure, "worker-command");
        }
        fail_sound_bank(NativePortSoundBankFailure::CommandQueue,
                        "command-ack");
    }

    void sequence_void(const SoundBankOpcode opcode,
                       const NativePortSoundSequenceHandle value) {
        SoundBankCommand command;
        command.sequence = value;
        call_void(opcode, command);
    }
    void sequence_async(const SoundBankOpcode opcode,
                        const NativePortSoundSequenceHandle value) {
        SoundBankCommand command;
        command.sequence = value;
        call_async(opcode, command);
    }
    void voice_void(const SoundBankOpcode opcode,
                    const NativePortSoundVoiceHandle value) {
        SoundBankCommand command;
        command.voice = value;
        call_void(opcode, command);
    }
    void voice_async(const SoundBankOpcode opcode,
                     const NativePortSoundVoiceHandle value) {
        SoundBankCommand command;
        command.voice = value;
        call_async(opcode, command);
    }
    void midi_void(const SoundBankOpcode opcode,
                   const NativePortSoundMidiPortHandle value) {
        SoundBankCommand command;
        command.port = value;
        call_void(opcode, command);
    }

    [[nodiscard]] SoundBankCommand require_command(
        const std::span<const std::byte> payload) const {
        SoundBankCommand command;
        if (!read_sound_object(payload, command))
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "command-payload");
        return command;
    }

    void require_empty(const std::span<const std::byte> payload) const {
        if (!payload.empty())
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "command-payload");
    }

    void execute(const SoundBankOpcode opcode,
                 const std::span<const std::byte> payload,
                 NativePortAudioCommandAckResult& result) {
        if (opcode == SoundBankOpcode::Construct) {
            require_empty(payload);
            if (core_ != nullptr)
                fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                                "duplicate-construct");
            core_ = std::make_unique<Core>(audio_, config_);
            return;
        }
        if (opcode == SoundBankOpcode::Destroy) {
            require_empty(payload);
            core_.reset();
            return;
        }
        if (core_ == nullptr)
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "worker-core");
        if (opcode == SoundBankOpcode::LoadCollection) {
            execute_load_collection(payload, result);
            return;
        }
        if (opcode == SoundBankOpcode::CloseAllMidiPorts ||
            opcode == SoundBankOpcode::ReleaseAllSequences ||
            opcode == SoundBankOpcode::UnloadAllCollections ||
            opcode == SoundBankOpcode::StopAll ||
            opcode == SoundBankOpcode::Reset ||
            opcode == SoundBankOpcode::Pump ||
            opcode == SoundBankOpcode::Snapshot ||
            opcode == SoundBankOpcode::CaptureDevelopmentState ||
            opcode == SoundBankOpcode::ValidateDevelopmentState ||
            opcode == SoundBankOpcode::RestoreDevelopmentState) {
            require_empty(payload);
            switch (opcode) {
            case SoundBankOpcode::CloseAllMidiPorts:
                core_->close_all_midi_ports();
                return;
            case SoundBankOpcode::ReleaseAllSequences:
                core_->release_all_sequences();
                return;
            case SoundBankOpcode::UnloadAllCollections:
                core_->unload_all_collections();
                return;
            case SoundBankOpcode::StopAll:
                core_->stop_all();
                return;
            case SoundBankOpcode::Reset:
                core_->reset();
                return;
            case SoundBankOpcode::Pump:
                core_->pump();
                return;
            case SoundBankOpcode::Snapshot:
                write_sound_ack(result, core_->snapshot());
                return;
            case SoundBankOpcode::CaptureDevelopmentState:
                staged_development_state_ = core_->capture_development_state();
                return;
            case SoundBankOpcode::ValidateDevelopmentState: {
                auto candidate = std::make_unique<Core>(audio_, config_);
                candidate->restore_development_state(staged_development_state_);
                staged_development_state_inventory_ =
                    candidate->development_state_inventory();
                return;
            }
            case SoundBankOpcode::RestoreDevelopmentState: {
                auto candidate = std::make_unique<Core>(audio_, config_);
                candidate->restore_development_state(staged_development_state_);
                core_.swap(candidate);
                return;
            }
            default:
                break;
            }
        }
        const auto command = require_command(payload);
        execute_fixed(opcode, command, result);
    }

    void execute_load_collection(
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) {
        if (payload.size() < sizeof(SoundBankLoadCommand))
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "load-payload");
        SoundBankLoadCommand command;
        std::memcpy(&command, payload.data(), sizeof(command));
        std::uint64_t expected = sizeof(command);
        const auto add = [&](const std::uint64_t value) {
            if (value > std::numeric_limits<std::uint64_t>::max() - expected)
                return false;
            expected += value;
            return expected <= std::numeric_limits<std::uint32_t>::max();
        };
        if (!add(command.logical_id_size) || !add(command.path_size) ||
            !add(command.byte_identity_size) || !add(command.byte_size) ||
            expected != payload.size())
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "load-payload");
        std::size_t cursor = sizeof(command);
        const std::string_view logical(
            reinterpret_cast<const char*>(payload.data() + cursor),
            command.logical_id_size);
        cursor += command.logical_id_size;
        const std::u8string path(
            reinterpret_cast<const char8_t*>(payload.data() + cursor),
            command.path_size);
        cursor += command.path_size;
        const std::string_view identity(
            reinterpret_cast<const char*>(payload.data() + cursor),
            command.byte_identity_size);
        cursor += command.byte_identity_size;
        std::vector<std::byte> bytes(command.byte_size);
        std::memcpy(bytes.data(), payload.data() + cursor, bytes.size());
        const NativePortContentFileBinding binding{
            logical, std::filesystem::path(path), identity,
            command.source_offset, command.byte_size};
        write_sound_ack(result,
                        core_->load_collection(binding, std::move(bytes)));
    }

    void execute_fixed(const SoundBankOpcode opcode,
                       const SoundBankCommand& c,
                       NativePortAudioCommandAckResult& result) {
        switch (opcode) {
        case SoundBankOpcode::UnloadCollection:
            core_->unload_collection(c.collection); return;
        case SoundBankOpcode::HasProgram:
            write_sound_ack(result, static_cast<std::uint8_t>(
                core_->has_program(c.collection, c.value8_0, c.value8_1))); return;
        case SoundBankOpcode::HasSequence:
            write_sound_ack(result, static_cast<std::uint8_t>(
                core_->has_sequence(c.collection, c.value8_0, c.value16))); return;
        case SoundBankOpcode::HasCollectionUnit:
            write_sound_ack(result, static_cast<std::uint8_t>(
                core_->has_collection_unit(c.collection,
                    static_cast<NativePortSoundCollectionUnitKind>(c.value8_0),
                    c.value8_1))); return;
        case SoundBankOpcode::BindPcmStreamRing:
            write_sound_ack(result, core_->bind_pcm_stream_ring(c.ring_config)); return;
        case SoundBankOpcode::ReleasePcmStreamRing:
            core_->release_pcm_stream_ring(c.ring); return;
        case SoundBankOpcode::PcmStreamRingSnapshot:
            write_sound_ack(result, core_->pcm_stream_ring_snapshot(c.ring)); return;
        case SoundBankOpcode::PredecodeCollectionSamples:
            core_->predecode_collection_samples(c.collection); return;
        case SoundBankOpcode::PlaySequence:
            write_sound_ack(result,
                core_->play_sequence(c.collection, c.sequence_config)); return;
        case SoundBankOpcode::PauseSequence:
            core_->pause_sequence(c.sequence); return;
        case SoundBankOpcode::ResumeSequence:
            core_->resume_sequence(c.sequence); return;
        case SoundBankOpcode::StopSequence:
            core_->stop_sequence(c.sequence); return;
        case SoundBankOpcode::ReleaseSequence:
            core_->release_sequence(c.sequence); return;
        case SoundBankOpcode::SetSequenceGainPan:
            core_->set_sequence_gain_pan(c.sequence, c.value0, c.value1); return;
        case SoundBankOpcode::SetSequencePitchBend:
            core_->set_sequence_pitch_bend(c.sequence, c.signed_value); return;
        case SoundBankOpcode::SetSequencePlaybackRate:
            core_->set_sequence_playback_rate(c.sequence, c.value0); return;
        case SoundBankOpcode::SetSequenceSendLevels:
            core_->set_sequence_send_levels(c.sequence, c.value8_0, c.value8_1); return;
        case SoundBankOpcode::NoteOn:
            write_sound_ack(result, core_->note_on(c.collection, c.note_config)); return;
        case SoundBankOpcode::NoteOff:
            core_->note_off(c.voice); return;
        case SoundBankOpcode::ReleaseVoice:
            core_->release_voice(c.voice); return;
        case SoundBankOpcode::SetVoiceGainPan:
            core_->set_voice_gain_pan(c.voice, c.value0, c.value1); return;
        case SoundBankOpcode::SetVoicePitchBend:
            core_->set_voice_pitch_bend(c.voice, c.signed_value); return;
        case SoundBankOpcode::SetVoiceSendLevels:
            core_->set_voice_send_levels(c.voice, c.value8_0, c.value8_1); return;
        case SoundBankOpcode::OpenMidiPort:
            write_sound_ack(result, core_->open_midi_port(c.collection, c.midi_config)); return;
        case SoundBankOpcode::CloseMidiPort:
            core_->close_midi_port(c.port); return;
        case SoundBankOpcode::SetMidiProgram:
            core_->set_midi_program(c.port, c.value8_0, c.value8_1); return;
        case SoundBankOpcode::SetMidiGainPan:
            core_->set_midi_gain_pan(c.port, c.value0, c.value1); return;
        case SoundBankOpcode::SetMidiPitchBend:
            core_->set_midi_pitch_bend(c.port, c.signed_value); return;
        case SoundBankOpcode::SetMidiPlaybackRate:
            core_->set_midi_playback_rate(c.port, c.value0); return;
        case SoundBankOpcode::SetMidiSendLevels:
            core_->set_midi_send_levels(c.port, c.value8_0, c.value8_1); return;
        case SoundBankOpcode::MidiNoteOn:
            write_sound_ack(result, core_->midi_note_on(c.port, c.value8_0, c.value8_1)); return;
        case SoundBankOpcode::MidiNoteOff:
            core_->midi_note_off(c.port, c.value8_0); return;
        case SoundBankOpcode::MidiPlaySequence:
            if (c.value8_1 > 1u)
                fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                                "midi-loop-flag");
            write_sound_ack(result, core_->midi_play_sequence(
                c.port, c.value8_0, c.value16, c.value8_1 != 0u)); return;
        case SoundBankOpcode::MidiPause:
            core_->midi_pause(c.port); return;
        case SoundBankOpcode::MidiResume:
            core_->midi_resume(c.port); return;
        case SoundBankOpcode::MidiStop:
            core_->midi_stop(c.port); return;
        case SoundBankOpcode::SequenceSnapshot:
            write_sound_ack(result, core_->sequence_snapshot(c.sequence)); return;
        case SoundBankOpcode::MidiPortSnapshot:
            write_sound_ack(result, core_->midi_port_snapshot(c.port)); return;
        default:
            fail_sound_bank(NativePortSoundBankFailure::WorkerFailure,
                            "opcode");
        }
    }

    NativePortPlatformServices& platform_;
    NativePortAudioEngine& audio_;
    NativePortSoundBankConfig config_;
    std::thread::id owner_thread_;
    std::unique_ptr<Core> core_;
    // Producer/worker handoff is serialized by synchronous queue commands;
    // no address crosses the pointer-free command ABI.
    mutable std::vector<std::uint8_t> staged_development_state_;
    mutable NativePortSoundBankDevelopmentStateInventory
        staged_development_state_inventory_;
};

void NativePortSoundBankEngine::execute_worker_command(
    void* const target,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    NativePortAudioCommandAckResult& result) noexcept {
    Impl::execute_worker_command(target, opcode, payload, result);
}

NativePortSoundBankEngine::NativePortSoundBankEngine(
    NativePortPlatformServices& platform,
    NativePortAudioEngine& audio,
    const NativePortSoundBankConfig& config)
    : impl_(std::make_unique<Impl>(platform, audio, config)) {}

NativePortSoundBankEngine::~NativePortSoundBankEngine() = default;

NativePortSoundCollectionHandle NativePortSoundBankEngine::load_collection(
    const NativePortContentFileBinding& binding) {
    return impl_->load_collection(binding);
}

void NativePortSoundBankEngine::unload_collection(
    const NativePortSoundCollectionHandle collection) {
    impl_->unload_collection(collection);
}

bool NativePortSoundBankEngine::has_program(
    const NativePortSoundCollectionHandle collection,
    const std::uint8_t bank,
    const std::uint8_t program) const {
    return impl_->has_program(collection, bank, program);
}

bool NativePortSoundBankEngine::has_sequence(
    const NativePortSoundCollectionHandle collection,
    const std::uint8_t bank,
    const std::uint16_t sequence) const {
    return impl_->has_sequence(collection, bank, sequence);
}

bool NativePortSoundBankEngine::has_collection_unit(
    const NativePortSoundCollectionHandle collection,
    const NativePortSoundCollectionUnitKind kind,
    const std::uint8_t bank) const {
    return impl_->has_collection_unit(collection, kind, bank);
}

NativePortSoundPcmStreamRingHandle
NativePortSoundBankEngine::bind_pcm_stream_ring(
    const NativePortSoundPcmStreamRingConfig& config) {
    return impl_->bind_pcm_stream_ring(config);
}

void NativePortSoundBankEngine::release_pcm_stream_ring(
    const NativePortSoundPcmStreamRingHandle ring) {
    impl_->release_pcm_stream_ring(ring);
}

NativePortSoundPcmStreamRingSnapshot
NativePortSoundBankEngine::pcm_stream_ring_snapshot(
    const NativePortSoundPcmStreamRingHandle ring) const {
    return impl_->pcm_stream_ring_snapshot(ring);
}

void NativePortSoundBankEngine::predecode_collection_samples(
    const NativePortSoundCollectionHandle collection) {
    impl_->predecode_collection_samples(collection);
}

NativePortSoundSequenceHandle NativePortSoundBankEngine::play_sequence(
    const NativePortSoundCollectionHandle collection,
    const NativePortSoundSequenceConfig& config) {
    return impl_->play_sequence(collection, config);
}

void NativePortSoundBankEngine::pause_sequence(
    const NativePortSoundSequenceHandle sequence) {
    impl_->pause_sequence(sequence);
}

void NativePortSoundBankEngine::resume_sequence(
    const NativePortSoundSequenceHandle sequence) {
    impl_->resume_sequence(sequence);
}

void NativePortSoundBankEngine::stop_sequence(
    const NativePortSoundSequenceHandle sequence) {
    impl_->stop_sequence(sequence);
}

void NativePortSoundBankEngine::release_sequence(
    const NativePortSoundSequenceHandle sequence) {
    impl_->release_sequence(sequence);
}

void NativePortSoundBankEngine::set_sequence_gain_pan(
    const NativePortSoundSequenceHandle sequence,
    const float gain,
    const float pan) {
    impl_->set_sequence_gain_pan(sequence, gain, pan);
}

void NativePortSoundBankEngine::set_sequence_pitch_bend(
    const NativePortSoundSequenceHandle sequence,
    const std::int16_t pitch_bend) {
    impl_->set_sequence_pitch_bend(sequence, pitch_bend);
}

void NativePortSoundBankEngine::set_sequence_playback_rate(
    const NativePortSoundSequenceHandle sequence,
    const float playback_rate) {
    impl_->set_sequence_playback_rate(sequence, playback_rate);
}

void NativePortSoundBankEngine::set_sequence_send_levels(
    const NativePortSoundSequenceHandle sequence,
    const std::uint8_t direct_level,
    const std::uint8_t effect_level) {
    impl_->set_sequence_send_levels(sequence, direct_level, effect_level);
}

NativePortSoundVoiceHandle NativePortSoundBankEngine::note_on(
    const NativePortSoundCollectionHandle collection,
    const NativePortSoundNoteConfig& config) {
    return impl_->note_on(collection, config);
}

void NativePortSoundBankEngine::note_off(
    const NativePortSoundVoiceHandle voice) {
    impl_->note_off(voice);
}

void NativePortSoundBankEngine::release_voice(
    const NativePortSoundVoiceHandle voice) {
    impl_->release_voice(voice);
}

void NativePortSoundBankEngine::set_voice_gain_pan(
    const NativePortSoundVoiceHandle voice,
    const float gain,
    const float pan) {
    impl_->set_voice_gain_pan(voice, gain, pan);
}

void NativePortSoundBankEngine::set_voice_pitch_bend(
    const NativePortSoundVoiceHandle voice,
    const std::int16_t pitch_bend) {
    impl_->set_voice_pitch_bend(voice, pitch_bend);
}

void NativePortSoundBankEngine::set_voice_send_levels(
    const NativePortSoundVoiceHandle voice,
    const std::uint8_t direct_level,
    const std::uint8_t effect_level) {
    impl_->set_voice_send_levels(voice, direct_level, effect_level);
}

NativePortSoundMidiPortHandle NativePortSoundBankEngine::open_midi_port(
    const NativePortSoundCollectionHandle collection,
    const NativePortSoundMidiPortConfig& config) {
    return impl_->open_midi_port(collection, config);
}

void NativePortSoundBankEngine::close_midi_port(
    const NativePortSoundMidiPortHandle port) {
    impl_->close_midi_port(port);
}

void NativePortSoundBankEngine::set_midi_program(
    const NativePortSoundMidiPortHandle port,
    const std::uint8_t bank,
    const std::uint8_t program) {
    impl_->set_midi_program(port, bank, program);
}

void NativePortSoundBankEngine::set_midi_gain_pan(
    const NativePortSoundMidiPortHandle port,
    const float gain,
    const float pan) {
    impl_->set_midi_gain_pan(port, gain, pan);
}

void NativePortSoundBankEngine::set_midi_pitch_bend(
    const NativePortSoundMidiPortHandle port,
    const std::int16_t pitch_bend) {
    impl_->set_midi_pitch_bend(port, pitch_bend);
}

void NativePortSoundBankEngine::set_midi_playback_rate(
    const NativePortSoundMidiPortHandle port,
    const float playback_rate) {
    impl_->set_midi_playback_rate(port, playback_rate);
}

void NativePortSoundBankEngine::set_midi_send_levels(
    const NativePortSoundMidiPortHandle port,
    const std::uint8_t direct_level,
    const std::uint8_t effect_level) {
    impl_->set_midi_send_levels(port, direct_level, effect_level);
}

NativePortSoundVoiceHandle NativePortSoundBankEngine::midi_note_on(
    const NativePortSoundMidiPortHandle port,
    const std::uint8_t note,
    const std::uint8_t velocity) {
    return impl_->midi_note_on(port, note, velocity);
}

void NativePortSoundBankEngine::midi_note_off(
    const NativePortSoundMidiPortHandle port,
    const std::uint8_t note) {
    impl_->midi_note_off(port, note);
}

NativePortSoundSequenceHandle NativePortSoundBankEngine::midi_play_sequence(
    const NativePortSoundMidiPortHandle port,
    const std::uint8_t sequence_bank,
    const std::uint16_t sequence,
    const bool enable_authored_loops) {
    return impl_->midi_play_sequence(
        port, sequence_bank, sequence, enable_authored_loops);
}

void NativePortSoundBankEngine::midi_pause(
    const NativePortSoundMidiPortHandle port) {
    impl_->midi_pause(port);
}

void NativePortSoundBankEngine::midi_resume(
    const NativePortSoundMidiPortHandle port) {
    impl_->midi_resume(port);
}

void NativePortSoundBankEngine::midi_stop(
    const NativePortSoundMidiPortHandle port) {
    impl_->midi_stop(port);
}

void NativePortSoundBankEngine::close_all_midi_ports() {
    impl_->close_all_midi_ports();
}

void NativePortSoundBankEngine::release_all_sequences() {
    impl_->release_all_sequences();
}

void NativePortSoundBankEngine::unload_all_collections() {
    impl_->unload_all_collections();
}

void NativePortSoundBankEngine::stop_all() { impl_->stop_all(); }

void NativePortSoundBankEngine::reset() { impl_->reset(); }

void NativePortSoundBankEngine::pump() { impl_->pump(); }

NativePortSoundSequenceSnapshot NativePortSoundBankEngine::sequence_snapshot(
    const NativePortSoundSequenceHandle sequence) const {
    return impl_->sequence_snapshot(sequence);
}

NativePortSoundMidiPortSnapshot NativePortSoundBankEngine::midi_port_snapshot(
    const NativePortSoundMidiPortHandle port) const {
    return impl_->midi_port_snapshot(port);
}

NativePortSoundBankSnapshot NativePortSoundBankEngine::snapshot() const {
    return impl_->snapshot();
}

std::vector<std::uint8_t>
NativePortSoundBankEngine::capture_development_state() const {
    return impl_->capture_development_state();
}

NativePortSoundBankDevelopmentStateInventory
NativePortSoundBankEngine::validate_development_state(
    const std::span<const std::uint8_t> state) const {
    return impl_->validate_development_state(state);
}

void NativePortSoundBankEngine::restore_development_state(
    const std::span<const std::uint8_t> state) {
    impl_->restore_development_state(state);
}

} // namespace katana::runtime

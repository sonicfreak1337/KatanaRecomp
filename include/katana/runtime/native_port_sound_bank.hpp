#pragma once

#include "katana/runtime/native_port_audio_engine.hpp"
#include "katana/runtime/native_port_platform.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_sound_bank_contract_version = 4u;

enum class NativePortSoundBankFailure : std::uint8_t {
    None,
    InvalidConfig,
    InvalidHandle,
    ContentOpen,
    ContentRead,
    InvalidMlt,
    UnsupportedUnit,
    InvalidProgramBank,
    InvalidSequenceBank,
    InvalidSequence,
    UnsupportedController,
    UnsupportedEffect,
    InvalidSample,
    ResourceLimit,
    AudioFeed,
    ThreadViolation,
};

class NativePortSoundBankError final : public std::runtime_error {
  public:
    NativePortSoundBankError(NativePortSoundBankFailure failure,
                             std::string_view operation);

    [[nodiscard]] NativePortSoundBankFailure failure() const noexcept;

  private:
    NativePortSoundBankFailure failure_;
};

struct NativePortSoundBankConfig final {
    std::uint32_t output_sample_rate = 44'100u;
    std::uint32_t render_block_frames = 256u;
    std::uint32_t target_feed_frames = 512u;
    std::uint32_t maximum_collections = 256u;
    std::uint32_t maximum_collection_bytes = 64u * 1024u * 1024u;
    std::uint64_t maximum_total_collection_bytes = 256ull * 1024ull * 1024ull;
    std::uint32_t maximum_units_per_collection = 4'096u;
    std::uint32_t maximum_sequences_per_bank = 4'096u;
    std::uint32_t maximum_events_per_sequence = 1'000'000u;
    std::uint32_t maximum_reference_depth = 16u;
    std::uint32_t maximum_active_sequences = 128u;
    std::uint32_t maximum_synth_voices = 128u;
    std::uint32_t maximum_midi_ports = 64u;
    std::uint32_t maximum_decoded_sample_frames = 16u * 1024u * 1024u;
    std::uint32_t maximum_render_blocks_per_pump = 16u;
};

struct NativePortSoundCollectionHandle final {
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFu && generation != 0u;
    }
};

struct NativePortSoundSequenceHandle final {
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFu && generation != 0u;
    }
};

struct NativePortSoundVoiceHandle final {
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFu && generation != 0u;
    }
};

struct NativePortSoundMidiPortHandle final {
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFu && generation != 0u;
    }
};

struct NativePortSoundSequenceConfig final {
    std::uint8_t sequence_bank = 0u;
    std::uint8_t program_bank = 0u;
    std::uint16_t sequence = 0u;
    float gain = 1.0f;
    float pan = 0.0f;
    float playback_rate = 1.0f;
    std::int16_t pitch_bend = 0;
    std::uint8_t direct_level = 127u;
    std::uint8_t effect_level = 127u;
    bool enable_authored_loops = true;
};

struct NativePortSoundNoteConfig final {
    std::uint8_t program_bank = 0u;
    std::uint8_t program = 0u;
    std::uint8_t note = 60u;
    std::uint8_t velocity = 127u;
    float gain = 1.0f;
    float pan = 0.0f;
    std::int16_t pitch_bend = 0;
    std::uint8_t direct_level = 127u;
    std::uint8_t effect_level = 127u;
};

struct NativePortSoundMidiPortConfig final {
    std::uint8_t program_bank = 0u;
    std::uint8_t program = 0u;
    float gain = 1.0f;
    float pan = 0.0f;
    float playback_rate = 1.0f;
    std::int16_t pitch_bend = 0;
    std::uint8_t direct_level = 127u;
    std::uint8_t effect_level = 127u;
};

enum class NativePortSoundSequenceState : std::uint8_t {
    Playing,
    Paused,
    Completed,
    Stopped,
};

struct NativePortSoundSequenceSnapshot final {
    NativePortSoundSequenceState state = NativePortSoundSequenceState::Stopped;
    std::uint64_t rendered_frames = 0u;
    std::uint64_t dispatched_events = 0u;
    std::uint64_t loop_count = 0u;
    std::uint32_t active_voices = 0u;
};

struct NativePortSoundMidiPortSnapshot final {
    std::uint8_t program_bank = 0u;
    std::uint8_t program = 0u;
    float gain = 1.0f;
    float pan = 0.0f;
    float playback_rate = 1.0f;
    std::int16_t pitch_bend = 0;
    std::uint8_t direct_level = 127u;
    std::uint8_t effect_level = 127u;
    std::uint32_t active_notes = 0u;
    bool sequence_active = false;
};

struct NativePortSoundBankSnapshot final {
    std::uint64_t loaded_collections = 0u;
    std::uint64_t unloaded_collections = 0u;
    std::uint64_t parsed_programs = 0u;
    std::uint64_t parsed_splits = 0u;
    std::uint64_t parsed_sequences = 0u;
    std::uint64_t parsed_events = 0u;
    std::uint64_t decoded_samples = 0u;
    std::uint64_t decoded_pcm16_samples = 0u;
    std::uint64_t decoded_pcm8_samples = 0u;
    std::uint64_t decoded_adpcm_samples = 0u;
    std::uint64_t decoded_sample_frames = 0u;
    std::uint64_t resident_collection_bytes = 0u;
    std::uint64_t rendered_frames = 0u;
    std::uint64_t effect_frames = 0u;
    std::uint32_t active_collections = 0u;
    std::uint32_t active_sequences = 0u;
    std::uint32_t active_voices = 0u;
    std::uint64_t feed_buffered_frames = 0u;
};

// Native semantic provider for Manatee title sound banks. MLT/SMPB/SMSB
// describe instruments and musical intent; this service parses those bounded
// assets and renders directly to host PCM. It never exposes sound RAM, AICA
// registers, ARM7 execution, command rings, DMA, interrupts or device timing.
//
// All methods are confined to the construction thread. Content is opened
// through an identity-bound NativePortContentFileBinding and handles are
// generation checked. Unknown formats/controllers/effects fail closed.
class NativePortSoundBankEngine final {
  public:
    NativePortSoundBankEngine(
        NativePortPlatformServices& platform,
        NativePortAudioEngine& audio,
        const NativePortSoundBankConfig& config = {});
    ~NativePortSoundBankEngine();

    NativePortSoundBankEngine(const NativePortSoundBankEngine&) = delete;
    NativePortSoundBankEngine& operator=(const NativePortSoundBankEngine&) = delete;
    NativePortSoundBankEngine(NativePortSoundBankEngine&&) = delete;
    NativePortSoundBankEngine& operator=(NativePortSoundBankEngine&&) = delete;

    [[nodiscard]] NativePortSoundCollectionHandle load_collection(
        const NativePortContentFileBinding& binding);
    void unload_collection(NativePortSoundCollectionHandle collection);
    [[nodiscard]] bool has_program(NativePortSoundCollectionHandle collection,
                                   std::uint8_t bank,
                                   std::uint8_t program) const;
    [[nodiscard]] bool has_sequence(NativePortSoundCollectionHandle collection,
                                    std::uint8_t bank,
                                    std::uint16_t sequence) const;
    // Bounded eager validation/preload for every unique sample referenced by
    // the collection. This uses the same PCM16/PCM8/ADPCM decoder and cache as
    // live notes, avoiding first-note stalls without exposing AICA memory.
    void predecode_collection_samples(
        NativePortSoundCollectionHandle collection);

    [[nodiscard]] NativePortSoundSequenceHandle play_sequence(
        NativePortSoundCollectionHandle collection,
        const NativePortSoundSequenceConfig& config);
    void pause_sequence(NativePortSoundSequenceHandle sequence);
    void resume_sequence(NativePortSoundSequenceHandle sequence);
    void stop_sequence(NativePortSoundSequenceHandle sequence);
    void release_sequence(NativePortSoundSequenceHandle sequence);
    void set_sequence_gain_pan(NativePortSoundSequenceHandle sequence,
                               float gain,
                               float pan);
    void set_sequence_pitch_bend(NativePortSoundSequenceHandle sequence,
                                 std::int16_t pitch_bend);
    void set_sequence_playback_rate(NativePortSoundSequenceHandle sequence,
                                    float playback_rate);
    void set_sequence_send_levels(NativePortSoundSequenceHandle sequence,
                                  std::uint8_t direct_level,
                                  std::uint8_t effect_level);

    [[nodiscard]] NativePortSoundVoiceHandle note_on(
        NativePortSoundCollectionHandle collection,
        const NativePortSoundNoteConfig& config);
    void note_off(NativePortSoundVoiceHandle voice);
    void release_voice(NativePortSoundVoiceHandle voice);
    void set_voice_gain_pan(NativePortSoundVoiceHandle voice,
                            float gain,
                            float pan);
    void set_voice_pitch_bend(NativePortSoundVoiceHandle voice,
                              std::int16_t pitch_bend);
    void set_voice_send_levels(NativePortSoundVoiceHandle voice,
                               std::uint8_t direct_level,
                               std::uint8_t effect_level);

    // Semantic Manatee/MIDI port surface. A port owns ordinary title audio
    // intent (bank/program, notes, sequence and live controls); it is not an
    // AICA command queue, sound-RAM view or device register abstraction.
    [[nodiscard]] NativePortSoundMidiPortHandle open_midi_port(
        NativePortSoundCollectionHandle collection,
        const NativePortSoundMidiPortConfig& config = {});
    void close_midi_port(NativePortSoundMidiPortHandle port);
    void set_midi_program(NativePortSoundMidiPortHandle port,
                          std::uint8_t bank,
                          std::uint8_t program);
    void set_midi_gain_pan(NativePortSoundMidiPortHandle port,
                           float gain,
                           float pan);
    void set_midi_pitch_bend(NativePortSoundMidiPortHandle port,
                             std::int16_t pitch_bend);
    void set_midi_playback_rate(NativePortSoundMidiPortHandle port,
                                float playback_rate);
    void set_midi_send_levels(NativePortSoundMidiPortHandle port,
                              std::uint8_t direct_level,
                              std::uint8_t effect_level);
    [[nodiscard]] NativePortSoundVoiceHandle midi_note_on(
        NativePortSoundMidiPortHandle port,
        std::uint8_t note,
        std::uint8_t velocity);
    void midi_note_off(NativePortSoundMidiPortHandle port,
                       std::uint8_t note);
    [[nodiscard]] NativePortSoundSequenceHandle midi_play_sequence(
        NativePortSoundMidiPortHandle port,
        std::uint8_t sequence_bank,
        std::uint16_t sequence,
        bool enable_authored_loops = true);
    void midi_pause(NativePortSoundMidiPortHandle port);
    void midi_resume(NativePortSoundMidiPortHandle port);
    void midi_stop(NativePortSoundMidiPortHandle port);

    // Explicit lifecycle boundaries keep stopped handles distinct from
    // released title resources. stop_all() silences playback while retaining
    // reusable ports, sequences and collections. The release operations
    // invalidate their respective generation-checked handles; reset()
    // invalidates every sound-bank handle and unloads every collection.
    void close_all_midi_ports();
    void release_all_sequences();
    void unload_all_collections();
    void stop_all();
    void reset();
    void pump();

    [[nodiscard]] NativePortSoundSequenceSnapshot sequence_snapshot(
        NativePortSoundSequenceHandle sequence) const;
    [[nodiscard]] NativePortSoundMidiPortSnapshot midi_port_snapshot(
        NativePortSoundMidiPortHandle port) const;
    [[nodiscard]] NativePortSoundBankSnapshot snapshot() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime

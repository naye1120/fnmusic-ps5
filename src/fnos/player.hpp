// ps5-homebrew-ui - Streaming playback of one NAS track: ranged HTTP reads,
// libavformat/libavcodec decoding, 48 kHz stereo float into a mixer stream.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoding runs on its own thread because a network read must never hold up
// the frame. That thread talks to the render thread through atomics only, and
// to the audio thread through the StreamRing the mixer reads.

#pragma once

#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <vector>

namespace hui::fnos
{

enum class PlayerState : int
{
    stopped,
    opening, // resolving the address and reading the header
    playing,
    paused,
    ended, // the stream reached its end on its own
};

enum class PlayerError : int
{
    none = 0,
    network,     // the reads failed
    denied,      // the server wants a login (401/403)
    missing,     // the audio file is gone
    unsupported, // no demuxer or decoder for this format
    decode,      // the data did not decode
};

// Chinese text for one reason, shown in the player banner.
const char *player_error_text(PlayerError error);

class Player
{
  public:
    Player();
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    // Attach before the audio thread starts: the mixer reads a slot pointer
    // without synchronisation. Slot 0 is the menu music, so this takes slot 1.
    void attach(audio::Mixer &mixer);

    // Starts `url` (Api::stream_url) with `headers`. Everything after that is
    // asynchronous: state() reports opening, then playing or an error.
    bool play(const std::string &url, const std::vector<std::string> &headers);
    void pause();
    void resume();
    void stop();
    // Jumps to `seconds`, or `seconds` ahead when it is negative.
    void seek(int seconds);
    void set_gain(float gain);
    // The length the NAS claims for this track, said before a byte is read. A
    // whole-album image answers with the length of the album, so the per-song
    // figure has to be able to overrule the container's.
    void ask_length(int seconds)
    {
        asked_.store(seconds);
    }

    PlayerState state() const
    {
        return static_cast<PlayerState>(state_.load(std::memory_order_relaxed));
    }
    bool active() const
    {
        const PlayerState value = state();
        return value == PlayerState::opening || value == PlayerState::playing;
    }
    int position() const
    {
        return position_.load(std::memory_order_relaxed);
    }
    // Seconds, or 0 until the header has been read. The NAS's own figure wins
    // when the container's is wildly longer than it: see ask_length().
    int duration() const
    {
        const int asked = claimed();
        return asked != 0 ? asked : duration_.load(std::memory_order_relaxed);
    }
    // The seconds the container reported, untouched by the server's figure.
    int container_length() const
    {
        return duration_.load(std::memory_order_relaxed);
    }
    PlayerError error() const
    {
        return static_cast<PlayerError>(error_.load(std::memory_order_relaxed));
    }
    // True once the stream ended by itself; the caller plays the next track.
    bool finished() const
    {
        return state() == PlayerState::ended;
    }
    // True when the song named by the NAS has played out but the bytes have
    // not: an image of a whole album runs on past the end of any one track of
    // it, and the queue would sit on the record until the last side ended.
    bool overrun() const
    {
        return claimed() != 0 && state() == PlayerState::playing && position() >= claimed();
    }

  private:
    static void *run(void *user);
    // Non-zero when the server's figure overrules the container's: no ordinary
    // song is both 60 seconds and half again longer than its own metadata
    // says, so a gap that wide is the whole album being counted.
    int claimed() const
    {
        const int asked = asked_.load(std::memory_order_relaxed);
        const int container = duration_.load(std::memory_order_relaxed);
        return asked > 0 && container > asked + 60 && container > asked * 3 / 2 ? asked : 0;
    }

    void decode(const std::string &url, const std::vector<std::string> &headers);
    // Moves the writer to a fresh ring and tells the mixer, so the samples
    // buffered before a seek are never heard.
    void retarget();
    void fail(PlayerError error);

    static constexpr std::size_t kSlot = 1;
    static constexpr float kStreamGain = 1.0f;
    // 1<<16 stereo frames is about 1.4 s of cushion for a slow network.
    static constexpr std::size_t kRingFrames = 1u << 16;

    audio::Mixer *mixer_ = nullptr;
#ifdef HUI_PS5
    // Only the console decodes: the PC preview keeps state for the screens and
    // never builds a ring or a thread.
    // The audio thread reads `ring_`; `last_ring_` was swapped out at the
    // previous seek and is no longer reachable by it.
    audio::StreamRing *ring_ = nullptr;
    audio::StreamRing *last_ring_ = nullptr;

    pthread_t worker_{};
    bool worker_running_ = false;
#endif
    std::atomic<bool> keep_running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> seek_request_{-1}; // seconds, -1 when nothing is pending
    std::atomic<int> state_{static_cast<int>(PlayerState::stopped)};
    std::atomic<int> position_{0};
    std::atomic<int> duration_{0};
    std::atomic<int> asked_{0}; // the NAS's own length for this track, 0 unknown
    std::atomic<int> error_{static_cast<int>(PlayerError::none)};

    // Written before the thread starts and read only while it runs.
    std::string url_;
    std::vector<std::string> headers_;
};

} // namespace hui::fnos

// ps5-homebrew-ui - The play queue on top of the library worker and the
// streaming decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/service.hpp"

#include "fnos/http.hpp"
#include "platform/ps5/system.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace hui::fnos
{

namespace
{

// The tail of the previous launch's log, in a rolling window. Reading the file
// whole behind a size cap is the wrong shape for this: one session of library
// traffic overflows any cap, and `read_file` then returns nothing at all — the
// line that names the death is the first thing lost.
std::string previous_run_tail(const std::string &path)
{
    constexpr std::size_t kWindow = 8u << 10;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return std::string();
    std::string window;
    bool rolled = false;
    char chunk[4096];
    for (;;)
    {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got <= 0)
            break; // a failed read still leaves the bytes already collected
        window.append(chunk, static_cast<std::size_t>(got));
        if (window.size() > kWindow)
        {
            window.erase(0, window.size() - kWindow);
            rolled = true;
        }
    }
    ::close(fd);
    if (rolled)
    {
        // The window opens in the middle of a line; that fragment is noise, not
        // evidence.
        const std::size_t newline = window.find('\n');
        if (newline != std::string::npos)
            window.erase(0, newline + 1);
    }
    return window;
}

// Cut a line to what fits on screen. Half a UTF-8 sequence draws as nothing at
// all, so back out of a continuation byte too.
void shrink(std::string &line, std::size_t max_bytes)
{
    while (line.size() > max_bytes)
    {
        line.pop_back();
        while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0u) == 0x80u)
            line.pop_back();
    }
}

// The last few lines the previous launch wrote. One line cannot say which side
// of a thread boundary a death fell on; the marker ladder only reads as a run,
// so the steps before it come along too.
std::vector<std::string> previous_run_lines(const std::string &data_root)
{
    std::vector<std::string> lines;
    // Two receipts, newest channel first: `steps.log` is written with
    // open()/write() by the markers themselves, `app.log` is stdout. Whichever
    // has the last word wins, and the difference between them is an answer in
    // its own right.
    std::string text = previous_run_tail(data_root + "/dev/steps.prev.log");
    if (text.empty())
        text = previous_run_tail(data_root + "/dev/app.prev.log");
    bool shell_closed = false;
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        const std::size_t stop = text.find('\n', cursor);
        const std::size_t end = stop == std::string::npos ? text.size() : stop;
        std::string line = text.substr(cursor, end - cursor);
        cursor = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const std::size_t mark = line.find("[HUI] ");
        if (mark != std::string::npos)
            line.erase(0, mark + 6);
        // A run the shell closed on its own leaves its marker as the last line:
        // nothing died, so there is nothing to report.
        shell_closed = line.find("closing") != std::string::npos;
        if (shell_closed)
            continue;
        shrink(line, 72u);
        lines.push_back(std::move(line));
        if (lines.size() > 5)
            lines.erase(lines.begin());
    }
    if (shell_closed)
        lines.clear();
    return lines;
}

} // namespace

Service &Service::instance()
{
    static Service service;
    return service;
}

bool Service::start(audio::Mixer &mixer, const std::string &data_root)
{
    crash_lines_ = previous_run_lines(data_root);
    crash_trace_ = crash_lines_.empty() ? std::string() : crash_lines_.back();
    // libcurl's own contract: one global init before any thread uses it. Both
    // the library worker and the decoder ask for music, and the first time two
    // threads touch curl together is the frame a song is chosen.
    if (!http_init())
        return false;
    // Slot 0 is the menu music's; the stream takes slot 1. Attaching after the
    // audio thread starts would let it read a null pointer.
    player_.attach(mixer);
    player_.set_gain(volume_ * volume_);
    if (!library_.start(&art_))
        return false;
    library_.restore(data_root);
    return true;
}

void Service::update(gfx::Renderer &renderer)
{
    art_.upload(renderer);
    library_.poll();
    // The track is over when the stream says so, or when the seconds the NAS
    // counted for it have gone by inside a file that is far longer than the
    // song: an album image never reaches its own end at the end of a track.
    if (!player_.finished() && !player_.overrun())
        return;
    if (repeat_ == Repeat::one)
    {
        play_at(index_);
        return;
    }
    const bool last = index_ + 1 >= static_cast<int>(queue_.size());
    if (last && repeat_ != Repeat::all && !shuffle_)
    {
        player_.stop();
        return;
    }
    next();
}

void Service::play_queue(const std::vector<Track> &tracks, int index)
{
    queue_ = tracks;
    if (index < 0 || index >= static_cast<int>(queue_.size()))
        return;
    play_at(index);
}

void Service::play_at(int index)
{
    if (index < 0 || index >= static_cast<int>(queue_.size()))
        return;
    last_shuffle_ = index_;
    index_ = index;
    const Track &track = queue_[static_cast<std::size_t>(index)];
    // The markers around a song choice are what tells a hardware crash apart
    // from a hung stream: the last line that lands is the step that died.
    sys::log("[HUI] play %d/%d", index, static_cast<int>(queue_.size()));
    const bool opened = player_.play(library_.stream_url(track.guid), library_.media_headers());
    // The NAS's own figure, said before the container has been read: some of
    // these files are whole-album images, and the only number that knows where
    // this track ends is the one in the listing.
    player_.ask_length(track.duration);
    sys::log("[HUI] play decoder thread=%d", opened ? 1 : 0);
    library_.request_lyric(track.guid);
    if (!track.cover_id.empty())
        library_.request_cover(track.cover_id);
    sys::log("[HUI] play asked for lyric and cover");
}

const Track *Service::current() const
{
    if (index_ < 0 || index_ >= static_cast<int>(queue_.size()))
        return nullptr;
    return &queue_[static_cast<std::size_t>(index_)];
}

void Service::toggle_play()
{
    switch (player_.state())
    {
    case PlayerState::playing:
        player_.pause();
        break;
    case PlayerState::paused:
        player_.resume();
        break;
    case PlayerState::stopped:
        play_at(index_);
        break;
    default: // opening or ended: let the stream come to us
        break;
    }
}

void Service::next()
{
    const int size = static_cast<int>(queue_.size());
    if (size == 0)
        return;
    if (shuffle_ && size > 1)
    {
        int pick = std::rand() % size;
        while (pick == last_shuffle_ || pick == index_)
            pick = (pick + 1) % size;
        play_at(pick);
        return;
    }
    play_at((index_ + 1) % size);
}

void Service::previous()
{
    const int size = static_cast<int>(queue_.size());
    if (size == 0)
        return;
    if (player_.position() > 4)
    {
        player_.seek(0); // a second press of previous starts the track again
        return;
    }
    play_at((index_ + size - 1) % size);
}

void Service::seek_to(int seconds)
{
    // Paused is seekable as well: the bar is on screen and its thumb has moved,
    // so a seek that only answers while sound runs reads as a dead button.
    const PlayerState state = player_.state();
    if (state == PlayerState::opening || state == PlayerState::playing ||
        state == PlayerState::paused)
    {
        player_.seek(seconds);
        return;
    }
    if (state == PlayerState::ended && index_ >= 0)
    {
        // The decoder thread is gone, so the only way to honour a seek on a
        // track that has played out is to open it again and ask for the seconds
        // back once the thread is up.
        play_at(index_);
        player_.seek(seconds);
    }
}

void Service::stop()
{
    player_.stop();
}

void Service::set_volume(float level)
{
    volume_ = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    player_.set_gain(volume_ * volume_);
}

void Service::toggle_shuffle()
{
    shuffle_ = !shuffle_;
}

void Service::cycle_repeat()
{
    repeat_ = static_cast<Repeat>((static_cast<int>(repeat_) + 1) % 3);
}

} // namespace hui::fnos

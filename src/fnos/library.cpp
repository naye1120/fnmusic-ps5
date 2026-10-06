// ps5-homebrew-ui - Library worker implementation: the queue, the calls, the
// answers, and the account file.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/library.hpp"

#include "core/save_file.hpp"
#include "fnos/art.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

#ifdef HUI_PS5
extern "C" int sceKernelUsleep(unsigned int microseconds);
#else
#include <unistd.h>
#endif

namespace hui::fnos
{
namespace
{

// The worker sleeps between jobs; 40 ms is far below what a person notices and
// keeps a payload thread from spinning on an empty queue.
constexpr int kIdleMilliseconds = 40;
// Answers the render thread has not come back for. Dropping the oldest keeps a
// hitch from building a queue that never drains.
constexpr std::size_t kMaxAnswers = 8;

constexpr const char *kShelfTitle[] = {
    "歌曲", "歌单", "专辑", "艺人", "风格", "收藏", "搜索", "设置",
};

void idle()
{
#ifdef HUI_PS5
    sceKernelUsleep(static_cast<unsigned int>(kIdleMilliseconds) * 1000u);
#else
    ::usleep(static_cast<unsigned int>(kIdleMilliseconds) * 1000u);
#endif
}

std::string trimmed(std::string value)
{
    const char *space = " \t\r\n";
    std::size_t begin = value.find_first_not_of(space);
    if (begin == std::string::npos)
        return {};
    std::size_t finish = value.find_last_not_of(space);
    return value.substr(begin, finish - begin + 1);
}

// One line of a save file: a percent-escaped value, so a token with a newline
// or an address with an equals sign still round-trips.
std::string unescape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    const auto digit = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t at = 0; at < value.size(); ++at)
    {
        if (value[at] != '%' || at + 2 >= value.size())
        {
            out.push_back(value[at]);
            continue;
        }
        const int high = digit(value[at + 1]);
        const int low = digit(value[at + 2]);
        if (high < 0 || low < 0)
        {
            out.push_back(value[at]);
            continue;
        }
        out.push_back(static_cast<char>(high * 16 + low));
        at += 2;
    }
    return out;
}

std::string escape(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value)
    {
        if (c == '%' || c == '\n' || c == '\r' || c == '=')
        {
            out.push_back('%');
            out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xfu]);
            out.push_back(hex[static_cast<unsigned char>(c) & 0xfu]);
        }
        else
            out.push_back(c);
    }
    return out;
}

// "[01:23.45]词" - every leading tag makes a line, so a repeated chorus needs
// no special case. The player reports whole seconds, so a line starts at the
// second its tag rounds to.
void parse_lrc(const std::string &text, std::vector<LyricLine> *out)
{
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t line_end = text.find('\n', at);
        if (line_end == std::string::npos)
            line_end = text.size();
        const std::string line = trimmed(text.substr(at, line_end - at));
        at = line_end + 1;
        std::vector<int> times;
        std::size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[')
        {
            const std::size_t close = line.find(']', cursor);
            if (close == std::string::npos)
                break;
            const std::string tag = line.substr(cursor + 1, close - cursor - 1);
            const std::size_t colon = tag.find(':');
            if (colon == std::string::npos)
            {
                cursor = close + 1;
                continue;
            }
            const int minutes = std::atoi(tag.substr(0, colon).c_str());
            const std::string rest = tag.substr(colon + 1);
            const std::size_t dot = rest.find('.');
            const int seconds = std::atoi(rest.substr(0, dot).c_str());
            // The digits after the point are fractions of a second: "4" means
            // four tenths, "45" forty-five hundredths, "456" a millisecond run.
            int hundredths = 0;
            if (dot != std::string::npos)
            {
                std::string fraction;
                for (std::size_t digit = dot + 1; digit < rest.size(); ++digit)
                {
                    if (rest[digit] < '0' || rest[digit] > '9')
                        break;
                    fraction.push_back(rest[digit]);
                }
                if (fraction.size() == 1)
                    hundredths = (fraction[0] - '0') * 10;
                else if (fraction.size() >= 2)
                    hundredths = (fraction[0] - '0') * 10 + (fraction[1] - '0');
            }
            if (minutes >= 0 && seconds >= 0)
                times.push_back(minutes * 60 + seconds + (hundredths >= 50 ? 1 : 0));
            cursor = close + 1;
        }
        const std::string words = trimmed(line.substr(cursor));
        if (words.empty())
            continue;
        for (const int seconds : times)
        {
            LyricLine entry;
            entry.at = seconds;
            entry.text = words;
            out->push_back(std::move(entry));
        }
    }
    std::sort(out->begin(), out->end(),
              [](const LyricLine &a, const LyricLine &b) { return a.at < b.at; });
}

// A "key=value" file a PC can write into the install folder.
bool read_conf(const std::string &path, std::string *server, std::string *username,
               std::string *password)
{
    std::string text;
    if (!save::read_file(path, &text, 4096))
        return false;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t line_end = text.find('\n', at);
        if (line_end == std::string::npos)
            line_end = text.size();
        const std::string line = text.substr(at, line_end - at);
        at = line_end + 1;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || line[0] == '#')
            continue;
        const std::string key = trimmed(line.substr(0, equals));
        const std::string value = trimmed(line.substr(equals + 1));
        if (key == "server" && server != nullptr)
            *server = value;
        else if (key == "username" && username != nullptr)
            *username = value;
        else if (key == "password" && password != nullptr)
            *password = value;
    }
    return server != nullptr && !server->empty();
}

} // namespace

Library::Library() = default;

Library::~Library()
{
    if (!running_)
        return;
    {
        std::lock_guard<std::mutex> lock(guard_);
        stop_ = true;
    }
    pthread_join(worker_, nullptr);
    running_ = false;
}

bool Library::start(ArtCache *art)
{
    art_ = art;
    if (running_)
        return true;
    if (pthread_create(&worker_, nullptr, run, this) != 0)
        return false;
    running_ = true;
    return true;
}

void *Library::run(void *user)
{
    static_cast<Library *>(user)->work();
    return nullptr;
}

void Library::work()
{
    for (;;)
    {
        Job job;
        {
            std::lock_guard<std::mutex> lock(guard_);
            if (stop_)
                return;
            if (!queue_.empty())
            {
                job = std::move(queue_.front());
                queue_.pop_front();
                working_ = true;
            }
        }
        if (job.request == kNone)
        {
            idle();
            continue;
        }
        Answer answer = ask(job);
        std::lock_guard<std::mutex> lock(guard_);
        working_ = false;
        if (stop_)
            return;
        if (answers_.size() >= kMaxAnswers)
            answers_.pop_front();
        answers_.push_back(std::move(answer));
    }
}

Library::Answer Library::ask(const Job &job)
{
    Answer answer;
    answer.request = job.request;
    answer.guid = job.guid;

    std::string server;
    std::string token;
    std::string device;
    {
        std::lock_guard<std::mutex> lock(guard_);
        server = base_;
        token = token_;
        device = device_id_;
    }
    api_.set_server(job.request == kLogin ? job.server : server);
    api_.set_token(job.request == kLogin || job.request == kSignOut ? std::string() : token);

    std::string error;
    bool ok = true;
    switch (job.request)
    {
    case kLogin:
        ok = api_.login(job.username, job.password, device, &error);
        answer.signed_in = ok;
        break;
    case kCheck:
    {
        TrackPage probe;
        ok = api_.tracks(1, &probe, &error);
        answer.signed_in = ok;
        break;
    }
    case kSignOut:
        answer.signed_in = false;
        break;
    case kSongs:
    {
        TrackPage page;
        ok = api_.tracks(1, &page, &error);
        answer.tracks = std::move(page);
        break;
    }
    case kPlaylists:
    {
        PlaylistPage page;
        ok = api_.playlists(1, &page, &error);
        answer.playlists = std::move(page);
        break;
    }
    case kAlbums:
    {
        AlbumPage page;
        ok = api_.albums(1, &page, &error);
        answer.albums = std::move(page);
        break;
    }
    case kArtists:
    {
        ArtistPage page;
        ok = api_.artists(&page, &error);
        answer.artists = std::move(page);
        break;
    }
    case kGenres:
    {
        GenrePage page;
        ok = api_.genres(&page, &error);
        answer.genres = std::move(page);
        break;
    }
    case kFavorites:
    {
        TrackPage page;
        ok = api_.favorites(&page, &error);
        answer.tracks = std::move(page);
        break;
    }
    case kSearch:
    {
        TrackPage page;
        ok = api_.search(job.text, &page, &error);
        answer.tracks = std::move(page);
        break;
    }
    case kPlaylist:
    case kAlbum:
    case kArtist:
    case kGenre:
    {
        TrackPage page;
        if (job.request == kPlaylist)
            ok = api_.playlist_tracks(job.guid, &page, &error);
        else if (job.request == kAlbum)
            ok = api_.album_tracks(job.guid, &page, &error);
        else if (job.request == kArtist)
            ok = api_.artist_tracks(job.guid, &page, &error);
        else
            ok = api_.genre_tracks(job.guid, &page, &error);
        answer.tracks = std::move(page);
        break;
    }
    case kFavoriteOn:
        ok = api_.favorite(job.guid, true, &error);
        break;
    case kFavoriteOff:
        ok = api_.favorite(job.guid, false, &error);
        break;
    case kLyric:
        ok = api_.lyric(job.guid, &answer.lyric, &error);
        break;
    case kCover:
    {
        std::string bytes;
        ok = api_.fetch_cover(job.guid, kCoverEdge, &bytes, &error);
        if (ok && !decode_image(bytes, kCoverEdge, &answer.cover, &answer.cover_width,
                                &answer.cover_height))
        {
            ok = false;
            error = "这张封面解不出来";
        }
        break;
    }
    default:
        ok = false;
        break;
    }

    answer.failed = !ok;
    answer.message = ok ? std::string() : error;
    if (job.request == kLogin || job.request == kCheck || job.request == kSignOut)
    {
        answer.server = api_.server();
        answer.token = api_.token();
    }
    return answer;
}

void Library::poll()
{
    for (;;)
    {
        Answer answer;
        {
            std::lock_guard<std::mutex> lock(guard_);
            if (answers_.empty())
                return;
            answer = std::move(answers_.front());
            answers_.pop_front();
        }
        apply(std::move(answer));
    }
}

void Library::apply(Answer &&answer)
{
    switch (answer.request)
    {
    case kCover:
        // Artwork is silent: a missing cover shows the plate instead.
        if (art_ != nullptr && !answer.failed && !answer.cover.empty())
        {
            art_->stage(answer.guid, std::move(answer.cover), answer.cover_width,
                        answer.cover_height);
        }
        return;
    case kLogin:
    case kCheck:
    case kSignOut:
    {
        {
            std::lock_guard<std::mutex> lock(guard_);
            base_ = answer.server;
            token_ = answer.token;
            signed_in_ = answer.signed_in && !answer.failed;
            message_ =
                answer.failed ? answer.message : (answer.signed_in ? "已连接服务器" : "请先登录");
            failed_ = answer.failed;
        }
        // A token is worth keeping, a failed one is not. persist() takes the
        // lock itself, so it runs after that scope has closed.
        persist();
        return;
    }
    case kFavoriteOn:
    case kFavoriteOff:
        // The row already shows the change; only a refusal needs saying.
        message_ = answer.message;
        failed_ = answer.failed;
        return;
    case kLyric:
        lyric_.clear();
        parse_lrc(answer.lyric, &lyric_);
        message_ = answer.failed ? answer.message : (lyric_.empty() ? "这首歌没有歌词" : "");
        failed_ = answer.failed;
        return;
    default:
        break;
    }

    if (answer.failed)
    {
        // A shelf that failed keeps what it had, so the screen does not blank
        // out because one page timed out.
        message_ = answer.message;
        failed_ = true;
        return;
    }
    message_.clear();
    failed_ = false;
    switch (answer.request)
    {
    case kSongs:
    case kFavorites:
    case kSearch:
    case kPlaylist:
    case kAlbum:
    case kArtist:
    case kGenre:
        tracks_ = std::move(answer.tracks.items);
        break;
    case kPlaylists:
        playlists_ = std::move(answer.playlists.items);
        break;
    case kAlbums:
        albums_ = std::move(answer.albums.items);
        break;
    case kArtists:
        artists_ = std::move(answer.artists.items);
        break;
    case kGenres:
        genres_ = std::move(answer.genres.items);
        break;
    default:
        break;
    }
}

bool Library::shelf_request(int request)
{
    switch (request)
    {
    case kSongs:
    case kPlaylists:
    case kAlbums:
    case kArtists:
    case kGenres:
    case kFavorites:
    case kSearch:
    case kPlaylist:
    case kAlbum:
    case kArtist:
    case kGenre:
        return true;
    default:
        return false;
    }
}

void Library::drop_shelf_jobs()
{
    std::lock_guard<std::mutex> lock(guard_);
    for (auto job = queue_.begin(); job != queue_.end();)
    {
        if (shelf_request(job->request))
            job = queue_.erase(job);
        else
            ++job;
    }
}

void Library::push(const Job &job, bool first)
{
    std::lock_guard<std::mutex> lock(guard_);
    if (first)
        queue_.push_front(job);
    else
        queue_.push_back(job);
}

bool Library::queued(int request, const std::string &guid) const
{
    std::lock_guard<std::mutex> lock(guard_);
    for (const Job &job : queue_)
    {
        if (job.request == request && job.guid == guid)
            return true;
    }
    return false;
}

bool Library::busy() const
{
    std::lock_guard<std::mutex> lock(guard_);
    return working_ || !queue_.empty();
}

void Library::show(Shelf shelf)
{
    shelf_ = shelf;
    collection_.clear();
    drop_shelf_jobs();
    failed_ = false;
    Job job;
    switch (shelf)
    {
    case Shelf::songs:
        job.request = kSongs;
        break;
    case Shelf::playlists:
        job.request = kPlaylists;
        break;
    case Shelf::albums:
        job.request = kAlbums;
        break;
    case Shelf::artists:
        job.request = kArtists;
        break;
    case Shelf::genres:
        job.request = kGenres;
        break;
    case Shelf::favorites:
        job.request = kFavorites;
        break;
    case Shelf::search:
        if (query_.empty())
        {
            message_ = "输入关键词搜索歌曲";
            return;
        }
        job.request = kSearch;
        job.text = query_;
        break;
    case Shelf::setup:
        message_ = signed_in_ ? "已经登录" : "填写服务器和账号后登录";
        return;
    }
    message_ = std::string("正在读取") + kShelfTitle[static_cast<int>(shelf)] + "…";
    push(job, true);
}

void Library::open(Shelf kind, const std::string &guid, const std::string &name)
{
    shelf_ = kind;
    collection_ = name;
    drop_shelf_jobs();
    failed_ = false;
    Job job;
    job.guid = guid;
    switch (kind)
    {
    case Shelf::playlists:
        job.request = kPlaylist;
        break;
    case Shelf::albums:
        job.request = kAlbum;
        break;
    case Shelf::artists:
        job.request = kArtist;
        break;
    case Shelf::genres:
        job.request = kGenre;
        break;
    default:
        return;
    }
    message_ = "正在读取《" + name + "》…";
    push(job, true);
}

void Library::search(const std::string &text)
{
    query_ = text;
    show(Shelf::search);
}

void Library::sign_in(std::string server, std::string username, std::string password)
{
    username_ = std::move(username);
    password_ = password; // kept, so a reboot does not cost the keyboard again
    Job job;
    job.request = kLogin;
    job.server = std::move(server);
    job.username = username_;
    job.password = std::move(password);
    drop_shelf_jobs();
    message_ = "正在登录…";
    failed_ = false;
    push(job, true);
}

void Library::sign_out()
{
    Job job;
    job.request = kSignOut;
    drop_shelf_jobs();
    push(job, true);
    {
        std::lock_guard<std::mutex> lock(guard_);
        token_.clear();
        signed_in_ = false;
    }
    password_.clear(); // leaving gives back the password that was kept for it
    message_ = "已退出登录";
    failed_ = false;
}

void Library::set_favorite(const std::string &guid, bool on)
{
    for (Track &track : tracks_)
    {
        if (track.guid == guid)
            track.favorite = on; // optimistic: the star answers at once
    }
    Job job;
    job.request = on ? kFavoriteOn : kFavoriteOff;
    job.guid = guid;
    push(job, false);
}

void Library::request_lyric(const std::string &guid)
{
    lyric_.clear();
    Job job;
    job.request = kLyric;
    job.guid = guid;
    push(job, true);
}

void Library::request_cover(const std::string &cover_id)
{
    if (cover_id.empty() || art_ == nullptr || !art_->wants(cover_id))
        return;
    if (queued(kCover, cover_id))
        return;
    Job job;
    job.request = kCover;
    job.guid = cover_id;
    push(job, false); // artwork waits behind the shelf the player is on
}

std::string Library::stream_url(const std::string &guid) const
{
    std::lock_guard<std::mutex> lock(guard_);
    Api scratch;
    scratch.set_server(base_);
    scratch.set_token(token_);
    return scratch.stream_url(guid);
}

std::vector<std::string> Library::media_headers() const
{
    std::lock_guard<std::mutex> lock(guard_);
    Api scratch;
    scratch.set_server(base_);
    scratch.set_token(token_);
    return scratch.media_headers();
}

std::string Library::server() const
{
    std::lock_guard<std::mutex> lock(guard_);
    return base_;
}

void Library::restore(const std::string &data_root)
{
    data_root_ = data_root;
    std::string conf_server;
    std::string conf_user;
    std::string conf_password;
    std::string saved_token;
    {
        std::lock_guard<std::mutex> lock(guard_);
        std::string bytes;
        // A saved listing has to be read whole: past this cap `read_file`
        // answers "no" and the library is simply gone until the next fetch, so
        // the number has to be above a real library, not above a comfortable
        // one.
        if (save::read_file(data_root + "/library.bin", &bytes, 8u << 20))
        {
            const save::Decoded saved = save::decode(save::Kind::library, bytes);
            if (saved.ok)
            {
                std::size_t at = 0;
                while (at < saved.payload.size())
                {
                    std::size_t line_end = saved.payload.find('\n', at);
                    if (line_end == std::string::npos)
                        line_end = saved.payload.size();
                    const std::string line = saved.payload.substr(at, line_end - at);
                    at = line_end + 1;
                    const std::size_t equals = line.find('=');
                    if (equals == std::string::npos)
                        continue;
                    const std::string key = line.substr(0, equals);
                    const std::string value = unescape(line.substr(equals + 1));
                    if (key == "server")
                        base_ = value;
                    else if (key == "token")
                        token_ = value;
                    else if (key == "username")
                        username_ = value;
                    else if (key == "password")
                        password_ = value;
                    else if (key == "device")
                        device_id_ = value;
                }
            }
        }
        // The install folder is the one place a PC can write, so the address and
        // the account may be handed over there; the password is used once and is
        // never kept. A prebuilt default rides along under /app0/assets. Only
        // before the first sign-in: afterwards the saved server wins, or the
        // setup page could never change it.
        if (token_.empty())
        {
            for (const char *path : {"/app0/fnos.conf", "/app0/assets/fnos.conf"})
            {
                if (read_conf(path, &conf_server, &conf_user, &conf_password))
                    break;
            }
            if (!conf_server.empty())
            {
                base_ = conf_server;
                if (!conf_user.empty())
                    username_ = conf_user;
            }
        }
        if (device_id_.empty())
        {
            unsigned long long seed =
                static_cast<unsigned long long>(std::time(nullptr)) * 1000003ull +
                static_cast<unsigned long long>(std::rand());
            char text[33];
            for (int part = 0; part < 4; ++part)
            {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                std::snprintf(text + part * 8, 9, "%08llx", seed >> 32);
            }
            text[32] = '\0';
            device_id_ = text;
        }
        signed_in_ = !token_.empty();
        saved_token = token_;
    }

    // Outside the lock: both of these take it themselves.
    if (saved_token.empty() && !conf_server.empty() && !conf_password.empty())
    {
        sign_in(conf_server, username_, conf_password);
        return;
    }
    if (!saved_token.empty())
    {
        Job job;
        job.request = kCheck;
        push(job, true);
        message_ = "正在连接服务器…";
        return;
    }
    message_ = signed_in_ ? "已经登录" : "填写服务器和账号后登录";
}

void Library::persist()
{
    if (data_root_.empty())
        return;
    std::lock_guard<std::mutex> lock(guard_);
    std::string payload;
    payload += "server=" + escape(base_) + "\n";
    payload += "token=" + escape(token_) + "\n";
    payload += "username=" + escape(username_) + "\n";
    payload += "password=" + escape(password_) + "\n";
    payload += "device=" + escape(device_id_) + "\n";
    save::write_atomic(data_root_ + "/library.bin", save::encode(save::Kind::library, 1, payload));
}

bool Library::load_config(const std::string &path, std::string *server, std::string *username,
                          std::string *password)
{
    return read_conf(path, server, username, password);
}

} // namespace hui::fnos

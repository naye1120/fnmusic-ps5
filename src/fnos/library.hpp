// ps5-homebrew-ui - The NAS library: one worker thread answers every request
// the screen makes, so a slow server never stops the frame.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screen posts a request and keeps drawing; the worker calls the blocking
// API, decodes the answer, and poll() moves the result onto the main thread
// once per frame. Answers are applied in the order they were asked for, so a
// shelf that was left two requests ago cannot overwrite the one now on screen.
// Artwork jobs share the same thread: two network threads against one libcurl
// session would need more locking than the console can spare.

#pragma once

#include "fnos/api.hpp"
#include "fnos/art.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

namespace hui::fnos
{

// Which shelf the screen is showing.
enum class Shelf : int
{
    songs, // the whole library
    playlists,
    albums,
    artists,
    genres,
    favorites,
    search,
    setup, // server and account
};

// One timed line of a lyric.
struct LyricLine
{
    int at = 0; // seconds from the start of the track
    std::string text;
};

class Library
{
  public:
    Library();
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    // Starts the worker. Artwork it decodes is uploaded through `art`, which
    // lives on the render thread and is only touched from poll().
    bool start(ArtCache *art);

    // ---- render thread ----
    // Moves finished answers into the shelves; call once per frame.
    void poll();
    // Asks for a shelf's content and forgets requests for the one before it.
    void show(Shelf shelf);
    // Tracks inside a playlist, album, artist or genre.
    void open(Shelf kind, const std::string &guid, const std::string &name);
    // Re-reads what is on screen: the shelf, or the collection opened inside it.
    void refresh();
    void search(const std::string &text);
    void sign_in(std::string server, std::string username, std::string password);
    void sign_out();
    void set_favorite(const std::string &guid, bool on);
    void request_lyric(const std::string &guid);
    // Queues one cover for the worker; skipped when it is cached or asked for.
    void request_cover(const std::string &cover_id);
    bool busy() const;

    // What playback needs: the address of a track and the headers it wants.
    // Both take the lock, so they stay consistent with the session.
    std::string stream_url(const std::string &guid) const;
    std::vector<std::string> media_headers() const;

    const std::vector<Track> &tracks() const
    {
        return tracks_;
    }
    const std::vector<Playlist> &playlists() const
    {
        return playlists_;
    }
    const std::vector<Album> &albums() const
    {
        return albums_;
    }
    const std::vector<Artist> &artists() const
    {
        return artists_;
    }
    const std::vector<Genre> &genres() const
    {
        return genres_;
    }
    const std::vector<LyricLine> &lyric() const
    {
        return lyric_;
    }
    Shelf shelf() const
    {
        return shelf_;
    }
    // The playlist, album, artist or genre whose tracks are on screen.
    const std::string &collection() const
    {
        return collection_;
    }
    const std::string &query() const
    {
        return query_;
    }
    // One line of Chinese for the banner: progress, or what went wrong.
    const std::string &message() const
    {
        return message_;
    }
    bool failed() const
    {
        return failed_;
    }
    bool signed_in() const
    {
        return signed_in_;
    }
    std::string server() const;
    std::string username() const
    {
        return username_;
    }
    // What the player typed as their password. It is kept on purpose: without
    // it a reboot costs a trip through the on-screen keyboard, and the same
    // file already holds the token that unlocks the library.
    std::string password() const
    {
        return password_;
    }
    // The 32 hex digits this console calls itself by, shown on the setup page
    // so the NAS's device list can be recognised.
    const std::string &device() const
    {
        return device_id_;
    }

    // The account outlives the app: /download0/hui/library.bin, checked and
    // atomic like the settings, and it carries the password as well as the
    // token. `server` and `device` are read from /app0/fnos.conf when that file
    // exists, which is how a PC hands the address and the account to a title
    // whose own folder it cannot write.
    void restore(const std::string &data_root);
    void persist();
    // Fills server/username/password from a "key=value" file; false if absent.
    bool load_config(const std::string &path, std::string *server, std::string *username,
                     std::string *password);

  private:
    enum Request : int
    {
        kNone,
        kSongs,
        kPlaylists,
        kAlbums,
        kArtists,
        kGenres,
        kFavorites,
        kSearch,
        kPlaylist,
        kAlbum,
        kArtist,
        kGenre,
        kLogin,
        kCheck,
        kSignOut,
        kFavoriteOn,
        kFavoriteOff,
        kCover,
        kLyric,
    };

    struct Job
    {
        int request = kNone;
        std::string guid;
        std::string name; // the collection's title, for the banner
        std::string text; // the search words
        std::string server;
        std::string username;
        std::string password; // already hashed by Api
    };

    struct Answer
    {
        int request = kNone;
        std::string guid;
        std::string message;
        bool failed = false;
        bool signed_in = false;
        // The session a login or a check left behind, for the mirrors.
        std::string server;
        std::string token;
        TrackPage tracks;
        PlaylistPage playlists;
        AlbumPage albums;
        ArtistPage artists;
        GenrePage genres;
        std::string lyric;
        std::vector<std::uint8_t> cover; // decoded RGBA
        int cover_width = 0;
        int cover_height = 0;
    };

    static void *run(void *user);
    void work();
    // Moves one finished answer onto the shelves. Render thread.
    void apply(Answer &&answer);
    // The worker: one job, one answer. Every network call happens here.
    Answer ask(const Job &job);
    // Drops queued shelf jobs a newer one replaces; cover and lyric jobs stay.
    void drop_shelf_jobs();
    // True for the requests that fill a whole shelf, so only the newest of
    // them survives. Covers, lyrics and favourites are additive.
    static bool shelf_request(int request);
    void push(const Job &job, bool first);
    bool queued(int request, const std::string &guid) const;

    Api api_; // the worker's client; it is synced from base_ and token_ per job
    ArtCache *art_ = nullptr;
    mutable std::mutex guard_;
    std::deque<Job> queue_;
    std::deque<Answer> answers_; // finished, waiting for poll()
    bool stop_ = false;
    bool working_ = false;
    // The session, the one truth both threads read under guard_. A login leaves
    // a token, and the token is what unlocks the library on the next boot.
    std::string base_;
    std::string token_;
    pthread_t worker_{};
    bool running_ = false;

    // Mirrored from the session whenever it changes.
    bool signed_in_ = false;

    Shelf shelf_ = Shelf::songs;
    std::string collection_;
    std::string collection_guid_; // what to re-ask for on a refresh
    std::string query_;
    std::string message_;
    bool failed_ = false;
    std::string username_;
    std::string password_; // typed once, then re-used: see password()
    std::string device_id_;
    std::string data_root_;

    std::vector<Track> tracks_;
    std::vector<Playlist> playlists_;
    std::vector<Album> albums_;
    std::vector<Artist> artists_;
    std::vector<Genre> genres_;
    std::vector<LyricLine> lyric_;
};

} // namespace hui::fnos

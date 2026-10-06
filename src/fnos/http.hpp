// ps5-homebrew-ui - Blocking HTTP on libcurl, with redirects that keep the
// caller's headers (the fnOS music API answers some GETs with a 302 to
// another host, and the session lives in a hand-rolled Cookie header).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hui::fnos
{

struct HttpResult
{
    long status = 0;      // final HTTP status; 0 when the transport failed
    std::string body;     // response bytes, capped at the requested maximum
    std::string error;    // curl text, or empty when the request completed
    std::string location; // set while following a redirect (debugging)

    bool ok() const
    {
        return error.empty() && status >= 200 && status < 300;
    }
};

// One call per process, before any other function here.
bool http_init();

// `headers` are raw "Name: value" lines and are repeated on every redirect
// hop, which is what the music API needs (its session cookie must survive the
// proxy hop to a mounted cloud drive). `timeout_seconds` bounds the whole
// transfer; the media stream uses its own, longer budget. Requests never run
// on the render thread - the caller owns a worker thread.
HttpResult http_request(const std::string &method, const std::string &url,
                        std::vector<std::string> headers, const std::string &body,
                        std::size_t max_bytes, int timeout_seconds);

// Percent-encodes a query value (search text is Chinese, so this is not
// optional). Returns the input unchanged if curl cannot allocate.
std::string http_escape(const std::string &value);

// Resolves a URL the way the API client does: follow redirects with Range
// bytes=0-0 and report the address the content finally arrives from, so the
// stream reader can ask for it directly instead of paying a 302 on every
// resumed connection. Returns the input URL unchanged on any failure.
std::string http_resolve_redirects(const std::string &url, std::vector<std::string> headers,
                                   int timeout_seconds);

// A ranged reader over one URL. The music stream is read in blocks: every
// read() is a GET with a Range header on a kept-alive connection, which is
// what makes byte-wise seeking cheap without holding a whole album in memory.
// Not copyable; one instance belongs to one thread.
class ByteReader
{
  public:
    ByteReader() = default;
    ~ByteReader();
    ByteReader(const ByteReader &) = delete;
    ByteReader &operator=(const ByteReader &) = delete;

    bool open(const std::string &url, std::vector<std::string> headers, std::string *error);
    // Fewer than `count` bytes only at end of file or on error.
    std::size_t read(std::uint64_t offset, void *out, std::size_t count);
    long long size() const
    {
        return size_;
    }
    const std::string &error() const
    {
        return error_;
    }
    void close();

  private:
#ifdef HUI_PS5
    void *easy_ = nullptr; // CURL *, kept opaque so this header needs no curl
    // The easy handle lives longer than open(), and curl keeps calling back
    // into whatever pointer its options name. Response headers go to a member
    // so no later range transfer can be handed the address of a stack string
    // that open() already destroyed.
    std::string response_headers_;
#endif
    long long size_ = -1;
    // A streamed song asks for bytes thousands of times; the crash ladder only
    // needs to see the beginning of one. Unused on the PC snapshot host, where
    // this reader has no transfers to describe.
    [[maybe_unused]] int log_left_ = 20;
    std::string url_;
    std::vector<std::string> headers_;
    std::string error_;
};

} // namespace hui::fnos

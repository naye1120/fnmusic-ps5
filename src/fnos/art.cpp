// ps5-homebrew-ui - Album artwork implementation: FFmpeg decode, GL upload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/art.hpp"

#ifdef HUI_PS5
#include "gfx/renderer.hpp"

#include <GL/glcorearb.h>
#endif

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace hui::fnos
{

#ifdef HUI_PS5

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace
{

// The bytes the server sent, read through a custom AVIO.
struct Memory
{
    const std::uint8_t *at = nullptr;
    const std::uint8_t *end = nullptr;
};

int read_memory(void *user, std::uint8_t *buffer, int size)
{
    auto *source = static_cast<Memory *>(user);
    const std::size_t left = static_cast<std::size_t>(source->end - source->at);
    const std::size_t want = std::min(left, static_cast<std::size_t>(size > 0 ? size : 0));
    std::memcpy(buffer, source->at, want);
    source->at += want;
    return static_cast<int>(want);
}

} // namespace

bool decode_image(const std::string &bytes, int edge, std::vector<std::uint8_t> *rgba, int *width,
                  int *height)
{
    if (bytes.empty() || rgba == nullptr || width == nullptr || height == nullptr)
        return false;

    Memory source{reinterpret_cast<const std::uint8_t *>(bytes.data()),
                  reinterpret_cast<const std::uint8_t *>(bytes.data()) + bytes.size()};
    constexpr int kIoSize = 16 * 1024;
    // Zeroed, and AVPROBE_PADDING_SIZE bytes longer than the size given to the
    // avio context: a container probe reads past the bytes it was given and is
    // promised zeros there.
    std::uint8_t *io = static_cast<std::uint8_t *>(av_mallocz(kIoSize + AVPROBE_PADDING_SIZE));
    if (io == nullptr)
        return false;
    AVIOContext *avio = avio_alloc_context(io, kIoSize, 0, &source, read_memory, nullptr, nullptr);
    if (avio == nullptr)
    {
        av_free(io);
        return false;
    }
    AVFormatContext *format = avformat_alloc_context();
    if (format == nullptr)
    {
        avio_context_free(&avio);
        return false;
    }
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    bool ok = false;
    AVFrame *frame = nullptr;
    AVFrame *scaled = nullptr;
    AVPacket *packet = av_packet_alloc();
    SwsContext *sws = nullptr;
    int out_w = 0;
    int out_h = 0;

    // The name is empty on purpose: the demuxer is chosen from the bytes.
    if (avformat_open_input(&format, "", nullptr, nullptr) >= 0 &&
        avformat_find_stream_info(format, nullptr) >= 0)
    {
        const int stream_index =
            av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index >= 0)
        {
            AVCodecParameters *parameters = format->streams[stream_index]->codecpar;
            const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
            AVCodecContext *context = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
            if (context != nullptr && avcodec_parameters_to_context(context, parameters) >= 0 &&
                avcodec_open2(context, codec, nullptr) >= 0)
            {
                frame = av_frame_alloc();
                while (av_read_frame(format, packet) >= 0)
                {
                    if (packet->stream_index == stream_index &&
                        avcodec_send_packet(context, packet) >= 0 &&
                        avcodec_receive_frame(context, frame) >= 0)
                        break;
                    av_packet_unref(packet);
                }
                if (frame != nullptr && frame->width > 0 && frame->height > 0)
                {
                    out_w = frame->width;
                    out_h = frame->height;
                    if (edge > 0 && std::max(out_w, out_h) > edge)
                    {
                        const double scale = static_cast<double>(edge) / std::max(out_w, out_h);
                        out_w = std::max(1, static_cast<int>(out_w * scale));
                        out_h = std::max(1, static_cast<int>(out_h * scale));
                    }
                    scaled = av_frame_alloc();
                    if (scaled != nullptr && av_image_alloc(scaled->data, scaled->linesize, out_w,
                                                            out_h, AV_PIX_FMT_RGBA, 32) >= 0)
                    {
                        sws = sws_getContext(
                            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                            out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        if (sws != nullptr)
                            sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                                      scaled->data, scaled->linesize);
                    }
                }
                if (sws != nullptr && scaled != nullptr)
                {
                    // Rows, because a plane's stride is padded past its width.
                    rgba->resize(static_cast<std::size_t>(out_w) * out_h * 4u);
                    for (int row = 0; row < out_h; ++row)
                    {
                        std::memcpy(rgba->data() + static_cast<std::size_t>(row) * out_w * 4u,
                                    scaled->data[0] +
                                        static_cast<std::size_t>(row) * scaled->linesize[0],
                                    static_cast<std::size_t>(out_w) * 4u);
                    }
                    *width = out_w;
                    *height = out_h;
                    ok = true;
                }
                avcodec_free_context(&context);
            }
            else if (context != nullptr)
                avcodec_free_context(&context);
        }
    }

    if (sws != nullptr)
        sws_freeContext(sws);
    if (scaled != nullptr)
    {
        av_freep(&scaled->data[0]);
        av_frame_free(&scaled);
    }
    if (frame != nullptr)
        av_frame_free(&frame);
    if (packet != nullptr)
        av_packet_free(&packet);
    if (format != nullptr)
        avformat_close_input(&format);
    if (avio != nullptr)
        avio_context_free(&avio);
    return ok;
}

#else // the PC preview host has no FFmpeg; the fake server sends no artwork.

bool decode_image(const std::string &, int, std::vector<std::uint8_t> *, int *, int *)
{
    return false;
}

#endif

void ArtCache::stage(const std::string &key, std::vector<std::uint8_t> rgba, int width, int height)
{
    if (key.empty() || rgba.empty() || width <= 0 || height <= 0)
        return;
    if (textures_.find(key) != textures_.end())
        return; // it is on the GPU already
    for (auto &waiting : incoming_)
    {
        if (waiting.key == key)
        {
            waiting.pixels = std::move(rgba);
            waiting.width = width;
            waiting.height = height;
            return;
        }
    }
    incoming_.push_back({key, std::move(rgba), width, height});
}

#ifdef HUI_PS5
void ArtCache::upload(gfx::Renderer &renderer)
{
    for (const Incoming &waiting : incoming_)
    {
        if (textures_.find(waiting.key) != textures_.end())
            continue;
        // Room for one more: drop the cover drawn longest ago.
        while (textures_.size() + 1 > kKeep)
        {
            auto oldest = textures_.begin();
            for (auto it = textures_.begin(); it != textures_.end(); ++it)
            {
                if (it->second.used < oldest->second.used)
                    oldest = it;
            }
            if (oldest->second.texture != 0)
                glDeleteTextures(1, &oldest->second.texture);
            textures_.erase(oldest);
        }
        Entry entry;
        entry.width = waiting.width;
        entry.height = waiting.height;
        entry.used = ++clock_;
        entry.texture =
            renderer.batch().create_texture(entry.width, entry.height, waiting.pixels.data());
        textures_.emplace(waiting.key, entry);
    }
    incoming_.clear();
}
#else
// The PC preview never decodes a cover (there is no FFmpeg), so this queue is
// always empty; the tests have no GL context to upload into.
void ArtCache::upload(gfx::Renderer &)
{
    incoming_.clear();
}
#endif

std::uint32_t ArtCache::texture(const std::string &key)
{
    const auto found = textures_.find(key);
    if (found == textures_.end())
        return 0;
    found->second.used = ++clock_;
    return found->second.texture;
}

bool ArtCache::size(const std::string &key, int *width, int *height) const
{
    const auto found = textures_.find(key);
    if (found == textures_.end())
        return false;
    *width = found->second.width;
    *height = found->second.height;
    return true;
}

bool ArtCache::wants(const std::string &key) const
{
    if (key.empty())
        return false;
    if (textures_.find(key) != textures_.end())
        return false;
    for (const Incoming &waiting : incoming_)
    {
        if (waiting.key == key)
            return false;
    }
    return true;
}

void ArtCache::clear()
{
#ifdef HUI_PS5
    for (auto &entry : textures_)
    {
        if (entry.second.texture != 0)
            glDeleteTextures(1, &entry.second.texture);
    }
#endif
    textures_.clear();
    incoming_.clear();
}

} // namespace hui::fnos

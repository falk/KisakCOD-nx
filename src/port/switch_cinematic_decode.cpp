// See switch_cinematic_decode.h.
#include "switch_cinematic_decode.h"

#include <string.h>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

struct SwitchCinematicVideoDecoder::Impl
{
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *sws = nullptr;
    // FFmpeg's av_image_alloc/av_image_fill_arrays write four pointers and
    // four linesizes (the fourth is null for YUV420P); sizing these for three
    // planes corrupts the members that follow.
    uint8_t *planeData[4] = { nullptr, nullptr, nullptr, nullptr };
    int planeStride[4] = { 0, 0, 0, 0 };
    int videoStream = -1;
    bool drained = false;
    bool havePacket = false;
    int64_t framesSeen = 0;
};

static void SetError(char *errText, size_t errTextSize, const char *what, int err)
{
    if (!errText || errTextSize == 0)
        return;
    char avError[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    if (err < 0)
        av_strerror(err, avError, sizeof(avError));
    if (avError[0])
        snprintf(errText, errTextSize, "%s: %s", what, avError);
    else
        snprintf(errText, errTextSize, "%s", what);
}

SwitchCinematicVideoDecoder::SwitchCinematicVideoDecoder()
    : m_impl(new Impl)
    , m_frameTimeMs(0)
{
    memset(&m_info, 0, sizeof(m_info));
    m_info.frameCount = -1;
    m_info.durationMs = -1;
}

SwitchCinematicVideoDecoder::~SwitchCinematicVideoDecoder()
{
    Close();
    delete m_impl;
}

bool SwitchCinematicVideoDecoder::IsOpen() const
{
    return m_impl && m_impl->format != nullptr;
}

bool SwitchCinematicVideoDecoder::Open(const char *path, char *errText, size_t errTextSize)
{
    Close();

    // Horizon paths are "sdmc:/..." and FFmpeg's URL splitter would read
    // "sdmc" as a protocol name ("Protocol not found").  Route through the
    // file: protocol explicitly; it opens whatever follows it verbatim.
    char url[600];
    if (strncmp(path, "file:", 5) == 0)
        snprintf(url, sizeof(url), "%s", path);
    else
        snprintf(url, sizeof(url), "file:%s", path);

    int err = avformat_open_input(&m_impl->format, url, nullptr, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "avformat_open_input failed", err);
        return false;
    }
    err = avformat_find_stream_info(m_impl->format, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "avformat_find_stream_info failed", err);
        Close();
        return false;
    }

    const AVCodec *decoder = nullptr;
    m_impl->videoStream = av_find_best_stream(m_impl->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (m_impl->videoStream < 0 || !decoder)
    {
        SetError(errText, errTextSize, "no video stream", m_impl->videoStream);
        Close();
        return false;
    }

    AVStream *stream = m_impl->format->streams[m_impl->videoStream];
    m_impl->codec = avcodec_alloc_context3(decoder);
    if (!m_impl->codec)
    {
        SetError(errText, errTextSize, "avcodec_alloc_context3 failed", AVERROR(ENOMEM));
        Close();
        return false;
    }
    err = avcodec_parameters_to_context(m_impl->codec, stream->codecpar);
    if (err < 0)
    {
        SetError(errText, errTextSize, "avcodec_parameters_to_context failed", err);
        Close();
        return false;
    }
    m_impl->codec->pkt_timebase = stream->time_base;
    err = avcodec_open2(m_impl->codec, decoder, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "avcodec_open2 failed", err);
        Close();
        return false;
    }

    m_impl->packet = av_packet_alloc();
    m_impl->frame = av_frame_alloc();
    if (!m_impl->packet || !m_impl->frame)
    {
        SetError(errText, errTextSize, "packet/frame alloc failed", AVERROR(ENOMEM));
        Close();
        return false;
    }

    m_info.width = m_impl->codec->width;
    m_info.height = m_impl->codec->height;
    m_info.chromaWidth = (m_info.width + 1) / 2;
    m_info.chromaHeight = (m_info.height + 1) / 2;
    AVRational frameRate = av_guess_frame_rate(m_impl->format, stream, nullptr);
    m_info.fps = frameRate.num > 0 && frameRate.den > 0 ? av_q2d(frameRate) : 0.0;
    if (stream->duration != AV_NOPTS_VALUE)
        m_info.durationMs = av_rescale_q(stream->duration, stream->time_base, AVRational{ 1, 1000 });
    else if (m_impl->format->duration != AV_NOPTS_VALUE)
        m_info.durationMs = m_impl->format->duration / (AV_TIME_BASE / 1000);
    if (stream->nb_frames > 0)
        m_info.frameCount = stream->nb_frames;
    else if (m_info.durationMs > 0 && m_info.fps > 0.0)
        m_info.frameCount = (int64_t)(m_info.durationMs * m_info.fps / 1000.0 + 0.5);

    const AVPixFmtDescriptor *srcDesc = av_pix_fmt_desc_get(m_impl->codec->pix_fmt);
    const bool srcIsYuv420 = srcDesc && !(srcDesc->flags & AV_PIX_FMT_FLAG_RGB)
        && srcDesc->log2_chroma_w == 1 && srcDesc->log2_chroma_h == 1;
    if (!srcIsYuv420)
    {
        m_impl->sws = sws_getContext(
            m_info.width, m_info.height, m_impl->codec->pix_fmt,
            m_info.width, m_info.height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_impl->sws)
        {
            SetError(errText, errTextSize, "sws_getContext failed", AVERROR(EINVAL));
            Close();
            return false;
        }
    }

    int allocErr = av_image_alloc(
        m_impl->planeData, m_impl->planeStride,
        m_info.width, m_info.height, AV_PIX_FMT_YUV420P, 32);
    if (allocErr < 0)
    {
        SetError(errText, errTextSize, "av_image_alloc failed", allocErr);
        Close();
        return false;
    }

    return true;
}

void SwitchCinematicVideoDecoder::Close()
{
    if (!m_impl)
        return;
    if (m_impl->planeData[0])
    {
        av_freep(&m_impl->planeData[0]);
        m_impl->planeData[1] = nullptr;
        m_impl->planeData[2] = nullptr;
    }
    if (m_impl->sws)
        sws_freeContext(m_impl->sws);
    if (m_impl->frame)
        av_frame_free(&m_impl->frame);
    if (m_impl->packet)
        av_packet_free(&m_impl->packet);
    if (m_impl->codec)
        avcodec_free_context(&m_impl->codec);
    if (m_impl->format)
        avformat_close_input(&m_impl->format);
    m_impl->sws = nullptr;
    m_impl->videoStream = -1;
    m_impl->drained = false;
    m_impl->havePacket = false;
    m_impl->framesSeen = 0;
    m_info.width = 0;
    m_info.height = 0;
    m_info.chromaWidth = 0;
    m_info.chromaHeight = 0;
    m_info.fps = 0.0;
    m_info.frameCount = -1;
    m_info.durationMs = -1;
    m_frameTimeMs = 0;
}

static void CopyPlane(uint8_t *dst, int dstStride, const uint8_t *src, int srcStride, int width, int height)
{
    if (dstStride == width && srcStride == width)
    {
        memcpy(dst, src, (size_t)width * height);
        return;
    }
    for (int y = 0; y < height; ++y)
        memcpy(dst + (size_t)y * dstStride, src + (size_t)y * srcStride, width);
}

void SwitchCinematicVideoDecoder::ConvertFrame(char *errText, size_t errTextSize)
{
    AVFrame *frame = m_impl->frame;
    if (m_impl->sws)
    {
        int scaled = sws_scale(
            m_impl->sws,
            frame->data, frame->linesize,
            0, m_info.height,
            m_impl->planeData, m_impl->planeStride);
        if (scaled <= 0)
            SetError(errText, errTextSize, "sws_scale produced no rows", AVERROR(EINVAL));
        return;
    }

    CopyPlane(m_impl->planeData[0], m_impl->planeStride[0], frame->data[0], frame->linesize[0],
              m_info.width, m_info.height);
    CopyPlane(m_impl->planeData[1], m_impl->planeStride[1], frame->data[1], frame->linesize[1],
              m_info.chromaWidth, m_info.chromaHeight);
    CopyPlane(m_impl->planeData[2], m_impl->planeStride[2], frame->data[2], frame->linesize[2],
              m_info.chromaWidth, m_info.chromaHeight);
}

int SwitchCinematicVideoDecoder::DecodeNext(char *errText, size_t errTextSize)
{
    if (!IsOpen())
    {
        SetError(errText, errTextSize, "decoder not open", AVERROR(EINVAL));
        return -1;
    }

    for (;;)
    {
        int err = avcodec_receive_frame(m_impl->codec, m_impl->frame);
        if (err == 0)
        {
            ConvertFrame(errText, errTextSize);
            AVStream *stream = m_impl->format->streams[m_impl->videoStream];
            int64_t ts = m_impl->frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = m_impl->frame->pts;
            if (ts != AV_NOPTS_VALUE)
                m_frameTimeMs = av_rescale_q(ts, stream->time_base, AVRational{ 1, 1000 });
            else if (m_info.fps > 0.0)
                m_frameTimeMs = (int64_t)(m_impl->framesSeen * 1000.0 / m_info.fps);
            ++m_impl->framesSeen;
            return 1;
        }
        if (err == AVERROR_EOF)
        {
            m_impl->drained = true;
            return 0;
        }
        if (err != AVERROR(EAGAIN))
        {
            SetError(errText, errTextSize, "avcodec_receive_frame failed", err);
            return -1;
        }

        // The decoder wants more input.  A packet that came back EAGAIN from
        // send_packet is retried after the next receive, so keep it ref'd.
        if (!m_impl->havePacket)
        {
            err = av_read_frame(m_impl->format, m_impl->packet);
            if (err == AVERROR(EAGAIN))
                continue;
            if (err < 0)
            {
                // Flush the decoder; the next receive returns EOF once drained.
                err = avcodec_send_packet(m_impl->codec, nullptr);
                if (err < 0 && err != AVERROR_EOF)
                {
                    SetError(errText, errTextSize, "avcodec send flush failed", err);
                    return -1;
                }
                continue;
            }
            if (m_impl->packet->stream_index != m_impl->videoStream)
            {
                av_packet_unref(m_impl->packet);
                continue;
            }
            m_impl->havePacket = true;
        }

        err = avcodec_send_packet(m_impl->codec, m_impl->packet);
        if (err == AVERROR(EAGAIN))
            continue;
        av_packet_unref(m_impl->packet);
        m_impl->havePacket = false;
        if (err < 0 && err != AVERROR_EOF)
        {
            SetError(errText, errTextSize, "avcodec_send_packet failed", err);
            return -1;
        }
    }
}

bool SwitchCinematicVideoDecoder::SeekToStart(char *errText, size_t errTextSize)
{
    if (!IsOpen())
    {
        SetError(errText, errTextSize, "decoder not open", AVERROR(EINVAL));
        return false;
    }
    int err = avformat_seek_file(m_impl->format, m_impl->videoStream, INT64_MIN, 0, INT64_MAX, 0);
    if (err < 0)
    {
        SetError(errText, errTextSize, "avformat_seek_file failed", err);
        return false;
    }
    avcodec_flush_buffers(m_impl->codec);
    m_impl->drained = false;
    m_impl->framesSeen = 0;
    m_frameTimeMs = 0;
    return true;
}

const uint8_t *SwitchCinematicVideoDecoder::Plane(int index) const
{
    if (index < 0 || index > 2 || !m_impl)
        return nullptr;
    return m_impl->planeData[index];
}

int SwitchCinematicVideoDecoder::PlaneStride(int index) const
{
    if (index < 0 || index > 2 || !m_impl)
        return 0;
    return m_impl->planeStride[index];
}

// ─────────────────────────────────────────────────────────────────────────
// Audio: BinkAudio via FFmpeg, resampled to the OpenAL cinematic stream's
// fixed S16 stereo 48 kHz format and pulled one chunk at a time.
//
// Streaming rather than decoded up front: the audio-bearing retail movies run
// up to ~100 s (~19 MB as S16 stereo 48 kHz) and every movie plays while a
// level load or the renderer owns the frame, so the decoder holds only the
// resampled remainder of one decoded frame.
// ─────────────────────────────────────────────────────────────────────────

namespace
{
const int kCinematicAudioRate = 48000;
const int kCinematicAudioChannels = 2;
} // namespace

struct SwitchCinematicAudioDecoder::Impl
{
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    int audioStream = -1;
    bool haveFrame = false;   // `frame` holds a decoded frame not yet converted
    bool draining = false;    // every packet was read and the decoder was flushed
    bool eof = false;         // no more audio, including the resampler tail
    bool swrDrained = false;  // the resampler's delay was already flushed out
    std::vector<int16_t> pending; // resampled frames not yet handed to the caller
    int pendingOffset = 0;        // frames of `pending` already served

    int PendingFrames() const { return (int)(pending.size() / kCinematicAudioChannels); }
};

SwitchCinematicAudioDecoder::SwitchCinematicAudioDecoder()
    : m_impl(new Impl)
{
    memset(&m_info, 0, sizeof(m_info));
}

SwitchCinematicAudioDecoder::~SwitchCinematicAudioDecoder()
{
    Close();
    delete m_impl;
}

bool SwitchCinematicAudioDecoder::IsOpen() const
{
    return m_impl && m_impl->format != nullptr;
}

bool SwitchCinematicAudioDecoder::Open(const char *path, char *errText, size_t errTextSize)
{
    Close();

    char url[600];
    if (strncmp(path, "file:", 5) == 0)
        snprintf(url, sizeof(url), "%s", path);
    else
        snprintf(url, sizeof(url), "file:%s", path);

    int err = avformat_open_input(&m_impl->format, url, nullptr, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "audio avformat_open_input failed", err);
        Close();
        return false;
    }
    err = avformat_find_stream_info(m_impl->format, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "audio avformat_find_stream_info failed", err);
        Close();
        return false;
    }

    const AVCodec *decoder = nullptr;
    m_impl->audioStream = av_find_best_stream(m_impl->format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (m_impl->audioStream < 0 || !decoder)
    {
        SetError(errText, errTextSize, "no audio stream", m_impl->audioStream);
        Close();
        return false;
    }

    AVStream *stream = m_impl->format->streams[m_impl->audioStream];
    m_impl->codec = avcodec_alloc_context3(decoder);
    if (!m_impl->codec)
    {
        SetError(errText, errTextSize, "audio avcodec_alloc_context3 failed", AVERROR(ENOMEM));
        Close();
        return false;
    }
    err = avcodec_parameters_to_context(m_impl->codec, stream->codecpar);
    if (err < 0)
    {
        SetError(errText, errTextSize, "audio avcodec_parameters_to_context failed", err);
        Close();
        return false;
    }
    m_impl->codec->pkt_timebase = stream->time_base;
    err = avcodec_open2(m_impl->codec, decoder, nullptr);
    if (err < 0)
    {
        SetError(errText, errTextSize, "audio avcodec_open2 failed", err);
        Close();
        return false;
    }

    if (m_impl->codec->sample_fmt == AV_SAMPLE_FMT_NONE || m_impl->codec->sample_rate <= 0)
    {
        SetError(errText, errTextSize, "audio codec has no sample format", AVERROR(EINVAL));
        Close();
        return false;
    }
    if (!av_channel_layout_check(&m_impl->codec->ch_layout) || m_impl->codec->ch_layout.nb_channels <= 0)
    {
        SetError(errText, errTextSize, "audio codec has no channel layout", AVERROR(EINVAL));
        Close();
        return false;
    }

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    err = swr_alloc_set_opts2(&m_impl->swr, &outLayout, AV_SAMPLE_FMT_S16, kCinematicAudioRate,
                              &m_impl->codec->ch_layout, m_impl->codec->sample_fmt,
                              m_impl->codec->sample_rate, 0, nullptr);
    if (err < 0 || !m_impl->swr || swr_init(m_impl->swr) < 0)
    {
        SetError(errText, errTextSize, "audio swr init failed", err < 0 ? err : AVERROR(EINVAL));
        Close();
        return false;
    }

    m_impl->packet = av_packet_alloc();
    m_impl->frame = av_frame_alloc();
    if (!m_impl->packet || !m_impl->frame)
    {
        SetError(errText, errTextSize, "audio frame/packet alloc failed", AVERROR(ENOMEM));
        Close();
        return false;
    }

    m_info.sampleRate = kCinematicAudioRate;
    m_info.channels = kCinematicAudioChannels;
    return true;
}

void SwitchCinematicAudioDecoder::Close()
{
    if (!m_impl)
        return;
    m_impl->pending.clear();
    if (m_impl->swr)
        swr_free(&m_impl->swr);
    if (m_impl->frame)
        av_frame_free(&m_impl->frame);
    if (m_impl->packet)
        av_packet_free(&m_impl->packet);
    if (m_impl->codec)
        avcodec_free_context(&m_impl->codec);
    if (m_impl->format)
        avformat_close_input(&m_impl->format);
    m_impl->audioStream = -1;
    m_impl->haveFrame = false;
    m_impl->draining = false;
    m_impl->eof = false;
    m_impl->swrDrained = false;
    m_impl->pendingOffset = 0;
    memset(&m_info, 0, sizeof(m_info));
}

bool SwitchCinematicAudioDecoder::SeekToStart(char *errText, size_t errTextSize)
{
    if (!IsOpen())
        return false;
    const int err = av_seek_frame(m_impl->format, m_impl->audioStream, 0, AVSEEK_FLAG_BACKWARD);
    if (err < 0)
    {
        SetError(errText, errTextSize, "audio av_seek_frame failed", err);
        return false;
    }
    avcodec_flush_buffers(m_impl->codec);
    swr_init(m_impl->swr); // drop the resampler's filter history
    m_impl->haveFrame = false;
    m_impl->draining = false;
    m_impl->eof = false;
    m_impl->swrDrained = false;
    m_impl->pending.clear();
    m_impl->pendingOffset = 0;
    return true;
}

// Drops the already-served prefix so decoded frames can be appended.
void SwitchCinematicAudioDecoder::CompactPending()
{
    if (m_impl->pendingOffset == 0)
        return;
    if (m_impl->pendingOffset >= m_impl->PendingFrames())
    {
        m_impl->pending.clear();
    }
    else
    {
        const size_t served = (size_t)m_impl->pendingOffset * kCinematicAudioChannels;
        m_impl->pending.erase(m_impl->pending.begin(), m_impl->pending.begin() + served);
    }
    m_impl->pendingOffset = 0;
}

// Converts the decoded frame in `frame` to S16 stereo and appends it to
// `pending`.
void SwitchCinematicAudioDecoder::ConvertPendingFrame()
{
    const int outCapacity = swr_get_out_samples(m_impl->swr, m_impl->frame->nb_samples);
    if (outCapacity > 0)
    {
        CompactPending();
        const size_t base = m_impl->pending.size();
        m_impl->pending.resize(base + (size_t)outCapacity * kCinematicAudioChannels);
        uint8_t *outPlanes[1] = { (uint8_t *)(m_impl->pending.data() + base) };
        const int outFrames = swr_convert(m_impl->swr, outPlanes, outCapacity,
                                          (const uint8_t **)m_impl->frame->extended_data,
                                          m_impl->frame->nb_samples);
        if (outFrames > 0)
            m_impl->pending.resize(base + (size_t)outFrames * kCinematicAudioChannels);
        else
            m_impl->pending.resize(base);
    }
    av_frame_unref(m_impl->frame);
    m_impl->haveFrame = false;
}

// Flushes the resampler's internal delay into `pending` so the tail of the
// track is not lost.
void SwitchCinematicAudioDecoder::DrainResampler()
{
    CompactPending();
    for (;;)
    {
        const int capacity = 4096;
        const size_t base = m_impl->pending.size();
        m_impl->pending.resize(base + (size_t)capacity * kCinematicAudioChannels);
        uint8_t *outPlanes[1] = { (uint8_t *)(m_impl->pending.data() + base) };
        const int outFrames = swr_convert(m_impl->swr, outPlanes, capacity, nullptr, 0);
        if (outFrames <= 0)
        {
            m_impl->pending.resize(base);
            break;
        }
        m_impl->pending.resize(base + (size_t)outFrames * kCinematicAudioChannels);
        if (outFrames < capacity)
            break;
    }
    m_impl->swrDrained = true;
}

int SwitchCinematicAudioDecoder::Decode(int16_t *dst, int maxFrames, char *errText, size_t errTextSize)
{
    if (!IsOpen() || !dst || maxFrames <= 0)
        return -1;

    int written = 0;
    while (written < maxFrames)
    {
        if (m_impl->pendingOffset >= m_impl->PendingFrames() && m_impl->pendingOffset > 0)
            CompactPending();
        if (m_impl->pendingOffset < m_impl->PendingFrames())
        {
            int take = m_impl->PendingFrames() - m_impl->pendingOffset;
            if (take > maxFrames - written)
                take = maxFrames - written;
            memcpy(dst + (size_t)written * kCinematicAudioChannels,
                   m_impl->pending.data() + (size_t)m_impl->pendingOffset * kCinematicAudioChannels,
                   (size_t)take * kCinematicAudioChannels * sizeof(int16_t));
            m_impl->pendingOffset += take;
            written += take;
            continue;
        }
        if (m_impl->eof)
            break;
        if (m_impl->haveFrame)
        {
            ConvertPendingFrame();
            continue;
        }

        const int err = avcodec_receive_frame(m_impl->codec, m_impl->frame);
        if (err == 0)
        {
            m_impl->haveFrame = true;
            continue;
        }
        if (err == AVERROR_EOF || (err == AVERROR(EAGAIN) && m_impl->draining))
        {
            if (!m_impl->swrDrained)
            {
                DrainResampler();
                continue;
            }
            m_impl->eof = true;
            break;
        }
        if (err != AVERROR(EAGAIN))
        {
            SetError(errText, errTextSize, "audio avcodec_receive_frame failed", err);
            return -1;
        }

        // The decoder wants more input: read until a packet for this stream is
        // accepted.
        bool sent = false;
        while (!sent)
        {
            const int readErr = av_read_frame(m_impl->format, m_impl->packet);
            if (readErr < 0)
            {
                const int flushErr = avcodec_send_packet(m_impl->codec, nullptr);
                if (flushErr < 0 && flushErr != AVERROR_EOF)
                {
                    SetError(errText, errTextSize, "audio flush failed", flushErr);
                    return -1;
                }
                m_impl->draining = true;
                m_impl->swrDrained = false;
                sent = true;
                break;
            }
            if (m_impl->packet->stream_index != m_impl->audioStream)
            {
                av_packet_unref(m_impl->packet);
                continue;
            }
            const int sendErr = avcodec_send_packet(m_impl->codec, m_impl->packet);
            av_packet_unref(m_impl->packet);
            if (sendErr == AVERROR(EAGAIN))
            {
                // Only reachable if the decoder was not drained before this
                // send, which would silently drop the packet.
                SetError(errText, errTextSize, "audio decoder was not drained before send", sendErr);
                return -1;
            }
            if (sendErr < 0)
            {
                SetError(errText, errTextSize, "audio avcodec_send_packet failed", sendErr);
                return -1;
            }
            sent = true;
        }
    }
    return written;
}

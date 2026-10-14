#include "io/decoder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// Random access over one movie.
//
// open() reads every packet of the video stream once, without decoding, and keeps the
// pts, dts and keyframe flag of each. Display order is the sorted pts of the packets the
// demuxer does not flag as discarded (edit lists), so frame_count and fps are measured
// from the stream itself. Frame 0 is then decoded to measure the dimensions.
//
// frame_at(i) either keeps decoding forward from where the decoder stands, or seeks to
// the last keyframe whose pts is at or before frame i and decodes forward from there,
// whichever decodes fewer frames.
//
// All ffmpeg state belongs to one worker thread. frame_at() hands it a request and waits;
// prefetch() hands it a request and returns, so the next frame of a sequential walk is
// decoded while the caller encodes the current one. frame_at() and prefetch() are meant
// to be called from one thread, the renderer's.

namespace vrc {
namespace {

// Seeking costs a demuxer seek, a decoder flush and a refill of the frame threads. When
// the keyframe for the target is at most this many frames past the current position,
// decoding forward is cheaper.
constexpr int64_t kForwardSlack = 4;

std::string av_error(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

int64_t order_ts(int64_t pts, int64_t dts) { return dts != AV_NOPTS_VALUE ? dts : pts; }

struct KeyInfo {
    int64_t pts = 0;
    int64_t seek_ts = 0;  // dts when known, the timestamp demuxers index seeks by
};

class FfmpegDecoder final : public Decoder {
  public:
    ~FfmpegDecoder() override {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            abort_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        sws_freeContext(sws_);
        av_packet_free(&pending_);
        av_packet_free(&pkt_);
        av_frame_free(&frame_);
        avcodec_free_context(&ctx_);
        avformat_close_input(&fmt_);
    }

    bool init(const std::string& path, std::string* error);

    const Source& source() const override { return source_; }

    Frame frame_at(int64_t index, std::string* error) override {
        if (index < 0 || index >= source_.frame_count) {
            if (error)
                *error = "Decoder::frame_at: frame " + std::to_string(index) +
                         " out of range [0, " + std::to_string(source_.frame_count) +
                         ") in " + source_.path;
            return {};
        }
        std::unique_lock<std::mutex> lk(m_);
        if (done_index_ != index) {
            if (running_ != index) {
                if (running_ >= 0 && running_is_prefetch_) abort_ = true;
                job_ = index;
                job_is_prefetch_ = false;
                cv_.notify_all();
            }
            cv_.wait(lk, [&] { return done_index_ == index; });
        }
        done_index_ = -1;
        if (!done_ok_) {
            if (error) *error = done_error_;
            return {};
        }
        return std::move(done_frame_);
    }

    void prefetch(int64_t index) override {
        if (index < 0 || index >= source_.frame_count) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_index_ == index || running_ == index || job_ == index) return;
        if (running_ >= 0 || job_ >= 0) return;  // busy: a hint never delays a demand
        job_ = index;
        job_is_prefetch_ = true;
        cv_.notify_all();
    }

  private:
    enum class Status { Ok, Failed, Aborted };

    void run();
    Status decode(int64_t index, Frame* out, std::string* error);
    bool seek_to(const KeyInfo& key, int64_t floor, std::string* error);
    int read_video_packet(AVPacket* pkt);
    int64_t display_index(int64_t pts) const;
    bool convert(const AVFrame* f, Frame* out, std::string* error);

    Source source_;

    // ffmpeg, owned by the worker once it runs.
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVPacket* pending_ = nullptr;  // first packet after a seek, already read
    bool has_pending_ = false;
    SwsContext* sws_ = nullptr;
    int sws_w_ = 0, sws_h_ = 0, sws_fmt_ = -1, sws_cs_ = -1, sws_range_ = -1;
    int stream_ = -1;

    // Index from the prepass.
    std::vector<int64_t> pts_;    // display order
    std::vector<KeyInfo> keys_;   // sorted by pts
    int64_t first_seek_ts_ = 0;   // order timestamp of the first packet of the stream

    // Decoder position, valid when positioned_. floor_ is the display index of the
    // keyframe decoding started from, cur_ the last frame produced since, or floor_ - 1
    // before the first. Any frame after cur_ and not before floor_ is reached by decoding
    // forward.
    bool positioned_ = false;
    bool eof_sent_ = false;
    int64_t floor_ = 0;
    int64_t cur_ = -1;

    // Worker hand-off.
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    int64_t job_ = -1;
    bool job_is_prefetch_ = false;
    int64_t running_ = -1;
    bool running_is_prefetch_ = false;
    std::atomic<bool> abort_{false};
    int64_t done_index_ = -1;
    bool done_ok_ = false;
    Frame done_frame_;
    std::string done_error_;
};

bool FfmpegDecoder::init(const std::string& path, std::string* error) {
    source_.path = path;
    auto fail = [&](const std::string& what) {
        if (error) *error = "Decoder::open " + path + ": " + what;
        return false;
    };

    int r = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
    if (r < 0) return fail("avformat_open_input: " + av_error(r));
    r = avformat_find_stream_info(fmt_, nullptr);
    if (r < 0) return fail("avformat_find_stream_info: " + av_error(r));

    const AVCodec* codec = nullptr;
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_ < 0) return fail("no video stream: " + av_error(stream_));
    if (codec == nullptr) return fail("no decoder for the video stream");
    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
        if (static_cast<int>(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;
    AVStream* st = fmt_->streams[stream_];

    ctx_ = avcodec_alloc_context3(codec);
    if (ctx_ == nullptr) return fail("avcodec_alloc_context3 failed");
    r = avcodec_parameters_to_context(ctx_, st->codecpar);
    if (r < 0) return fail("avcodec_parameters_to_context: " + av_error(r));
    ctx_->pkt_timebase = st->time_base;
    ctx_->thread_count = 0;
    ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    r = avcodec_open2(ctx_, codec, nullptr);
    if (r < 0) return fail("avcodec_open2: " + av_error(r));

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    pending_ = av_packet_alloc();
    if (!frame_ || !pkt_ || !pending_) return fail("out of memory");

    // Prepass: every packet of the stream, read, not decoded.
    bool first = true;
    for (;;) {
        r = read_video_packet(pkt_);
        if (r == AVERROR_EOF) break;
        if (r < 0) return fail("prepass av_read_frame: " + av_error(r));
        const int64_t pts = pkt_->pts;
        const int64_t dts = pkt_->dts;
        const bool key = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        const bool discard = (pkt_->flags & AV_PKT_FLAG_DISCARD) != 0;
        av_packet_unref(pkt_);
        if (pts == AV_NOPTS_VALUE)
            return fail("a video packet has no pts, frames cannot be indexed");
        if (first) {
            first_seek_ts_ = order_ts(pts, dts);
            first = false;
        }
        if (key) keys_.push_back({pts, order_ts(pts, dts)});
        if (!discard) pts_.push_back(pts);
    }
    if (pts_.empty()) return fail("no video frames");
    if (keys_.empty()) return fail("no keyframe in the video stream");
    std::sort(pts_.begin(), pts_.end());
    if (std::adjacent_find(pts_.begin(), pts_.end()) != pts_.end())
        return fail("two video packets share a pts, frames cannot be indexed");
    std::sort(keys_.begin(), keys_.end(),
              [](const KeyInfo& a, const KeyInfo& b) { return a.pts < b.pts; });

    source_.frame_count = static_cast<int64_t>(pts_.size());
    // Mean frame rate over the measured span. A single frame has no span, so its rate
    // cannot be measured and stays 0; an output at that rate needs an explicit --fps.
    if (pts_.size() >= 2) {
        const double span = static_cast<double>(pts_.back() - pts_.front()) *
                            av_q2d(st->time_base);
        if (span > 0.0) source_.fps = static_cast<double>(pts_.size() - 1) / span;
    }

    // Dimensions come from a decoded frame, not from the stream header.
    std::string err;
    Frame f0;
    if (decode(0, &f0, &err) != Status::Ok) return fail("decoding frame 0: " + err);
    source_.width = f0.width;
    source_.height = f0.height;

    // Frame 0 is the most likely first request: keep it as a finished prefetch.
    done_index_ = 0;
    done_ok_ = true;
    done_frame_ = std::move(f0);

    worker_ = std::thread([this] { run(); });
    return true;
}

void FfmpegDecoder::run() {
    for (;;) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return quit_ || job_ >= 0; });
        if (quit_) return;
        const int64_t index = job_;
        running_ = index;
        running_is_prefetch_ = job_is_prefetch_;
        job_ = -1;
        abort_ = false;
        lk.unlock();

        Frame f;
        std::string err;
        const Status s = decode(index, &f, &err);

        lk.lock();
        running_ = -1;
        if (s != Status::Aborted) {
            done_index_ = index;
            done_ok_ = s == Status::Ok;
            done_frame_ = std::move(f);
            done_error_ = std::move(err);
        }
        cv_.notify_all();
    }
}

int FfmpegDecoder::read_video_packet(AVPacket* pkt) {
    for (;;) {
        const int r = av_read_frame(fmt_, pkt);
        if (r < 0) return r;
        if (pkt->stream_index == stream_) return 0;
        av_packet_unref(pkt);
    }
}

int64_t FfmpegDecoder::display_index(int64_t pts) const {
    const auto it = std::lower_bound(pts_.begin(), pts_.end(), pts);
    if (it == pts_.end() || *it != pts) return -1;
    return static_cast<int64_t>(it - pts_.begin());
}

bool FfmpegDecoder::seek_to(const KeyInfo& key, int64_t floor, std::string* error) {
    avcodec_flush_buffers(ctx_);
    av_packet_unref(pending_);
    has_pending_ = false;
    eof_sent_ = false;
    positioned_ = false;
    cur_ = -1;

    // First the keyframe itself. If the demuxer lands past it, which some do when they
    // seek on pts rather than dts, fall back to the start of the stream: slower, exact.
    const int64_t targets[2] = {key.seek_ts, first_seek_ts_};
    for (const int64_t ts : targets) {
        if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) continue;
        for (;;) {
            const int r = read_video_packet(pending_);
            if (r < 0) break;
            if (!(pending_->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pending_);
                continue;
            }
            if (order_ts(pending_->pts, pending_->dts) <= key.seek_ts) {
                has_pending_ = true;
                positioned_ = true;
                floor_ = floor;
                cur_ = floor - 1;
                return true;
            }
            av_packet_unref(pending_);
            break;
        }
    }
    if (error)
        *error = "cannot seek to the keyframe at pts " + std::to_string(key.pts) + " in " +
                 source_.path;
    return false;
}

FfmpegDecoder::Status FfmpegDecoder::decode(int64_t index, Frame* out, std::string* error) {
    const int64_t target_pts = pts_[static_cast<size_t>(index)];

    // Last keyframe at or before the target in display order. None means the target
    // precedes every keyframe (an open GOP at the head of the file): decode from the start.
    auto k = std::upper_bound(keys_.begin(), keys_.end(), target_pts,
                              [](int64_t t, const KeyInfo& ki) { return t < ki.pts; });
    const KeyInfo& key = k == keys_.begin() ? keys_.front() : *(k - 1);
    const int64_t key_display = static_cast<int64_t>(
        std::lower_bound(pts_.begin(), pts_.end(), key.pts) - pts_.begin());

    const bool forward = positioned_ && index > cur_ && index >= floor_ &&
                         key_display - cur_ <= kForwardSlack;
    if (!forward && !seek_to(key, std::min(key_display, index), error)) return Status::Failed;

    for (;;) {
        if (abort_) return Status::Aborted;
        int r = avcodec_receive_frame(ctx_, frame_);
        if (r == 0) {
            const int64_t ts = frame_->pts != AV_NOPTS_VALUE ? frame_->pts
                                                            : frame_->best_effort_timestamp;
            const int64_t d = display_index(ts);
            if (d < 0) {
                av_frame_unref(frame_);
                if (error)
                    *error = "decoded a frame at pts " + std::to_string(ts) +
                             " that the prepass did not index, in " + source_.path;
                positioned_ = false;
                return Status::Failed;
            }
            cur_ = std::max(cur_, d);
            if (d == index) {
                const bool ok = convert(frame_, out, error);
                av_frame_unref(frame_);
                return ok ? Status::Ok : Status::Failed;
            }
            av_frame_unref(frame_);
            if (d > index) {
                if (error)
                    *error = "the decoder never produced frame " + std::to_string(index) +
                             " of " + source_.path;
                return Status::Failed;
            }
            continue;
        }
        if (r == AVERROR_EOF) {
            positioned_ = false;
            if (error)
                *error = "end of stream before frame " + std::to_string(index) + " of " +
                         source_.path;
            return Status::Failed;
        }
        if (r != AVERROR(EAGAIN)) {
            positioned_ = false;
            if (error) *error = "avcodec_receive_frame: " + av_error(r);
            return Status::Failed;
        }

        // The decoder wants input.
        if (eof_sent_) {
            positioned_ = false;
            if (error) *error = "decoder stalled after end of stream in " + source_.path;
            return Status::Failed;
        }
        AVPacket* p = pkt_;
        if (has_pending_) {
            p = pending_;
            has_pending_ = false;
        } else {
            r = read_video_packet(pkt_);
            if (r == AVERROR_EOF) {
                avcodec_send_packet(ctx_, nullptr);
                eof_sent_ = true;
                continue;
            }
            if (r < 0) {
                positioned_ = false;
                if (error) *error = "av_read_frame: " + av_error(r);
                return Status::Failed;
            }
        }
        r = avcodec_send_packet(ctx_, p);
        av_packet_unref(p);
        if (r < 0 && r != AVERROR_INVALIDDATA) {
            positioned_ = false;
            if (error) *error = "avcodec_send_packet: " + av_error(r);
            return Status::Failed;
        }
    }
}

bool FfmpegDecoder::convert(const AVFrame* f, Frame* out, std::string* error) {
    AVPixelFormat fmt = static_cast<AVPixelFormat>(f->format);
    // The yuvj formats are their yuv counterparts at full range; swscale wants the latter.
    bool full = f->color_range == AVCOL_RANGE_JPEG;
    switch (fmt) {
        case AV_PIX_FMT_YUVJ420P: fmt = AV_PIX_FMT_YUV420P; full = true; break;
        case AV_PIX_FMT_YUVJ422P: fmt = AV_PIX_FMT_YUV422P; full = true; break;
        case AV_PIX_FMT_YUVJ444P: fmt = AV_PIX_FMT_YUV444P; full = true; break;
        case AV_PIX_FMT_YUVJ440P: fmt = AV_PIX_FMT_YUV440P; full = true; break;
        case AV_PIX_FMT_YUVJ411P: fmt = AV_PIX_FMT_YUV411P; full = true; break;
        default: break;
    }
    int cs = f->colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RESERVED)
        cs = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);

    if (sws_ == nullptr || sws_w_ != f->width || sws_h_ != f->height || sws_fmt_ != fmt ||
        sws_cs_ != cs || sws_range_ != static_cast<int>(full)) {
        sws_freeContext(sws_);
        sws_ = sws_getContext(f->width, f->height, fmt, f->width, f->height,
                              AV_PIX_FMT_RGBA, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr,
                              nullptr, nullptr);
        if (sws_ == nullptr) {
            if (error)
                *error = std::string("cannot convert ") +
                         (desc ? desc->name : "unknown pixel format") + " to RGBA";
            return false;
        }
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB)) {
            const int* coeffs = sws_getCoefficients(cs);
            sws_setColorspaceDetails(sws_, coeffs, full ? 1 : 0,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                     1 << 16);
        }
        sws_w_ = f->width;
        sws_h_ = f->height;
        sws_fmt_ = fmt;
        sws_cs_ = cs;
        sws_range_ = full ? 1 : 0;
    }

    out->width = f->width;
    out->height = f->height;
    out->stride = f->width * 4;
    out->pixels.assign(static_cast<size_t>(out->stride) * f->height, 0);
    uint8_t* dst[4] = {out->pixels.data(), nullptr, nullptr, nullptr};
    const int dst_stride[4] = {out->stride, 0, 0, 0};
    const int h = sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dst_stride);
    if (h != f->height) {
        if (error) *error = "sws_scale to RGBA failed";
        *out = Frame{};
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<Decoder> Decoder::open(const std::string& path, std::string* error) {
    auto d = std::make_unique<FfmpegDecoder>();
    if (!d->init(path, error)) return nullptr;
    return d;
}

}  // namespace vrc

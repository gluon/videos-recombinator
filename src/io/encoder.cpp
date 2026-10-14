#include "io/encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// One movie file, one video stream. Frames arrive as RGBA8 at any size. write() brings
// them to the output size under the scaling policy, in RGBA, then converts once to the
// codec's pixel format. Output frames are numbered 0, 1, 2... in a 1/fps time base, so
// the file holds exactly one frame per write().
//
// Codecs, as in the original's menu:
//   prores422   prores_ks, profile 2 (standard), yuv422p10
//   prores4444  prores_ks, profile 4 (4444),     yuva444p10, alpha kept
//   h264        libx264,                         yuv420p, even dimensions only
//   mjpeg       mjpeg,                           yuv420p full range, BT.601
// YUV output is BT.709 and tagged as such, except mjpeg: JPEG decoders, libavcodec's
// included, read every JPEG as BT.601 full range whatever the container says.

namespace vrc {
namespace {

std::string av_error(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

struct CodecChoice {
    const char* encoder;
    AVPixelFormat pix_fmt;
    int prores_profile;  // -1 when not ProRes
    bool full_range;
    bool even_only;
    AVColorSpace matrix;
};

bool choose_codec(const std::string& name, CodecChoice* c) {
    if (name == "prores422") {
        *c = {"prores_ks", AV_PIX_FMT_YUV422P10, 2, false, false, AVCOL_SPC_BT709};
    } else if (name == "prores4444") {
        *c = {"prores_ks", AV_PIX_FMT_YUVA444P10, 4, false, false, AVCOL_SPC_BT709};
    } else if (name == "h264") {
        *c = {"libx264", AV_PIX_FMT_YUV420P, -1, false, true, AVCOL_SPC_BT709};
    } else if (name == "mjpeg") {
        *c = {"mjpeg", AV_PIX_FMT_YUV420P, -1, true, false, AVCOL_SPC_BT470BG};
    } else {
        return false;
    }
    return true;
}

// Opaque black, matching FrameCache::black(). Padding with zeroed RGBA would be
// transparent in a format that carries alpha, prores4444 among them.
void fill_opaque_black(std::vector<uint8_t>* buf) {
    std::fill(buf->begin(), buf->end(), 0);
    for (size_t i = 3; i < buf->size(); i += 4) (*buf)[i] = 255;
}

// Copies `rows` rows of `row_bytes` between two RGBA buffers of different strides.
void copy_rows(const uint8_t* src, int src_stride, uint8_t* dst, int dst_stride,
               int row_bytes, int rows) {
    for (int y = 0; y < rows; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                    src + static_cast<size_t>(y) * src_stride, static_cast<size_t>(row_bytes));
}

class FfmpegEncoder final : public Encoder {
  public:
    ~FfmpegEncoder() override {
        // finish() reports failures; here only the resources go. A file left without its
        // trailer by an aborted render is not playable.
        if (oc_ && opened_file_ && !(oc_->oformat->flags & AVFMT_NOFILE))
            avio_closep(&oc_->pb);
        sws_freeContext(to_out_);
        sws_freeContext(resize_);
        av_frame_free(&yuv_);
        av_packet_free(&pkt_);
        avcodec_free_context(&cc_);
        avformat_free_context(oc_);
    }

    bool init(const std::string& path, const OutputParams& out, std::string* error);
    bool write(const Frame& frame, std::string* error) override;
    bool finish(std::string* error) override;

  private:
    bool resize_into(const uint8_t* src, int src_stride, int sw, int sh, uint8_t* dst,
                     int dst_stride, int dw, int dh, std::string* error);
    bool send(AVFrame* f, std::string* error);

    OutputParams out_;
    std::string path_;
    AVFormatContext* oc_ = nullptr;
    AVCodecContext* cc_ = nullptr;
    AVStream* st_ = nullptr;
    AVFrame* yuv_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* to_out_ = nullptr;   // RGBA at output size to the codec pixel format
    SwsContext* resize_ = nullptr;   // RGBA to RGBA, any size to any size
    std::vector<uint8_t> canvas_;    // RGBA at output size
    std::vector<uint8_t> fitted_;    // RGBA at the fitted size, before padding
    int64_t next_pts_ = 0;
    bool opened_file_ = false;
    bool finished_ = false;
};

bool FfmpegEncoder::init(const std::string& path, const OutputParams& out,
                         std::string* error) {
    out_ = out;
    path_ = path;
    auto fail = [&](const std::string& what) {
        if (error) *error = "Encoder::create " + path + ": " + what;
        return false;
    };

    CodecChoice choice;
    if (!choose_codec(out.codec, &choice))
        return fail("unknown codec '" + out.codec +
                    "', expected prores422 | prores4444 | h264 | mjpeg");
    if (out.width <= 0 || out.height <= 0)
        return fail("output size " + std::to_string(out.width) + "x" +
                    std::to_string(out.height) + " is not valid");
    if (!(out.fps > 0.0) || !std::isfinite(out.fps))
        return fail("output frame rate " + std::to_string(out.fps) +
                    " is not valid, pass --fps");
    if (choice.even_only && (out.width % 2 != 0 || out.height % 2 != 0))
        return fail(out.codec + " needs even dimensions, got " + std::to_string(out.width) +
                    "x" + std::to_string(out.height));

    const AVCodec* codec = avcodec_find_encoder_by_name(choice.encoder);
    if (codec == nullptr)
        return fail(std::string("encoder ") + choice.encoder +
                    " is not in this ffmpeg build");

    int r = avformat_alloc_output_context2(&oc_, nullptr, nullptr, path.c_str());
    if (oc_ == nullptr) r = avformat_alloc_output_context2(&oc_, nullptr, "mov", path.c_str());
    if (oc_ == nullptr) return fail("avformat_alloc_output_context2: " + av_error(r));

    const AVRational rate = av_d2q(out.fps, 100000);
    cc_ = avcodec_alloc_context3(codec);
    if (cc_ == nullptr) return fail("avcodec_alloc_context3 failed");
    cc_->width = out.width;
    cc_->height = out.height;
    cc_->pix_fmt = choice.pix_fmt;
    cc_->time_base = av_inv_q(rate);
    cc_->framerate = rate;
    cc_->sample_aspect_ratio = AVRational{1, 1};
    cc_->color_primaries = AVCOL_PRI_BT709;
    cc_->color_trc = AVCOL_TRC_BT709;
    cc_->colorspace = choice.matrix;
    cc_->color_range = choice.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    cc_->thread_count = 0;
    if (oc_->oformat->flags & AVFMT_GLOBALHEADER) cc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (choice.prores_profile >= 0) {
        r = av_opt_set_int(cc_->priv_data, "profile", choice.prores_profile, 0);
        if (r < 0) return fail("setting the ProRes profile: " + av_error(r));
    }
    r = avcodec_open2(cc_, codec, nullptr);
    if (r < 0) return fail(std::string("avcodec_open2 ") + choice.encoder + ": " + av_error(r));

    st_ = avformat_new_stream(oc_, nullptr);
    if (st_ == nullptr) return fail("avformat_new_stream failed");
    r = avcodec_parameters_from_context(st_->codecpar, cc_);
    if (r < 0) return fail("avcodec_parameters_from_context: " + av_error(r));
    st_->time_base = cc_->time_base;
    st_->avg_frame_rate = rate;

    if (!(oc_->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&oc_->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) return fail("avio_open: " + av_error(r));
        opened_file_ = true;
    }
    r = avformat_write_header(oc_, nullptr);
    if (r < 0) return fail("avformat_write_header: " + av_error(r));

    yuv_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!yuv_ || !pkt_) return fail("out of memory");
    yuv_->format = choice.pix_fmt;
    yuv_->width = out.width;
    yuv_->height = out.height;
    yuv_->color_primaries = cc_->color_primaries;
    yuv_->color_trc = cc_->color_trc;
    yuv_->colorspace = cc_->colorspace;
    yuv_->color_range = cc_->color_range;
    r = av_frame_get_buffer(yuv_, 0);
    if (r < 0) return fail("av_frame_get_buffer: " + av_error(r));

    to_out_ = sws_getContext(out.width, out.height, AV_PIX_FMT_RGBA, out.width, out.height,
                             choice.pix_fmt, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr,
                             nullptr, nullptr);
    if (to_out_ == nullptr) return fail("cannot convert RGBA to the codec pixel format");
    const int* matrix = sws_getCoefficients(choice.matrix);
    sws_setColorspaceDetails(to_out_, matrix, 1, matrix, choice.full_range ? 1 : 0, 0,
                             1 << 16, 1 << 16);

    canvas_.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    fill_opaque_black(&canvas_);
    return true;
}

bool FfmpegEncoder::resize_into(const uint8_t* src, int src_stride, int sw, int sh,
                                uint8_t* dst, int dst_stride, int dw, int dh,
                                std::string* error) {
    resize_ = sws_getCachedContext(resize_, sw, sh, AV_PIX_FMT_RGBA, dw, dh, AV_PIX_FMT_RGBA,
                                   SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr,
                                   nullptr);
    if (resize_ == nullptr) {
        if (error) *error = "Encoder::write: cannot build the resampler";
        return false;
    }
    const uint8_t* s[4] = {src, nullptr, nullptr, nullptr};
    const int ss[4] = {src_stride, 0, 0, 0};
    uint8_t* d[4] = {dst, nullptr, nullptr, nullptr};
    const int ds[4] = {dst_stride, 0, 0, 0};
    if (sws_scale(resize_, s, ss, 0, sh, d, ds) != dh) {
        if (error) *error = "Encoder::write: resampling failed";
        return false;
    }
    return true;
}

bool FfmpegEncoder::write(const Frame& frame, std::string* error) {
    if (finished_) {
        if (error) *error = "Encoder::write after finish";
        return false;
    }
    if (!frame.valid() || frame.stride < frame.width * 4 ||
        frame.pixels.size() < static_cast<size_t>(frame.stride) * frame.height) {
        if (error) *error = "Encoder::write: invalid frame";
        return false;
    }

    const int W = out_.width, H = out_.height;
    const int w = frame.width, h = frame.height;
    const uint8_t* rgba = frame.pixels.data();
    int rgba_stride = frame.stride;

    if (w != W || h != H) {
        uint8_t* canvas = canvas_.data();
        const int cs = W * 4;
        switch (out_.scaling) {
            case Scaling::Stretch:
                if (!resize_into(rgba, frame.stride, w, h, canvas, cs, W, H, error))
                    return false;
                break;
            case Scaling::Fill: {
                // Crop the source to the output aspect ratio, centred, then resample.
                int cw = w, ch = h;
                if (static_cast<int64_t>(w) * H > static_cast<int64_t>(W) * h)
                    cw = std::max(1, static_cast<int>(std::lround(double(h) * W / H)));
                else
                    ch = std::max(1, static_cast<int>(std::lround(double(w) * H / W)));
                const uint8_t* crop = rgba + static_cast<size_t>((h - ch) / 2) * frame.stride +
                                      static_cast<size_t>((w - cw) / 2) * 4;
                if (cw == W && ch == H)
                    copy_rows(crop, frame.stride, canvas, cs, W * 4, H);
                else if (!resize_into(crop, frame.stride, cw, ch, canvas, cs, W, H, error))
                    return false;
                break;
            }
            case Scaling::Fit: {
                // Resample inside the output, aspect ratio kept, centred, padded with the
                // same opaque black a black slot uses.
                int fw = W, fh = H;
                if (static_cast<int64_t>(w) * H > static_cast<int64_t>(W) * h)
                    fh = std::max(1, static_cast<int>(std::lround(double(W) * h / w)));
                else
                    fw = std::max(1, static_cast<int>(std::lround(double(H) * w / h)));
                const uint8_t* fitted = rgba;
                int fitted_stride = frame.stride;
                if (fw != w || fh != h) {
                    fitted_.resize(static_cast<size_t>(fw) * fh * 4);
                    if (!resize_into(rgba, frame.stride, w, h, fitted_.data(), fw * 4, fw, fh,
                                     error))
                        return false;
                    fitted = fitted_.data();
                    fitted_stride = fw * 4;
                }
                fill_opaque_black(&canvas_);
                copy_rows(fitted, fitted_stride,
                          canvas + static_cast<size_t>((H - fh) / 2) * cs +
                              static_cast<size_t>((W - fw) / 2) * 4,
                          cs, fw * 4, fh);
                break;
            }
        }
        rgba = canvas;
        rgba_stride = cs;
    }

    int r = av_frame_make_writable(yuv_);
    if (r < 0) {
        if (error) *error = "av_frame_make_writable: " + av_error(r);
        return false;
    }
    const uint8_t* s[4] = {rgba, nullptr, nullptr, nullptr};
    const int ss[4] = {rgba_stride, 0, 0, 0};
    if (sws_scale(to_out_, s, ss, 0, H, yuv_->data, yuv_->linesize) != H) {
        if (error) *error = "Encoder::write: RGBA to codec pixel format failed";
        return false;
    }
    yuv_->pts = next_pts_++;
    return send(yuv_, error);
}

bool FfmpegEncoder::send(AVFrame* f, std::string* error) {
    int r = avcodec_send_frame(cc_, f);
    if (r < 0) {
        if (error) *error = "avcodec_send_frame: " + av_error(r);
        return false;
    }
    for (;;) {
        r = avcodec_receive_packet(cc_, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) {
            if (error) *error = "avcodec_receive_packet: " + av_error(r);
            return false;
        }
        av_packet_rescale_ts(pkt_, cc_->time_base, st_->time_base);
        pkt_->stream_index = st_->index;
        r = av_interleaved_write_frame(oc_, pkt_);
        if (r < 0) {
            if (error) *error = "av_interleaved_write_frame: " + av_error(r);
            return false;
        }
    }
}

bool FfmpegEncoder::finish(std::string* error) {
    if (finished_) {
        if (error) *error = "Encoder::finish called twice";
        return false;
    }
    finished_ = true;
    if (!send(nullptr, error)) return false;
    int r = av_write_trailer(oc_);
    if (r < 0) {
        if (error) *error = "av_write_trailer: " + av_error(r);
        return false;
    }
    if (opened_file_) {
        r = avio_closep(&oc_->pb);
        opened_file_ = false;
        if (r < 0) {
            if (error) *error = "closing " + path_ + ": " + av_error(r);
            return false;
        }
    }
    return true;
}

}  // namespace

std::unique_ptr<Encoder> Encoder::create(const std::string& path, const OutputParams& out,
                                         std::string* error) {
    auto e = std::make_unique<FfmpegEncoder>();
    if (!e->init(path, out, error)) return nullptr;
    return e;
}

std::string next_output_path(const std::string& dir, std::string* error) {
    namespace fs = std::filesystem;
    if (dir.empty()) {
        if (error) *error = "next_output_path: no output directory given (--out DIR)";
        return {};
    }
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec)) {
        if (error) *error = "next_output_path: " + dir + " is not a directory";
        return {};
    }
    for (int i = 0; i <= 999; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "vid%03d.mov", i);
        const fs::path p = fs::path(dir) / name;
        if (!fs::exists(fs::symlink_status(p, ec))) {
            if (ec && ec != std::errc::no_such_file_or_directory) {
                if (error) *error = "next_output_path: " + p.string() + ": " + ec.message();
                return {};
            }
            return p.string();
        }
    }
    if (error) *error = "next_output_path: vid000.mov to vid999.mov all exist in " + dir;
    return {};
}

}  // namespace vrc

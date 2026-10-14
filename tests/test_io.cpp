// Integration tests for src/io. They need real movie files, which tests/test_render.sh
// generates with ffmpeg before calling each mode:
//
//   test_io decode  <movie> <frames> <width> <height> <fps>
//   test_io cache   <movie>
//   test_io scaling <movie-dir>
//   test_io outpath <empty-dir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "io/decoder.h"
#include "io/encoder.h"
#include "io/frame_cache.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                 \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                 \
            return 1;                                                            \
        }                                                                        \
    } while (0)

std::unique_ptr<vrc::Decoder> open_or_die(const std::string& path) {
    std::string err;
    auto d = vrc::Decoder::open(path, &err);
    if (!d) std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), err.c_str());
    return d;
}

bool same(const vrc::Frame& a, const vrc::Frame& b) {
    return a.width == b.width && a.height == b.height && a.stride == b.stride &&
           a.pixels == b.pixels;
}

// Every frame decoded in order is the reference. Then a fresh decoder is driven in a
// shuffled order, backwards, and with prefetch hints that are mostly wrong, and every
// frame must match the reference byte for byte. Consecutive reference frames must
// differ, so an off-by-one in the seek cannot pass.
int test_decode(const std::string& path, int64_t frames, int width, int height,
                double fps) {
    auto seq = open_or_die(path);
    REQUIRE(seq);
    const vrc::Source& s = seq->source();
    std::printf("decode %s: %lld frames %dx%d %.6f fps\n", path.c_str(),
                static_cast<long long>(s.frame_count), s.width, s.height, s.fps);
    CHECK(s.path == path);
    CHECK(s.frame_count == frames);
    CHECK(s.width == width);
    CHECK(s.height == height);
    CHECK(std::fabs(s.fps - fps) < 1e-6);
    REQUIRE(s.frame_count == frames);

    std::vector<vrc::Frame> ref;
    std::string err;
    for (int64_t i = 0; i < frames; ++i) {
        ref.push_back(seq->frame_at(i, &err));
        if (!ref.back().valid()) std::fprintf(stderr, "frame %lld: %s\n", (long long)i, err.c_str());
        REQUIRE(ref.back().valid());
        CHECK(ref.back().width == width && ref.back().height == height);
        CHECK(ref.back().stride == width * 4);
        CHECK(ref.back().pixels.size() == static_cast<size_t>(width) * height * 4);
    }
    for (int64_t i = 0; i + 1 < frames; ++i) CHECK(ref[i].pixels != ref[i + 1].pixels);

    auto rnd = open_or_die(path);
    REQUIRE(rnd);
    std::vector<int64_t> order;
    for (int64_t i = 0; i < frames; ++i) order.push_back(i);
    std::mt19937_64 rng(1234);
    std::shuffle(order.begin(), order.end(), rng);
    for (int64_t i = frames - 1; i >= 0; --i) order.push_back(i);
    for (int64_t i = 0; i < frames; i += 7) order.push_back(i);
    std::uniform_int_distribution<int64_t> any(0, frames - 1);
    int mismatches = 0;
    for (const int64_t i : order) {
        const vrc::Frame f = rnd->frame_at(i, &err);
        if (!f.valid()) std::fprintf(stderr, "random frame %lld: %s\n", (long long)i, err.c_str());
        if (!same(f, ref[static_cast<size_t>(i)])) ++mismatches;
        rnd->prefetch(any(rng));
    }
    // A demand that interrupts a prefetch, possibly between its seek and its first frame,
    // must not leave the decoder believing it stands where it does not.
    for (int rep = 0; rep < 400; ++rep) {
        rnd->prefetch(any(rng));
        const int64_t i = any(rng);
        if (!same(rnd->frame_at(i, &err), ref[static_cast<size_t>(i)])) ++mismatches;
    }
    if (mismatches) std::fprintf(stderr, "%d random-access frames differ\n", mismatches);
    CHECK(mismatches == 0);

    err.clear();
    CHECK(!rnd->frame_at(frames, &err).valid());
    CHECK(!err.empty());
    err.clear();
    CHECK(!rnd->frame_at(-1, &err).valid());
    CHECK(!err.empty());
    return 0;
}

int test_cache(const std::string& path) {
    auto dec = open_or_die(path);
    auto ref = open_or_die(path);
    REQUIRE(dec && ref);
    const vrc::Source& s = dec->source();
    REQUIRE(s.frame_count >= 10);
    const size_t frame_bytes = static_cast<size_t>(s.width) * s.height * 4;
    std::string err;

    vrc::FrameCache cache(frame_bytes * 3);
    cache.attach(0, dec.get());
    for (int64_t i = 0; i < 10; ++i) {
        const vrc::Frame* f = cache.get(0, i, &err);
        REQUIRE(f != nullptr);
        CHECK(same(*f, ref->frame_at(i, &err)));
    }
    auto st = cache.stats();
    CHECK(st.misses == 10);
    CHECK(st.hits == 0);
    CHECK(st.evictions == 7);
    CHECK(st.bytes == frame_bytes * 3);

    CHECK(cache.get(0, 9, &err) != nullptr);  // most recent, still there
    CHECK(cache.get(0, 7, &err) != nullptr);  // oldest kept
    st = cache.stats();
    CHECK(st.hits == 2);
    CHECK(st.misses == 10);

    const vrc::Frame* f0 = cache.get(0, 0, &err);  // evicted long ago
    REQUIRE(f0 != nullptr);
    CHECK(same(*f0, ref->frame_at(0, &err)));
    st = cache.stats();
    CHECK(st.misses == 11);
    CHECK(st.evictions == 8);
    CHECK(st.bytes <= frame_bytes * 3);

    // Least recently used went: 7 and 9 were touched after 8, so 8 was evicted, not them.
    CHECK(cache.get(0, 7, &err) != nullptr);
    CHECK(cache.get(0, 9, &err) != nullptr);
    st = cache.stats();
    CHECK(st.hits == 4);
    CHECK(st.misses == 11);

    // A budget smaller than one frame still holds the frame it just returned.
    vrc::FrameCache tiny(1);
    tiny.attach(0, dec.get());
    const vrc::Frame* t = tiny.get(0, 3, &err);
    REQUIRE(t != nullptr);
    CHECK(same(*t, ref->frame_at(3, &err)));
    CHECK(tiny.stats().bytes == frame_bytes);
    CHECK(tiny.get(0, 4, &err) != nullptr);
    CHECK(tiny.stats().evictions == 1);
    CHECK(tiny.stats().bytes == frame_bytes);

    err.clear();
    CHECK(cache.get(5, 0, &err) == nullptr);
    CHECK(!err.empty());
    err.clear();
    CHECK(cache.get(0, s.frame_count, &err) == nullptr);
    CHECK(!err.empty());

    // Black slots are opaque black, so that a format carrying alpha does not turn them
    // into holes: RGB at zero, alpha at 255.
    const vrc::Frame& b = cache.black(16, 8);
    CHECK(b.width == 16 && b.height == 8 && b.stride == 64);
    CHECK(b.pixels.size() == 16 * 8 * 4);
    bool opaque_black = true;
    for (size_t i = 0; i < b.pixels.size(); ++i)
        opaque_black = opaque_black && b.pixels[i] == (i % 4 == 3 ? 255 : 0);
    CHECK(opaque_black);
    return 0;
}

struct Rgb {
    int r, g, b;
};

// 100x100 RGBA, four horizontal bands of 25 rows: yellow, red, blue, cyan.
vrc::Frame banded() {
    const Rgb bands[4] = {{255, 255, 0}, {255, 0, 0}, {0, 0, 255}, {0, 255, 255}};
    vrc::Frame f;
    f.width = 100;
    f.height = 100;
    f.stride = 400;
    f.pixels.resize(400 * 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) {
            uint8_t* p = &f.pixels[static_cast<size_t>(y) * 400 + x * 4];
            p[0] = static_cast<uint8_t>(bands[y / 25].r);
            p[1] = static_cast<uint8_t>(bands[y / 25].g);
            p[2] = static_cast<uint8_t>(bands[y / 25].b);
            p[3] = 255;
        }
    return f;
}

bool near(const vrc::Frame& f, int x, int y, Rgb c) {
    const uint8_t* p = &f.pixels[static_cast<size_t>(y) * f.stride + x * 4];
    const bool ok = std::abs(p[0] - c.r) <= 16 && std::abs(p[1] - c.g) <= 16 &&
                    std::abs(p[2] - c.b) <= 16;
    if (!ok)
        std::fprintf(stderr, "pixel (%d,%d) is %d,%d,%d, expected %d,%d,%d\n", x, y, p[0],
                     p[1], p[2], c.r, c.g, c.b);
    return ok;
}

// Encodes three frames under one policy into a 200x100 output, reads the file back with
// the decoder, and returns its first frame.
vrc::Frame encode_and_read(const std::string& path, const std::string& codec,
                           vrc::Scaling scaling, const vrc::Frame& in) {
    vrc::OutputParams out;
    out.width = 200;
    out.height = 100;
    out.fps = 25.0;
    out.codec = codec;
    out.scaling = scaling;
    std::string err;
    auto enc = vrc::Encoder::create(path, out, &err);
    if (!enc) {
        std::fprintf(stderr, "Encoder::create %s: %s\n", path.c_str(), err.c_str());
        return {};
    }
    for (int i = 0; i < 3; ++i)
        if (!enc->write(in, &err)) {
            std::fprintf(stderr, "write: %s\n", err.c_str());
            return {};
        }
    if (!enc->finish(&err)) {
        std::fprintf(stderr, "finish: %s\n", err.c_str());
        return {};
    }
    auto dec = open_or_die(path);
    if (!dec) return {};
    if (dec->source().frame_count != 3 || dec->source().width != 200 ||
        dec->source().height != 100 || std::fabs(dec->source().fps - 25.0) > 1e-6) {
        std::fprintf(stderr, "%s: read back %lld frames %dx%d %.3f fps\n", path.c_str(),
                     (long long)dec->source().frame_count, dec->source().width,
                     dec->source().height, dec->source().fps);
        return {};
    }
    return dec->frame_at(0, &err);
}

int test_scaling(const std::string& dir) {
    const Rgb yellow{255, 255, 0}, red{255, 0, 0}, blue{0, 0, 255}, cyan{0, 255, 255},
        black{0, 0, 0};
    const vrc::Frame in = banded();

    for (const std::string codec : {"prores422", "prores4444", "h264", "mjpeg"}) {
        // Stretch: the whole source, squashed horizontally into 200x100.
        vrc::Frame f = encode_and_read(dir + "/stretch_" + codec + ".mov", codec,
                                       vrc::Scaling::Stretch, in);
        REQUIRE(f.valid());
        CHECK(near(f, 10, 5, yellow));
        CHECK(near(f, 100, 37, red));
        CHECK(near(f, 190, 62, blue));
        CHECK(near(f, 100, 95, cyan));

        // Fill: rows 25 to 74 of the source, scaled to cover the output.
        f = encode_and_read(dir + "/fill_" + codec + ".mov", codec, vrc::Scaling::Fill, in);
        REQUIRE(f.valid());
        CHECK(near(f, 10, 5, red));
        CHECK(near(f, 100, 40, red));
        CHECK(near(f, 100, 60, blue));
        CHECK(near(f, 190, 95, blue));

        // Fit: the source at 100x100, centred, black pillars of 50 on each side.
        f = encode_and_read(dir + "/fit_" + codec + ".mov", codec, vrc::Scaling::Fit, in);
        REQUIRE(f.valid());
        CHECK(near(f, 10, 50, black));
        CHECK(near(f, 190, 50, black));
        CHECK(near(f, 100, 5, yellow));
        CHECK(near(f, 100, 37, red));
        CHECK(near(f, 100, 62, blue));
        CHECK(near(f, 100, 95, cyan));

        // The Fit pillars must be opaque. In prores4444, which carries alpha, a zero
        // alpha there would make them holes rather than black bars.
        if (codec == "prores4444") {
            const uint8_t pillar_alpha = f.pixels[static_cast<size_t>(50) * f.stride + 10 * 4 + 3];
            if (pillar_alpha != 255)
                std::fprintf(stderr, "prores4444 Fit pillar alpha is %d, expected 255\n",
                             pillar_alpha);
            CHECK(pillar_alpha == 255);
        }
    }

    // Refusals are loud.
    vrc::OutputParams bad;
    bad.width = 201;
    bad.height = 100;
    bad.fps = 25.0;
    bad.codec = "h264";
    std::string err;
    CHECK(!vrc::Encoder::create(dir + "/odd.mov", bad, &err));
    CHECK(!err.empty());
    bad.width = 200;
    bad.codec = "theora";
    err.clear();
    CHECK(!vrc::Encoder::create(dir + "/unknown.mov", bad, &err));
    CHECK(!err.empty());
    bad.codec = "mjpeg";
    bad.fps = 0.0;
    err.clear();
    CHECK(!vrc::Encoder::create(dir + "/nofps.mov", bad, &err));
    CHECK(!err.empty());
    return 0;
}

int test_outpath(const std::string& dir) {
    namespace fs = std::filesystem;
    std::string err;
    CHECK(vrc::next_output_path("", &err).empty());
    CHECK(!err.empty());
    err.clear();
    CHECK(vrc::next_output_path(dir + "/does-not-exist", &err).empty());
    CHECK(!err.empty());

    const std::string first = vrc::next_output_path(dir, &err);
    CHECK(fs::path(first) == fs::path(dir) / "vid000.mov");
    std::ofstream(fs::path(dir) / "vid000.mov").put('x');
    std::ofstream(fs::path(dir) / "vid002.mov").put('x');
    CHECK(fs::path(vrc::next_output_path(dir, &err)) == fs::path(dir) / "vid001.mov");
    std::ofstream(fs::path(dir) / "vid001.mov").put('x');
    CHECK(fs::path(vrc::next_output_path(dir, &err)) == fs::path(dir) / "vid003.mov");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    int r = 1;
    if (mode == "decode" && argc == 7)
        r = test_decode(argv[2], std::atoll(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]),
                        std::atof(argv[6]));
    else if (mode == "cache" && argc == 3)
        r = test_cache(argv[2]);
    else if (mode == "scaling" && argc == 3)
        r = test_scaling(argv[2]);
    else if (mode == "outpath" && argc == 3)
        r = test_outpath(argv[2]);
    else
        std::fprintf(stderr, "usage: test_io decode|cache|scaling|outpath ...\n");
    if (r == 0 && g_failures == 0) {
        std::printf("test_io %s: ok\n", mode.c_str());
        return 0;
    }
    std::fprintf(stderr, "test_io %s: %d failure(s)\n", mode.c_str(), g_failures + r);
    return 1;
}

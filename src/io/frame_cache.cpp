#include "io/frame_cache.h"

// NOT IMPLEMENTED. Contract is in frame_cache.h, approach is in docs/SPEC.md section 7.

namespace vrc {

struct FrameCache::Impl {
    size_t budget_bytes = 0;
    Frame black_frame;
    Stats stats;
};

FrameCache::FrameCache(size_t budget_bytes) : impl_(new Impl) {
    impl_->budget_bytes = budget_bytes;
}

FrameCache::~FrameCache() { delete impl_; }

void FrameCache::attach(int source_index, Decoder* decoder) {
    (void)source_index;
    (void)decoder;
}

const Frame* FrameCache::get(int source_index, int64_t frame, std::string* error) {
    (void)source_index;
    (void)frame;
    if (error) *error = "FrameCache::get is not implemented yet";
    return nullptr;
}

const Frame& FrameCache::black(int width, int height) {
    Frame& f = impl_->black_frame;
    if (f.width != width || f.height != height) {
        f.width = width;
        f.height = height;
        f.stride = width * 4;
        f.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    }
    return f;
}

FrameCache::Stats FrameCache::stats() const { return impl_->stats; }

}  // namespace vrc

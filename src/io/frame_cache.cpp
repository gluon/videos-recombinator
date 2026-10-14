#include "io/frame_cache.h"

#include <functional>
#include <list>
#include <unordered_map>

// LRU over decoded frames, keyed by (source, frame), bounded by the bytes of pixel data
// it holds. The most recently returned frame is never evicted, so a pointer from get()
// stays valid until the next call to get(), even when one frame alone exceeds the budget.
// After each get() the decoder is asked to prefetch the next frame of the same source,
// which is what a plan walking a segment will ask for next.

namespace vrc {

namespace {

struct Key {
    int source;
    int64_t frame;
    bool operator==(const Key& o) const { return source == o.source && frame == o.frame; }
};

struct KeyHash {
    size_t operator()(const Key& k) const {
        return std::hash<int64_t>()(k.frame) ^
               (std::hash<int>()(k.source) * size_t{0x9e3779b97f4a7c15ULL});
    }
};

struct Entry {
    Key key;
    Frame frame;
};

size_t frame_bytes(const Frame& f) { return f.pixels.size(); }

}  // namespace

struct FrameCache::Impl {
    size_t budget_bytes = 0;
    Frame black_frame;
    Stats stats;
    std::unordered_map<int, Decoder*> decoders;
    std::list<Entry> lru;  // front is the most recent
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;

    void prefetch_next(int source_index, Decoder* d, int64_t frame) {
        const int64_t next = frame + 1;
        if (next >= d->source().frame_count) return;
        if (index.count(Key{source_index, next})) return;
        d->prefetch(next);
    }
};

FrameCache::FrameCache(size_t budget_bytes) : impl_(new Impl) {
    impl_->budget_bytes = budget_bytes;
}

FrameCache::~FrameCache() { delete impl_; }

void FrameCache::attach(int source_index, Decoder* decoder) {
    impl_->decoders[source_index] = decoder;
}

const Frame* FrameCache::get(int source_index, int64_t frame, std::string* error) {
    Impl& m = *impl_;
    const auto d = m.decoders.find(source_index);
    if (d == m.decoders.end() || d->second == nullptr) {
        if (error)
            *error = "FrameCache::get: no decoder attached for source " +
                     std::to_string(source_index);
        return nullptr;
    }
    Decoder* dec = d->second;

    const Key key{source_index, frame};
    const auto hit = m.index.find(key);
    if (hit != m.index.end()) {
        ++m.stats.hits;
        m.lru.splice(m.lru.begin(), m.lru, hit->second);
        m.prefetch_next(source_index, dec, frame);
        return &m.lru.front().frame;
    }

    ++m.stats.misses;
    Frame f = dec->frame_at(frame, error);
    if (!f.valid()) {
        if (error && error->empty())
            *error = "FrameCache::get: decoder returned no frame for source " +
                     std::to_string(source_index) + " frame " + std::to_string(frame);
        return nullptr;
    }
    m.stats.bytes += frame_bytes(f);
    m.lru.push_front(Entry{key, std::move(f)});
    m.index[key] = m.lru.begin();

    while (m.stats.bytes > m.budget_bytes && m.lru.size() > 1) {
        Entry& old = m.lru.back();
        m.stats.bytes -= frame_bytes(old.frame);
        m.index.erase(old.key);
        m.lru.pop_back();
        ++m.stats.evictions;
    }

    m.prefetch_next(source_index, dec, frame);
    return &m.lru.front().frame;
}

const Frame& FrameCache::black(int width, int height) {
    Frame& f = impl_->black_frame;
    if (f.width != width || f.height != height) {
        f.width = width;
        f.height = height;
        f.stride = width * 4;
        // Opaque black, not zeroed RGBA. A zero alpha would make every black slot
        // transparent in a format that carries alpha, prores4444 among them, and a
        // transparent frame is not a black frame.
        f.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
        for (size_t i = 3; i < f.pixels.size(); i += 4) f.pixels[i] = 255;
    }
    return f;
}

FrameCache::Stats FrameCache::stats() const { return impl_->stats; }

}  // namespace vrc

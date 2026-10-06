#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace nxframe {
struct VideoQueueDelaySample {
    double avgMs=0, p95Ms=0, maxMs=0;
    uint64_t count=0, omitted=0;
};

// Bounded, allocation-free producer storage. Sorting happens on the reporting
// thread after releasing the mutex. Samples describe frames reaching encoding;
// evicted frames are counted separately by the capture queue.
class VideoQueueDelay {
public:
    void record(double ms) {
        if (!std::isfinite(ms) || ms<0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_==samples_.size()) { ++omitted_; return; }
        samples_[size_++]=ms;
    }
    VideoQueueDelaySample takeInterval() {
        std::array<double,4096> values;
        size_t n;
        VideoQueueDelaySample result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            n=size_; std::copy_n(samples_.begin(),n,values.begin());
            result.omitted=omitted_; size_=0; omitted_=0;
        }
        result.count=n;
        if (!n) return result;
        double total=0;
        for(size_t i=0;i<n;++i) total+=values[i];
        std::sort(values.begin(),values.begin()+n);
        result.avgMs=total/n;
        result.p95Ms=values[(95*n+99)/100-1];
        result.maxMs=values[n-1];
        return result;
    }
private:
    std::mutex mutex_;
    std::array<double,4096> samples_{};
    size_t size_=0;
    uint64_t omitted_=0;
};
} // namespace nxframe

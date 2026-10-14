// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/muxer_ts.h"
#include <iostream>

int main()
{
    auto* video = avcodec_alloc_context3(nullptr);
    if (!video) return 1;
    video->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codec_id = AV_CODEC_ID_H264;
    video->width = 1920; video->height = 1080;
    video->time_base = AVRational{1, 25};
    int failures = 0;
    struct Case { int service, pmt, interval; const char* option; };
    for (const auto& c : {Case{65536, 2048, 30, "mpegts_service_id"},
                          Case{1, 8191, 30, "mpegts_pmt_start_pid"},
                          Case{1, 2048, -2, "pcr_period"}}) {
        MuxerTS mux;
        mux.setVideoCodecContext(video);
        mux.setPidConfiguration(c.service, 512, {}, 512, c.pmt, c.interval);
        if (mux.initialize() || mux.getLastError().find(c.option) == std::string::npos) {
            std::cerr << "Failed to report rejected option " << c.option << "\n"; ++failures;
        }
    }
    for (int interval : {-1, 30}) {
        MuxerTS mux;
        mux.setVideoCodecContext(video);
        mux.setPidConfiguration(1, 512, {}, 512, 2048, interval);
        if (!mux.initialize() || !mux.resetLiveSession()) {
            std::cerr << "Valid options rejected: " << mux.getLastError() << "\n"; ++failures;
        }
    }
    avcodec_free_context(&video);
    if (!failures) std::cout << "MPEG-TS option error checks passed\n";
    return failures ? 1 : 0;
}

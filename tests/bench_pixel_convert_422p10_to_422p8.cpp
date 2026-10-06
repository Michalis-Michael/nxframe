#include "video/pixel_convert.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using nxframe::pixel_convert::Backend;
using nxframe::pixel_convert::Yuv422p10View;
using nxframe::pixel_convert::Yuv422p8View;

struct Stats { double avg=0,p50=0,p95=0,p99=0,max=0; };

static Stats stats(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    Stats s;
    s.avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    auto q=[&](double f){ return v[static_cast<size_t>(f*(v.size()-1))]; };
    s.p50=q(.50); s.p95=q(.95); s.p99=q(.99); s.max=v.back();
    return s;
}

static void print(const char* name, const Stats& s)
{
    std::cout << std::left << std::setw(10) << name
              << " avg=" << std::fixed << std::setprecision(3) << s.avg
              << " p50=" << s.p50 << " p95=" << s.p95 << " p99=" << s.p99
              << " max=" << s.max << " ms\n";
}

int main(int argc, char** argv)
{
    const int width = argc > 1 ? std::stoi(argv[1]) : 1920;
    const int height = argc > 2 ? std::stoi(argv[2]) : 1080;
    const int iterations = argc > 3 ? std::stoi(argv[3]) : 1000;
    if (width <= 0 || height <= 0 || (width & 1) || iterations < 10) return 2;
    const int cw=width/2;

    std::vector<uint16_t> y(static_cast<size_t>(width)*height), u(static_cast<size_t>(cw)*height), v(u.size());
    std::vector<uint8_t> yo(y.size()), uo(u.size()), vo(v.size());
    std::mt19937 rng(1234); std::uniform_int_distribution<int> d(0,1023);
    for(auto& x:y)x=d(rng); for(auto& x:u)x=d(rng); for(auto& x:v)x=d(rng);

    Yuv422p10View src;
    src.data[0]=reinterpret_cast<const uint8_t*>(y.data());
    src.data[1]=reinterpret_cast<const uint8_t*>(u.data());
    src.data[2]=reinterpret_cast<const uint8_t*>(v.data());
    src.stride[0]=width*2; src.stride[1]=cw*2; src.stride[2]=cw*2;
    src.width=width; src.height=height;
    Yuv422p8View dst;
    dst.data[0]=yo.data(); dst.data[1]=uo.data(); dst.data[2]=vo.data();
    dst.stride[0]=width; dst.stride[1]=cw; dst.stride[2]=cw;
    dst.width=width; dst.height=height;

    SwsContext* sws=sws_getContext(width,height,AV_PIX_FMT_YUV422P10LE,width,height,AV_PIX_FMT_YUV422P,SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!sws){ std::cerr<<"sws_getContext failed\n"; return 1; }
    const uint8_t* swsSrc[4]={src.data[0],src.data[1],src.data[2],nullptr};
    int swsSrcStride[4]={src.stride[0],src.stride[1],src.stride[2],0};
    uint8_t* swsDst[4]={dst.data[0],dst.data[1],dst.data[2],nullptr};
    int swsDstStride[4]={dst.stride[0],dst.stride[1],dst.stride[2],0};

    auto benchSimd=[&](Backend b){
        std::vector<double> t; t.reserve(iterations);
        for(int i=0;i<20;++i) nxframe::pixel_convert::convert422p10To422p8(src,dst,b,nullptr);
        for(int i=0;i<iterations;++i){
            auto a=std::chrono::steady_clock::now();
            if(!nxframe::pixel_convert::convert422p10To422p8(src,dst,b,nullptr)) return std::vector<double>{};
            auto z=std::chrono::steady_clock::now();
            t.push_back(std::chrono::duration<double,std::milli>(z-a).count());
        }
        return t;
    };
    auto benchSws=[&](){
        std::vector<double> t; t.reserve(iterations);
        for(int i=0;i<20;++i) sws_scale(sws,swsSrc,swsSrcStride,0,height,swsDst,swsDstStride);
        for(int i=0;i<iterations;++i){ auto a=std::chrono::steady_clock::now(); sws_scale(sws,swsSrc,swsSrcStride,0,height,swsDst,swsDstStride); auto z=std::chrono::steady_clock::now(); t.push_back(std::chrono::duration<double,std::milli>(z-a).count()); }
        return t;
    };

    std::cout << width << "x" << height << " iterations=" << iterations << "\n";
    auto sw=benchSws(); print("swscale",stats(sw));
    if(nxframe::pixel_convert::cpuHasAvx2()){ auto x=benchSimd(Backend::AVX2); if(!x.empty()) print("avx2",stats(x)); }
    if(nxframe::pixel_convert::cpuHasAvx512Bw()){ auto x=benchSimd(Backend::AVX512); if(!x.empty()) print("avx512",stats(x)); }
    sws_freeContext(sws);
    return 0;
}

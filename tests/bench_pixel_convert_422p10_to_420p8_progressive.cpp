extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include "video/pixel_convert.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace pc = nxframe::pixel_convert;
using Clock = std::chrono::steady_clock;

struct Stats { double avg=0, p50=0, p95=0, p99=0, max=0; };

Stats stats(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    double sum=0; for (double x:v) sum+=x;
    std::sort(v.begin(), v.end());
    auto q=[&](double p){ return v[static_cast<size_t>(std::min<double>(v.size()-1, std::floor(p*(v.size()-1))))]; };
    s.avg=sum/v.size(); s.p50=q(0.50); s.p95=q(0.95); s.p99=q(0.99); s.max=v.back(); return s;
}

void print(const char* name, const Stats& s)
{
    std::cout << std::left << std::setw(10) << name << " avg=" << std::fixed << std::setprecision(3)
              << s.avg << " p50=" << s.p50 << " p95=" << s.p95 << " p99=" << s.p99 << " max=" << s.max << " ms\n";
}

int main(int argc, char** argv)
{
    const int width = argc > 1 ? std::stoi(argv[1]) : 1920;
    const int height = argc > 2 ? std::stoi(argv[2]) : 1080;
    const int iterations = argc > 3 ? std::stoi(argv[3]) : 1000;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || iterations <= 0) return 2;

    const int cw = width/2;
    const int ch = height/2;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0,1023);
    std::vector<uint16_t> y(static_cast<size_t>(width)*height), u(static_cast<size_t>(cw)*height), v(static_cast<size_t>(cw)*height);
    for(auto& x:y)x=static_cast<uint16_t>(dist(rng)); for(auto& x:u)x=static_cast<uint16_t>(dist(rng)); for(auto& x:v)x=static_cast<uint16_t>(dist(rng));
    std::vector<uint8_t> dy(static_cast<size_t>(width)*height), du(static_cast<size_t>(cw)*ch), dv(static_cast<size_t>(cw)*ch);

    pc::Yuv422p10View src{{reinterpret_cast<const uint8_t*>(y.data()),reinterpret_cast<const uint8_t*>(u.data()),reinterpret_cast<const uint8_t*>(v.data())},{width*2,cw*2,cw*2},width,height};
    pc::Yuv420p8View dst{{dy.data(),du.data(),dv.data()},{width,cw,cw},width,height};

    auto benchNx=[&](pc::Backend b){
        std::vector<double> times; times.reserve(iterations);
        for(int i=0;i<32;++i) pc::convert422p10To420p8Progressive(src,dst,b,nullptr);
        for(int i=0;i<iterations;++i){ auto a=Clock::now(); if(!pc::convert422p10To420p8Progressive(src,dst,b,nullptr)) return std::vector<double>{}; auto z=Clock::now(); times.push_back(std::chrono::duration<double,std::milli>(z-a).count()); }
        return times;
    };

    SwsContext* sws=sws_getContext(width,height,AV_PIX_FMT_YUV422P10LE,width,height,AV_PIX_FMT_YUV420P,SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!sws){ std::cerr<<"sws_getContext failed\n"; return 1; }
    const uint8_t* srcData[4]={src.data[0],src.data[1],src.data[2],nullptr}; int srcStride[4]={src.stride[0],src.stride[1],src.stride[2],0};
    uint8_t* dstData[4]={dst.data[0],dst.data[1],dst.data[2],nullptr}; int dstStride[4]={dst.stride[0],dst.stride[1],dst.stride[2],0};
    auto benchSws=[&](){ std::vector<double> times; times.reserve(iterations); for(int i=0;i<32;++i)sws_scale(sws,srcData,srcStride,0,height,dstData,dstStride); for(int i=0;i<iterations;++i){auto a=Clock::now();sws_scale(sws,srcData,srcStride,0,height,dstData,dstStride);auto z=Clock::now();times.push_back(std::chrono::duration<double,std::milli>(z-a).count());} return times; };

    std::cout<<width<<"x"<<height<<" iterations="<<iterations<<" progressive binomial4\n";
    print("swscale",stats(benchSws()));
    if(pc::cpuHasAvx2()) print("avx2",stats(benchNx(pc::Backend::AVX2)));
    if(pc::cpuHasAvx512Bw()) print("avx512",stats(benchNx(pc::Backend::AVX512)));
    sws_freeContext(sws);
    return 0;
}

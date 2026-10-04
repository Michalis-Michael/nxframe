/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA.
 * Synthetic HEVC benchmark. No SDI hardware, muxer or network is exercised.
 */
#include "encoders/encoder_x265.h"
#include "input/simd_v210_avx2.h"
#include "stage_timing.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>

using Clock=std::chrono::steady_clock;
static double milliseconds(Clock::duration elapsed)
{
    return std::chrono::duration<double,std::milli>(elapsed).count();
}
static int integer(const std::string& value)
{
    size_t used=0;
    const int n=std::stoi(value,&used);
    if(used!=value.size()) throw std::invalid_argument("invalid integer: "+value);
    return n;
}
static double percentile(const std::vector<double>& sorted,unsigned percent)
{
    return sorted[std::min(sorted.size()-1,(sorted.size()*percent+99)/100-1)];
}
// Test fixture only: pack the known planar input before the measured unpack.
static void packFixture(const VideoFrame& f,std::vector<uint8_t>& packed,int rowBytes)
{
    const auto* y=reinterpret_cast<const uint16_t*>(f.buffer.get());
    const auto* u=y+size_t(f.width)*f.height;
    const auto* v=u+size_t(f.width/2)*f.height;
    for(int row=0;row<f.height;++row) {
        auto* dst=packed.data()+size_t(row)*rowBytes;
        for(int x=0;x<f.width;x+=6) {
            const size_t a=size_t(row)*f.width+x,b=size_t(row)*(f.width/2)+x/2;
            const uint32_t words[4]={uint32_t(u[b])|(uint32_t(y[a])<<10)|(uint32_t(v[b])<<20),
                uint32_t(y[a+1])|(uint32_t(u[b+1])<<10)|(uint32_t(y[a+2])<<20),
                uint32_t(v[b+1])|(uint32_t(y[a+3])<<10)|(uint32_t(u[b+2])<<20),
                uint32_t(y[a+4])|(uint32_t(v[b+2])<<10)|(uint32_t(y[a+5])<<20)};
            std::memcpy(dst,words,sizeof(words)); dst+=sizeof(words);
        }
    }
}
int main(int argc,char** argv)
{
    try {
        json config={{"codec","x265"},{"width",1920},{"height",1080},{"framerate",50},
            {"bitrate",35000000},{"vbv-maxrate",35000000},{"vbv_bufsize",35000000},
            {"max_b_frames",0},{"preset","superfast"},{"profile","main422-10"},{"interlaced",false},
            {"gop",{{"size",50},{"min_keyint",50},{"scenecut",0},{"closed",true}}},
            {"output",{{"bit_depth",10},{"chroma","422"}}},
            {"color",{{"primaries","bt709"},{"transfer","bt709"},{"matrix","bt709"},
                      {"range","limited"},{"chroma_location","left"}}}};
        if(argc>1 && std::string(argv[1])!="-") { std::ifstream input(argv[1]); input>>config; }
        const int count=argc>2 ? integer(argv[2]) : 150;
        if(count<10 || count>10000) throw std::invalid_argument("frame count must be 10..10000");
        bool realtime=false;
        std::string unpack="none", output="preset", rawInput;
        for(int arg=3;arg<argc;++arg) {
            const std::string option=argv[arg];
            const auto eq=option.find('=');
            const auto key=option.substr(0,eq),value=eq==std::string::npos ? "" : option.substr(eq+1);
            if(option=="--realtime") realtime=true;
            else if(key=="--asm" || key=="--pools") config["additional_options"][key.substr(2)]=value;
            else if(key=="--frame-threads") {
                auto& opts=config["additional_options"];
                if(!opts.is_object()) opts=json::object();
                opts.erase("threads"); opts.erase("frame_threads");
                opts["frame-threads"]=integer(value);
            } else if(key=="--unpack") unpack=value;
            else if(key=="--output") output=value;
            else if(key=="--input-yuv422p10") {
                if(value.empty()) throw std::invalid_argument("raw input filename is empty");
                rawInput=value;
            }
            else throw std::invalid_argument("unknown option: "+option);
        }
        using Unpack=void(*)(const uint8_t*,int,int,int,uint16_t*,uint16_t*,uint16_t*);
        Unpack unpackFn=nullptr;
        if(unpack=="scalar") unpackFn=v210_to_yuv422p10le_scalar;
        else if(unpack=="avx2" && cpu_has_avx2()) unpackFn=v210_to_yuv422p10le_avx2;
        else if(unpack=="avx512" && cpu_has_avx512_v210()) unpackFn=v210_to_yuv422p10le_avx512;
        else if(unpack!="none") throw std::invalid_argument("unsupported unpack path: "+unpack);
        if(output!="preset") {
            if(output!="42210" && output!="42010" && output!="4208") throw std::invalid_argument("invalid output");
            // Output overrides are benchmark cases, not edits to the supplied preset.
            config.erase("pix_fmt");
            config["output"]={{"bit_depth",output=="4208" ? 8 : 10},{"chroma",output=="42210" ? "422" : "420"}};
            config["profile"]=output=="42210" ? "main422-10" : output=="42010" ? "main10" : "main";
        }
        EncoderX265 encoder(config);
        if(!encoder.initialize()) throw std::runtime_error("encoder did not initialize");
        const auto* context=encoder.getCodecContext();
        const int width=context->width,height=context->height;
        if(unpackFn && width%6) throw std::invalid_argument("v210 fixture requires width divisible by 6");
        const size_t rawFrameBytes=size_t(width)*height*4;
        std::ifstream raw;
        uint64_t sourceFrames=0,sourceIndex=0;
        if(!rawInput.empty()) {
            raw.open(rawInput,std::ios::binary|std::ios::ate);
            if(!raw) throw std::invalid_argument("cannot open raw input: "+rawInput);
            const auto length=raw.tellg();
            if(length<=0 || static_cast<uint64_t>(length)%rawFrameBytes)
                throw std::invalid_argument("raw input must contain complete preset-sized yuv422p10le frames");
            sourceFrames=static_cast<uint64_t>(length)/rawFrameBytes;
            raw.seekg(0);
        }
        const int rowBytes=((width+47)/48)*128;
        std::vector<uint8_t> packed(unpackFn ? size_t(rowBytes)*height : 0);
        stage_timing::set_enabled(true,false);
        std::vector<double> times,unpackTimes,latencies; times.reserve(count);
        std::vector<Clock::time_point> submitted(count);
        std::vector<bool> seen(count,false);
        std::vector<AVPacketPtr> packets; packets.reserve(8);
        size_t produced=0,late=0; int first=-1; double total=0;
        uint64_t outputBytes=0,keyPackets=0;
        const auto period=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
            double(context->time_base.num)/context->time_base.den));
        const auto began=Clock::now();
        auto observe=[&](const std::vector<AVPacketPtr>& received,int inputs) {
            const auto now=Clock::now();
            if(!received.empty() && first<0) first=inputs;
            for(const auto& p:received) {
                if(p->pts<0 || p->pts>=count || seen[size_t(p->pts)]) throw std::runtime_error("invalid/duplicate output PTS");
                seen[size_t(p->pts)]=true;
                outputBytes+=static_cast<uint64_t>(p->size);
                if(p->flags & AV_PKT_FLAG_KEY) ++keyPackets;
                latencies.push_back(milliseconds(now-submitted[size_t(p->pts)])); ++produced;
            }
        };
        for(int i=0;i<count;++i) {
            VideoFrame f;
            f.width=width; f.height=height; f.pix_fmt=AV_PIX_FMT_YUV422P10LE;
            f.time_base=context->time_base; f.pts=i;
            f.buffer_size=size_t(width)*height*4; f.buffer=make_shared_u8(f.buffer_size);
            if(!f.buffer) throw std::bad_alloc();
            auto* y=reinterpret_cast<uint16_t*>(f.buffer.get());
            auto* u=y+size_t(width)*height; auto* v=u+size_t(width/2)*height;
            // Repeated 100-frame loop produces a content discontinuity.
            // Generation/packing is outside the measured encode/unpack calls.
            if(raw.is_open()) {
                if(sourceIndex==sourceFrames) { raw.clear(); raw.seekg(0); sourceIndex=0; }
                raw.read(reinterpret_cast<char*>(f.buffer.get()),static_cast<std::streamsize>(rawFrameBytes));
                if(!raw || raw.gcount()!=static_cast<std::streamsize>(rawFrameBytes))
                    throw std::runtime_error("raw input read failed");
                ++sourceIndex;
            } else {
                for(int row=0;row<height;++row)
                    for(int x=0;x<width;++x) y[size_t(row)*width+x]=64+((x/8+row/8+(i%100)*2)%876);
                std::fill(u,v,uint16_t(512)); std::fill(v,y+size_t(width)*height*2,uint16_t(512));
            }
            if(unpackFn) packFixture(f,packed,rowBytes);
            if(realtime) {
                const auto deadline=began+period*i;
                std::this_thread::sleep_until(deadline);
                if(Clock::now()-deadline>std::chrono::milliseconds(1)) ++late;
            }
            submitted[i]=Clock::now();
            if(unpackFn) {
                auto start=Clock::now();
                unpackFn(packed.data(),rowBytes,width,height,y,u,v);
                unpackTimes.push_back(milliseconds(Clock::now()-start));
            }
            auto start=Clock::now();
            encoder.encodeVideoFramePackets(f,packets);
            const auto elapsed=milliseconds(Clock::now()-start);
            times.push_back(elapsed); total+=elapsed;
            observe(packets,i+1);
        }
        auto start=Clock::now(); auto tail=encoder.flush();
        const auto drain=milliseconds(Clock::now()-start); observe(tail,count);
        const double wall=milliseconds(Clock::now()-began);
        std::sort(times.begin(),times.end()); std::sort(latencies.begin(),latencies.end());
        std::sort(unpackTimes.begin(),unpackTimes.end());
        rusage usage{}; getrusage(RUSAGE_SELF,&usage);
        double sendNs=0,receiveNs=0,convertNs=0; uint64_t conversions=0;
        for(const auto& t:stage_timing::registry().snapshot()) {
            if(t.name=="x265_send_frame") sendNs=t.total_ns;
            if(t.name=="x265_receive_packet") receiveNs=t.total_ns;
            if(t.name=="x265_convert") { convertNs=t.total_ns; conversions=t.calls; }
        }
        json result={{"frames",count},{"packets",produced},{"first_packet_after_inputs",first},
            {"source",rawInput.empty()?"synthetic-ramp":"raw-yuv422p10le"},{"source_frames",sourceFrames},
            {"output_bytes",outputBytes},{"key_packets",keyPackets},
            {"actual_video_mbps",outputBytes*8.0*context->framerate.num/context->framerate.den/count/1e6},
            {"encode_call_avg_ms",total/count},{"encode_call_p50_ms",percentile(times,50)},
            {"encode_call_p95_ms",percentile(times,95)},{"encode_call_p99_ms",percentile(times,99)},
            {"encode_call_max_ms",times.back()},{"flush_ms",drain},{"wall_ms",wall},
            {"fixture_throughput_fps",count*1000.0/wall},{"realtime",realtime},{"late_inputs_gt1ms",late},
            {"peak_rss_mib",usage.ru_maxrss/1024.0},{"unpack",unpack},{"output",output},
            {"conversion_calls",conversions},{"conversion_avg_ms",conversions ? convertNs/conversions/1e6 : 0},
            {"wrapper_excluding_ffmpeg_calls_avg_us",((total+drain)*1e6-sendNs-receiveNs)/(count*1000.0)}};
        for(const auto& t:stage_timing::registry().snapshot()) {
            if(t.name=="x265_key_output_submit" || t.name=="x265_inter_output_submit" || t.name=="x265_submit_caller_cpu")
                result[t.name]={{"calls",t.calls},{"avg_ms",t.calls?t.total_ns/double(t.calls)/1e6:0},
                                {"max_ms",t.max_ns/1e6}};
        }
        if(!latencies.empty()) result["input_to_packet_p95_ms"]=percentile(latencies,95);
        if(!unpackTimes.empty()) {
            result["unpack_p50_ms"]=percentile(unpackTimes,50);
            result["unpack_p99_ms"]=percentile(unpackTimes,99);
        }
        std::cout<<"RESULT "<<result.dump()<<'\n';
        return produced==size_t(count)?0:2;
    } catch(const std::exception& e) {
        std::cerr<<"Benchmark failed: "<<e.what()<<'\n'; return 1;
    }
}

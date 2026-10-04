/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA. Sender-only terminal telemetry.
 */
#pragma once
#include "core/video_queue_delay.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include "core/system_resource_monitor.h"
#include "core/sender_diagnostic_log.h"

namespace nxframe {
struct DashboardTiming { double avgMs=0, maxMs=0; uint64_t calls=0; };
struct DashboardState {
    std::string input="Waiting for frames", signal="WAITING", source="--", internal="--";
    std::string encoder="--", audio="--", inputAudio="Not observed", endpoint="--", peer="--", transport="--";
    std::string device="--", codec="--", preset="--", profile="--", output="--", colour="--";
    std::string level="auto (requested)", target="--", gop="--", bframes="--", threads="auto", lookahead="auto", latency="--";
    std::string genlock="NOT APPLICABLE", referenceFormat="--";
    std::string network="WAITING", timecode="Not observed", captions="Not observed", anc="Not observed";
    uint64_t ancPackets=0, lost=0, retransmitted=0, dropped=0, reconnects=0;
    double rtt=0; uint64_t sendBufferMs=0;
    bool inputSample=false;
    std::chrono::steady_clock::time_point inputUpdated{};
    bool networkSample=false;
    std::chrono::steady_clock::time_point networkUpdated{};
};
struct DashboardInterval {
    ResourceSample resources;
    double seconds=0, uptime=0, capture=0, encoded=0, audio=0, mbps=0, budgetMs=0;
    uint64_t overBudget=0, encodeCalls=0;
    uint64_t evictions=0, evictionDelta=0, encodedTotal=0, muxErrors=0, sendErrors=0;
    uint64_t overflowV=0, overflowA=0, recoveryV=0, recoveryA=0, waitingV=0, waitingA=0, repairs=0;
    size_t vq=0, aq=0, vpq=0, apq=0, peakVq=0, captureQueueFrames=1;
    VideoQueueDelaySample queueDelay;
    bool recovering=false, keyframeWait=false;
    std::map<std::string,DashboardTiming> timing;
};
// Sanitizes device names and log text before placing them in a terminal.
inline std::string dashboardSafe(const std::string& text) {
    std::string out;
    for (unsigned char c:text) if (c>=32 && c!=127) out+=static_cast<char>(c);
    return out;
}
inline std::string dashboardClock(double seconds) {
    auto n=static_cast<uint64_t>(std::max(0.0,seconds));
    std::ostringstream o; o<<std::setfill('0')<<std::setw(2)<<n/3600<<":"<<std::setw(2)<<(n/60)%60<<":"<<std::setw(2)<<n%60;
    return o.str();
}
inline std::string dashboardResources(const ResourceSample& r) {
    std::ostringstream o; o<<std::fixed<<std::setprecision(1);
    o<<"CPU       System: "; if(r.cpuValid) o<<r.systemCpuPct<<"%"; else o<<"--";
    o<<" | NxFrame: "; if(r.processCpuValid) o<<r.processCpuPct<<"%"; else o<<"--";
    o<<" (100% = one logical CPU)\n";
    o<<"CORES     Physical: "<<(r.physical?std::to_string(r.physical):"--")
        <<" | Logical: "<<r.logical<<" | Process affinity: "<<r.affinity<<"\n";
    o<<"RAM       NxFrame RSS: "; if(r.processMemoryValid) o<<r.rssKiB/1024.0<<" MiB"; else o<<"--";
    o<<" | System used/total: ";
    if(r.memoryValid) o<<(r.totalKiB-r.availableKiB)/1048576.0<<"/"<<r.totalKiB/1048576.0<<" GiB";
    else o<<"--";
    o<<"\n"; return o.str();
}
inline std::string dashboardNumber(double value,int precision=1) {
    std::ostringstream o; o<<std::fixed<<std::setprecision(precision)<<value; return o.str();
}
inline std::string dashboardFit(const std::string& raw,size_t width) {
    auto value=dashboardSafe(raw);
    if(value.size()>width) value=value.substr(0,width>0?width-1:0)+(width?"~":"");
    return value+std::string(width-value.size(),' ');
}
inline std::string dashboardRow(const std::string& label,const std::string& left,const std::string& right="",unsigned width=96) {
    unsigned split=width/2;
    return dashboardFit(label,12)+dashboardFit(left,split-12)+dashboardSafe(right).substr(0,width-split)+"\n";
}
inline std::string renderSenderDashboard(const DashboardState& s,const DashboardInterval& p,
        const std::deque<std::string>& events,bool stopped=false,bool details=false,unsigned width=96) {
    width=std::max(60u,width);
    std::ostringstream o;
    const std::string rule(width,'-');
    auto row=[&](const std::string& label,const std::string& left,const std::string& right="") { o<<dashboardRow(label,left,right,width); };
    auto separator=[&]() { o<<rule<<"\n"; };
    const bool stale=s.inputSample && std::chrono::steady_clock::now()-s.inputUpdated>std::chrono::seconds(1);
    const auto& r=p.resources;
    row("NxFrame", "SENDER",std::string(stopped?"STOPPED   ":"RUNNING   ")+dashboardClock(p.uptime));
    separator();
    row("SYSTEM",r.system);
    row("CPU",r.cpuModel,"Cores: "+std::to_string(r.physical)+" / Threads: "+std::to_string(r.logical));
    row("RAM",r.memoryValid?dashboardNumber(r.totalKiB/1048576.0)+" GiB total":"--","CPUs allowed: "+std::to_string(r.affinity));
    row("CPU USAGE",r.cpuValid?"System: "+dashboardNumber(r.systemCpuPct)+"%":"System: --",r.processCpuValid?"NxFrame: "+dashboardNumber(r.processCpuPct)+"%":"NxFrame: --");
    row("RAM USAGE",r.memoryValid?"System: "+dashboardNumber((r.totalKiB-r.availableKiB)/1048576.0)+" / "+dashboardNumber(r.totalKiB/1048576.0)+" GiB":"System: --",r.processMemoryValid?"NxFrame: "+dashboardNumber(r.rssKiB/1048576.0,2)+" GiB":"NxFrame: --");
    separator();
    row("INPUT",s.device,"Signal: "+(stale?std::string("STALE - no recent frames"):s.signal));
    row("Detected",s.input,"Source: "+s.source);
    row("Internal",s.internal,"Colour: "+s.colour);
    row("Capture",dashboardNumber(p.capture,2)+" fps","Audio: "+s.inputAudio);
    row("Genlock",s.genlock,"Reference: "+s.referenceFormat);
    separator();
    row("ENCODER",s.codec,"Preset: "+s.preset);
    row("Output",s.output,"Profile: "+s.profile);
    row("Bitrate",s.target,"GOP: "+s.gop);
    row("B-frames",s.bframes,"Lookahead: "+s.lookahead);
    row("Threads",s.threads,"Audio: "+s.audio);
    separator();
    const unsigned stageWidth=width/2, avgWidth=(width-stageWidth)/2;
    o<<dashboardFit("PERFORMANCE (ms)",stageWidth)<<dashboardFit("AVERAGE",avgWidth)<<"MAXIMUM (session)\n";
    const std::pair<const char*,const char*> stages[]={
        {"Video encode","video_encode_zc"},{"v210 unpack","decklink_unpack_v210"},
        {"Audio encode","audio_encode"},{"Video mux write","mux_video_write"},{"Transport send","srt_send"}};
    for(const auto& st:stages) {
        auto t=p.timing.find(st.second);
        if(std::string(st.second)=="video_encode_zc" && (t==p.timing.end() || !t->second.calls)) t=p.timing.find("video_encode_copy");
        bool valid=t!=p.timing.end() && t->second.calls;
        o<<dashboardFit(st.first,stageWidth)<<dashboardFit(valid?dashboardNumber(t->second.avgMs,3):"--",avgWidth)
         <<(valid?dashboardNumber(t->second.maxMs,3):"--")<<"\n";
    }
    separator();
    row("CADENCE","Capture: "+dashboardNumber(p.capture,2)+" fps","Encoded: "+dashboardNumber(p.encoded,2)+" packets/s");
    row("Budget",dashboardNumber(p.budgetMs)+" ms/frame","Over budget: "+std::to_string(p.overBudget)+" / "+std::to_string(p.encodeCalls));
    row("Queues","Raw V/A: "+std::to_string(p.vq)+" / "+std::to_string(p.aq),"Encoded V/A: "+std::to_string(p.vpq)+" / "+std::to_string(p.apq));
    row("Queue wait",p.queueDelay.count ? dashboardNumber(p.queueDelay.avgMs,2)+" ms avg / "+dashboardNumber(p.queueDelay.p95Ms,2)+" ms p95" : "-- (no sample)",
        "Max: "+dashboardNumber(p.queueDelay.maxMs,2)+" ms / slots: "+std::to_string(p.captureQueueFrames)+" / peak: "+std::to_string(p.peakVq));
    row("Drops","Capture: +"+std::to_string(p.evictionDelta)+" / total "+std::to_string(p.evictions),"Overflow V/A: "+std::to_string(p.overflowV)+" / "+std::to_string(p.overflowA));
    separator();
    row("TRANSPORT",s.transport,"State: "+s.network);
    row("Endpoint",s.endpoint,"Peer: "+s.peer);
    row("Send rate",dashboardNumber(p.mbps,2)+" Mbps","Latency: "+s.latency+" (configured)");
    row("RTT",s.networkSample?dashboardNumber(s.rtt,2)+" ms":"-- (no sample)","Send buffer: "+(s.networkSample?std::to_string(s.sendBufferMs)+" ms":"--"));
    row("Packets",s.networkSample?"Lost: "+std::to_string(s.lost)+" / RTX: "+std::to_string(s.retransmitted):"-- (no sample)","Dropped: "+(s.networkSample?std::to_string(s.dropped):"--"));
    row("Recovery","Reconnects: "+std::to_string(s.reconnects),"Discarded V/A: "+std::to_string(p.recoveryV)+" / "+std::to_string(p.recoveryA));
    separator();
    row("METADATA","ANC packets: "+std::to_string(s.ancPackets),"Captions: "+s.captions);
    row("Timecode",s.timecode,"Timestamp repairs: "+std::to_string(p.repairs));
    row("HEALTH","Mux: "+std::to_string(p.muxErrors)+" / Send: "+std::to_string(p.sendErrors),std::string("Status: ")+(p.recovering?"WAITING / GATED":p.evictionDelta?"WARNING / CAPTURE DROPS":p.muxErrors||p.sendErrors?"CHECK ERRORS":"OK"));
    separator();
    o<<"EVENT       "<<dashboardSafe(events.empty()?"No recent events":events.back()).substr(0,width-12)<<"\n";
    o<<"Ctrl+C: stop   Refresh: 1s   Details: "<<(details?"ON":"OFF")<<"\n";
    return o.str();
}

class SenderDashboard {
    class Buffer:public std::streambuf {
        SenderDashboard& owner_; bool error_;
        std::map<std::thread::id,std::string> pending_;
        std::mutex pendingMutex_;
    public:
        Buffer(SenderDashboard& o,bool e):owner_(o),error_(e){}
        void clear() { std::lock_guard<std::mutex> lock(pendingMutex_); pending_.clear(); }
    protected:
        std::streamsize xsputn(const char* s,std::streamsize n) override {
            std::vector<std::string> completed;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto& line=pending_[std::this_thread::get_id()];
                for (std::streamsize i=0;i<n;++i) {
                    if(s[i]=='\n') { completed.push_back(std::move(line)); line.clear(); }
                    else if(line.size()<8192) line+=s[i];
                }
            }
            // TTY events only touch state, so terminal backpressure cannot hold
            // up a producer recording a log line. Raw redirected output keeps
            // its normal synchronous behavior.
            for (const auto& line:completed) owner_.lineLocked(line,error_);
            return n;
        }
        int_type overflow(int_type c) override {
            if(traits_type::eq_int_type(c,traits_type::eof())) return traits_type::not_eof(c);
            char ch=traits_type::to_char_type(c); xsputn(&ch,1); return c;
        }
        int sync() override { return 0; }
    };
    // Rendering/log output is serialized separately from capture-facing state.
    std::mutex ioMutex_, mutex_; std::atomic<bool> active_{false}; bool tty_=false,verbose_=false;
    int terminalFd_=-1;
    termios savedTerminal_{}; bool terminalModeSaved_=false;
    size_t viewport_=0; std::string navigation_;
    std::streambuf *oldOut_=nullptr,*oldErr_=nullptr;
    Buffer out_{*this,false},err_{*this,true};
    DashboardState state_; std::deque<std::string> events_;
    SenderDiagnosticLog diagnosticLog_;
    std::chrono::steady_clock::time_point started_{};
    void terminalLocked(const std::string& text) {
        const char* data=text.data(); size_t left=text.size();
        while(left) { auto n=::write(terminalFd_,data,left); if(n<=0) break; data+=n; left-=static_cast<size_t>(n); }
    }
    void rawLocked(const std::string& text,bool error=false) {
        auto* b=error?oldErr_:oldOut_; if(b) { b->sputn(text.data(),static_cast<std::streamsize>(text.size())); b->pubsync(); }
    }
    void lineLocked(const std::string& raw,bool error) {
        auto text=dashboardSafe(raw); if(text.empty()) return;
        diagnosticLog_.submit("[LOG] "+text+"\n");
        bool diagnostic=text.find("[DIAG]")!=std::string::npos || text.find("[TIMING]")!=std::string::npos ||
            text.find("[PERF]")!=std::string::npos || text.find("[SRT] stats")!=std::string::npos ||
            text.find(" diagnostic ")!=std::string::npos || text.find("decklink_unpack_v210 avg_us")!=std::string::npos;
        {
            std::lock_guard<std::mutex> stateLock(mutex_);
            if(!active() || (diagnostic && !verbose_)) return;
            if(tty_) {
                auto t=std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count();
                events_.push_back(dashboardClock(t)+" "+text);
                while(events_.size()>4) events_.pop_front();
                return;
            }
        }
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        rawLocked(text+"\n",error);
    }
public:
    bool active() const { return active_.load(std::memory_order_relaxed); }
    bool verbose() const { return verbose_; }
    bool diagnosticsEnabled() const { return diagnosticLog_.enabled(); }
    bool interactive() const { return tty_; }
    void start(const DashboardState& initial,bool verbose,bool diagnostics=false) {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        std::lock_guard<std::mutex> lk(mutex_); if(active()) return;
        state_=initial; events_.clear(); out_.clear(); err_.clear(); verbose_=verbose;
        // Draw to the controlling terminal, independent of stdout/stderr pipes.
        terminalFd_=::open("/dev/tty",O_RDWR | O_CLOEXEC);
        if(terminalFd_<0 && isatty(STDOUT_FILENO)) terminalFd_=::dup(STDOUT_FILENO);
        tty_=terminalFd_>=0;
        viewport_=0; navigation_.clear(); terminalModeSaved_=false;
        if(tty_ && tcgetattr(terminalFd_,&savedTerminal_)==0) {
            auto mode=savedTerminal_; mode.c_lflag &= ~(ICANON | ECHO);
            mode.c_cc[VMIN]=0; mode.c_cc[VTIME]=0;
            terminalModeSaved_=tcsetattr(terminalFd_,TCSANOW,&mode)==0;
        }
        started_=std::chrono::steady_clock::now();
        oldOut_=std::cout.rdbuf(); oldErr_=std::cerr.rdbuf();
        if(diagnostics) {
            const char* configured=std::getenv("NXFRAME_DIAGNOSTIC_LOG");
            const std::string path=configured && *configured ? configured :
                "sender_diagnostics_"+std::to_string(::getpid())+".log";
            if(diagnosticLog_.start(path)) {
                rawLocked("[Main] Sender diagnostics: "+path+"\n");
                diagnosticLog_.submit("[SESSION_START] pid="+std::to_string(::getpid())+"\n");
            } else rawLocked("[WARN] Cannot open sender diagnostic log: "+path+"\n",true);
        }
        if(tty_) terminalLocked("\033[?1049h\033[?25l");
        std::cout.rdbuf(&out_); std::cerr.rdbuf(&err_); active_.store(true);
    }
    template<class F> void update(F f) {
        if(!active()) return;
        std::lock_guard<std::mutex> lk(mutex_); if(active()) f(state_);
    }
    void recordInterval(const DashboardInterval& p) {
        if(!diagnosticLog_.enabled()) return;
        DashboardState s;
        { std::lock_guard<std::mutex> lock(mutex_); s=state_; }
        std::ostringstream o; o<<std::fixed<<std::setprecision(3);
        o<<"[INTERVAL] uptime_s="<<p.uptime<<" interval_s="<<p.seconds
         <<" capture_fps="<<p.capture<<" encoded_pps="<<p.encoded<<" audio_pps="<<p.audio
         <<" send_mbps="<<p.mbps<<" encoded_total="<<p.encodedTotal
         <<" capture_drop_delta="<<p.evictionDelta<<" capture_drop_total="<<p.evictions
         <<" encode_calls="<<p.encodeCalls<<" over_budget="<<p.overBudget<<" budget_ms="<<p.budgetMs
         <<" capture_queue_frames="<<p.captureQueueFrames<<" raw_video_peak="<<p.peakVq
         <<" queue_wait_avg_ms="<<p.queueDelay.avgMs<<" queue_wait_p95_ms="<<p.queueDelay.p95Ms
         <<" queue_wait_max_ms="<<p.queueDelay.maxMs<<" queue_wait_samples="<<p.queueDelay.count
         <<" queue_wait_omitted="<<p.queueDelay.omitted
         <<" queues_v_a_vp_ap="<<p.vq<<"/"<<p.aq<<"/"<<p.vpq<<"/"<<p.apq
         <<" overflow_v_a="<<p.overflowV<<"/"<<p.overflowA
         <<" recovery_v_a="<<p.recoveryV<<"/"<<p.recoveryA
         <<" waiting_v_a="<<p.waitingV<<"/"<<p.waitingA
         <<" mux_errors="<<p.muxErrors<<" send_errors="<<p.sendErrors<<" repairs="<<p.repairs
         <<" recovering="<<p.recovering<<" keyframe_wait="<<p.keyframeWait
         <<" network="<<dashboardSafe(s.network)<<" loss="<<s.lost<<" retrans="<<s.retransmitted
         <<" transport_drop="<<s.dropped<<" reconnects="<<s.reconnects
         <<" rtt_ms="<<s.rtt<<" send_buffer_ms="<<s.sendBufferMs
         <<" cpu_valid="<<p.resources.cpuValid<<" cpu_pct="<<p.resources.systemCpuPct
         <<" process_cpu_valid="<<p.resources.processCpuValid<<" process_cpu_pct="<<p.resources.processCpuPct
         <<" rss_valid="<<p.resources.processMemoryValid<<" rss_kib="<<p.resources.rssKiB
         <<" log_dropped="<<diagnosticLog_.dropped()<<" log_errors="<<diagnosticLog_.errors()<<"\n";
        o<<"[FORMAT] input="<<dashboardSafe(s.input)<<" signal="<<dashboardSafe(s.signal)
         <<" codec="<<dashboardSafe(s.codec)<<" preset="<<dashboardSafe(s.preset)
         <<" output="<<dashboardSafe(s.output)<<" threads="<<dashboardSafe(s.threads)
         <<" lookahead="<<dashboardSafe(s.lookahead)<<"\n";
        for(const auto& kv:p.timing) if(kv.second.calls)
            o<<"[TIMING] stage="<<kv.first<<" avg_ms="<<kv.second.avgMs
             <<" max_session_ms="<<kv.second.maxMs<<" calls="<<kv.second.calls<<"\n";
        diagnosticLog_.submit(o.str());
    }
    void render(const DashboardInterval& p,bool stopped=false) {
        std::lock_guard<std::mutex> ioLock(ioMutex_); if(!active()) return;
        DashboardState state;
        std::deque<std::string> events;
        {
            std::lock_guard<std::mutex> stateLock(mutex_);
            state=state_; events=events_;
        }
        auto text=renderSenderDashboard(state,p,events,stopped,verbose_);
        if(tty_) {
            winsize w{}; ioctl(terminalFd_,TIOCGWINSZ,&w);
            if(!w.ws_row) w.ws_row=24;
            if(!w.ws_col) w.ws_col=80;
            text=renderSenderDashboard(state,p,events,stopped,verbose_,std::max(60u,unsigned(w.ws_col)-1));
            std::vector<std::string> rows; std::istringstream input(text); std::string row;
            while(std::getline(input,row)) rows.push_back(row);
            const unsigned usable=w.ws_row>2?w.ws_row-2:1;
            const size_t last=rows.size()>usable?rows.size()-usable:0;
            if(terminalModeSaved_) {
                pollfd fd{terminalFd_,POLLIN,0}; char keys[64];
                if(::poll(&fd,1,0)>0 && (fd.revents & POLLIN)) {
                    auto n=::read(terminalFd_,keys,sizeof(keys));
                    if(n>0) navigation_.append(keys,static_cast<size_t>(n));
                }
                // Keep incomplete escape sequences for the next refresh.
                while(!navigation_.empty()) {
                    if(navigation_[0]!='\033') { navigation_.erase(0,1); continue; }
                    if(navigation_.size()<3) break;
                    size_t end=navigation_.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ~",2);
                    if(end==std::string::npos) break;
                    auto key=navigation_.substr(0,end+1); navigation_.erase(0,end+1);
                    if(key=="\033[A") viewport_=viewport_?viewport_-1:0;
                    else if(key=="\033[B") viewport_=std::min(last,viewport_+1);
                    else if(key=="\033[5~") viewport_=viewport_>usable?viewport_-usable:0;
                    else if(key=="\033[6~") viewport_=std::min(last,viewport_+usable);
                    else if(key=="\033[H" || key=="\033[1~") viewport_=0;
                    else if(key=="\033[F" || key=="\033[4~") viewport_=last;
                }
                if(navigation_.size()>64) navigation_.clear();
            }
            viewport_=std::min(viewport_,last);
            std::string clipped;
            for(size_t i=viewport_;i<rows.size() && i<viewport_+usable;++i)
                clipped+=rows[i].substr(0,std::max(1,int(w.ws_col)-1))+"\033[K\n";
            if(last) {
                auto footer="Up/Down PgUp/PgDn: view | Ctrl+C: stop | Rows "+std::to_string(viewport_+1)+"-"+std::to_string(std::min(rows.size(),viewport_+usable))+"/"+std::to_string(rows.size());
                clipped+=footer.substr(0,std::max(1,int(w.ws_col)-1))+"\033[K";
            }
            terminalLocked("\033[H"+clipped+"\033[J");
        } else rawLocked("\n"+text+"\n");
    }
    void finish(const DashboardInterval& p) {
        std::lock_guard<std::mutex> ioLock(ioMutex_); if(!active()) return;
        std::deque<std::string> events;
        uint64_t reconnects;
        {
            std::lock_guard<std::mutex> stateLock(mutex_);
            active_.store(false); events=events_; reconnects=state_.reconnects;
        }
        std::cout.rdbuf(oldOut_); std::cerr.rdbuf(oldErr_);
        if(terminalModeSaved_) { tcsetattr(terminalFd_,TCSANOW,&savedTerminal_); terminalModeSaved_=false; }
        if(tty_) terminalLocked("\033[?25h\033[?1049l");
        if(terminalFd_>=0) { ::close(terminalFd_); terminalFd_=-1; }
        if(tty_) for(const auto& e:events) rawLocked("[EVENT] "+e+"\n");
        std::ostringstream o; o<<"[SESSION] duration="<<dashboardClock(p.uptime)<<" encoded_packets="<<p.encodedTotal
            <<" capture_drops="<<p.evictions<<" reconnects="<<reconnects<<" mux_errors="<<p.muxErrors<<" send_errors="<<p.sendErrors<<"\n";
        diagnosticLog_.submit(o.str()); diagnosticLog_.stop();
        if(diagnosticLog_.dropped() || diagnosticLog_.errors())
            rawLocked("[WARN] Diagnostic log dropped_records="+std::to_string(diagnosticLog_.dropped())+
                      " write_errors="+std::to_string(diagnosticLog_.errors())+"\n",true);
        rawLocked(o.str()); oldOut_=oldErr_=nullptr;
    }
};
inline SenderDashboard& senderDashboard() { static SenderDashboard d; return d; }
} // namespace nxframe

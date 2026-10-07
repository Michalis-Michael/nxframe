/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA. Receiver-only terminal telemetry.
 */
#pragma once

#include "core/system_resource_monitor.h"
#include "core/sender_diagnostic_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <iomanip>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <streambuf>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>

namespace nxframe {

inline std::string receiverDashboardSafe(const std::string& text) {
    std::string out;
    for (unsigned char c:text) if (c>=32 && c!=127) out+=static_cast<char>(c);
    return out;
}
inline std::string receiverDashboardClock(double seconds) {
    auto n=static_cast<uint64_t>(std::max(0.0,seconds));
    std::ostringstream o; o<<std::setfill('0')<<std::setw(2)<<n/3600<<":"<<std::setw(2)<<(n/60)%60<<":"<<std::setw(2)<<n%60;
    return o.str();
}
inline std::string receiverDashboardNumber(double value,int precision=1) {
    std::ostringstream o; o<<std::fixed<<std::setprecision(precision)<<value; return o.str();
}
inline std::string receiverDashboardFit(const std::string& raw,size_t width) {
    auto value=receiverDashboardSafe(raw);
    if(value.size()>width) value=value.substr(0,width>0?width-1:0)+(width?"~":"");
    return value+std::string(width-value.size(),' ');
}
inline std::string receiverDashboardRow(const std::string& label,const std::string& left,const std::string& right="",unsigned width=96) {
    unsigned split=width/2;
    return receiverDashboardFit(label,12)+receiverDashboardFit(left,split-12)+receiverDashboardSafe(right).substr(0,width-split)+"\n";
}

struct ReceiverDashboardState {
    std::string source="--", destination="--";
    std::string transport="--", mode="--", endpoint="--", network="WAITING";
    std::string video="Waiting for stream", audio="Waiting for stream";
    std::string outputDevice="--", output="Waiting for format";
    std::string latency="--";
};

struct ReceiverDashboardInterval {
    ResourceSample resources;
    double uptime=0.0, seconds=0.0;
    double receiveMbps=0.0, decodedVideoFps=0.0, decodedAudioFps=0.0;
    uint64_t receivedPackets=0, receivedBytes=0, transportDrops=0;

    uint64_t demuxPackets=0, demuxSyncErrors=0, demuxContinuityErrors=0;
    uint64_t demuxDiscontinuities=0, demuxOverflowEvents=0;
    uint64_t demuxVideoDrops=0, demuxAudioDrops=0;
    size_t demuxInputBytes=0, demuxVideoQ=0, demuxAudioQ=0;

    uint64_t decodedVideoTotal=0, decodedAudioTotal=0, decoderVideoDrops=0;
    uint64_t acquisitionDrops=0;
    size_t decodedVideoQ=0, decodedAudioQ=0;
    size_t decodedVideoPeak=0, decodedAudioPeak=0;
    size_t packedAudioQ=0, packedAudioPeak=0, fifoSamples=0;

    bool queueAvDeltaValid=false;
    double queueAvDeltaMs=0.0;
    bool scheduledAvDeltaValid=false;
    double scheduledAvDeltaMs=0.0;
    uint64_t softLoss=0, hardLoss=0, reconnectResets=0, sourceGeneration=0;

    bool rtp=false;
    uint64_t rtpGaps=0, rtpOutOfOrder=0, rtpDuplicates=0, rtpMalformed=0;
    uint64_t tsSyncErrors=0, tsContinuityErrors=0;

    bool decklink=false;
    uint64_t outputVideoTotal=0, outputVideoDrops=0, outputAudioDrops=0;
    uint64_t scheduleFailures=0, completionWarnings=0;
    uint32_t hwVideoQ=0, hwAudioSamples=0, scheduledVideo=0;
};

inline std::string renderReceiverDashboard(const ReceiverDashboardState& s,
                                            const ReceiverDashboardInterval& p,
                                            const std::deque<std::string>& events,
                                            bool stopped=false,
                                            unsigned width=96)
{
    width=std::max(60u,width);
    std::ostringstream o;
    const std::string rule(width,'-');
    auto row=[&](const std::string& label,const std::string& left,const std::string& right="") {
        o<<receiverDashboardRow(label,left,right,width);
    };
    auto separator=[&]() { o<<rule<<"\n"; };
    const auto& r=p.resources;

    row("NxFrame","RECEIVER",std::string(stopped?"STOPPED   ":"RUNNING   ")+receiverDashboardClock(p.uptime));
    separator();
    row("SYSTEM",r.system);
    row("CPU",r.cpuModel,"Cores: "+std::to_string(r.physical)+" / Threads: "+std::to_string(r.logical));
    row("RAM",r.memoryValid?receiverDashboardNumber(r.totalKiB/1048576.0)+" GiB total":"--","CPUs allowed: "+std::to_string(r.affinity));
    row("CPU USAGE",r.cpuValid?"System: "+receiverDashboardNumber(r.systemCpuPct)+"%":"System: --",r.processCpuValid?"NxFrame: "+receiverDashboardNumber(r.processCpuPct)+"%":"NxFrame: --");
    row("RAM USAGE",r.memoryValid?"System: "+receiverDashboardNumber((r.totalKiB-r.availableKiB)/1048576.0)+" / "+receiverDashboardNumber(r.totalKiB/1048576.0)+" GiB":"System: --",r.processMemoryValid?"NxFrame: "+receiverDashboardNumber(r.rssKiB/1048576.0,2)+" GiB":"NxFrame: --");
    separator();

    row("INPUT",s.source,"State: "+s.network);
    row("Transport",s.transport+" "+s.mode,"Endpoint: "+s.endpoint);
    row("Receive",receiverDashboardNumber(p.receiveMbps,2)+" Mbps","Packets: "+std::to_string(p.receivedPackets)+" / drops: "+std::to_string(p.transportDrops));
    if(p.rtp) {
        row("RTP","Missing: "+std::to_string(p.rtpGaps)+" / OOO: "+std::to_string(p.rtpOutOfOrder),"Duplicates: "+std::to_string(p.rtpDuplicates)+" / malformed: "+std::to_string(p.rtpMalformed));
    }
    separator();

    row("PROGRAM",s.video,"Audio: "+s.audio);
    row("DECODE","Video: "+receiverDashboardNumber(p.decodedVideoFps,2)+" fps","Audio: "+receiverDashboardNumber(p.decodedAudioFps,2)+" frames/s");
    row("Decoded","Video: "+std::to_string(p.decodedVideoTotal),"Audio: "+std::to_string(p.decodedAudioTotal));
    row("Decoder Q","Video: "+std::to_string(p.decodedVideoQ)+" / peak "+std::to_string(p.decodedVideoPeak),"Audio: "+std::to_string(p.decodedAudioQ)+" / peak "+std::to_string(p.decodedAudioPeak));
    row("Decoder drop","Queue: "+std::to_string(p.decoderVideoDrops),"Acquisition: "+std::to_string(p.acquisitionDrops));
    separator();

    row("DEMUX","Input: "+std::to_string(p.demuxInputBytes/1024)+" KiB buffered","Generation: "+std::to_string(p.sourceGeneration));
    row("Queues","Video: "+std::to_string(p.demuxVideoQ),"Audio: "+std::to_string(p.demuxAudioQ));
    row("TS health","Sync: "+std::to_string(p.demuxSyncErrors)+" / CC: "+std::to_string(p.demuxContinuityErrors),"Discontinuities: "+std::to_string(p.demuxDiscontinuities));
    row("Overflow","Input: "+std::to_string(p.demuxOverflowEvents),"Queue drops V/A: "+std::to_string(p.demuxVideoDrops)+" / "+std::to_string(p.demuxAudioDrops));
    separator();

    row("AUDIO","Packed Q: "+std::to_string(p.packedAudioQ)+" / peak "+std::to_string(p.packedAudioPeak),"FIFO: "+std::to_string(p.fifoSamples)+" samples");
    row("Queue A/V",p.queueAvDeltaValid?receiverDashboardNumber(p.queueAvDeltaMs,2)+" ms":"-- (not locked)","Receiver queue staging");
    row("SDI sched",p.scheduledAvDeltaValid?"A/V: "+receiverDashboardNumber(p.scheduledAvDeltaMs,2)+" ms":"A/V: N/A","DeckLink scheduling horizon");
    row("Recovery","Soft loss: "+std::to_string(p.softLoss)+" / hard: "+std::to_string(p.hardLoss),"Resets: "+std::to_string(p.reconnectResets));
    separator();

    row("OUTPUT",s.destination,s.outputDevice);
    if(p.decklink) {
        row("SDI",s.output,"Buffered V/A: "+std::to_string(p.hwVideoQ)+" / "+std::to_string(p.hwAudioSamples));
        row("Playout","Frames: "+std::to_string(p.outputVideoTotal),"Scheduled: "+std::to_string(p.scheduledVideo));
        row("Drops","Video: "+std::to_string(p.outputVideoDrops),"Audio: "+std::to_string(p.outputAudioDrops));
        row("Health","Schedule failures: "+std::to_string(p.scheduleFailures),"Completion warnings: "+std::to_string(p.completionWarnings));
    } else {
        row("Sink","Decoded frames drained by test mode");
    }
    separator();
    const bool warn=p.transportDrops || p.demuxSyncErrors || p.demuxContinuityErrors || p.demuxOverflowEvents ||
                    p.decoderVideoDrops || p.outputVideoDrops || p.scheduleFailures || p.completionWarnings || p.hardLoss;
    row("HEALTH",warn?"CHECK COUNTERS":"OK","Refresh: 1s");
    separator();
    o<<"EVENT       "<<receiverDashboardSafe(events.empty()?"No recent events":events.back()).substr(0,width-12)<<"\n";
    o<<"Ctrl+C: stop   Refresh: 1s\n";
    return o.str();
}

class ReceiverDashboard {
    class Buffer:public std::streambuf {
        ReceiverDashboard& owner_; bool error_;
        std::map<std::thread::id,std::string> pending_;
        std::mutex pendingMutex_;
    public:
        Buffer(ReceiverDashboard& o,bool e):owner_(o),error_(e){}
        void clear() { std::lock_guard<std::mutex> lock(pendingMutex_); pending_.clear(); }
    protected:
        std::streamsize xsputn(const char* s,std::streamsize n) override {
            std::vector<std::string> completed;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto& line=pending_[std::this_thread::get_id()];
                for(std::streamsize i=0;i<n;++i) {
                    if(s[i]=='\n') { completed.push_back(std::move(line)); line.clear(); }
                    else if(line.size()<8192) line+=s[i];
                }
            }
            for(const auto& line:completed) owner_.lineLocked(line,error_);
            return n;
        }
        int_type overflow(int_type c) override {
            if(traits_type::eq_int_type(c,traits_type::eof())) return traits_type::not_eof(c);
            char ch=traits_type::to_char_type(c); xsputn(&ch,1); return c;
        }
        int sync() override { return 0; }
    };

    std::mutex ioMutex_,mutex_;
    std::atomic<bool> active_{false}; bool tty_=false;
    int terminalFd_=-1; termios savedTerminal_{}; bool terminalModeSaved_=false;
    size_t viewport_=0; std::string navigation_;
    std::streambuf *oldOut_=nullptr,*oldErr_=nullptr;
    Buffer out_{*this,false},err_{*this,true};
    ReceiverDashboardState state_; std::deque<std::string> events_;
    SenderDiagnosticLog diagnosticLog_;
    std::chrono::steady_clock::time_point started_{};

    void terminalLocked(const std::string& text) {
        const char* data=text.data(); size_t left=text.size();
        while(left) { auto n=::write(terminalFd_,data,left); if(n<=0) break; data+=n; left-=static_cast<size_t>(n); }
    }
    void rawLocked(const std::string& text,bool error=false) {
        auto* b=error?oldErr_:oldOut_;
        if(b) { b->sputn(text.data(),static_cast<std::streamsize>(text.size())); b->pubsync(); }
    }
    void lineLocked(const std::string& raw,bool error) {
        auto text=receiverDashboardSafe(raw); if(text.empty()) return;
        diagnosticLog_.submit("[LOG] "+text+"\n");
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if(!active()) return;
            if(tty_) {
                auto t=std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count();
                events_.push_back(receiverDashboardClock(t)+" "+text);
                while(events_.size()>4) events_.pop_front();
                return;
            }
        }
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        rawLocked(text+"\n",error);
    }
public:
    bool active() const { return active_.load(std::memory_order_relaxed); }
    bool interactive() const { return tty_; }

    void start(const ReceiverDashboardState& initial) {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        std::lock_guard<std::mutex> lk(mutex_); if(active()) return;
        state_=initial; events_.clear(); out_.clear(); err_.clear();
        terminalFd_=::open("/dev/tty",O_RDWR|O_CLOEXEC);
        if(terminalFd_<0 && isatty(STDOUT_FILENO)) terminalFd_=::dup(STDOUT_FILENO);
        tty_=terminalFd_>=0; viewport_=0; navigation_.clear(); terminalModeSaved_=false;
        if(tty_ && tcgetattr(terminalFd_,&savedTerminal_)==0) {
            auto mode=savedTerminal_; mode.c_lflag &= ~(ICANON|ECHO); mode.c_cc[VMIN]=0; mode.c_cc[VTIME]=0;
            terminalModeSaved_=tcsetattr(terminalFd_,TCSANOW,&mode)==0;
        }
        started_=std::chrono::steady_clock::now();
        oldOut_=std::cout.rdbuf(); oldErr_=std::cerr.rdbuf();
        const char* configured=std::getenv("NXFRAME_RECEIVER_DIAGNOSTIC_LOG");
        const std::string path=configured && *configured ? configured :
            "receiver_diagnostics_"+std::to_string(::getpid())+".log";
        if(diagnosticLog_.start(path)) {
            rawLocked("[Main] Receiver diagnostics: "+path+"\n");
            diagnosticLog_.submit("[SESSION_START] pid="+std::to_string(::getpid())+"\n");
        } else {
            rawLocked("[WARN] Cannot open receiver diagnostic log: "+path+"\n",true);
        }
        if(tty_) terminalLocked("\033[?1049h\033[?25l");
        std::cout.rdbuf(&out_); std::cerr.rdbuf(&err_); active_.store(true);
    }

    template<class F> void update(F f) {
        if(!active()) return;
        std::lock_guard<std::mutex> lk(mutex_); if(active()) f(state_);
    }


    void recordInterval(const ReceiverDashboardInterval& p) {
        if(!diagnosticLog_.enabled()) return;
        ReceiverDashboardState s;
        { std::lock_guard<std::mutex> lk(mutex_); s=state_; }
        std::ostringstream o; o<<std::fixed<<std::setprecision(3);
        o<<"[INTERVAL] uptime_s="<<p.uptime<<" interval_s="<<p.seconds
         <<" receive_mbps="<<p.receiveMbps<<" decoded_video_fps="<<p.decodedVideoFps
         <<" decoded_audio_fps="<<p.decodedAudioFps<<" received_packets="<<p.receivedPackets
         <<" transport_drops="<<p.transportDrops<<" demux_sync="<<p.demuxSyncErrors
         <<" demux_cc="<<p.demuxContinuityErrors<<" demux_disc="<<p.demuxDiscontinuities
         <<" demux_overflow="<<p.demuxOverflowEvents<<" decoder_drop="<<p.decoderVideoDrops
         <<" acquisition_drop="<<p.acquisitionDrops<<" decoded_vq="<<p.decodedVideoQ
         <<" decoded_aq="<<p.decodedAudioQ<<" packed_aq="<<p.packedAudioQ
         <<" fifo_samples="<<p.fifoSamples<<" queue_av_valid="<<p.queueAvDeltaValid
         <<" queue_av_ms="<<p.queueAvDeltaMs<<" sdi_av_valid="<<p.scheduledAvDeltaValid
         <<" sdi_av_ms="<<p.scheduledAvDeltaMs<<" hw_video_q="<<p.hwVideoQ
         <<" hw_audio_samples="<<p.hwAudioSamples<<" scheduled_video="<<p.scheduledVideo
         <<" output_video="<<p.outputVideoTotal<<" output_v_drop="<<p.outputVideoDrops
         <<" output_a_drop="<<p.outputAudioDrops<<" schedule_fail="<<p.scheduleFailures
         <<" completion_warn="<<p.completionWarnings<<" network="<<receiverDashboardSafe(s.network)
         <<" log_dropped="<<diagnosticLog_.dropped()<<" log_errors="<<diagnosticLog_.errors()<<"\n";
        diagnosticLog_.submit(o.str());
    }

    void render(const ReceiverDashboardInterval& p,bool stopped=false) {
        std::lock_guard<std::mutex> ioLock(ioMutex_); if(!active()) return;
        ReceiverDashboardState state; std::deque<std::string> events;
        { std::lock_guard<std::mutex> lk(mutex_); state=state_; events=events_; }
        auto text=renderReceiverDashboard(state,p,events,stopped);
        if(tty_) {
            winsize w{}; ioctl(terminalFd_,TIOCGWINSZ,&w); if(!w.ws_row) w.ws_row=24; if(!w.ws_col) w.ws_col=80;
            text=renderReceiverDashboard(state,p,events,stopped,std::max(60u,unsigned(w.ws_col)-1));
            std::vector<std::string> rows; std::istringstream input(text); std::string row;
            while(std::getline(input,row)) rows.push_back(row);
            const unsigned usable=w.ws_row>2?w.ws_row-2:1;
            const size_t last=rows.size()>usable?rows.size()-usable:0;
            if(terminalModeSaved_) {
                pollfd fd{terminalFd_,POLLIN,0}; char keys[64];
                if(::poll(&fd,1,0)>0 && (fd.revents&POLLIN)) {
                    auto n=::read(terminalFd_,keys,sizeof(keys)); if(n>0) navigation_.append(keys,static_cast<size_t>(n));
                }
                while(!navigation_.empty()) {
                    if(navigation_[0]!='\033') { navigation_.erase(0,1); continue; }
                    if(navigation_.size()<3) break;
                    size_t end=navigation_.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ~",2); if(end==std::string::npos) break;
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

    void finish(const ReceiverDashboardInterval& p) {
        std::lock_guard<std::mutex> ioLock(ioMutex_); if(!active()) return;
        std::deque<std::string> events;
        { std::lock_guard<std::mutex> lk(mutex_); active_.store(false); events=events_; }
        std::cout.rdbuf(oldOut_); std::cerr.rdbuf(oldErr_);
        if(terminalModeSaved_) { tcsetattr(terminalFd_,TCSANOW,&savedTerminal_); terminalModeSaved_=false; }
        if(tty_) terminalLocked("\033[?25h\033[?1049l");
        if(terminalFd_>=0) { ::close(terminalFd_); terminalFd_=-1; }
        if(tty_) for(const auto& e:events) rawLocked("[EVENT] "+e+"\n");
        std::ostringstream o;
        o<<"[SESSION] duration="<<receiverDashboardClock(p.uptime)
         <<" received_packets="<<p.receivedPackets
         <<" decoded_video="<<p.decodedVideoTotal
         <<" decoded_audio="<<p.decodedAudioTotal
         <<" transport_drops="<<p.transportDrops
         <<" demux_cc_errors="<<p.demuxContinuityErrors
         <<" output_video_drops="<<p.outputVideoDrops<<"\n";
        diagnosticLog_.submit(o.str());
        diagnosticLog_.stop();
        if(diagnosticLog_.dropped() || diagnosticLog_.errors())
            rawLocked("[WARN] Diagnostic log dropped_records="+std::to_string(diagnosticLog_.dropped())+
                      " write_errors="+std::to_string(diagnosticLog_.errors())+"\n",true);
        rawLocked(o.str()); oldOut_=oldErr_=nullptr;
    }
};

inline ReceiverDashboard& receiverDashboard() { static ReceiverDashboard d; return d; }

} // namespace nxframe

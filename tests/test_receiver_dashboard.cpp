#include "core/receiver_dashboard.h"

#include <sstream>
#include <stdexcept>

static void require(bool ok) {
    if (!ok) throw std::runtime_error("receiver dashboard regression failed");
}

int main() {
    nxframe::ReceiverDashboardState s;
    s.source="srt://127.0.0.1:9000";
    s.destination="decklink 0";
    s.transport="SRT";
    s.mode="listener";
    s.network="Connected";
    s.video="h264 | 1920x1080 @ 25.00";
    s.audio="aac | 48000 Hz | 2 ch";
    s.outputDevice="DeckLink 8K Pro";
    s.output="DeckLink scheduled SDI";

    nxframe::ReceiverDashboardInterval p;
    p.uptime=65.0;
    p.receiveMbps=25.0;
    p.decodedVideoFps=25.0;
    p.decodedAudioFps=46.875;
    p.receivedPackets=1000;
    p.decodedVideoTotal=250;
    p.decodedAudioTotal=469;
    p.demuxVideoQ=2;
    p.demuxAudioQ=4;
    p.decodedVideoQ=3;
    p.decodedAudioQ=5;
    p.packedAudioQ=2;
    p.queueAvDeltaValid=true;
    p.queueAvDeltaMs=112.5;
    p.scheduledAvDeltaValid=true;
    p.scheduledAvDeltaMs=0.75;
    p.decklink=true;
    p.outputVideoTotal=245;
    p.hwVideoQ=4;
    p.hwAudioSamples=9600;

    nxframe::SystemResourceMonitor monitor;
    monitor.sample();
    p.resources=monitor.sample();

    for(unsigned width:{79u,96u,119u}) {
        const auto text=nxframe::renderReceiverDashboard(s,p,{},false,width);
        require(text.find("RECEIVER")!=std::string::npos);
        require(text.find("Transport")!=std::string::npos);
        require(text.find("DECODE")!=std::string::npos);
        require(text.find("DEMUX")!=std::string::npos);
        require(text.find("OUTPUT")!=std::string::npos);
        require(text.find("Queue A/V")!=std::string::npos);
        require(text.find("SDI sched")!=std::string::npos);
        require(text.find("A/V:")!=std::string::npos);
        std::istringstream rows(text);
        std::string row;
        while(std::getline(rows,row)) require(row.size()<=width);
    }

    require(nxframe::receiverDashboardSafe("abc\033[2J\r\n").find('\033')==std::string::npos);
    return 0;
}

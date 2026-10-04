/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA. Linux resource sampling.
 */
#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sched.h>

namespace nxframe {
struct ResourceSample {
    bool cpuValid=false, processCpuValid=false, memoryValid=false, processMemoryValid=false;
    double systemCpuPct=0, processCpuPct=0;
    uint64_t rssKiB=0, totalKiB=0, availableKiB=0;
    unsigned logical=0, physical=0, affinity=0;
    std::string system="Linux", cpuModel="Not reported";
};
class SystemResourceMonitor {
    ResourceSample sample_;
    uint64_t lastTotal_=0,lastIdle_=0,lastProcess_=0;
    bool systemPrimed_=false,processPrimed_=false;
    std::chrono::steady_clock::time_point previous_{};
    long ticks_=sysconf(_SC_CLK_TCK);
public:
    SystemResourceMonitor() {
        std::ifstream release("/etc/os-release"); std::string line;
        while(std::getline(release,line)) if(line.compare(0,12,"PRETTY_NAME=")==0) {
            sample_.system=line.substr(12);
            if(sample_.system.size()>1 && sample_.system.front()=='"' && sample_.system.back()=='"')
                sample_.system=sample_.system.substr(1,sample_.system.size()-2);
            break;
        }
        std::ifstream cpuInfo("/proc/cpuinfo");
        while(std::getline(cpuInfo,line)) {
            auto colon=line.find(':');
            if(colon!=std::string::npos && line.substr(0,colon).find("model name")!=std::string::npos) {
                sample_.cpuModel=line.substr(colon+1);
                auto first=sample_.cpuModel.find_first_not_of(" \t");
                sample_.cpuModel=first==std::string::npos?"Not reported":sample_.cpuModel.substr(first);
                break;
            }
        }
        sample_.logical=static_cast<unsigned>(std::max(0L,sysconf(_SC_NPROCESSORS_ONLN)));
        cpu_set_t set; CPU_ZERO(&set);
        if(sched_getaffinity(0,sizeof(set),&set)==0) sample_.affinity=CPU_COUNT(&set);
        std::set<std::pair<int,int>> cores;
        const auto configured=static_cast<unsigned>(std::max(0L,sysconf(_SC_NPROCESSORS_CONF)));
        for(unsigned cpu=0;cpu<configured;++cpu) {
            int online=1; std::ifstream enabled("/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/online");
            if(enabled && enabled>>online && !online) continue;
            std::string path="/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/topology/";
            int package=-1,core=-1;
            std::ifstream p(path+"physical_package_id"), c(path+"core_id");
            if(p>>package && c>>core) cores.emplace(package,core);
        }
        sample_.physical=static_cast<unsigned>(cores.size());
    }
    ResourceSample sample() {
        auto now=std::chrono::steady_clock::now();
        double elapsed=previous_==std::chrono::steady_clock::time_point{}?0:
            std::chrono::duration<double>(now-previous_).count();
        sample_.cpuValid=sample_.processCpuValid=false;
        std::ifstream cpu("/proc/stat"); std::string line;
        if(std::getline(cpu,line)) {
            std::istringstream fields(line); std::string name; uint64_t values[8]{};
            fields>>name; bool ok=name=="cpu";
            for(auto& v:values) if(!(fields>>v)) ok=false;
            uint64_t total=0; for(auto v:values) total+=v;
            uint64_t idle=values[3]+values[4];
            if(ok && systemPrimed_ && total>lastTotal_ && idle>=lastIdle_) {
                auto dt=total-lastTotal_, di=idle-lastIdle_;
                sample_.systemCpuPct=100.0*double(dt-std::min(dt,di))/dt;
                sample_.cpuValid=true;
            }
            if(ok) { lastTotal_=total; lastIdle_=idle; systemPrimed_=true; }
        }
        std::ifstream process("/proc/self/stat");
        if(std::getline(process,line)) {
            // comm is parenthesized and can contain spaces or closing parentheses.
            auto close=line.rfind(')');
            if(close!=std::string::npos) {
                std::istringstream fields(line.substr(close+2)); std::string field;
                uint64_t user=0,system=0; bool ok=true;
                for(int i=0;i<=12;++i) {
                    if(!(fields>>field)) { ok=false; break; }
                    if(i==11 || i==12) {
                        std::istringstream value(field); uint64_t n=0;
                        if(!(value>>n)) ok=false;
                        if(i==11) user=n; else system=n;
                    }
                }
                auto ticks=user+system;
                if(ok && processPrimed_ && ticks>=lastProcess_ && elapsed>0 && ticks_>0) {
                    sample_.processCpuPct=100.0*double(ticks-lastProcess_)/ticks_/elapsed;
                    sample_.processCpuValid=true;
                }
                if(ok) { lastProcess_=ticks; processPrimed_=true; }
            }
        }
        sample_.processMemoryValid=false;
        std::ifstream status("/proc/self/status");
        while(std::getline(status,line)) if(line.compare(0,6,"VmRSS:")==0) {
            std::istringstream v(line.substr(6)); sample_.processMemoryValid=bool(v>>sample_.rssKiB); break;
        }
        bool totalFound=false,availableFound=false;
        std::ifstream memory("/proc/meminfo");
        while(std::getline(memory,line)) {
            std::istringstream values(line); std::string name; uint64_t n=0;
            if(!(values>>name>>n)) continue;
            if(name=="MemTotal:") { sample_.totalKiB=n; totalFound=true; }
            if(name=="MemAvailable:") { sample_.availableKiB=n; availableFound=true; }
        }
        sample_.memoryValid=totalFound && availableFound && sample_.availableKiB<=sample_.totalKiB;
        previous_=now; return sample_;
    }
};
} // namespace nxframe

#include "core/sender_dashboard.h"
#include <stdexcept>
#include <future>
#include <condition_variable>
#include <fstream>
#include <sys/wait.h>
static void require(bool ok) { if(!ok) throw std::runtime_error("dashboard regression failed"); }

// A deliberately blocked output must not block capture-facing state updates.
class BlockingOutput : public std::streambuf {
public:
    std::mutex mutex;
    std::condition_variable condition;
    bool entered=false, released=false;
    std::streamsize xsputn(const char*,std::streamsize n) override {
        std::unique_lock<std::mutex> lock(mutex);
        entered=true; condition.notify_all();
        condition.wait(lock,[&] { return released; });
        return n;
    }
};
static void blockedOutputDoesNotBlockUpdates(const nxframe::DashboardState& state,
                                           const nxframe::DashboardInterval& interval)
{
    BlockingOutput output;
    auto* original=std::cout.rdbuf(&output);
    nxframe::SenderDashboard d;
    d.start(state,false);
    if (d.interactive()) { output.released=true; d.finish(interval); std::cout.rdbuf(original); return; }
    std::thread renderer([&] { d.render(interval); });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(output.mutex);
        entered=output.condition.wait_for(lock,std::chrono::seconds(2),[&] { return output.entered; });
    }
    auto writer=std::async(std::launch::async,[&] {
        d.update([](nxframe::DashboardState& s) { s.signal="LOCKED"; });
    });
    const bool completed=writer.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.released=true;
    }
    output.condition.notify_all(); renderer.join(); writer.get();
    d.finish(interval); std::cout.rdbuf(original);
    require(entered && completed);
}

static void diagnosticHistory(const nxframe::DashboardState& s,const nxframe::DashboardInterval& p) {
    char name[]="/tmp/nxframe_diagnostics_XXXXXX";
    const int fd=mkstemp(name); require(fd>=0); close(fd);
    require(setenv("NXFRAME_DIAGNOSTIC_LOG",name,1)==0);
    nxframe::SenderDashboard d;
    d.start(s,false,true);
    require(d.diagnosticsEnabled());
    // Diagnostic records survive the dashboard's non-verbose display filter.
    std::cerr<<"[DIAG] retained in file only\n";
    std::vector<std::thread> producers;
    for(int i=0;i<4;++i) producers.emplace_back([i] {
        for(int j=0;j<10;++j) std::cerr<<"[WARN] producer"<<i<<" record"<<j<<"\n";
    });
    for(auto& t:producers) t.join();
    d.recordInterval(p); d.finish(p);
    std::ifstream file(name); std::ostringstream contents; contents<<file.rdbuf();
    const auto text=contents.str();
    require(text.find("[DIAG] retained in file only")!=std::string::npos);
    require(text.find("capture_drop_delta=8")!=std::string::npos);
    require(text.find("stage=video_encode_zc avg_ms=22.000 max_session_ms=91.000 calls=44")!=std::string::npos);
    require(text.find("[SESSION]")!=std::string::npos);
    require(text.find("dropped_records=0 write_errors=0")!=std::string::npos);
    for(int i=0;i<4;++i) for(int j=0;j<10;++j)
        require(text.find("producer"+std::to_string(i)+" record"+std::to_string(j))!=std::string::npos);
    nxframe::SenderDiagnosticLog logger;
    require(!logger.start("/dev/null"));
    const std::string fifo=std::string(name)+".fifo";
    require(mkfifo(fifo.c_str(),0600)==0); require(!logger.start(fifo)); unlink(fifo.c_str());
    require(logger.start(name));
    logger.submit(std::string(65537,'x')); require(logger.dropped()==1);
    logger.submit("[RESTART] works\n"); logger.stop();
    std::ifstream restarted(name); std::ostringstream history; history<<restarted.rdbuf();
    require(history.str().find("[RESTART] works")!=std::string::npos);
    require(history.str().find("dropped_records=1 write_errors=0")!=std::string::npos);
    unlink(name); unsetenv("NXFRAME_DIAGNOSTIC_LOG");
}

static void diagnosticOwnership() {
    char directory[]="/tmp/nxframe_log_owner_XXXXXX";
    require(mkdtemp(directory)!=nullptr); require(chmod(directory,0755)==0);
    const std::string path=std::string(directory)+"/diagnostics.log";
    const char* user=std::getenv("SUDO_UID"); const char* group=std::getenv("SUDO_GID");
    const bool hadUser=user!=nullptr,hadGroup=group!=nullptr;
    const std::string oldUser=user?user:"",oldGroup=group?group:"";
    nxframe::SenderDiagnosticLog logger;
    // Some isolated build environments map only UID/GID 0. Still exercise
    // descriptor-based ownership there, but skip switching to an unmapped ID.
    const bool mappedUser=geteuid()==0 && chown(directory,65534,65534)==0;
    if(mappedUser) require(chown(directory,0,0)==0);
    const unsigned owner=mappedUser?65534:static_cast<unsigned>(geteuid());
    const unsigned ownerGroup=mappedUser?65534:static_cast<unsigned>(getegid());
    if(geteuid()==0) {
        setenv("SUDO_UID",std::to_string(owner).c_str(),1);
        setenv("SUDO_GID",std::to_string(ownerGroup).c_str(),1);
    }
    require(logger.start(path)); logger.submit("readable diagnostic\n"); logger.stop();
    struct stat info{}; require(stat(path.c_str(),&info)==0);
    require((info.st_mode & 0777)==0600);
    require(info.st_uid==owner);
    require(info.st_gid==ownerGroup);
    if(geteuid()==0) {
        const auto child=fork(); require(child>=0);
        if(child==0) {
            if(mappedUser && (setgid(65534)!=0 || setuid(65534)!=0)) _exit(2);
            const int fd=open(path.c_str(),O_RDONLY); if(fd<0) _exit(3);
            char data[32]; const auto n=read(fd,data,sizeof(data)); close(fd); _exit(n>0?0:4);
        }
        int status=0; require(waitpid(child,&status,0)==child);
        require(WIFEXITED(status) && WEXITSTATUS(status)==0);
        // An existing file's owner/mode must not change when appending.
        require(chmod(path.c_str(),0640)==0);
        setenv("SUDO_UID","65533",1); setenv("SUDO_GID","65533",1);
        require(logger.start(path)); logger.stop(); require(stat(path.c_str(),&info)==0);
        require(info.st_uid==owner && info.st_gid==ownerGroup && (info.st_mode&0777)==0640);
        const std::string invalid=std::string(directory)+"/invalid.log";
        for(const char* value:{"-1","4294967295","999999999999999999999999","12bad",""}) {
            setenv("SUDO_UID",value,1); require(!logger.start(invalid)); unlink(invalid.c_str());
        }
    }
    if(hadUser) setenv("SUDO_UID",oldUser.c_str(),1); else unsetenv("SUDO_UID");
    if(hadGroup) setenv("SUDO_GID",oldGroup.c_str(),1); else unsetenv("SUDO_GID");
    unlink(path.c_str()); rmdir(directory);
}

int main(int argc,char**) {
    nxframe::DashboardState s; s.input="1920x1080p50"; s.signal="LOCKED"; s.encoder="x265 / veryfast | Main422-10 | Target 35 Mbps";
    nxframe::DashboardInterval p; p.seconds=1; p.capture=50; p.encoded=44; p.evictions=89; p.evictionDelta=8;
    p.timing["video_encode_zc"]={22,91,44};
    nxframe::SystemResourceMonitor monitor; monitor.sample();
    auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(30);
    volatile uint64_t work=0; while(std::chrono::steady_clock::now()<until) ++work;
    p.resources=monitor.sample();
    require(p.resources.cpuValid && p.resources.processCpuValid);
    require(p.resources.systemCpuPct>=0 && p.resources.systemCpuPct<=100);
    require(p.resources.processCpuPct>0);
    require(p.resources.memoryValid && p.resources.processMemoryValid && p.resources.rssKiB>0);
    require(p.resources.logical>0 && p.resources.affinity>0);
    s.device="DeckLink 0"; s.codec="HEVC / x265"; s.preset="veryfast";
    auto text=nxframe::renderSenderDashboard(s,p,{});
    require(text.find("SYSTEM") < text.find("INPUT"));
    require(!p.resources.system.empty() && !p.resources.cpuModel.empty());
    for(unsigned width:{79u,93u,119u}) {
        auto resized=nxframe::renderSenderDashboard(s,p,{},false,false,width);
        require(resized.find("Preset:")==resized.find("ENCODER")+width/2);
        std::istringstream lines(resized); std::string line;
        while(std::getline(lines,line)) require(line.size()<=width);
    }
    require(text.find("SYSTEM")!=std::string::npos);
    require(text.find("CPU USAGE")!=std::string::npos);
    std::istringstream rows(text); std::string row; unsigned count=0;
    while(std::getline(rows,row)) { require(row.size()<=96); ++count; }
    require(count<=58);
    require(text.find("Preset:")==text.find("ENCODER")+48);
    require(text.find("Capture: +8 / total 89")!=std::string::npos);
    require(text.find("MAXIMUM (session)")!=std::string::npos);
    require(text.find("no sample")!=std::string::npos);
    require(nxframe::dashboardSafe("abc\033[2J\r\n").find('\033')==std::string::npos);
    s.inputSample=true; s.inputUpdated=std::chrono::steady_clock::now()-std::chrono::seconds(2);
    require(nxframe::renderSenderDashboard(s,p,{}).find("STALE")!=std::string::npos);
    s.inputSample=false;
    blockedOutputDoesNotBlockUpdates(s,p);
    diagnosticHistory(s,p);
    diagnosticOwnership();
    auto& d=nxframe::senderDashboard(); d.start(s,false);
    std::thread t([] { std::cerr<<"[WARN] test event\n"; }); t.join();
    std::cout<<"[SRT] stats hidden\n";
    d.render(p); d.finish(p);
    if(argc>1) { d.start(s,true); std::cout<<"[DIAG] visible in verbose\n"; d.finish(p); }
    std::cout<<"PASS: dashboard renderer, routing and stream restoration\n";
}

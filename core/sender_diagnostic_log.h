/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nxframe {
// Producers never perform file I/O. A slow disk can lose diagnostic records,
// but cannot create an unbounded backlog or block a capture/encode producer.
class SenderDiagnosticLog {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool stopping_=false;
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> dropped_{0}, errors_{0};
    int fd_=-1;
    bool writeRecord(const std::string& text) {
        size_t offset=0;
        while(offset<text.size()) {
            const auto n=::write(fd_,text.data()+offset,text.size()-offset);
            if(n<0 && errno==EINTR) continue;
            if(n<=0) { ++errors_; return false; }
            offset+=static_cast<size_t>(n);
        }
        return true;
    }
    template<class T> static bool parseId(const char* text,T& result) {
        if(!text || !*text) return false;
        for(const char* p=text;*p;++p) if(*p<'0' || *p>'9') return false;
        errno=0; char* end=nullptr;
        const auto value=std::strtoull(text,&end,10);
        // All-ones UID/GID means "leave unchanged" to fchown, not a user.
        if(errno || !end || *end || value>=std::numeric_limits<T>::max()) return false;
        result=static_cast<T>(value); return true;
    }
    static bool assignCreator(int fd) {
        if(::geteuid()!=0) return true;
        const char* user=std::getenv("SUDO_UID");
        const char* group=std::getenv("SUDO_GID");
        if(!user && !group) return true; // Direct root/service execution.
        uid_t uid{}; gid_t gid{};
        if(!parseId(user,uid) || !parseId(group,gid)) return false;
        return ::fchown(fd,uid,gid)==0;
    }
    void run() {
        for(;;) {
            std::string record;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock,[&] { return stopping_ || !queue_.empty(); });
                if(queue_.empty()) break;
                record=std::move(queue_.front()); queue_.pop_front();
            }
            if(!writeRecord(record)) {
                enabled_.store(false);
                std::lock_guard<std::mutex> lock(mutex_);
                dropped_.fetch_add(queue_.size()); queue_.clear();
                break;
            }
        }
        writeRecord("[DIAGNOSTIC_LOG] dropped_records="+std::to_string(dropped_.load())+
                    " write_errors="+std::to_string(errors_.load())+"\n");
    }
public:
    ~SenderDiagnosticLog() { stop(); }
    bool start(const std::string& path) {
        stop();
        // Reject devices/FIFOs and symlinks: diagnostics must be a regular file.
        const int flags=O_WRONLY|O_APPEND|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW;
        fd_=::open(path.c_str(),flags|O_CREAT|O_EXCL,0600);
        const bool created=fd_>=0;
        if(fd_<0 && errno==EEXIST) fd_=::open(path.c_str(),flags);
        // Use the descriptor, not a second path lookup. Only files created by
        // this session inherit the invoking sudo user's ownership.
        if(created && !assignCreator(fd_)) { ::close(fd_); fd_=-1; return false; }
        struct stat info{};
        if(fd_<0) return false;
        if(::fstat(fd_,&info)!=0 || !S_ISREG(info.st_mode)) { ::close(fd_); fd_=-1; return false; }
        stopping_=false; dropped_=0; errors_=0; enabled_=true;
        try { worker_=std::thread([this] { run(); }); }
        catch(...) { enabled_=false; ::close(fd_); fd_=-1; return false; }
        return true;
    }
    void submit(const std::string& record) {
        if(!enabled_.load()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!enabled_.load() || stopping_) return;
            if(queue_.size()>=256 || record.size()>65536) { ++dropped_; return; }
            queue_.push_back(record);
        }
        ready_.notify_one();
    }
    void stop() {
        enabled_=false;
        { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
        ready_.notify_one();
        if(worker_.joinable()) worker_.join();
        if(fd_>=0) { ::close(fd_); fd_=-1; }
    }
    bool enabled() const { return enabled_.load(); }
    uint64_t dropped() const { return dropped_.load(); }
    uint64_t errors() const { return errors_.load(); }
};
} // namespace nxframe

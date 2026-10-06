#include "core/video_queue_delay.h"
#include "core/bounded_queue.h"
#include <limits>
#include <stdexcept>
#include <thread>
static void require(bool ok) { if(!ok) throw std::runtime_error("queue delay regression"); }
int main() {
    nxframe::VideoQueueDelay d;
    require(d.takeInterval().count==0);
    d.record(-1); d.record(std::numeric_limits<double>::infinity());
    d.record(std::numeric_limits<double>::quiet_NaN());
    for(int i=100;i>=1;--i) d.record(i);
    auto s=d.takeInterval();
    require(s.count==100 && s.avgMs==50.5 && s.p95Ms==95 && s.maxMs==100 && s.omitted==0);
    require(d.takeInterval().count==0);
    for(int i=0;i<4100;++i) d.record(1);
    s=d.takeInterval();
    require(s.count==4096 && s.omitted==4 && s.p95Ms==1);
    require(d.takeInterval().omitted==0);
    std::thread producer([&] { for(int i=0;i<2000;++i) d.record(2); });
    uint64_t total=0;
    while(total<2000) { s=d.takeInterval(); total+=s.count; require(s.omitted==0); std::this_thread::yield(); }
    producer.join();
    for(size_t slots:{size_t(2),size_t(4)}) {
        BoundedQueue<int> q(slots);
        for(int i=0;i<6;++i) require(q.push_drop_oldest(i));
        require(q.size()==slots && q.evicted_oldest()==6-slots);
        int value=-1; require(q.pop(value) && value==int(6-slots));
        q.stop(); require(!q.push_drop_oldest(9));
    }
}

#include "app/HostAutomation.hpp"
#include <atomic>
#include <array>
#include <iostream>
#include <thread>

int main() {
    using namespace flows::app;
    struct Map { std::uint64_t revision=0; std::array<std::uint64_t,32> values{}; };
    LatestMailbox<Map> mailbox;
    for(std::uint64_t i=1;i<=1000;++i) { Map m; m.revision=i; m.values.fill(i); mailbox.push(m); }
    Map received;
    if(!mailbox.pop(received) || received.revision!=1000 || mailbox.pop(received)) return 1;
    std::atomic<bool> done=false;
    std::thread publisher([&] { for(std::uint64_t i=1001;i<=100000;++i) { Map m; m.revision=i; m.values.fill(i); mailbox.push(m); } done.store(true); });
    bool valid=true; std::uint64_t previous=1000;
    do {
        if(mailbox.pop(received)) {
            valid=valid && received.revision>previous;
            for(auto value:received.values) valid=valid && value==received.revision;
            previous=received.revision;
        }
    } while(!done.load() || previous<100000);
    publisher.join();
    if(!valid) return 2;
    HostQueue<std::uint32_t,256> events;
    for(std::uint32_t i=0;i<256;++i) if(!events.push(i)) return 3;
    if(events.push(256)) return 4;
    for(std::uint32_t i=0;i<256;++i) { std::uint32_t value; if(!events.pop(value) || value!=i) return 5; }
    std::uint32_t value; if(events.pop(value)) return 6;
    std::cout<<"Latest macro map and bounded host event queues passed.\n";
}

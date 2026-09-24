#include "buttons.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
struct Trace {
    edl::Buttons buttons;
    uint32_t now = 0;
    std::vector<uint8_t> events;
    void sample(uint8_t mask, unsigned duration) {
        for (unsigned i = 0; i < duration; ++i) {
            auto event = buttons.update(mask, now++);
            if (event) events.push_back(event);
        }
    }
};
int main() {
    { Trace t; t.sample(0,20); t.sample(1,20); t.sample(0,400); assert(t.events == std::vector<uint8_t>{1}); }
    { Trace t; t.sample(0,20); t.sample(2,20); t.sample(0,400); assert(t.events == std::vector<uint8_t>{3}); }
    { Trace t; t.sample(0,20); t.sample(1,3); t.sample(0,3); t.sample(1,3); t.sample(0,400); assert(t.events.empty()); }
    { Trace t; t.sample(0,20); t.sample(1,20); t.sample(0,80); t.sample(1,20); t.sample(0,400); assert(t.events == std::vector<uint8_t>{7}); }
    { Trace t; t.sample(0,20); t.sample(2,20); t.sample(0,80); t.sample(2,20); t.sample(0,400); assert(t.events == std::vector<uint8_t>{8}); }
    { Trace t; t.sample(0,20); t.sample(1,1100); t.sample(0,400); assert(t.events == std::vector<uint8_t>{2}); }
    { Trace t; t.sample(0,20); t.sample(3,20); t.sample(0,400); assert(t.events == std::vector<uint8_t>{5}); }
    { Trace t; t.sample(0,20); t.sample(3,1100); t.sample(0,400); assert(t.events == std::vector<uint8_t>{6}); }
    { Trace t; t.sample(0,20); t.sample(1,20); t.sample(0,400); t.sample(1,20); t.sample(0,400); assert((t.events == std::vector<uint8_t>{1,1})); }
}

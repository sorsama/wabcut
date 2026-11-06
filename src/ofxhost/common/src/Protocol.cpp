#include <wabcut/ofxhost/Protocol.h>

#include <cstdio>

namespace wabcut::ofxhost {

std::string blockName(std::uint32_t hostPid, std::uint64_t instance, std::uint32_t generation) {
    char name[96];
    std::snprintf(name, sizeof name, "Local\\wabcut-ofx-%u-%llu-%u", hostPid, static_cast<unsigned long long>(instance), generation);
    return name;
}

} // namespace wabcut::ofxhost

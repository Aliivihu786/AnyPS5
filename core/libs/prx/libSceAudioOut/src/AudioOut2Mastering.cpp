#include <cstdint>
#include <stdexcept>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAudioOut2MasteringInit(uint32_t flags) {
    if (flags != 0) {
        throw std::runtime_error("sceAudioOut2MasteringInit: flags must be 0");
    }
    return 0;
}

int APS5_VABI sceAudioOut2MasteringTerm(void) {
    return 0;
}

int APS5_VABI sceAudioOut2MasteringGetState(AudioOut2MasteringStatesHeader* state, uint32_t output, AudioOut2UserHandle user) {
    (void)output;
    (void)user;
    if (state == nullptr) {
        return static_cast<int>(0x80260502);
    }
    state->states_id = 0;
    return 0;
}

int APS5_VABI sceAudioOut2MasteringSetParam(const AudioOut2MasteringParamsHeader* param, uint32_t output, uint32_t flags) {
    (void)output;
    (void)flags;
    if (param == nullptr) {
        throw std::runtime_error("sceAudioOut2MasteringSetParam: param is null");
    }
    return 0;
}

}

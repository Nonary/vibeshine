#pragma once
#if defined(_WIN32) && defined(VIBE_AUDIO_TEST_INSTRUMENTATION)
#include "../tests/host714/support/usb_metrics.h"
#else
namespace audio {
class usb_lab_metrics {
public:
 explicit usb_lab_metrics(const atmos_format_t&){}
 static std::uint64_t now(){return 0;}
 constexpr bool enabled()const{return false;}
 void next_section(){}
 template<class... Args>void record(Args&&...){}
};
}
#endif

//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  \\ harry denholm \\ ishani            ishani.org/shelf/ouroveon/
//

#include "pch.h"

#include "math/rng.h"

#if OURO_PLATFORM_OSX
#include <mach/mach_time.h>
#endif

namespace math {

#if OURO_PLATFORM_LINUX
static uint64_t GetTickCountMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_nsec / 1000000) + ((uint64_t)ts.tv_sec * 1000ull);
}
#endif 

//----------------------------------------------------------------------------------------------------------------------
void RNG32::reseed()
{
    // by default, seed with the ms-since-system-start ( or similar )
    reseed((uint32_t)(
#if OURO_PLATFORM_WIN
        ::GetTickCount64()
#elif OURO_PLATFORM_LINUX
        GetTickCountMs()
#elif OURO_PLATFORM_OSX
        mach_absolute_time()
#else
        #error oh dear
#endif
    & UINT32_MAX ));
}

//----------------------------------------------------------------------------------------------------------------------
RNG32::RNG32()
{
    reseed();
}

} // namespace math
// SAIGE RHI — build configuration and diagnostics shims.
//
// Until Core publishes its logging/assert facilities (issue #2, D-11), the RHI uses these local macros.
// If Core defines SAIGE_ASSERT before this header is included, RHI asserts route to it automatically.
#pragma once

#if defined(SAIGE_ASSERT)
#    define SAIGE_RHI_ASSERT(cond, msg) SAIGE_ASSERT(cond, msg)
#else
#    include <cassert>
#    define SAIGE_RHI_ASSERT(cond, msg) assert((cond) && (msg))
#endif

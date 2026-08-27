//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Minimal, dependency-free logging shared by the qdc-tunnel target binary and the
// qdc-tunnel-host binary. Writes to stderr; verbosity is toggled at runtime via
// SetVerbose().

#ifndef QDC_TUNNEL_LOGGING_H
#define QDC_TUNNEL_LOGGING_H

#include <cstdio>

namespace qdc
{
    namespace Log
    {
        // Global verbose flag (set from --verbose). Defined in Logging.cpp.
        extern bool g_verbose;

        inline void SetVerbose(bool v) { g_verbose = v; }
    }
}

// Info/warn/error always print; verbose only when enabled.
#define QDC_LOGI(...)                                                          \
    do {                                                                       \
        std::fprintf(stderr, "[qdc-tunnel][I] " __VA_ARGS__);                  \
        std::fprintf(stderr, "\n");                                            \
    } while (0)

#define QDC_LOGW(...)                                                          \
    do {                                                                       \
        std::fprintf(stderr, "[qdc-tunnel][W] " __VA_ARGS__);                  \
        std::fprintf(stderr, "\n");                                            \
    } while (0)

#define QDC_LOGE(...)                                                          \
    do {                                                                       \
        std::fprintf(stderr, "[qdc-tunnel][E] " __VA_ARGS__);                  \
        std::fprintf(stderr, "\n");                                            \
    } while (0)

#define QDC_LOGV(...)                                                          \
    do {                                                                       \
        if (qdc::Log::g_verbose) {                                             \
            std::fprintf(stderr, "[qdc-tunnel][V] " __VA_ARGS__);              \
            std::fprintf(stderr, "\n");                                        \
        }                                                                      \
    } while (0)

#endif // QDC_TUNNEL_LOGGING_H

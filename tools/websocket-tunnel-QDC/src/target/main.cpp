//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// qdc-tunnel-target: device-side reverse tunnel for QDC targets (Android or Qualcomm
// Linux / IoT).
//
// Runs on the QDC-hosted device (pushed via `adb push`/`scp` and started via
// `adb shell`/`ssh`). For each --port-map TCP:WS it opens:
//   - a TCP listener on the device-local TCP port (profiler clients connect here),
//   - a server socket on WS port (the SDP host connects here, via adb-forward or
//     ssh -L),
// and multiplexes many device-local streams over the single host connection.
//
// This is the C++ replacement for the original Java TunnelTargetReverse /
// TargetTunnelPort (which required app_process + a DEX jar on Android, or a bundled
// JRE on IoT). It is a single static ELF with no JVM / third-party dependencies, so
// deploying it is a single small push instead of a multi-megabyte runtime bundle.
//
// Usage:
//   qdc-tunnel-target --port-map 6500:8900 --port-map 6502:8902 [--bind-host 0.0.0.0] [-v]

#include "Logging.h"
#include "TunnelPort.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct PortMapping
    {
        uint16_t tcpPort;
        uint16_t wsPort;
    };

    std::vector<std::unique_ptr<qdc::TunnelPort>> g_tunnels;

    void PrintUsage()
    {
        std::fprintf(stderr,
            "qdc-tunnel-target - QDC device-side reverse tunnel\n"
            "\n"
            "Usage:\n"
            "  qdc-tunnel-target --port-map <tcpPort>:<wsPort> [--port-map ...] [options]\n"
            "\n"
            "Options:\n"
            "  --port-map TCP:WS   Port mapping (repeatable). TCP = device-local port\n"
            "                      profiler clients connect to; WS = port the SDP host\n"
            "                      connects into (reached via adb-forward or ssh -L).\n"
            "  --bind-host HOST    Bind address for listeners (default: 0.0.0.0).\n"
            "  -v, --verbose       Enable verbose logging.\n"
            "  -h, --help          Show this help.\n"
            "\n"
            "Example:\n"
            "  qdc-tunnel-target --port-map 6500:8900 --port-map 6502:8902\n");
    }

    bool ParsePortMap(const char* arg, PortMapping& out)
    {
        const char* colon = std::strchr(arg, ':');
        if (colon == nullptr)
        {
            return false;
        }
        std::string tcpStr(arg, colon);
        std::string wsStr(colon + 1);
        if (tcpStr.empty() || wsStr.empty())
        {
            return false;
        }

        long tcp = std::strtol(tcpStr.c_str(), nullptr, 10);
        long ws = std::strtol(wsStr.c_str(), nullptr, 10);
        if (tcp <= 0 || tcp > 65535 || ws <= 0 || ws > 65535)
        {
            return false;
        }
        out.tcpPort = static_cast<uint16_t>(tcp);
        out.wsPort = static_cast<uint16_t>(ws);
        return true;
    }

    void HandleSignal(int /*sig*/)
    {
        for (auto& t : g_tunnels)
        {
            if (t)
            {
                t->Stop();
            }
        }
    }
}

int main(int argc, char** argv)
{
    std::string bindHost = "0.0.0.0";
    std::vector<PortMapping> mappings;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--port-map")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "ERROR: --port-map requires an argument\n");
                PrintUsage();
                return 1;
            }
            PortMapping pm{};
            if (!ParsePortMap(argv[++i], pm))
            {
                std::fprintf(stderr, "ERROR: invalid --port-map '%s' (expected TCP:WS)\n", argv[i]);
                return 1;
            }
            mappings.push_back(pm);
        }
        else if (arg == "--bind-host")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "ERROR: --bind-host requires an argument\n");
                return 1;
            }
            bindHost = argv[++i];
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            qdc::Log::SetVerbose(true);
        }
        else if (arg == "-h" || arg == "--help")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "ERROR: unknown argument '%s'\n", arg.c_str());
            PrintUsage();
            return 1;
        }
    }

    if (mappings.empty())
    {
        std::fprintf(stderr, "ERROR: at least one --port-map is required\n");
        PrintUsage();
        return 1;
    }

    // Ignore SIGPIPE so a broken socket write returns EPIPE instead of killing us.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    // Create a tunnel per mapping.
    for (const auto& pm : mappings)
    {
        g_tunnels.push_back(std::make_unique<qdc::TunnelPort>(bindHost, pm.tcpPort, pm.wsPort));
    }

    QDC_LOGI("starting %zu tunnel(s)", g_tunnels.size());

    // Run each tunnel in its own thread.
    std::vector<std::thread> threads;
    for (auto& t : g_tunnels)
    {
        qdc::TunnelPort* tunnel = t.get();
        threads.emplace_back([tunnel]()
        {
            if (!tunnel->Run())
            {
                QDC_LOGE("[%s] failed to start", tunnel->Label().c_str());
            }
        });
    }

    for (auto& th : threads)
    {
        if (th.joinable())
        {
            th.join();
        }
    }

    QDC_LOGI("all tunnels stopped");
    return 0;
}

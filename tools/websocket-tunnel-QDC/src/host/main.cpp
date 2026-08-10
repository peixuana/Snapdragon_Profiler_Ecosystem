//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// qdc-tunnel-host: host-side end of the QDC reverse tunnel. Runs on the machine
// running Snapdragon Profiler (Linux or Windows). For each --port-map WS:FWD it
// dials a WebSocket into the device tunnel's ws port and forwards each multiplexed
// stream to a local TCP service (forward port, where SDPCore listens).
//
// This is the C++ / cross-platform replacement for the Java TunnelHostReverse.
//
// Usage:
//   qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502
//   qdc-tunnel-host --remote-host <ip> --remote-port 8900 --forward-port 6500

#include "HostTunnelPort.h"
#include "Logging.h"
#include "NetCompat.h"

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
        uint16_t wsPort;  // remote ws port to connect to
        uint16_t fwdPort; // local forward port to deliver to
    };

    std::vector<std::unique_ptr<qdc::HostTunnelPort>> g_tunnels;

    void PrintUsage()
    {
        std::fprintf(stderr,
            "qdc-tunnel-host - QDC host-side reverse tunnel\n"
            "\n"
            "Usage:\n"
            "  qdc-tunnel-host --remote-host HOST --port-map <wsPort>:<fwdPort> [--port-map ...] [options]\n"
            "\n"
            "Options:\n"
            "  --remote-host HOST   Host running the device WebSocket server, reached\n"
            "                       via ssh -L / adb-forward (usually 127.0.0.1). Required.\n"
            "  --port-map WS:FWD    Port mapping (repeatable). WS = remote ws port to\n"
            "                       connect to; FWD = local TCP port to forward to\n"
            "                       (where SDPCore listens).\n"
            "  --forward-host HOST  Host to forward to (default: 127.0.0.1).\n"
            "  --remote-port PORT   Single-port legacy alternative to --port-map (ws port).\n"
            "  --forward-port PORT  Single-port legacy alternative to --port-map (fwd port).\n"
            "  --no-reconnect       Do not reconnect after the tunnel drops.\n"
            "  -v, --verbose        Enable verbose logging.\n"
            "  -h, --help           Show this help.\n"
            "\n"
            "Example:\n"
            "  qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502\n");
    }

    bool ParsePort(const char* arg, uint16_t& out)
    {
        long v = std::strtol(arg, nullptr, 10);
        if (v <= 0 || v > 65535)
        {
            return false;
        }
        out = static_cast<uint16_t>(v);
        return true;
    }

    bool ParsePortMap(const char* arg, PortMapping& out)
    {
        const char* colon = std::strchr(arg, ':');
        if (colon == nullptr)
        {
            return false;
        }
        std::string wsStr(arg, colon);
        std::string fwdStr(colon + 1);
        if (wsStr.empty() || fwdStr.empty())
        {
            return false;
        }
        return ParsePort(wsStr.c_str(), out.wsPort) && ParsePort(fwdStr.c_str(), out.fwdPort);
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
    std::string remoteHost;
    std::string forwardHost = "127.0.0.1";
    std::vector<PortMapping> mappings;
    uint16_t singleWsPort = 0;
    uint16_t singleFwdPort = 0;
    bool reconnect = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto needArg = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "ERROR: %s requires an argument\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--remote-host")
        {
            remoteHost = needArg("--remote-host");
        }
        else if (arg == "--forward-host")
        {
            forwardHost = needArg("--forward-host");
        }
        else if (arg == "--port-map")
        {
            PortMapping pm{};
            if (!ParsePortMap(needArg("--port-map"), pm))
            {
                std::fprintf(stderr, "ERROR: invalid --port-map '%s' (expected WS:FWD)\n", argv[i]);
                return 1;
            }
            mappings.push_back(pm);
        }
        else if (arg == "--remote-port")
        {
            if (!ParsePort(needArg("--remote-port"), singleWsPort))
            {
                std::fprintf(stderr, "ERROR: invalid --remote-port\n");
                return 1;
            }
        }
        else if (arg == "--forward-port")
        {
            if (!ParsePort(needArg("--forward-port"), singleFwdPort))
            {
                std::fprintf(stderr, "ERROR: invalid --forward-port\n");
                return 1;
            }
        }
        else if (arg == "--no-reconnect")
        {
            reconnect = false;
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

    // Collapse the single-port legacy flags into a one-element port-map.
    if (mappings.empty() && singleWsPort != 0 && singleFwdPort != 0)
    {
        mappings.push_back(PortMapping{ singleWsPort, singleFwdPort });
    }

    if (remoteHost.empty())
    {
        std::fprintf(stderr, "ERROR: --remote-host is required\n");
        PrintUsage();
        return 1;
    }
    if (mappings.empty())
    {
        std::fprintf(stderr, "ERROR: at least one --port-map (or --remote-port/--forward-port) is required\n");
        PrintUsage();
        return 1;
    }

    if (!qdc::net::Init())
    {
        std::fprintf(stderr, "ERROR: network init failed\n");
        return 1;
    }
    qdc::net::IgnoreSigpipe();
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    for (const auto& pm : mappings)
    {
        g_tunnels.push_back(std::make_unique<qdc::HostTunnelPort>(
            remoteHost, pm.wsPort, forwardHost, pm.fwdPort, reconnect));
    }

    QDC_LOGI("starting %zu host tunnel(s) to %s", g_tunnels.size(), remoteHost.c_str());

    std::vector<std::thread> threads;
    for (auto& t : g_tunnels)
    {
        qdc::HostTunnelPort* tunnel = t.get();
        threads.emplace_back([tunnel]() { tunnel->Run(); });
    }

    for (auto& th : threads)
    {
        if (th.joinable())
        {
            th.join();
        }
    }

    qdc::net::Shutdown();
    QDC_LOGI("all host tunnels stopped");
    return 0;
}

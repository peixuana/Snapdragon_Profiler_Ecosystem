# ============================================================================================================
# 
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
# 
# ============================================================================================================ 


"""Snapdragon Profiler Ecosystem – unified CLI entry point.

Usage:
    python main.py <tool> [tool-specific arguments ...]

Available tools are registered in TOOLS_REGISTRY below.  To add a new tool,
simply create a module under ``tools/`` with a ``main()`` function that uses
``argparse``, then add one line to the registry mapping.

Examples:
    python main.py converter input.csv -o output.json
    python main.py converter input1.csv input2.csv --output-dir output/
    python main.py split-renderstage input.pftrace output.pftrace
"""

import importlib
import sys

# ---------------------------------------------------------------------------
# Tool registry – maps CLI name → dotted module path.
# Each module MUST expose a ``main()`` function (using argparse internally).
# ---------------------------------------------------------------------------
TOOLS_REGISTRY: dict[str, str] = {
    "converter": "tools.converter.convert_sdp_csv_to_perfetto_json",
    "split-renderstage": "tools.converter.split_renderstage_by_process",
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        _print_help()
        sys.exit(0)

    tool_name = sys.argv[1]

    if tool_name not in TOOLS_REGISTRY:
        print(f"Error: Unknown tool '{tool_name}'.")
        print(f"Available tools: {', '.join(sorted(TOOLS_REGISTRY))}")
        sys.exit(1)

    module_path = TOOLS_REGISTRY[tool_name]
    module = importlib.import_module(module_path)

    # Remove the dispatcher's own argv[0] and the tool name so the tool's
    # argparse sees only its own arguments.
    sys.argv = [f"main.py {tool_name}"] + sys.argv[2:]
    module.main()


def _print_help():
    print("Snapdragon Profiler Ecosystem – unified CLI entry point\n")
    print("Usage: python main.py <tool> [tool-specific arguments ...]\n")
    print("Available tools:")
    for name, mod in sorted(TOOLS_REGISTRY.items()):
        print(f"  {name:20s} → {mod}")
    print("\nPass -h/--help after a tool name for tool-specific help.")
    print("Example: python main.py converter --help")
    print("Example: python main.py split-renderstage --debug input.pftrace")


if __name__ == "__main__":
    main()
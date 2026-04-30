# ============================================================================================================
# 
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
# 
# ============================================================================================================ 

"""Backward-compatible test entry point.

The test suite has moved to tests/test_convert_sdp_csv_to_perfetto_json.py.
This wrapper re-exports everything so that ``pytest test_main.py`` still works.
"""

from tests.test_convert_sdp_csv_to_perfetto_json import *  # noqa: F401,F403
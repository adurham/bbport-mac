// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the frame statistics report (BB_FRAME_STATS), taken at each flip.
#pragma once

namespace BbVideoOut {
bool FrameStatsEnabled();
void OnFlipStats();
} // namespace BbVideoOut

/// bbport: a marker line in the frame log (BB_FRAME_LOG), written by the log's own stream — a
/// second stream on the same path would write where the log's offset has already passed, and the
/// log's next flush would write over it. The capture hotkey (window.cpp) marks its screenshot with
/// one, so the frames around it can be found in the CSV.
extern "C" void bb_frame_log_marker(const char* text);

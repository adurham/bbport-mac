// SPDX-License-Identifier: GPL-2.0-or-later
// bbport gcn: the shader programs the game BINDS, for a coverage measurement (BB_SHADER_BINDS).
//
// A pipeline is made at the first DRAW with its state, so the pipeline store only ever holds what
// was drawn. The game, however, binds shaders it does not always draw with in the same submission:
// a program written to SPI_SHADER_PGM_LO_xS and never drawn with by the frames that ran is
// foreknowledge the store has no way to use yet. This records the bind path (the SH register
// writes, CommandProcessor::ApplyGraphicsRegisterPacket), not the draws.
//
// Off unless BB_SHADER_BINDS names a file; then one line per distinct (stage, program hash) and,
// for a program whose code is not there yet, one per distinct address — written under a mutex, so
// the draw-preparation workers may call it too. Behaviour is unchanged either way.
#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include "common/types.h"
#include "gcn/regs_shader.h"

namespace Gcn::BindLog {

inline constexpr std::array<const char*, 6> StageNames = {"ps", "vs", "gs", "es", "hs", "ls"};

struct State {
    bool on{};
    FILE* file{};
    std::mutex mutex;
    /// Program hashes already written, and the addresses whose code was not there when bound.
    std::unordered_set<u64> hashes;
    std::unordered_set<u64> addresses;
    u64 bound_total{};
    u64 unresolved{};
};

inline State& Instance() {
    static State state;
    static const bool configured = [](State& s) {
        const char* path = std::getenv("BB_SHADER_BINDS");
        if (path && *path) {
            s.file = std::fopen(path, "a");
            s.on = s.file != nullptr;
        }
        return true;
    }(state);
    (void)configured;
    return state;
}

/// One bind of `program`: its address, and the program's own hash when its "OrbShdr" header is
/// where the leading "s_mov_b32 vcc_hi, length" puts it. `stage` is 0..5 as Regs::ProgramForStage.
inline void Note(u32 stage, u64 address, const u32* code) {
    State& s = Instance();
    if (!s.on || address == 0 || code == nullptr) {
        return;
    }
    const BinaryInfo* info = DirectBinaryInfo(code);
    std::scoped_lock lock{s.mutex};
    ++s.bound_total;
    if (info == nullptr) {
        ++s.unresolved;
        if (s.addresses.insert(address).second) {
            std::fprintf(s.file, "BIND stage=%s addr=%#llx hash=none\n",
                         stage < StageNames.size() ? StageNames[stage] : "?",
                         static_cast<unsigned long long>(address));
        }
        return;
    }
    if (s.hashes.insert(info->shader_hash).second) {
        std::fprintf(s.file, "BIND stage=%s addr=%#llx hash=%#llx\n",
                     stage < StageNames.size() ? StageNames[stage] : "?",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(info->shader_hash));
        std::fflush(s.file);
    }
    (void)s.addresses;
}

/// Every graphics stage the register file holds a program for.
inline void NoteRegs(const Regs& regs) {
    if (!Instance().on) {
        return;
    }
    for (u32 stage = 0; stage < StageNames.size(); ++stage) {
        const ShaderProgram* program = regs.ProgramForStage(stage);
        if (program == nullptr) {
            continue;
        }
        const u32* code = program->Address<u32*>();
        Note(stage, reinterpret_cast<u64>(code), code);
    }
}

} // namespace Gcn::BindLog

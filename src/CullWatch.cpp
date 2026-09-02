#include "PCH.h"

#include "CullWatch.h"

#include "Settings.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
    // How many DISTINCT writer addresses to keep. One is usually the whole
    // answer; the extra slots are there because "two writers taking turns" is a
    // real possibility and a single-slot probe would hide it behind whichever
    // one happened to land first.
    constexpr std::size_t kMaxSites = 8;

    // Stop after this many trips. The watch costs a debug exception per write,
    // which at one write per frame is fine for a measurement and pointless to
    // keep paying once the addresses have stopped changing.
    constexpr std::uint32_t kMaxHits = 400;

    // How many trips to keep IN ORDER, with the value each one left behind.
    // The FIRST N rather than the last N, deliberately: the watch arms on the
    // node in the same breath as we cull it, so the flag is set when recording
    // starts and the write that first clears it is the one being hunted. A ring
    // holding the most recent trips would age that moment out.
    constexpr std::size_t kTraceLen = 48;

    // NiAVObject flag bit 0 is kHidden, which is what GetAppCulled reads. Every
    // engine site round 3 caught works on exactly this bit: `or [rax+0xF4], 1`
    // to hide, `and [rax+0xF4], 0xFFFFFFFE` to show. Bit clear means on screen.
    constexpr std::uint32_t kHiddenBit = 0x1u;

    std::atomic<std::uintptr_t> g_watched{ 0 };
    std::atomic<std::uint32_t>  g_hits{ 0 };
    std::atomic<std::uint32_t>  g_frame{ 0 };
    std::atomic<bool>           g_overflowed{ false };
    void*                       g_veh = nullptr;

    // Written ONLY by the exception handler, read only by Report on the game
    // thread. The handler runs on the same thread that owns the debug
    // registers, so these need no locking; they are atomic so Report cannot
    // read a torn slot if that assumption ever stops holding.
    std::atomic<std::uintptr_t> g_sites[kMaxSites]{};
    std::atomic<std::uint32_t>  g_siteHits[kMaxSites]{};
    std::atomic<std::uint32_t>  g_siteLeftVisible[kMaxSites]{};

    // The ordered trace. Same ownership rules as the tallies above.
    std::atomic<std::uintptr_t> g_traceRip[kTraceLen]{};
    std::atomic<std::uint32_t>  g_traceFlags[kTraceLen]{};
    std::atomic<std::uint32_t>  g_traceFrame[kTraceLen]{};
    std::atomic<std::uint32_t>  g_traceCount{ 0 };

    void RecordSite(std::uintptr_t a_rip, bool a_leftVisible) {
        for (std::size_t i = 0; i < kMaxSites; ++i) {
            const auto have = g_sites[i].load(std::memory_order_relaxed);
            if (have == a_rip) {
                g_siteHits[i].fetch_add(1, std::memory_order_relaxed);
                if (a_leftVisible) {
                    g_siteLeftVisible[i].fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            if (have == 0) {
                g_sites[i].store(a_rip, std::memory_order_relaxed);
                g_siteHits[i].store(1, std::memory_order_relaxed);
                g_siteLeftVisible[i].store(a_leftVisible ? 1u : 0u, std::memory_order_relaxed);
                return;
            }
        }
        g_overflowed.store(true, std::memory_order_relaxed);
    }

    void RecordTrace(std::uintptr_t a_rip, std::uint32_t a_flags) {
        const auto slot = g_traceCount.fetch_add(1, std::memory_order_relaxed);
        if (slot >= kTraceLen) {
            return;  // the opening moments are recorded; the tallies carry the rest
        }
        g_traceRip[slot].store(a_rip, std::memory_order_relaxed);
        g_traceFlags[slot].store(a_flags, std::memory_order_relaxed);
        g_traceFrame[slot].store(g_frame.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    }

    // Game thread only, so no atomic. How much of the ordered trace has already
    // reached the log; the cadence report prints the new entries and nothing
    // else, or a full trace would be re-printed every few hundred frames.
    std::uint32_t g_tracePrinted = 0;

    struct Site {
        char           module[64];
        std::uintptr_t rva;
    };

    // ⚠ ON THE GAME THREAD ONLY. GetModuleHandleEx takes the loader lock, which
    // is exactly the sort of thing the exception handler is forbidden to touch,
    // so resolution is deferred to here rather than done at capture time.
    Site Resolve(std::uintptr_t a_rip) {
        Site out{};
        std::snprintf(out.module, sizeof(out.module), "%s", "(unresolved)");
        out.rva = 0;
        HMODULE mod = nullptr;
        char    path[MAX_PATH]{};
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(a_rip), &mod) &&
            mod && ::GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* slash = std::strrchr(path, '\\');
            std::snprintf(out.module, sizeof(out.module), "%s", slash ? slash + 1 : path);
            out.rva = a_rip - reinterpret_cast<std::uintptr_t>(mod);
        }
        return out;
    }

    void ReportTrace(const char* a_why) {
        const auto raw  = g_traceCount.load(std::memory_order_relaxed);
        const auto have = raw < kTraceLen ? raw : static_cast<std::uint32_t>(kTraceLen);
        if (have == 0) {
            return;
        }
        for (auto i = g_tracePrinted; i < have; ++i) {
            const auto flags = g_traceFlags[i].load(std::memory_order_relaxed);
            const auto site  = Resolve(g_traceRip[i].load(std::memory_order_relaxed));
            spdlog::info("CullWatch [{}]: trip {} at frame {}, {}+{:#x} left the flags word "
                         "{:#010x}, so he is {} (OS-103b).",
                         a_why, i + 1, g_traceFrame[i].load(std::memory_order_relaxed),
                         site.module, site.rva, flags,
                         (flags & kHiddenBit) ? "CULLED" : "ON SCREEN");
        }
        g_tracePrinted = have;

        // ⚠ THIS IS THE LINE THE ROUND IS FOR. Whoever wrote last and left the
        // bit clear is holding him on screen, and the count after it says
        // whether anybody argued: a scoped hide-then-restore scaffold always has
        // trips following its clear, a real leak has none.
        bool          found       = false;
        std::uint32_t lastVisible = 0;
        for (std::uint32_t i = 0; i < have; ++i) {
            if ((g_traceFlags[i].load(std::memory_order_relaxed) & kHiddenBit) == 0) {
                lastVisible = i;
                found       = true;
            }
        }
        if (!found) {
            spdlog::info("CullWatch [{}]: none of the first {} trip(s) left him on screen, "
                         "every one kept the cull bit set (OS-103b).",
                         a_why, have);
        } else {
            const auto site = Resolve(g_traceRip[lastVisible].load(std::memory_order_relaxed));
            spdlog::info("CullWatch [{}]: the last write that left him ON SCREEN was trip {} "
                         "at frame {}, from {}+{:#x}, with {} later trip(s) recorded. That is "
                         "the site to decompile (OS-103b).",
                         a_why, lastVisible + 1,
                         g_traceFrame[lastVisible].load(std::memory_order_relaxed),
                         site.module, site.rva, have - lastVisible - 1);
        }
        if (raw > kTraceLen) {
            spdlog::info("CullWatch [{}]: {} further trip(s) are not in the ordered trace, "
                         "which keeps the first {} (OS-103b).",
                         a_why, raw - kTraceLen, kTraceLen);
        }
    }

    // ⚠ NOTHING IN HERE MAY ALLOCATE, LOG OR TAKE A LOCK. It runs as a vectored
    // exception handler, which is about the most hostile context in the
    // process: the game thread is mid-instruction and anything that can block
    // or re-enter is a deadlock or a second fault. Record and return.
    LONG CALLBACK Veh(EXCEPTION_POINTERS* a_info) {
        auto* const rec = a_info ? a_info->ExceptionRecord : nullptr;
        auto* const ctx = a_info ? a_info->ContextRecord : nullptr;
        if (!rec || !ctx || rec->ExceptionCode != EXCEPTION_SINGLE_STEP) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // DR6 bit 0 is "DR0 tripped". Any other single-step belongs to somebody
        // else (a debugger, another plugin) and must be passed along untouched.
        if ((ctx->Dr6 & 0x1ull) == 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        ctx->Dr6 = 0;  // ours, so consume the status or it re-reports forever
        // ⚠ DISARMED BUT STILL ARMED, AND THIS IS WHERE THE CTD WAS. Disarm
        // clears DR0 through a worker that has to suspend this thread first, so
        // it lands milliseconds later; the game thread meanwhile walks straight
        // into RestoreAll and writes the very flags word being watched. Clearing
        // the enable bit HERE, in the context about to be resumed, is the only
        // disarm that takes effect on the instruction after this one.
        if (g_watched.load(std::memory_order_relaxed) == 0) {
            ctx->Dr7 &= ~0x1ull;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // ⚠ A DATA BREAKPOINT IS A TRAP, NOT A FAULT: the CPU reports AFTER the
        // storing instruction retires, so Rip is the address of the NEXT
        // instruction. The writer is immediately above it. Reported raw rather
        // than adjusted, because the instruction length is not known here and a
        // guessed subtraction would be a fabricated address.
        //
        // That same property is what makes the value readable here: the store
        // has already landed, so the word holds the RESULT of the write rather
        // than what it was before. One plain load, no allocation and no lock,
        // which is the whole budget this context allows. The address is live for
        // the entire armed window - Disarm takes the breakpoint off before the
        // node can die, and it is the only writer of g_watched.
        const auto watched = g_watched.load(std::memory_order_relaxed);
        const auto flags   = *reinterpret_cast<const volatile std::uint32_t*>(watched);
        const auto rip     = static_cast<std::uintptr_t>(ctx->Rip);
        RecordSite(rip, (flags & kHiddenBit) == 0);
        RecordTrace(rip, flags);
        if (g_hits.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxHits) {
            // Self-limiting: clear the local-enable bit so the watch stops
            // paying for itself. Done through the context being resumed, which
            // is the only safe way to touch DR7 from in here.
            ctx->Dr7 &= ~0x1ull;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Program DR0/DR7 on the game thread.
    //
    // ⚠ FROM A WORKER, AGAINST A SUSPENDED TARGET, AND NOT ON THE PSEUDO
    // HANDLE. Set/GetThreadContext on GetCurrentThread() is the shorter route
    // and it is not documented to work; the supported shape is to suspend the
    // target thread from a different one. The worker does nothing but the four
    // API calls, so there is no lock it can be holding when the game thread
    // stops.
    void ProgramDebugRegisters(std::uintptr_t a_address) {
        HANDLE dup = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                               ::GetCurrentProcess(), &dup, 0, FALSE,
                               DUPLICATE_SAME_ACCESS)) {
            spdlog::warn("CullWatch: could not duplicate the game thread handle; "
                         "the watch is NOT armed.");
            return;
        }
        std::thread([dup, a_address] {
            if (::SuspendThread(dup) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::GetThreadContext(dup, &ctx)) {
                    if (a_address != 0) {
                        ctx.Dr0 = a_address;
                        // DR7: L0 (bit 0) enables DR0 for this thread; bits
                        // 16-17 = 01 is "break on write"; bits 18-19 = 11 is a
                        // four-byte length, which is exactly the flags word and
                        // is why the address has to be 4-aligned (it is: the
                        // member sits at 0xF4 in NiAVObject).
                        ctx.Dr7 &= ~0xF0000ull;
                        ctx.Dr7 |= (0x1ull << 16) | (0x3ull << 18);
                        ctx.Dr7 |= 0x1ull;
                    } else {
                        ctx.Dr0 = 0;
                        ctx.Dr7 &= ~0x1ull;
                    }
                    ctx.Dr6 = 0;
                    ::SetThreadContext(dup, &ctx);
                }
                ::ResumeThread(dup);
            }
            ::CloseHandle(dup);
        }).detach();
    }
}

namespace MTB::CullWatch {

    void Arm(RE::NiAVObject* a_node) {
        if (!Settings::GetSingleton().playerCullProbe || !a_node) {
            return;
        }
        if (g_hits.load(std::memory_order_relaxed) >= kMaxHits) {
            return;  // measured its fill already; the handler disarmed itself
        }
        // The flags word itself, addressed through CommonLib's accessor rather
        // than a hand-written offset. GetFlags() is a RelocateMember, so it
        // resolves to 0xF4 on SE and AE and 0x10C on VR by itself; a literal
        // here would silently watch four innocent bytes on the odd one out.
        const auto address = reinterpret_cast<std::uintptr_t>(&a_node->GetFlags());
        if (g_watched.exchange(address, std::memory_order_relaxed) == address) {
            return;  // same node, already watched
        }
        if (!g_veh) {
            // First, so the breakpoint cannot fire before there is anything to
            // catch it. Priority 1 puts us ahead of the crash loggers, which
            // matters: an unhandled EXCEPTION_SINGLE_STEP reaching one of them
            // would be reported as a crash.
            g_veh = ::AddVectoredExceptionHandler(1, Veh);
            if (!g_veh) {
                spdlog::warn("CullWatch: AddVectoredExceptionHandler failed; the watch is "
                             "NOT armed and nothing will be recorded.");
                g_watched.store(0, std::memory_order_relaxed);
                return;
            }
        }
        spdlog::info("CullWatch: watching the player's flags word at {:#x} for writes "
                     "(OS-103b). Up to {} trip(s), then it takes itself off.",
                     address, kMaxHits);
        ProgramDebugRegisters(address);
    }

    void Disarm() {
        if (g_watched.exchange(0, std::memory_order_relaxed) == 0) {
            return;
        }
        // ⚠ THE BREAKPOINT COMES OFF BEFORE THE NODE CAN DIE. A watch left on
        // freed memory does not fail quietly, it fires on whatever the
        // allocator hands out next and reports a completely innocent writer.
        ProgramDebugRegisters(0);
        // ⚠ THE HANDLER STAYS. Removing it here is what crashed the game on
        // menu exit: g_watched is cleared and the handler torn down on this
        // thread immediately, while DR0 stays live until the worker above gets
        // scheduled, so the next write to the watched word raised a single step
        // with nothing installed to answer it. RestoreAll writes that exact word
        // a few calls later, every single time.
        //
        // Leaving it installed costs two compares on exceptions that are not
        // ours, and it is what lets the disarm above be finished synchronously
        // by the handler itself. It is only ever installed with the probe on.
    }

    void Tick() { g_frame.fetch_add(1, std::memory_order_relaxed); }

    void Report(const char* a_why) {
        const auto hits = g_hits.load(std::memory_order_relaxed);
        if (hits == 0) {
            spdlog::info("CullWatch [{}]: 0 write(s) caught. Either the watch never armed, "
                         "or the writer is NOT on the game thread; debug registers are "
                         "per thread and this one only watches ours (OS-103b).",
                         a_why);
            return;
        }
        for (std::size_t i = 0; i < kMaxSites; ++i) {
            const auto rip = g_sites[i].load(std::memory_order_relaxed);
            if (rip == 0) {
                break;
            }
            const auto site    = Resolve(rip);
            const auto trips   = g_siteHits[i].load(std::memory_order_relaxed);
            const auto visible = g_siteLeftVisible[i].load(std::memory_order_relaxed);
            spdlog::info("CullWatch [{}]: writer #{} is {}+{:#x} (return address {:#x}), "
                         "{} trip(s), {} of them left him ON SCREEN and {} left him culled. "
                         "The storing instruction is immediately ABOVE that address: a data "
                         "breakpoint reports after the write (OS-103b).",
                         a_why, i + 1, site.module, site.rva, rip, trips, visible,
                         trips - visible);
        }
        if (g_overflowed.load(std::memory_order_relaxed)) {
            spdlog::warn("CullWatch [{}]: more than {} distinct writers, so the list above "
                         "is partial (OS-103b).",
                         a_why, kMaxSites);
        }
        ReportTrace(a_why);
    }
}

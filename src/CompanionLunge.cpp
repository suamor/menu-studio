#include "PCH.h"

#include "CompanionLunge.h"

#include "AnimEventProbe.h"  // IsArmed: ONE armed flag, never a second copy
#include "Settings.h"

#include <algorithm>
#include <cstring>

namespace MTB::CompanionLunge {

    namespace {

        // Small and fixed. The sink runs on a graph thread and must never
        // allocate, take a lock, or be the reason a frame stalls, so slots are
        // claimed with a compare-exchange and the tag text is written exactly
        // once by whoever claims it.
        constexpr std::size_t kMaxTags   = 32;
        constexpr std::size_t kTagChars  = 48;

        // ⚠ THREE STATES, NOT A BOOL. The sink runs on a GRAPH thread. With a
        // single "claimed" flag, the claimer sets it and then writes the tag
        // text, so another thread can see claimed and strncmp a half-written
        // buffer. kClaiming exists purely so the text is never readable until
        // it is whole.
        enum : unsigned { kFree = 0, kClaiming = 1, kReady = 2 };

        struct TagCount {
            std::atomic<unsigned> state{ kFree };
            char                  tag[kTagChars]{};  // written only under kClaiming
            std::atomic<unsigned> count{ 0 };
        };
        TagCount g_tags[kMaxTags];

        std::atomic<bool>     g_holding{ false };
        std::atomic<unsigned> g_events{ 0 };   // total while armed, for the report
        std::atomic<unsigned> g_worst{ 0 };    // highest single-tag count
        char                  g_worstTag[kTagChars]{};  // main thread only, at report

        // What we are attached to. The manager pointer is HELD, which is the
        // whole reason this is safe: it keeps the graphs alive, so removing the
        // sink later cannot touch a destroyed event source. Her 3D is rebuilt on
        // most gear switches, and a raw graph pointer would be dangling by the
        // second click.
        RE::BSAnimationGraphManagerPtr g_manager;
        std::uint32_t                  g_ownerId = 0;
        bool                           g_attached = false;

        class Sink : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            static Sink* GetSingleton() {
                static Sink singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BSAnimationGraphEvent*                    a_event,
                RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                // kContinue ALWAYS, on every path. This sits in the same chain
                // the game's own listeners use, and swallowing an event here
                // would change behaviour for an actor we are only watching.
                if (!a_event || !AnimEventProbe::IsArmed()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const char* const tag = a_event->tag.c_str();
                if (!tag || !*tag) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                Record(tag);
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            static void Record(const char* a_tag) {
                g_events.fetch_add(1, std::memory_order_relaxed);
                const auto& cfg = Settings::GetSingleton();
                for (auto& slot : g_tags) {
                    const unsigned state = slot.state.load(std::memory_order_acquire);
                    if (state == kReady) {
                        if (std::strncmp(slot.tag, a_tag, kTagChars - 1) != 0) {
                            continue;
                        }
                    } else if (state == kClaiming) {
                        continue;  // someone is mid-write; their tag is not ours to read
                    } else {
                        unsigned expected = kFree;
                        if (!slot.state.compare_exchange_strong(
                                expected, kClaiming, std::memory_order_acq_rel)) {
                            continue;  // lost the race for this slot, try the next
                        }
                        // The CAS made this thread the sole writer, and kReady
                        // is published with release AFTER the text is whole.
                        std::strncpy(slot.tag, a_tag, kTagChars - 1);
                        slot.state.store(kReady, std::memory_order_release);
                    }
                    const unsigned n =
                        slot.count.fetch_add(1, std::memory_order_relaxed) + 1;
                    unsigned worst = g_worst.load(std::memory_order_relaxed);
                    while (n > worst &&
                           !g_worst.compare_exchange_weak(worst, n,
                                                          std::memory_order_relaxed)) {
                    }
                    // ⚠ THE THRESHOLD IS AN ESTIMATE, not a measurement, and it
                    // is an INI value for exactly that reason. The player's loop
                    // was five tags cycling once per 3.3 s, so a settled idle
                    // repeating one tag four times inside a single menu should
                    // not happen. Nothing has ever recorded HER stream, though,
                    // so the disarm report prints the histogram and the next
                    // field run turns this number into a measured one.
                    if (cfg.companionLungeGuard && n >= cfg.companionLungeRepeats &&
                        !g_holding.exchange(true, std::memory_order_relaxed)) {
                        spdlog::warn("companion lunge: '{}' raised {} times while this menu "
                                     "was open. That is a graph cycling rather than settling, "
                                     "so her body is being HELD on its caught pose for the "
                                     "rest of the session. Her face keeps ticking.",
                                     slot.tag, n);
                    }
                    return;
                }
                // Table full. Not a failure: it means her stream is more varied
                // than a loop, which is the healthy shape.
            }
        };

        void Detach() {
            if (g_attached && g_manager) {
                for (auto& graph : g_manager->graphs) {
                    if (graph) {
                        graph->RemoveEventSink(Sink::GetSingleton());
                    }
                }
            }
            g_attached = false;
            g_manager.reset();
            g_ownerId = 0;
        }

        void ClearCounts() {
            for (auto& slot : g_tags) {
                // Count first, then free the slot. The text is deliberately
                // left alone: only a claimer writes it, and freeing the state
                // last means a racing sink either counts into a slot that is
                // about to reset (one lost event) or claims it cleanly. Zeroing
                // the buffer here would be a genuine data race for no gain.
                slot.count.store(0, std::memory_order_relaxed);
                slot.state.store(kFree, std::memory_order_release);
            }
            g_events.store(0, std::memory_order_relaxed);
            g_worst.store(0, std::memory_order_relaxed);
            g_holding.store(false, std::memory_order_relaxed);
        }

    }  // namespace

    void Sync(RE::Actor* a_companion) {
        if (!a_companion) {
            Detach();
            return;
        }
        RE::BSAnimationGraphManagerPtr manager;
        if (!a_companion->GetAnimationGraphManager(manager) || !manager) {
            Detach();  // her graph went away with a 3D rebuild - stop pointing at it
            return;
        }
        const std::uint32_t id = a_companion->GetFormID();
        if (g_attached && g_ownerId == id && g_manager == manager) {
            return;  // unchanged, and this is the common case
        }
        // Off the old one BEFORE the new one. A half-swapped attachment would
        // feed one actor's events into another's counters, and the counters are
        // the entire basis for holding her.
        Detach();
        int attached = 0;
        for (auto& graph : manager->graphs) {
            if (graph) {
                // BShkbAnimationGraph IS a BSTEventSource<BSAnimationGraphEvent>
                // (its 4th base), so this is the supported path the game's own
                // listeners use rather than vtable arithmetic.
                graph->AddEventSink(Sink::GetSingleton());
                ++attached;
            }
        }
        if (attached == 0) {
            return;  // manager with no graph yet - retried next tick
        }
        g_manager  = manager;  // the ref that keeps them alive for the detach
        g_ownerId  = id;
        g_attached = true;
        spdlog::info("companion lunge: watching '{}' 0x{:08X} across {} graph(s). Nothing "
                     "has recorded her event stream before, so this is also the trace.",
                     a_companion->GetName(), id, attached);
    }

    bool ShouldHold() {
        return Settings::GetSingleton().companionLungeGuard &&
               g_holding.load(std::memory_order_relaxed);
    }

    void ArmedSessionBegin() { ClearCounts(); }

    void ArmedSessionReport() {
        const unsigned total = g_events.load(std::memory_order_relaxed);
        if (!g_attached || total == 0) {
            return;  // never watched her this session - no false zero
        }
        // The worst offender by name, which is the number the threshold is set
        // against, plus the whole histogram so a false positive is visible as
        // one rather than inferred from a hold that should not have happened.
        unsigned worst = 0;
        g_worstTag[0] = '\0';
        std::string histogram;
        for (auto& slot : g_tags) {
            if (slot.state.load(std::memory_order_acquire) != kReady) {
                continue;
            }
            const unsigned n = slot.count.load(std::memory_order_relaxed);
            if (n > worst) {
                worst = n;
                std::strncpy(g_worstTag, slot.tag, kTagChars - 1);
            }
            if (!histogram.empty()) {
                histogram += ", ";
            }
            histogram += slot.tag;
            histogram += "=";
            histogram += std::to_string(n);
        }
        spdlog::info("companion lunge: {} event(s) while the menu was open, busiest '{}' x{} "
                     "(threshold {}). {} [{}]",
                     total, g_worstTag[0] ? g_worstTag : "?", worst,
                     Settings::GetSingleton().companionLungeRepeats,
                     g_holding.load(std::memory_order_relaxed)
                         ? "HELD: her graph was caught cycling and stopped being stepped."
                         : "Not held: nothing repeated often enough to read as a loop.",
                     histogram);
        ClearCounts();
    }

    void Reset() {
        Detach();
        ClearCounts();
    }

}  // namespace MTB::CompanionLunge

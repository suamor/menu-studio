#include "PCH.h"

// The linker-provided base of THIS module. Used only to ask Windows for our own
// DLL path, so the load banner can report the file's real build time.
// PCH.h's comment claims RE/Skyrim.h pulls in <Windows.h>; it does not bring in
// these types here, so include it explicitly rather than trusting the comment.
#include <Windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;

#include "Bubble.h"
#include "CameraGate.h"
#include "StudioCamera.h"
#include "CbpcDrive.h"
#include "FootIkGate.h"
#include "FsmpDrive.h"
#include "ItemPreviewBroker.h"
#include "MenuInputGate.h"
#include "Settings.h"
#include "SettingsUI.h"
#include "VersionCheck.h"

#include <SimpleIni.h>

namespace {
    constexpr auto kLogName = "MenuStudio.log";
    // ⚠ A DIAGNOSTIC BUILD SAYS SO ON ITS OWN LOAD LINE. A field report names a
    // DLL, not a commit, and the one thing worse than no log is a log nobody can
    // place: three builds have carried the 1.1.5 number now.
#ifdef MENUSTUDIO_DIAG
    constexpr auto kVersion = "1.1.6-diag5";
#else
    constexpr auto kVersion = "1.1.6";
#endif

    enum class RuntimeGate {
        kAuto = 0,   // widen, and let the self-check decide
        kForce = 1,  // load even if the self-check fails
        kStrict = 2  // pre-0.7.2 behaviour: 1.5.97 or 1.6.1130+ only
    };

    // ⚠ NOT Settings. Settings::Load() runs at kDataLoaded, which is roughly
    // 25 seconds later in a field log - the same trap that left the diagnostic
    // probes reading a compiled-in default forever. The gate has to decide
    // here, so it reads the one key it needs itself.
    RuntimeGate ReadRuntimeGate() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(L"Data/SKSE/Plugins/MenuStudio.ini") < 0) {
            return RuntimeGate::kAuto;
        }
        switch (ini.GetLongValue("Compatibility", "iRuntimeGate", 0)) {
        case 1:
            return RuntimeGate::kForce;
        case 2:
            return RuntimeGate::kStrict;
        default:
            return RuntimeGate::kAuto;
        }
    }

    // Open the log file in a_dir. Returns nullptr when it cannot be opened
    // there, so the caller can carry on with whatever else worked instead of
    // throwing out of the loader: a directory that resolves is not the same as
    // one we may write to.
    spdlog::sink_ptr OpenSink(const std::filesystem::path& a_dir) {
        const auto path = a_dir / kLogName;

        // ⚠ KEEP ONE PREVIOUS SESSION. The sink below truncates at boot, and
        // on 2026-08-04 that destroyed two field runs in one evening: the
        // player finishes a test, relaunches out of habit, and the evidence is
        // gone before anyone reads it. BardHero and Stage Manager already keep
        // a .prev for exactly this reason. Errors ignored on purpose - a
        // failed rotation must never cost the log itself.
        std::error_code ec;
        auto            prev = path;
        prev.replace_extension(".prev.log");
        std::filesystem::rename(path, prev, ec);

        try {
            return std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    // Where this DLL is, which under MO2 is inside the virtual Data tree and so
    // is really the Overwrite folder. Empty when Windows will not say.
    std::filesystem::path DllFolder() {
        wchar_t self[MAX_PATH]{};
        if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH)) {
            return {};
        }
        return std::filesystem::path(self).parent_path();
    }

    void SetupLog() {
        // ⚠ A LOG THAT NEVER APPEARED WAS SILENT, AND IT COST A FIELD REPORT.
        // On 2026-09-01 a reporter installed a diagnostic build, SKSE wrote
        // "loaded correctly" for it, and no log was ever produced.
        // log_directory() asks Windows for the Documents folder and hands back
        // nothing when that call fails. The old code returned here without a
        // word, so the whole symptom was a user saying the mod did not make a
        // log, with nothing anywhere to read to find out why.
        std::vector<spdlog::sink_ptr> sinks;
        std::string                   where;

        const auto documents = SKSE::log::log_directory();
        if (documents) {
            if (auto sink = OpenSink(*documents)) {
                sinks.push_back(std::move(sink));
                where = documents->string();
            }
        }

        // ⚠ A DIAGNOSTIC BUILD WRITES TWO COPIES, ON PURPOSE. The same reporter
        // could not find the log at all, and their save path says Mod Organizer,
        // whose virtual filesystem can catch our write while leaving SKSE's
        // alone. Beside the DLL IS the Overwrite folder under MO2, so a second
        // copy there means it does not matter which of the two a reporter opens.
        // ⛔ Not in release: that is a duplicate of every session's log, several
        // megabytes each, written for everyone to solve a rare problem.
#ifdef MENUSTUDIO_DIAG
        const bool besideDll = true;
#else
        const bool besideDll = sinks.empty();
#endif
        if (besideDll) {
            if (const auto dir = DllFolder(); !dir.empty()) {
                if (auto sink = OpenSink(dir)) {
                    sinks.push_back(std::move(sink));
                    where = where.empty() ? dir.string() : where + " AND " + dir.string();
                }
            }
        }

        if (sinks.empty()) {
            return;
        }

        auto logger = std::make_shared<spdlog::logger>("global", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

        // ⚠ SAY WHERE, IN THE LOG ITSELF. A reporter who found one copy can be
        // told the other exists, and a reporter who found none can be told what
        // we tried. This line is the answer to "where is my log".
        spdlog::info("Log written to: {}", where);
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            MTB::Settings::GetSingleton().Load();
            MTB::Bubble::Register();
            MTB::FsmpDrive::Init();
            MTB::CbpcDrive::Init();
            MTB::FootIkGate::Init();
            MTB::SettingsUI::Register();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            // A menu-open quickload never delivers the close event (AP lesson);
            // reset all armed state.
            MTB::Bubble::GetSingleton().ForceReset();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // §3.2: re-read the INI on every save load so edits apply
            // without relaunching. No menus are open here, so the sMenus
            // set swaps safely.
            MTB::Settings::GetSingleton().Load();
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    // ⚠ THE BUILD STAMP IS NOT DECORATION. MO2 profiles can point at any of
    // several Menu Studio mod folders (the plain dev one that build.bat
    // deploys to, plus a versioned folder per shipped release), and the
    // version string is IDENTICAL across them - a dev build and the release it
    // came from both say "0.7.0". A whole field round was read as "the fix did
    // not work" when the profile had simply loaded the release folder and the
    // new code was never in the process. __DATE__/__TIME__ are the compile
    // moment, so this line alone settles WHICH BINARY RAN, from the log, with
    // no access to the machine. Never remove it to tidy the header up.
    // ⚠ THE DLL'S OWN FILE TIME, NOT __DATE__/__TIME__.
    //
    // The first version of this used __DATE__ __TIME__ and was WRONG within an
    // hour: those expand at COMPILE time for THIS translation unit only, so a
    // build that changed Bubble.cpp and relinked left plugin.cpp untouched and
    // the stamp reported the previous build. A stamp that silently lags is
    // worse than none, because it is trusted. The module's last-write time is
    // the link moment and is always right.
    std::string built = "unknown";
    {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            SYSTEMTIME                st{};
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
                FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
                built = fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}Z", st.wYear, st.wMonth,
                                    st.wDay, st.wHour, st.wMinute, st.wSecond);
            }
        }
    }
    spdlog::info("MenuStudio {} loading (runtime {}), built {}.", kVersion,
                 a_skse->RuntimeVersion().string(), built);
    // Universal DLL: SE 1.5.97 OR any AE from 1.6.317 up.
    //
    // ⚠ THE OLD GATE REFUSED EVERYTHING BETWEEN, AND THAT WAS THE BUG. Users
    // on the mid 1.6 builds saw SKSE's "reported as incompatible during load"
    // dialog, which is this function returning false - our own message, never
    // a crash. The refusal was written when the AE half of the Offsets.h table
    // had been verified on exactly one binary (1.6.1170), and "verified on one
    // build" was turned into "refuse every other build". Every engine address
    // goes through Address Library ids, which the database re-points per
    // build, so the refusal was mostly protecting us from a lack of evidence.
    //
    // The evidence is now gathered on the user's own machine instead:
    // VersionCheck::Run() checks every id for real membership in THIS build's
    // database and locates the frame-driver call site by matching its call
    // target, then this gate refuses only if that critical piece is missing.
    // The difference that matters is that we now decline on a MEASUREMENT
    // rather than on a version number, and either way we decline cleanly -
    // a plugin that loads and then misbehaves is far worse for a user than one
    // that says no.
    const auto ver     = a_skse->RuntimeVersion();
    const auto gate    = ReadRuntimeGate();
    const bool known   = ver == SKSE::RUNTIME_SSE_1_5_97 || ver >= REL::Version(1, 6, 317, 0);
    if (gate == RuntimeGate::kStrict) {
        // The pre-0.7.2 behaviour, kept as an escape hatch: if widening the
        // gate turns out to hurt someone, they can put it back without us
        // shipping a build.
        if (ver != SKSE::RUNTIME_SSE_1_5_97 && ver < REL::Version(1, 6, 1130, 0)) {
            spdlog::error("Unsupported Skyrim runtime {} and iRuntimeGate=2 (strict), "
                          "not loading.", ver.string());
            return false;
        }
    } else if (!known) {
        spdlog::error("Unsupported Skyrim runtime {}: Menu Studio needs SE 1.5.97 or AE "
                      "1.6.317+; not loading.", ver.string());
        return false;
    }

    MTB::VersionCheck::Run();
    if (!MTB::VersionCheck::CriticalOk()) {
        if (gate != RuntimeGate::kForce) {
            spdlog::error("Address self-check FAILED on runtime {}: Menu Studio's frame driver "
                          "has nowhere to install, so the mod would load and do nothing. Not "
                          "loading. Send MenuStudio.log to the author; set "
                          "iRuntimeGate=1 under [Compatibility] to load anyway.", ver.string());
            return false;
        }
        spdlog::warn("Address self-check FAILED but iRuntimeGate=1 (force), loading anyway. "
                     "Expect the pause features to do nothing.");
    }

    SKSE::Init(a_skse);
    // Four write_call<5> users now: the frame driver, CameraGate, and the two
    // devirtualized TESCamera::Update sites. 64 held exactly four stubs with
    // nothing spare, so doubled.
    SKSE::AllocTrampoline(128);

    MTB::ItemPreviewBroker::Install();
    MTB::Bubble::InstallHook();
    MTB::MenuInputGate::Install();
    MTB::CameraGate::Install();
    // After CameraGate on purpose: that one gates the engine's collision pull-in
    // inside the position builder, this one re-stamps our own transform in the
    // tail of PlayerCamera::Update. They touch different things and neither
    // depends on the other's ordering, but keeping the camera hooks together
    // means a future reader finds both at once.
    MTB::StudioCamera::InstallHook();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        spdlog::error("Failed to register SKSE messaging listener; aborting load.");
        return false;
    }

    spdlog::info("MenuStudio loaded.");
    return true;
}

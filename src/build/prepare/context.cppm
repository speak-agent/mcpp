// mcpp.build.prepare.context — the types prepare_build fills and reads:
// the BuildContext it returns, the BuildOverrides a verb hands it, where the
// resolved toolchain came from, the cache mode, and the profile and
// cache-mode resolvers the fast paths share with it. Re-exported by
// mcpp.build.prepare.

export module mcpp.build.prepare.context;

import std;
import mcpp.manifest;
import mcpp.toolchain.model;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.triple;
import mcpp.toolchain.msvc;
import mcpp.toolchain.stdmod;
import mcpp.xlings.runtime_selection;
import mcpp.runtime.binding;
import mcpp.wire;
import mcpp.build.plan;
import mcpp.modgraph.graph;
import mcpp.bmi_cache;

namespace mcpp::build {

// Where the resolved toolchain spec came from.
//
// This exists so mcpp can tell its own guesses apart from the user's
// instructions. When a resolved toolchain turns out to be unusable on this
// machine (the motivating case: a Windows default that targets the MSVC ABI
// on a box with no Visual Studio), mcpp may quietly revise a default it
// picked itself — but a spec the user wrote into mcpp.toml must produce an
// error instead. A project that needs the MSVC ABI to link vcpkg-built .lib
// files is worse off with a silent ABI swap than with a failed build.
//
// Deliberately derived from the two config layers that already exist rather
// than persisted: no new field, nothing to keep in sync on disk.
export enum class TcOrigin {
    None,               // nothing resolved yet
    ManifestToolchain,  // mcpp.toml [toolchain]           — user explicit
    TargetSection,      // mcpp.toml [target.X].toolchain  — user explicit
    GlobalDefault,      // `mcpp toolchain default`        — user explicit
    TargetPin,          // triple.cppm vocabulary convention
    GraphRequirement,   // `requires = ["mcpp:compiler=…"]` in the graph
    FirstRun,           // chosen and persisted by this very invocation
};

// `GlobalDefault` IS DELIBERATELY NOT LISTED, AND THE REASON IS A MEASURED
// REGRESSION RATHER THAN A JUDGEMENT ABOUT WHOSE OPINION COUNTS.
//
// A target row's pin does not name a preferred compiler. It names the payload
// that supplies THAT TARGET'S C library — the mingw payload for
// `x86_64-windows-gnu`, the musl-gcc payload for `*-linux-musl`. Whether the
// user's own default can serve the target instead depends on whether something
// ELSE supplies the target side, and that is knowable only after the dependency
// graph is resolved, which is after this line.
//
// Making the global default outrank the pin was tried and measured: a project
// with no dependencies, a global default of `llvm@22.1.8` and
// `--target x86_64-windows-gnu` stopped building, because clang alone carries no
// C runtime for that target while the payload the row names does. That is a
// working build turned into a failing one by an upgrade.
//
// What the user actually loses is ergonomics, and that is addressed where it is
// visible: when the pin replaces a default the user wrote down, the status line
// SAYS SO and names the one-line override. The structural fix is to defer the
// pin the way the target side itself was deferred — resolve it after the graph,
// where the question it answers has an answer.
export inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}

// MAY A BUILD THAT RESOLVED THIS WAY WRITE THE MACHINE'S DEFAULT?
//
// `GraphRequirement` is the one origin that must not: it is a property of a
// package this project depends on, not of this machine. Two branches persist a
// default — the Windows first-run diversion, whose condition is
// `tcSpec.has_value()`, and the MSVC repair, whose gate is "mcpp chose this
// itself" — and a compiler chosen by `requires = ["mcpp:compiler=…"]` satisfies
// both. Measured against the design rather than a run, because it needs a
// Windows box with no toolchain: a bare machine building ONE llvm-requiring
// project would have handed llvm to every later project that asked for nothing.
//
// NAMED RATHER THAN SPELLED INLINE AT EACH SITE. There are two today; the
// third would be written by someone who never read this note, and a predicate
// with a name is something they can find.
export inline bool tc_origin_may_persist(TcOrigin o) {
    return o != TcOrigin::GraphRequirement;
}

// How a resolution came about, for the status line. A convention that replaced
// nothing needs no explanation; one that replaced a user's stated preference is
// a decision the user did not make and must be told about.
export constexpr std::string_view tc_origin_name(TcOrigin o) {
    switch (o) {
        case TcOrigin::ManifestToolchain: return "[toolchain] in mcpp.toml";
        case TcOrigin::TargetSection:     return "[target.<triple>] in mcpp.toml";
        case TcOrigin::GlobalDefault:     return "your default";
        case TcOrigin::TargetPin:         return "target default";
        case TcOrigin::GraphRequirement:  return "required by the dependency graph";
        case TcOrigin::FirstRun:          return "first-run default";
        case TcOrigin::None:              break;
    }
    return {};
}

// What to tell a user whose build targets the MSVC ABI on a machine that
// cannot serve it. Two shapes, because the two states need different fixes:
//
//   • cl.exe was found but the Windows SDK was not — a half-installed VS.
//     Point at the missing SDK component; switching toolchains would be an
//     over-correction for someone who clearly wants MSVC.
//   • nothing usable at all — the bare-Windows case. Lead with the MinGW-w64
//     route, which needs no Visual Studio and is already a verified target,
//     and keep the "install the C++ workload" option second.
export std::string msvc_unavailable_guidance(const mcpp::toolchain::Toolchain& tc) {
    namespace pins = mcpp::toolchain::triple::pins;
    const bool haveVcTools = tc.compiler == mcpp::toolchain::CompilerId::MSVC;
    if (haveVcTools && mcpp::toolchain::msvc::find_msvc_tools_dir()) {
        return std::format(
            "msvc {} was detected at {}, but no Windows SDK was found —\n"
            "       cl.exe cannot compile without the UCRT/SDK headers.\n"
            "       Install the 'Windows 11 SDK' component via the Visual Studio\n"
            "       Installer (it is part of the Desktop development with C++\n"
            "       workload), then retry.",
            tc.version, tc.binaryPath.string());
    }
    return std::format(
        "this build targets the MSVC ABI, which needs Visual Studio /\n"
        "       Build Tools (MSVC STL + Windows SDK) — neither was found.\n"
        "\n"
        "       No Visual Studio? Use the self-contained MinGW-w64 toolchain\n"
        "       (no Visual Studio required, `import std` works):\n"
        "         mcpp toolchain default {} --target {}\n"
        "\n"
        "       Have Visual Studio? Install the 'Desktop development with C++'\n"
        "       workload — it provides the MSVC STL and the Windows SDK.",
        pins::kSuggestGccMingw, pins::kFirstRunWinGnuTarget);
}

// How this invocation may use the global dependency cache.
//
//   Global  read + write  (default)
//   Local   neither — every dependency is compiled inside this project's
//           target/, which is what every build did before the cache worked
//   Off     neither, and this build's target/<triple>/<fp>/ directory is
//           cleared first (a full cold rebuild). Sibling build dirs — other
//           profiles, other targets — are left alone.
//
// `--no-cache` used to be the only switch and it meant "clear the build dir",
// which says nothing about a cache (and its help text claimed all of target/);
// it stays as a deprecated alias for Off.
export enum class CacheMode { Global, Local, Off };

export std::optional<CacheMode> parse_cache_mode(std::string_view v) {
    if (v == "global") return CacheMode::Global;
    if (v == "local")  return CacheMode::Local;
    if (v == "off" || v == "none") return CacheMode::Off;
    return std::nullopt;
}

export std::string_view cache_mode_name(CacheMode m) {
    switch (m) {
        case CacheMode::Local: return "local";
        case CacheMode::Off:   return "off";
        default:               return "global";
    }
}

// A condition a planning pass reports instead of acting on (plan_only): the
// code is stable and the message is for people. Emitted as diagnostics by the
// command that asked for the plan, at the severity carried here — most notes
// are warnings the document is still complete despite (a lock that would
// change, a generated file left unmaterialized); a build program whose run
// failed under `plan_only` (#699 item 2, E3) is an error, because the sets it
// would have shaped are described without its directives.
export struct PlanNote {
    std::string code;
    std::string message;
    mcpp::wire::Severity severity = mcpp::wire::Severity::Warning;
    // The absolute, native path of the file the condition is about (a
    // package's `build.mcpp`), empty when the note names no file.
    // `mcpp.build.build_database::render` rewrites it to the workspace-
    // relative form every other `path` in the document uses.
    std::string path;
};

export struct BuildContext {
    // THE PER-MACHINE JOB DEFAULT, carried so it is read once.
    //
    // `[build] default_jobs` in `$MCPP_HOME/config.toml` is the machine's
    // answer to "how many at once". `prepare_build` resolves it into the build
    // schedule itself; this field exists for the SECOND reader --
    // `mcpp test`'s runner concurrency (execute.cppm) -- which calls
    // `resolve_jobs` again after this function has returned. Recorded rather
    // than re-read, because a second `load_or_init` there would be a second
    // parser of one file, and because the two readers must not be able to
    // disagree about the machine.
    int                             globalDefaultJobs = 0;
    // --strict: degradations reported through mcpp::diag become errors.
    // Carried on the context because the build's degradations are discovered
    // during backend emission, i.e. after prepare_build has returned — the
    // single place that settles the policy is run_build_plan (execute.cppm).
    bool                            strict = false;
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      tc;
    mcpp::toolchain::Fingerprint    fp;
    mcpp::xlings::runtime::RuntimeSelection runtimeSelection;
    mcpp::platform::runtime::RuntimeBinding runtimeBinding;
    std::filesystem::path           projectRoot;
    // THE SOURCE TREES THIS BUILD READ THAT ARE NOT UNDER `projectRoot`.
    //
    // A `path` dependency — which is what every workspace member is to its
    // siblings — contributes translation units from a directory the fast path
    // has no way to name. `sources_newer_than` sweeps the project being built,
    // so a NEW FILE appearing in such a tree is invisible to it: ninja cannot
    // report an edge that was never emitted, and the fast path replays a
    // build.ninja that predates the file. Measured before this field existed:
    // `mcpp build` printed `Finished dev in 0.00s` and the module was never
    // compiled.
    //
    // Recorded rather than re-derived, because the authoritative answer is
    // which packages this build ACTUALLY read from source — the fast path
    // cannot resolve dependencies without becoming prepare_build, and a second
    // derivation would drift from the first exactly when a resolution rule
    // changes. Written into `.build_cache`; see BuildCacheEntry::depSourceRoots.
    std::vector<std::filesystem::path> depSourceRoots;
    // `<payload>/bin` and then `<payload>` of every installed `[xlings] deps`
    // payload of the runtime-owner manifest, in declaration order (#544); the
    // pair comes from runner_lookup::payload_search_dirs. Read by
    // choose_runner's lookup (mcpp.build.runner_lookup) so a runner may name
    // a program the project declared without writing the payload's
    // home-and-version path into the manifest. Computed by the same
    // resolution `fillXpkgDirs` uses for build programs; a payload that is
    // declared but not installed contributes nothing, and the lookup then
    // continues to PATH.
    std::vector<std::filesystem::path> xlingsDepBinDirs;
    // The payload directory of every `[xlings]` address resolved above, for
    // the fast path's presence check (#716). See BuildCacheEntry::xlingsPayloads.
    std::vector<std::filesystem::path> xlingsPayloads;
    // True when the graph declared a `when = "run"` tool that THIS invocation
    // did not provision, because it was not going to execute anything. The
    // build cache records it so `mcpp run`'s fast path declines an entry a
    // plain `mcpp build` wrote — see BuildCacheEntry::runTierPending.
    bool runTierPending = false;
    // What `--features` asked for, verbatim. Carried so the build cache entry
    // can record the set its artefacts were built with — the output directory
    // is keyed on a fingerprint that includes the features and the entry was
    // not, which let a plain build serve a featured artefact.
    std::string activeFeatureRequest;
    std::filesystem::path           outputDir;
    std::filesystem::path           stdBmi;
    std::filesystem::path           stdObject;
    // plan_only: what the std module build WOULD be (sources are on the
    // toolchain), set when the graph imports std. A build compiles it instead
    // and leaves this empty.
    std::optional<mcpp::toolchain::StdModuleDescription> stdModule;
    // The packages this build read from an editable source tree: the root, and
    // every package whose root is neither in a store nor in a hash-addressed git
    // checkout (the same test depSourceRoots applies). `sources` are the
    // package's source globs, relative to `root`. Read by the build database for
    // the inputs it lists.
    struct SourcePackage {
        std::string                 name;
        std::filesystem::path       root;
        std::vector<std::string>    sources;
    };
    std::vector<SourcePackage>      sourcePackages;
    std::vector<PlanNote>           planNotes;
    mcpp::build::BuildPlan          plan;
    // The scanned module graph. Only `mcpp pack` reads it — see the note at
    // the assignment for why the plan cannot answer its question.
    mcpp::modgraph::Graph           graph;
    // Resolved profile name (resolve_profile_name). Carried so run_build_plan
    // can record it in .build_cache — without it the fast path cannot tell
    // whether a cached build.ninja was generated for the profile being asked
    // for — and so `Finished <profile>` stops being a hardcoded "release".
    std::string                     profile;
    // WHY THIS COMPILER — carried so the QUERY can answer it too.
    //
    // A build says so on its status line. `mcpp why toolchain --format json`
    // exists precisely to answer "what would this resolve to, and why", and a
    // consumer that had to parse the prose to learn that a dependency chose the
    // compiler would be doing the substring matching the machine interface was
    // introduced to remove.
    struct CompilerChoice {
        std::string origin;      // tc_origin_name(): who decided
        std::string requiredBy;  // the package, when the graph decided
        std::string replaced;    // the spec displaced, when one was
    };
    CompilerChoice                  compilerChoice;
    // Resolved global-cache mode. Read side is honored in prepare_build; write
    // side in run_build_plan.
    CacheMode                       cacheMode = CacheMode::Global;

    // M3.2 BMI cache: deps that did NOT hit cache and therefore need
    // populate_from(...) AFTER backend.build succeeds.
    struct CacheTask {
        mcpp::bmi_cache::CacheKey       key;
        mcpp::bmi_cache::DepArtifacts   artifacts;
    };
    std::vector<CacheTask>          depsToPopulate;

    // Deps that DID hit the global cache, and how many compile units each one
    // spared. run_build_plan reports the count so the "Cached" line cannot be
    // true-looking and empty at the same time.
    struct CachedDep {
        std::string name;
        std::string version;
        std::size_t units = 0;
    };
    std::vector<CachedDep>          cachedDeps;

    // What the dependency walk actually RESOLVED, keyed by the root manifest's
    // dependency map key. The "Compiling <dep> v<version>" banner used to read
    // `manifest.dependencies[...].version` — the constraint as authored — so a
    // caret dep announced itself as `v^1.92.8` (mcpp#363). The resolution result
    // already existed inside prepare_build; the banner and mcpp.lock were simply
    // reading the input instead of the output. Both now read this.
    std::map<std::string, std::string> resolvedVersions;
};

// The ONE cache-mode resolver, for the same reason resolve_profile_name exists:
// execute.cppm's fast paths deliberately skip prepare_build, so they need to
// settle the mode from the same rule. Pure in (manifest, override, environment).
//
// `--cache` on the command line already bypasses the fast path, so the override
// argument is empty there; it is threaded anyway so there is exactly one place
// where precedence is written down.
//
// Precedence: --cache > MCPP_BUILD_CACHE > [build] cache > global. An
// unparseable value falls through to the next source rather than silently
// meaning "global" — see prepare_build, which also reports it.
export CacheMode resolve_cache_mode(const mcpp::manifest::Manifest& m,
                                    std::string_view override_mode) {
    if (auto v = parse_cache_mode(override_mode)) return *v;
    if (const char* e = std::getenv("MCPP_BUILD_CACHE"); e && *e)
        if (auto v = parse_cache_mode(e)) return *v;
    if (auto v = parse_cache_mode(m.buildConfig.cacheMode)) return *v;
    return CacheMode::Global;
}

// The ONE profile-name resolver. Shared with execute.cppm's fast paths:
// they deliberately skip prepare_build, so before this existed they had no
// idea which profile the request meant — and `.build_cache` keyed entries by
// target triple alone. Net effect: `mcpp build --release` followed by a bare
// `mcpp build` reported success in 0.00s and left the RELEASE artifacts in
// place. The rule is pure (manifest + one override string), so both sides can
// evaluate it without resolving a toolchain or scanning the module graph.
//
// Precedence: --profile/--release/--dev > [build].default-profile > `fallback`.
// The global default is "dev" (-O0 -g) per the dominant convention
// (Cargo/Meson/CMake/Zig/Bazel/MSBuild all default to debug).
//
// `fallback` exists for ONE caller: `mcpp pack`, where the artifact leaves this
// machine and an unoptimized build with the publisher's absolute source paths
// in it is never what was meant. It changes the LAST step only, so a manifest
// that states `[build] default-profile` still decides — packaging an artifact
// with different flags than `mcpp build` produces would be its own surprise.
// Adding a parameter here rather than a second resolver keeps the precedence
// rule in one function, which is why this function exists at all.
export std::string resolve_profile_name(const mcpp::manifest::Manifest& m,
                                        std::string_view override_name,
                                        std::string_view fallback = "dev") {
    if (!override_name.empty())                 return std::string(override_name);
    if (!m.buildConfig.defaultProfile.empty())  return m.buildConfig.defaultProfile;
    return fallback.empty() ? std::string("dev") : std::string(fallback);
}

// THE OVERRIDE NAME THE COMMAND LINE STATES, OR "" FOR NONE (#649 E9).
//
// `--profile NAME` > `--release` > `--dev`, and one function for every verb
// that takes the spellings (`build`, `run`, `test`, `emit build-database`,
// `pack`). The rule used to be written twice and the copies disagreed:
// `mcpp build --profile dev --release` built `dev` while `mcpp run` given the
// same line built `release`. Its result is `resolve_profile_name`'s
// `override_name`.
export std::string profile_override_from_flags(std::string_view profileOption,
                                               bool release, bool dev) {
    if (!profileOption.empty()) return std::string(profileOption);
    if (release)                return "release";
    if (dev)                    return "dev";
    return {};
}

// Command-level overrides (--target / --static).
// Empty defaults preserve pre-existing behaviour exactly.
export struct BuildOverrides {
    // Where the package being built LIVES (its mcpp.toml). Empty = walk up from
    // the process cwd, which is what every user-facing invocation does. Set by
    // the tool-provisioning pass, which builds a package that lives in the
    // registry rather than under the cwd.
    std::filesystem::path project_root;
    // Where mcpp WRITES. Empty = the project root, which is the historical
    // (and for a normal build, correct) behaviour.
    //
    // The two are separate because a registry package root is shared across
    // projects and may be read-only — build_program.cppm has said so in a
    // comment since G2, and until now nothing could honour it for anything
    // bigger than build.mcpp's own scratch dir. Splitting "source" from "work"
    // is what lets mcpp build such a package at all.
    //
    // EVERYTHING derived from it moves together: target/, mcpp.lock,
    // compile_commands.json, .mcpp/, and build.mcpp's artifact dir. Moving
    // only some would be worse than moving none — a half-redirected build
    // writes into the shared root anyway, just less visibly.
    std::filesystem::path work_dir;
    // PLANNING TO DESCRIBE, NOT TO BUILD (`mcpp emit build-database`).
    //
    // With `work_dir` pointed outside the project, three things still reached
    // it or ran a compiler, and this switch settles each: the std module is
    // described (mcpp::toolchain::describe_std_module) instead of compiled; the
    // root package's `[build] generated_files`, which are sources and live in
    // the source tree, are compared instead of written, and a missing or stale
    // one is recorded in BuildContext::planNotes; and mcpp.lock is READ from the
    // project root, as the resolution input it is, while the lock this planning
    // produces is written under `work_dir`. Build programs still run.
    bool        plan_only = false;
    // #355 tool provisioning re-enters prepare_build for the tool package. A
    // tool package's own build.mcpp may legitimately want another tool (gRPC's
    // wants protoc), so the depth cannot be 1 — but an unbounded chain is a
    // bug, and hanging is a worse diagnostic than a named cycle.
    int         tool_depth = 0;
    // The request chain, for that diagnostic. "root → grpc:grpc_cpp_plugin → …"
    std::string tool_chain;
    // The (package source, tool) pairs being built by the enclosing sub-builds,
    // outermost first. A request for one of them is the tool's own build asking
    // for itself, refused at its first repetition (#649 E6).
    std::vector<std::string> tool_chain_sources;
    // The toolchain spec a host-tool sub-build uses, decided by the build that
    // requested the tool and recorded in the tool's store key (#710). Beats
    // every other source, `--toolchain` included, because the requesting build
    // already took `--toolchain` into account when it decided. Empty for every
    // user-facing invocation.
    std::string toolchain;
    // Use THIS manifest instead of reading `<project_root>/mcpp.toml`.
    //
    // Required for a `compat`-style registry package (Form B), which ships no
    // mcpp.toml at all — its manifest is synthesized from the `.lua`
    // descriptor during resolution. Without this the tool sub-build could only
    // ever handle packages that carry their own manifest (Form A), which
    // excludes most of the index, protobuf among them.
    //
    // Must be the PRISTINE manifest, before feature activation: the sub-build
    // activates its own feature set, and starting from an already-activated
    // copy would fold the same feature sources in twice.
    // A shared_ptr rather than an optional<Manifest>: BuildOverrides is an
    // EXPORTED struct, and embedding a large value type in the module
    // interface made GCC fail to write the cluster at all
    // ('failed to read compiled module cluster ...: Bad file data' when
    // mcpp.build.execute imported it). A pointer keeps the exported layout
    // trivial, and it also avoids copying the manifest per tool build.
    std::shared_ptr<const mcpp::manifest::Manifest> preloaded_manifest;
    // Nested source/tool builds inherit the consumer root's local development
    // OS. A dependency's own [xlings].subos is never consulted or propagated.
    std::shared_ptr<const mcpp::xlings::runtime::RuntimeSelection>
        inherited_runtime_selection;
    std::shared_ptr<const mcpp::platform::runtime::RuntimeBinding>
        inherited_runtime_binding;
    std::string target_triple;       // empty = host triple, fall through to [toolchain]
    // --accel: the device backends and architectures this build targets, in
    // the wire form mcpp.pack.abi_tag reads. Overrides `[build] accel`, the
    // same relationship --target has with [toolchain].
    std::string accel;
    bool        force_static = false; // --static (or implied by musl target)
    std::string package_filter;      // -p <name>: only build this workspace member
    // --profile <name>. Empty = fall through to `[build] default-profile`, then
    // to `profile_fallback` below, whose own default is "dev". The comment here
    // said "release" for as long as `mcpp build --help` did, and neither had
    // been true since the global default moved (see resolve_profile_name and
    // tests/e2e/87_build_default_profile.sh).
    std::string profile;
    // What `resolve_profile_name` falls back to when neither the command line
    // nor `[build] default-profile` says. Empty = "dev", which is every
    // interactive command. `mcpp pack` sets "release": see resolve_profile_name.
    std::string profile_fallback;
    std::string features;            // --features a,b,c (root package activation)
    bool        strict = false;      // --strict: schema warnings become errors
    std::string capabilities;        // --cap blas=openblas,lapack=mkl (provider pins)
    std::string cache_mode;          // --cache global|local|off ("" = unset)
    // Whether the caller intends to EXECUTE what it builds, which is what
    // decides the `when = "run"` tool tier. `mcpp run` and `mcpp test` set it;
    // `mcpp build`, `mcpp pack` and every internal sub-build do not.
    //
    // A SEPARATE FLAG FROM `includeDevDeps`, THOUGH `mcpp test` SETS BOTH.
    // One says which PACKAGES enter the graph, the other which TOOLS are
    // installed, and `mcpp run` needs the second without the first.
    bool        will_run = false;
    // ── The packaging pass, when this prepare is one (mcpp 2026.9.11.1+) ────
    //
    // `mcpp pack --format <name>` prepares TWICE, and these two fields are the
    // whole difference between the passes. The first sets neither: build
    // programs run, declare the formats they provide, and submit no dist
    // action because none was asked for. The second sets both, after the link
    // and after staging, so the claiming member submits an action whose input
    // is a directory that by then exists.
    //
    // NEITHER VALUE IS DERIVED HERE, AND THAT IS THE POINT. `pack_stage_dir` is
    // a function of the package name, the version, the resolved triple and the
    // mode, and the resolved triple is not known until a prepare has run.
    // Computing it a second time before prepare -- from the host triple, say --
    // is the shape where two derivations of one value agree on every machine
    // the author has. Both are read out of what the first pass and `make_plan`
    // already answered.
    std::string           pack_format;
    std::filesystem::path pack_stage_dir;
    // WHY THERE IS NO STAGED TREE, when there is none and a format was still
    // requested. Empty otherwise.
    //
    // A dispatched format does not require the built-in bundling to have
    // succeeded -- see the note in `mcpp.pack.pipeline`. When it did not, the
    // reason travels here so `${mcpp.stage_dir}`'s refusal can name it instead
    // of saying only that the placeholder is unavailable. A member author
    // reading "this build is not packaging" for a build that plainly is would
    // be sent looking in the wrong place.
    std::string           pack_stage_reason;
    // #649 E5: the strip decision and the debug-symbol directory that pass
    // resolved, "1" or "0" and absolute, set only beside `pack_format`. A
    // member that stages libraries of its own reads them through
    // `mcpp::pack_strip()` and `mcpp::pack_debug_symbols_dir()`, so
    // `--no-strip` reaches its files as it reaches the engine's.
    std::string           pack_strip;
    std::filesystem::path pack_debug_symbols_dir;
};

} // namespace mcpp::build

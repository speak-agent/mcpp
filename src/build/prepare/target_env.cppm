// mcpp.build.prepare.target_env — the target-facing answers prepare_build
// derives from a manifest and a resolved toolchain: the `[target.<triple>]`
// row, its sysroot and toolchain, the MSVC toolset a clang MSVC-ABI build
// compiles against, the minimum platform version, the values a build
// program is told about its package and its target, and whether the graph
// imports std. Imported by mcpp.build.prepare and by its unit tests.

export module mcpp.build.prepare.target_env;

import std;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.gcc;
import mcpp.freestanding.target;
import mcpp.platform;
import mcpp.platform.macos;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.xlings;
import mcpp.project;
import mcpp.ui;
import mcpp.build.build_program;

namespace mcpp::build {

// Spelling-independent `[target.<triple>]` lookup.
//
// A section keyed `x86_64-w64-mingw32` matches a resolved `x86_64-windows-gnu`,
// and unparseable keys compare exactly (the escape hatch for custom triples).
// Factored out of the toolchain-override path because the sysroot override must
// use the SAME matching: two lookups that disagreed about spelling would give a
// section that applies to `toolchain` and not to `sysroot`, which is a defect
// nobody would think to look for.
export const mcpp::manifest::TargetEntry*
find_target_entry(const mcpp::manifest::Manifest& m,
                  const mcpp::toolchain::triple::Triple& t)
{
    if (auto it = m.targetOverrides.find(t.str()); it != m.targetOverrides.end())
        return &it->second;
    for (auto const& [key, entry] : m.targetOverrides) {
        if (auto k = mcpp::toolchain::triple::parse(key); k && k->str() == t.str())
            return &entry;
    }
    return nullptr;
}

// The project's `[target.<triple>].sysroot`, or nullptr when it declared none.
export const std::string*
sysroot_override(const mcpp::manifest::Manifest& m,
                 const mcpp::toolchain::triple::Triple& t)
{
    auto* e = find_target_entry(m, t);
    return (e && e->sysrootDeclared) ? &e->sysroot : nullptr;
}

// The toolchain a host tool's package chose for itself, read the way its own
// build reads it (#710): the package's manifest with the root-position keys of
// the workspace that lists it (`inherit_workspace_root_position`), then its
// host row's `[target.<host>] toolchain`, then `[toolchain]`. nullopt when none
// names one.
export std::optional<std::string>
host_tool_declared_toolchain(const mcpp::manifest::Manifest& tool,
                             const std::filesystem::path& toolRoot,
                             std::string_view platform) {
    auto effective = tool;
    if (const auto wsRoot = mcpp::project::find_workspace_root(toolRoot); !wsRoot.empty())
        if (auto ws = mcpp::manifest::load(wsRoot / "mcpp.toml");
            ws && mcpp::project::is_workspace_member(*ws, wsRoot, toolRoot))
            mcpp::project::inherit_workspace_root_position(effective, *ws, wsRoot);
    if (auto* row = find_target_entry(effective, mcpp::toolchain::triple::host_triple());
        row && !row->toolchain.empty())
        return row->toolchain;
    return effective.toolchain.for_platform(platform);
}

// THE MSVC TOOLSET A CLANG `*-windows-msvc` BUILD COMPILES AGAINST.
//
// On an MSVC-ABI row the compiler is the toolchain and the MSVC toolset -- its
// STL and CRT, and the Windows SDK that follows it -- is the sysroot, named by
// `[target.<triple>].sysroot` (default `msvc@system`). Until this existed the
// clang driver searched the machine for headers and libraries while mcpp
// searched it again for `std.ixx`, by a different order, so a machine with two
// installations could compile one toolset's `std.ixx` against another's
// headers, and the choice reached neither the cache key nor any report. The
// choice is made here once, recorded on the toolchain, and handed to the
// driver by the link model.
//
// `msvc@system` that finds nothing returns without binding, so the build
// reaches the "targeting the MSVC ABI without a usable MSVC" diagnosis that
// already names the alternatives.
export std::expected<void, std::string>
bind_msvc_sysroot(mcpp::toolchain::Toolchain& tc,
                  const mcpp::manifest::Manifest& m,
                  const std::function<std::expected<mcpp::config::GlobalConfig*,
                                                    std::string>()>& cfgOf)
{
    namespace msvc = mcpp::toolchain::msvc;
    auto tt = mcpp::toolchain::triple::parse(tc.targetTriple);
    if (!tt) return {};
    const std::string* declared = sysroot_override(m, *tt);
    const std::string text = declared ? *declared : std::string("msvc@system");
    auto spec = mcpp::toolchain::parse_toolchain_spec(text);
    if (!spec)
        return std::unexpected(std::format(
            "[target.{}].sysroot = '{}': {}", tt->str(), text, spec.error()));
    if (spec->family != mcpp::toolchain::Family::Msvc)
        return std::unexpected(std::format(
            "[target.{}].sysroot = '{}': on an MSVC-ABI row the sysroot is an "
            "MSVC toolset (msvc@system, msvc@<toolset> or xim:msvc@<toolset>)",
            tt->str(), text));

    msvc::ToolsetNeeds needs;
    needs.cl = false;   // clang compiles against the toolset; it does not run cl.exe
    const bool systemSel = spec->version.empty() || spec->version == "system";
    std::vector<msvc::VsInstance> instances;
    std::optional<msvc::ToolsetChoice> choice;
    if (!spec->ecosystemOnly) {
        instances = msvc::enumerate_vs_instances();
        choice = msvc::select_system_toolset(
            instances, msvc::msvc_env_snapshot(),
            systemSel ? std::string_view("system") : std::string_view(spec->version),
            needs);
        if (!choice && systemSel) return {};
    }

    std::string origin = "system";
    if (!choice) {
        // THE PACKAGE: `xim:` asked for it, or no installed toolset matched.
        auto cfg = cfgOf();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::toolchain::ToolchainSpec pkgSpec = *spec;
        pkgSpec.target = {};
        auto pkg = mcpp::toolchain::to_xim_package(pkgSpec);
        mcpp::fetcher::Fetcher fetcher(**cfg);
        mcpp::fetcher::InstallProgressHandler progress;
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true,
                                                 &progress);
        if (!payload) {
            // `xim:` never looked at the machine, so the refusal does not
            // report on it.
            if (spec->ecosystemOnly)
                return std::unexpected(std::format(
                    "[target.{}].sysroot = '{}': the package could not be "
                    "provided: {}\n"
                    "  packages: `mcpp toolchain list --available msvc`",
                    tt->str(), text, payload.error().message));
            std::string onMachine;
            for (auto const& line : msvc::describe_system_toolsets(instances, needs))
                onMachine += "\n    " + line;
            return std::unexpected(std::format(
                "[target.{}].sysroot = '{}' matches no toolset on this machine, "
                "and the package could not be provided: {}\n"
                "  installed on this machine:{}\n"
                "  packages: `mcpp toolchain list --available msvc`",
                tt->str(), text, payload.error().message,
                onMachine.empty() ? std::string(" none") : onMachine));
        }
        auto inst = mcpp::toolchain::resolve_managed_msvc(
            mcpp::config::make_xlings_env(**cfg), pkg, /*identifyVersion=*/false);
        if (!inst) return std::unexpected(inst.error());
        choice.emplace();
        choice->vsRoot   = inst->vsRoot;
        choice->version  = inst->toolsVersion;
        choice->toolsDir = inst->vsRoot / "VC" / "Tools" / "MSVC" / inst->toolsVersion;
        choice->product  = "xim:msvc@" + inst->toolsVersion;
        choice->via      = "package";
        origin = "managed";
    }
    for (auto const& n : choice->notes) mcpp::ui::info("note", n);

    // THE SDK FOLLOWS THE TOOLSET'S ORIGIN: a package binds the windows-sdk
    // installed with it, a machine's toolset takes the machine's SDK by the
    // search `msvc@system` has always used. The same function the cl.exe row
    // uses, asked about the toolset directory rather than a cl.exe.
    auto sdk = msvc::resolve_sdk_for(choice->toolsDir / "bin");
    if (!sdk.note.empty()) mcpp::ui::info("note", sdk.note);

    tc.msvcToolsDir     = choice->toolsDir;
    tc.msvcToolsVersion = choice->version;
    tc.msvcOrigin       = origin;
    tc.msvcProduct      = choice->product;
    if (sdk.sdk) {
        tc.windowsSdkRoot    = sdk.sdk->root;
        tc.windowsSdkVersion = sdk.sdk->version;
    }
    // The STL is this toolset's, so its version is the standard library's.
    tc.stdlibVersion = choice->version;

    // THE STD MODULE OF THIS TOOLSET, replacing the `std.ixx` detection found
    // by its own search. A toolset without one leaves `import std` unavailable
    // rather than borrowing another toolset's.
    //
    // Only `std` is rebound. Detection never gave this row a `std.compat`
    // source, and the clang builder for it passes the file without
    // `-x c++-module`: given `std.compat.ixx`, clang takes it for linker
    // input, `--precompile` writes nothing and exits 0, and the next command
    // fails on the missing BMI (measured on the Windows runners).
    std::error_code ec;
    const auto ixx = choice->toolsDir / "modules" / "std.ixx";
    const bool msvcStl = tc.stdModuleSource.empty()
                      || tc.stdModuleSource.filename() == "std.ixx";
    if (msvcStl && std::filesystem::exists(ixx, ec)) {
        tc.stdModuleSource   = ixx;
        tc.hasImportStd      = true;
        tc.importStdMinLevel = msvc::std_module_min_level_for_stl(ixx);
    } else if (msvcStl && !tc.stdModuleSource.empty()) {
        tc.stdModuleSource.clear();
        tc.hasImportStd = false;
    }

    mcpp::ui::info("Resolved", std::format(
        "sysroot {} → MSVC {} ({}: {}){}", spec->spec_str(), choice->version,
        origin, choice->product,
        tc.windowsSdkVersion.empty()
            ? std::string{}
            : std::format(" · Windows SDK {}", tc.windowsSdkVersion)));
    return {};
}

// ON THE CL.EXE ROW THE COMPILER IS ITS OWN SYSROOT: cl.exe cannot compile
// against another toolset's STL. A declared sysroot is therefore a second
// statement of the compiler's toolset, and one that names a different
// toolset is refused rather than silently ignored.
export std::expected<void, std::string>
check_cl_row_sysroot(const mcpp::toolchain::Toolchain& tc,
                     const mcpp::manifest::Manifest& m)
{
    auto tt = mcpp::toolchain::triple::parse(tc.targetTriple);
    if (!tt) return {};
    const std::string* declared = sysroot_override(m, *tt);
    if (!declared) return {};
    auto spec = mcpp::toolchain::parse_toolchain_spec(*declared);
    if (!spec || spec->version.empty() || spec->version == "system") return {};
    // <tools>/bin/Host<h>/<t>/cl.exe → <tools> is named by the toolset.
    const auto toolset = tc.binaryPath.parent_path().parent_path()
                             .parent_path().parent_path().filename().string();
    if (mcpp::toolchain::msvc::toolset_version_matches(spec->version, toolset))
        return {};
    return std::unexpected(std::format(
        "[target.{}].sysroot = '{}' names a different toolset than the "
        "compiler ({}, toolset {}). With cl.exe the compiler is its own "
        "sysroot: pin the toolset in the toolchain (`msvc@<toolset>`) and "
        "drop `sysroot`, or build with clang to compile against another "
        "toolset.",
        tt->str(), *declared, tc.binaryPath.string(), toolset));
}

// THE PROJECT'S MINIMUM PLATFORM VERSION FOR THIS TARGET, in one place.
//
// Two platforms fuse it into the effective triple and each names it in its own
// words: macOS's deployment target lives in `[build]` because it applies to
// every Apple artefact a project produces, and Android's API level lives in
// `[target.<triple>]` because it applies to one row. `llvm_triple` takes one
// parameter for both, so the choice between them is made here rather than at
// each of its call sites -- there are two, and a decision made twice is the
// shape this codebase records most often.
export std::string min_platform_version(const mcpp::manifest::Manifest& m,
                                 const mcpp::toolchain::triple::Triple& t,
                                 const std::filesystem::path& compilerPath) {
    if (t.is_android()) {
        if (auto it = m.targetOverrides.find(t.str()); it != m.targetOverrides.end())
            if (it->second.minApiLevel > 0)
                return std::to_string(it->second.minApiLevel);
        // AND THERE IS NO SUCH THING AS LEAVING IT OUT. This returned an empty
        // string with the comment "the NDK's own default, which clang
        // supplies", which was never verified and is false. Measured:
        //
        //     --target=aarch64-unknown-linux-android   (no level)
        //     sys/cdefs.h:365:2: error: Unversioned target triples are not
        //       supported!
        //
        // bionic refuses it, so the level is mandatory and a project that
        // never heard of API levels still needs one. The NDK declares the
        // floor it supports in `meta/platforms.json` and that is the honest
        // default -- the payload's own answer, which moves when the payload
        // does. macOS is the same shape and already works this way: its
        // default comes from the platform module, not from the manifest.
        // THE PAYLOAD'S OWN ANSWER FIRST, AND THE ENGINE'S DERIVATION AS
        // THE FALLBACK. `platform_floor` in `.mcpp-toolchain.json` is the
        // same number by a channel that does not require this engine to know
        // that an NDK keeps it in `meta/platforms.json`, nor that file's
        // schema. A payload shipping no descriptor still resolves, which is
        // what makes the descriptor additive.
        //
        // A MALFORMED descriptor is read as absence HERE ONLY, because this
        // function has no error channel and does not need one: a
        // payload-provided compiler reaches this point through
        // `payload_frontend`, which refuses a malformed descriptor by name
        // before any of these decisions are made.
        if (auto desc =
                mcpp::toolchain::payload_descriptor_for_compiler(compilerPath);
            desc && *desc && !(*desc)->platformFloor.empty())
            return (*desc)->platformFloor;
        if (auto level = mcpp::toolchain::ndk_min_api_level(compilerPath);
            level > 0)
            return std::to_string(level);
        return {};   // the caller refuses; see android_api_level_refusal
    }
    // APPLE'S TWO PLATFORMS ANSWER FROM TWO KEYS, ONE SLOT.
    //
    // "14.0" is a macOS version and means nothing to an iOS SDK, so the
    // project states them separately -- and only one of them can apply to any
    // one target, which is why they still share this function's single return
    // and the single fingerprint slot behind it.
    //
    // Empty is a legal answer here and not a refusal, unlike Android's, and
    // for the iOS rows prepare fills it with the located SDK's version before
    // this is read: an unversioned `arm64-apple-ios` made clang refuse
    // thread-local storage (measured, Xcode 16.4), so the driver's own
    // default is not the SDK's. Bionic rejects the unversioned triple
    // outright, which is the other half of the asymmetry.
    if (t.is_ios()) return m.buildConfig.iosDeploymentTarget;
    // AND ONLY FOR A macOS TARGET. `deployment_target` itself no longer
    // consults the host (#685); the discriminator is `t.os`, which is this
    // function's own target parameter and is available regardless of what
    // machine mcpp runs on. A non-Apple target (Linux, Windows, wasm,
    // freestanding) answers empty here, same as it always has.
    if (t.os == "macos")
        return mcpp::platform::macos::deployment_target(
            /*targetIsMacos=*/true, m.buildConfig.macosDeploymentTarget);
    return {};
}

// `[package]`, for the build program of the package that declares it.
//
// ONE CALL RATHER THAN A FIELD PER SITE. Two places build a
// `BuildProgramEnv` -- the dependency loop and the root -- and the values a
// build program is told about its own package are the same question in both.
// Setting them field by field at each site is how the two answers drift: the
// root gained `packageName` and the dependency loop gained it separately, and
// a value added to only one of them is a rule package that works for a root
// project and not for a dependency, with nothing failing to say so.
export void fill_package_build_env(mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& m)
{
    e.packageName        = m.package.name;
    e.packageNamespace   = m.package.namespace_;
    e.packageVersion     = m.package.version;
    e.packageDescription = m.package.description;
    e.packageLicense     = m.package.license;
    e.packageRepo        = m.package.repo;
    // ';' rather than ',': an author entry is conventionally `Name <mail@host>`
    // and a name may carry a comma, so a comma-joined list cannot be split back
    // into the entries it was made from.
    e.packageAuthors.clear();
    for (auto const& a : m.package.authors) {
        if (!e.packageAuthors.empty()) e.packageAuthors += ';';
        e.packageAuthors += a;
    }
}

// The target-facing answers a `build.mcpp` may ask the engine for.
//
// ONE function because there are TWO call sites — the root project and each
// dependency — and four values derived independently in two places is the
// shape this codebase keeps paying for. A board package that got the right
// answer as a root project and a stale one as a dependency would fail only in
// the consuming build, which is the harder direction to debug.
export void fill_target_build_env(mcpp::build::BuildProgramEnv& e,
                           const mcpp::manifest::Manifest& m,
                           const mcpp::toolchain::Toolchain* tc,
                           const mcpp::config::GlobalConfig* cfg)
{
    // The registry SubOS is where payloads are installed, whichever
    // toolchain or link mode resolved, so this is set before the toolchain
    // gate below.
    if (cfg) {
        const auto view = mcpp::xlings::paths::sysroot(mcpp::config::make_xlings_env(*cfg));
        e.pkgConfigLibdir = (view / "usr" / "lib" / "pkgconfig").generic_string()
            + mcpp::platform::env::path_list_separator()
            + (view / "usr" / "share" / "pkgconfig").generic_string();
    }
    e.toolchainDir  = (tc && !tc->binaryPath.empty())
        ? tc->binaryPath.parent_path().parent_path().string() : std::string{};
    e.targetSysroot = tc ? tc->targetSysrootRoot.string() : std::string{};
    e.compilerId    = !tc ? std::string{}
        : tc->compiler == mcpp::toolchain::CompilerId::GCC   ? "gcc"
        : tc->compiler == mcpp::toolchain::CompilerId::Clang ? "clang"
        : tc->compiler == mcpp::toolchain::CompilerId::MSVC  ? "msvc"
        : std::string{};
    e.targetLibc    = tc ? tc->targetSysrootPkg : std::string{};
    // Read from the toolchain the engine resolved, the same field the cache
    // key, the ABI tag and the toolchain fingerprint read. Not re-derived from
    // `compilerId`: clang answers "libc++" or "libstdc++" depending on how the
    // payload was configured, and deriving it here would restate an assumption
    // the resolver already measured.
    e.cxxStdlib     = tc ? tc->stdlibId : std::string{};
    if (!tc) return;

    // The two flags mcpp passes to ITS OWN compiler, so a rule package driving
    // a second compiler passes the same two. Both read from the single
    // producer that already decides them for the engine's own command lines —
    // `resolve_link_model` for the sysroot, `gcc::binutils_prefix_dir` for the
    // `-B` — rather than a fifth re-derivation of either.
    if (auto lm = mcpp::toolchain::resolve_link_model(*tc);
        lm.mode == mcpp::toolchain::CLibMode::Sysroot)
        e.toolchainSysroot = lm.sysroot.string();
    e.toolchainBinutilsDir = mcpp::toolchain::gcc::binutils_prefix_dir(*tc).string();

    // The C LIBRARY's sub-directory for this ISA profile, from the freestanding
    // table — the same single read point the compile flags use.
    //
    // Gated on there being a C library at all, and the gate is the point: the
    // value is a multilib convention, so on the zero-libc tier there is nothing
    // for it to be a convention OF. Emitting `rv64gc/lp64d` there would hand a
    // kernel a path into a directory that does not exist, and the name of the
    // accessor would be a lie. All three libc-facing answers are empty together.
    if (!e.targetSysroot.empty())
        if (auto spec = mcpp::freestanding::resolve(tc->targetTriple))
            e.targetLibcProfile = std::string(spec->libdir);

    // Which builtins library the RESOLVED toolchain ships. Freestanding only:
    // on a hosted target the driver links them without being asked, and
    // handing a package a name it must not use would invite it to.
    if (auto t = mcpp::toolchain::triple::parse(tc->targetTriple);
        t && t->is_freestanding()) {
        e.targetBuiltinsLib = mcpp::toolchain::is_clang(*tc)
            ? "clang_rt.builtins-" + t->arch
            : std::string("gcc");
    }

    // #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION. One call, so a new consumer
    // (`dist-apple`, `dist-apk`) reads the same answer the compiler flag and
    // the fingerprint slot already resolved, rather than restating it.
    if (auto tt = mcpp::toolchain::triple::parse(tc->targetTriple))
        e.minPlatformVersion = min_platform_version(m, *tt, tc->binaryPath);
}

bool is_std_module(std::string_view name) {
    return name == "std" || name == "std.compat";
}

export bool graph_or_targets_import_std(const mcpp::modgraph::Graph& graph,
                                 const mcpp::manifest::Manifest& manifest,
                                 const std::filesystem::path& projectRoot) {
    for (auto& u : graph.units) {
        for (auto& req : u.requires_) {
            if (is_std_module(req.logicalName))
                return true;
        }
    }

    // Some target entry files can be added to the plan after the package scan.
    // Check them here so std BMI setup matches what make_plan will compile: they
    // are read by the same scan_entry_file make_plan reads them with.
    const auto extTable = mcpp::extension_table_for(manifest.buildConfig.moduleExtensions,
                                                    manifest.buildConfig.deviceExtensions);
    for (auto& t : manifest.targets) {
        if (t.main.empty()) continue;
        const auto entry = mcpp::modgraph::scan_entry_file(projectRoot / t.main,
                                                           manifest.package.name, extTable);
        for (auto const& req : entry.requires_)
            if (is_std_module(req.logicalName)) return true;
    }
    return false;
}

} // namespace mcpp::build

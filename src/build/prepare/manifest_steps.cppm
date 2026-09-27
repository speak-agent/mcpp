// mcpp.build.prepare.manifest_steps — what prepare_build does to a
// package's manifest between loading it and capturing it: the warning for
// descriptor keys this engine does not know, the generated files, the notes
// for flag elements whose words changed, the inheritance of a workspace
// member reached as a dependency, the second conditional pass (predicates
// that name a target-side layer), and feature activation. Imported by
// mcpp.build.prepare only.

export module mcpp.build.prepare.manifest_steps;

import std;
import mcpp.manifest;
import mcpp.targetside;
import mcpp.ui;
import mcpp.platform;
import mcpp.project;
import mcpp.build.flags;
import mcpp.build.prepare_inputs;
import mcpp.build.prepare.manifest_merge;

namespace mcpp::build {

// mcpp#237: surface xpkg-descriptor mcpp-segment keys this mcpp did not
// recognise. The parser collects them into `xpkgUnknownKeys` and skips the
// value; without this a typo like `dependencies = {...}` (correct key: `deps`)
// dropped the dependency with no diagnostic. Called at the descriptor-adoption
// sites (a fetched dep with no mcpp.toml, synthesized from the index `mcpp={}`
// block) — the single place the descriptor becomes a build input. Warning (not
// hard error) keeps forward-compat: an older mcpp building a newer descriptor
// should not fail outright, only tell the user what it ignored.
export inline void warn_unknown_xpkg_keys(const mcpp::manifest::Manifest& dm,
                                   std::string_view depLabel) {
    // A LAYER NAME THIS ENGINE DOES NOT KNOW IS A VERSION GAP, NOT A TYPO,
    // WHEN IT ARRIVES FROM A DEPENDENCY.
    //
    // The reserved `mcpp:` prefix is a closed set so a misspelling cannot
    // silently disable a behaviour. Refusing a DEPENDENCY's manifest for it made
    // the set closed in a second sense nobody intended: a published package
    // could never declare a layer named after the reader was released.
    // Ignoring the layer and saying so is what this engine already does for
    // every other unknown key, and it is the only response that lets the
    // vocabulary grow.
    for (auto const& cap : dm.unknownCapabilities) {
        auto why = mcpp::targetside::parse_capability(cap);
        mcpp::ui::warning(std::format(
            "dependency '{}': {}\n"
            "       Ignored, and this build proceeds without that layer. "
            "A newer mcpp may resolve it.",
            depLabel,
            why ? std::format("`{}` names no capability mcpp knows.", cap)
                : why.error()));
    }
    for (auto const& key : dm.xpkgUnknownKeys) {
        auto suggestion = mcpp::manifest::closest_known_xpkg_key(key);
        if (suggestion.empty())
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored (schema mismatch or typo)", depLabel, key));
        else
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored; did you mean '{}'?", depLabel, key, suggestion));
    }
}

// `stale`, when given, turns the function into a comparison: nothing is
// created or written, and every declared file that is missing or differs from
// its declared content is appended. A build that describes itself rather than
// running (BuildOverrides::plan_only) reads the root package's generated files
// this way, because they live in the source tree it promises not to write.
export std::expected<void, std::string>
materialize_generated_files(const std::filesystem::path& root,
                            const mcpp::manifest::Manifest& manifest,
                            std::vector<std::filesystem::path>* stale = nullptr)
{
    for (auto const& [relPath, content] : manifest.buildConfig.generatedFiles) {
        if (relPath.empty()) {
            return std::unexpected("generated_files contains an empty path");
        }
        if (relPath.is_absolute()) {
            return std::unexpected(std::format(
                "generated_files path '{}' must be relative", relPath.generic_string()));
        }
        auto const genericPath = relPath.generic_string();
        for (std::size_t begin = 0; begin <= genericPath.size();) {
            auto const end = genericPath.find('/', begin);
            auto const part = genericPath.substr(begin, end == std::string::npos
                                                           ? std::string::npos
                                                           : end - begin);
            if (part == "..") {
                return std::unexpected(std::format(
                    "generated_files path '{}' must not escape the package root",
                    relPath.generic_string()));
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }

        auto out = root / relPath.lexically_normal();

        // Skip the write when the on-disk content is already identical: ninja
        // is mtime-driven, and an unconditional rewrite bumps the mtime every
        // build, recompiling every TU that #includes the materialized file
        // (via depfiles) — e.g. a frozen-snapshot config.h included by
        // thousands of TUs. Change detection is already owned by the
        // fingerprint (content is folded in above), so skipping only
        // preserves the mtime — mirroring the build.mcpp cache design,
        // which likewise avoids mtime churn on unchanged outputs.
        {
            std::ifstream is(out, std::ios::binary);
            if (is) {
                std::string existing((std::istreambuf_iterator<char>(is)),
                                     std::istreambuf_iterator<char>());
                if (is && existing == content) {
                    continue;
                }
            }
        }
        if (stale) {
            stale->push_back(out);
            continue;
        }

        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot create directory for generated file '{}': {}",
                out.string(), ec.message()));
        }
        std::ofstream os(out, std::ios::binary);
        if (!os) {
            return std::unexpected(std::format(
                "cannot write generated file '{}'", out.string()));
        }
        os << content;
        if (!os) {
            return std::unexpected(std::format(
                "failed while writing generated file '{}'", out.string()));
        }
    }
    return {};
}

// ── An element whose words changed in 2026.9.17.1 (#655) ─────────────────────
//
// A compile-flag element used to reach the compiler as its host's command-line
// reader made it (POSIX `sh`, or the MSVCRT rules), after ninja had replaced
// `$` sequences, with a `-D` element containing a space quoted whole (#234).
// It now reaches the compiler as `flag_words` reads it, and a `defines` value
// is one word. Most spellings mean the same under both readings; the ones that
// do not are told what the compiler receives now and what it received before.
// The previous reading is modelled on quote removal only: `sh` expansions
// (`$VAR`, globs) are not reproduced.
//
// WHEN IT IS SAID. The notes are collected while manifests load and released
// where the output directory is decided, only if that directory has no
// build.ninja yet. The fingerprint names the mcpp version and every flag, so
// that is the first plan after an upgrade, after a flag was edited, or in a
// fresh checkout. A build that repeats a plan says nothing, so a manifest that
// is already spelled for the new reading is not warned about on every run.
export std::vector<std::pair<std::string, std::string>>& pending_flag_words_notes() {
    static std::vector<std::pair<std::string, std::string>> notes;
    return notes;
}
std::vector<std::string> previous_release_words(std::string element, bool define) {
    if (define) element = "-D" + element;
    if ((element.starts_with("-D") || element.starts_with("/D"))
        && element.find(' ') != std::string::npos)
        element = mcpp::build::shell_quote_arg(element);
    std::string line;
    for (std::size_t i = 0; i < element.size(); ++i) {
        if (element[i] != '$' || i + 1 == element.size()) { line.push_back(element[i]); continue; }
        const char n = element[i + 1];
        if (n == '$' || n == ' ' || n == ':') { line.push_back(n); ++i; continue; }
        // A ninja variable reference: `${name}` or `$name`, empty on a compile edge.
        std::size_t j = i + 1;
        if (n == '{') {
            while (j < element.size() && element[j] != '}') ++j;
        } else {
            while (j + 1 < element.size()
                   && (std::isalnum(static_cast<unsigned char>(element[j + 1]))
                       || element[j + 1] == '_' || element[j + 1] == '-'))
                ++j;
        }
        i = j;
    }
    return mcpp::manifest::host_command_words(line, mcpp::platform::is_windows);
}

export void report_flag_words_changes(const mcpp::manifest::Manifest& m) {
    auto show = [](const std::vector<std::string>& words) {
        std::string out = "[";
        for (auto const& w : words)
            out += std::format("{}'{}'", out.size() > 1 ? ", " : "", w);
        return out + "]";
    };
    auto note_change = [&](std::string what, std::string hint) {
        auto note = std::pair{std::move(what), std::move(hint)};
        auto& notes = pending_flag_words_notes();
        if (std::ranges::find(notes, note) == notes.end()) notes.push_back(std::move(note));
    };
    auto const who = m.package.name.empty() ? std::string("(root)") : m.package.name;
    auto check = [&](std::string_view where, const std::vector<std::string>& list,
                     bool define) {
        for (auto const& e : list) {
            auto now = define ? std::vector<std::string>{"-D" + e}
                              : mcpp::manifest::flag_words(e);
            auto before = previous_release_words(e, define);
            if (now == before) continue;
            note_change(std::format(
                "{}: {} element '{}' reaches the compiler as {}; mcpp before "
                "2026.9.17.1 passed {} on this host",
                who, where, e, show(now), show(before)),
                std::string(
                "a compile-flag element is read by one syntax on every host, and a "
                "`defines` entry is one value (docs/04-mcpp-toml.md, "
                "\"Compile-flag syntax\"); spell the element so that it reads as the "
                "words meant"));
        }
    };
    // THE SAME QUESTION FOR THE LINK FLAGS, which take the reading from
    // 2026.9.26.2 (#703). Before, a `-L` or `-Wl,-rpath,` element was escaped
    // for ninja, so its text reached the host's reader as written, and any
    // other element was pasted into the ninja rule, so ninja replaced its `$`
    // sequences first. `$ORIGIN` written plainly reads the same under both
    // models, because neither reproduces the shell's expansion that lost it;
    // an element escaped for ninja or for the shell by hand is what differs.
    auto check_link = [&](std::string_view where, const std::vector<std::string>& list) {
        for (auto const& e : list) {
            auto now = mcpp::manifest::flag_words(e);
            auto before = e.starts_with("-L") || e.starts_with("-Wl,-rpath,")
                ? mcpp::manifest::host_command_words(e, mcpp::platform::is_windows)
                : previous_release_words(e, false);
            if (now == before) continue;
            note_change(std::format(
                "{}: {} element '{}' reaches the linker as {}; mcpp before "
                "2026.9.26.2 passed {} on this host",
                who, where, e, show(now), show(before)),
                std::string(
                "a link-flag element is read by the compile-flag syntax, so `$ORIGIN` "
                "reaches the linker as written and an element escaped for ninja or the "
                "shell by hand is no longer unescaped (docs/04-mcpp-toml.md, "
                "\"Compile-flag syntax\"); spell the element so that it reads as the "
                "words meant"));
        }
    };
    auto const& bc = m.buildConfig;
    check_link("[build] ldflags", bc.ldflags);
    check("[build] cflags", bc.cflags, false);
    check("[build] cxxflags", bc.cxxflags, false);
    check("[build] defines", bc.defines, true);
    for (auto const& gf : bc.globFlags) {
        check("flags cflags", gf.cflags, false);
        check("flags cxxflags", gf.cxxflags, false);
        check("flags asmflags", gf.asmflags, false);
        check("flags defines", gf.defines, true);
    }
    for (auto const& [feature, defines] : bc.featureDefines)
        check(std::format("features.{} defines", feature), defines, true);
    for (auto const& [feature, globs] : bc.featureFlags) {
        for (auto const& gf : globs) {
            check(std::format("features.{} cflags", feature), gf.cflags, false);
            check(std::format("features.{} cxxflags", feature), gf.cxxflags, false);
            check(std::format("features.{} asmflags", feature), gf.asmflags, false);
            check(std::format("features.{} defines", feature), gf.defines, true);
        }
    }
    for (auto const& t : m.targets) {
        check(std::format("targets.{} cflags", t.name), t.cflags, false);
        check(std::format("targets.{} cxxflags", t.name), t.cxxflags, false);
        check(std::format("targets.{} defines", t.name), t.defines, true);
    }
}

// WHAT A MEMBER RECEIVES FROM ITS WORKSPACE WHEN IT IS REACHED AS A DEPENDENCY.
//
// Three parts of the inheritance matter to a dependency: `[workspace.package]`
// (a member may omit `version`), `x.workspace = true` dependency entries
// (without the merge the entry reaches resolution with neither version nor
// path), and `[workspace.build]`. They are applied at the dependency's LOAD
// site, before the conditional merge and the `defines` fold, which is the
// order the root follows; `makePackageRoot` only captures the result (#690).
// `[toolchain]`, `[target.<triple>]` and `[indices]` are decided by the root
// for the whole graph and are not applied to a dependency.
//
// One function for every way a member is reached: a sibling `path`
// dependency, a member of a git-hosted workspace, and a member inside an
// index package's archive. The same commit then compiles the same way in its
// own checkout and in every consumer's graph.
export std::optional<std::string>
inherit_as_workspace_member(mcpp::manifest::Manifest& member,
                            const mcpp::manifest::Manifest& workspace,
                            const std::filesystem::path& workspaceRoot,
                            const std::filesystem::path& memberDir) {
    mcpp::project::inherit_workspace_package(member, workspace);
    mcpp::project::merge_workspace_deps(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_build(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_xlings(member, workspace);
    return mcpp::project::workspace_inheritance_error(member, memberDir);
}

// The workspace whose `members` list `memberDir`, searched upward from its
// parent and never above `bound` (an index package's install root: the
// archive is the only tree the package's author wrote).
export std::optional<std::pair<mcpp::manifest::Manifest, std::filesystem::path>>
workspace_listing(const std::filesystem::path& memberDir,
                  const std::filesystem::path& bound) {
    auto inside = [&](const std::filesystem::path& p) {
        auto rel = p.lexically_normal().lexically_relative(bound.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    };
    for (auto p = memberDir.parent_path(); inside(p); p = p.parent_path()) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            if (auto ws = mcpp::manifest::load(p / "mcpp.toml");
                ws && ws->workspace.present
                && mcpp::project::is_workspace_member(*ws, p, memberDir))
                return std::pair{std::move(*ws), p};
        }
        if (p == p.parent_path()) break;
    }
    return std::nullopt;
}

// ── The SECOND conditional pass: predicates that name a target-side layer ────
//
// #540/#494. `docs/14` documents a package adapting to the C library it was
// built over — `[target.'cfg(c-abi = "musl")'.build] std-module-flags =
// ["-D_GNU_SOURCE"]`, wrong for picolibc — and `stdModuleFlags` was moved onto
// BuildInputs FOR this, its member comment saying membership "is what makes the
// cfg axis carry it". Nothing evaluated the predicate: `cfgpred::Ctx` was built
// from the triple alone, so every such section was dropped in silence and the
// package built with the wrong C-library configuration, successfully.
//
// WHY A SECOND PASS AND NOT AN EARLIER CONTEXT. A layer is answerable only
// after dependency resolution — a package in the graph may supply the C library
// (openkal-musl under a `-gnu` triple), which is exactly why the triple's `env`
// segment is a REQUEST and not the answer (docs/specs/target-side.md §3.4). The
// first merge runs before resolution because conditional DEPENDENCIES have to.
//
// WHERE IT RUNS. Between `tsd::resolve` and the P1689 scan — the same window in
// which build.mcpp already contributes build inputs by mirroring into
// `packages[0]`. Everything downstream reads the snapshot from there on: the
// scan, `stdModuleFlags` collection, the fingerprint, and `compute_flags`.
//
// SCOPE. Build INPUTS only, which is what docs/14 promises ("available in
// [build] sections only"). Dependencies are excluded by construction — they are
// already resolved by now — and a section that tries is reported rather than
// silently ignored; see `warn_layer_predicate_dependencies`.
export bool merge_layer_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx)
{
    bool any = false;
    for (auto const& cc : m.conditionalConfigs) {
        if (!cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        any = true;
        mcpp::manifest::append(m.buildConfig, cc.inputs);
        // Same mirror the first pass does: `modules.sources` is the scanner's
        // own view and is not a BuildInputs member.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
        for (auto const& f : cc.frameworks)
            m.runtimeConfig.linkIntent.frameworks.push_back(f);
        // NO `merge_conditional_xlings` HERE, DELIBERATELY. A cc that reaches
        // this pass has a layer in its predicate, and one carrying tools was
        // refused long before — see `layer_predicated_xlings_refusal`. Folding
        // it here would be folding after the tools were provisioned and after
        // every build.mcpp ran, which is the silent-absence failure the
        // refusal exists to prevent.
    }
    // Re-fold only when something was added. The call is safe either way — the
    // function clears `defines` after folding and says so — but skipping it
    // keeps this pass a no-op for the overwhelming majority of manifests, which
    // name no layer at all.
    if (any) fold_build_defines_into_flags(m.buildConfig);
    return any;
}

// Feature-activation closure — THE single implementation (build.mcpp env
// contract, Stage 2a feature-deps, and the main feature pass all call this):
// seed = [features].default ∪ requested, expanded transitively over implies;
// the literal name "default" is never itself a feature.
//
// `seedDefault` is the funnel for consumer-side `default-features = false`
// (#242, Cargo parity): when false the dependency's own `[features].default`
// is NOT seeded, so only the explicitly `requested` features (and their
// transitive `implies`) activate. The root package always seeds its own
// default (seedDefault=true); a dependency passes its dep spec's
// `defaultFeatures` flag. `requested` is applied identically either way.
export std::vector<std::string> feature_closure(const mcpp::manifest::Manifest& pm,
                                         const std::vector<std::string>& requested,
                                         bool seedDefault = true)
{
    std::vector<std::string> act, q;
    if (seedDefault)
        if (auto it = pm.featuresMap.find("default"); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    q.insert(q.end(), requested.begin(), requested.end());
    std::set<std::string> seen;
    while (!q.empty()) {
        auto f = q.back(); q.pop_back();
        if (f == "default" || !seen.insert(f).second) continue;
        act.push_back(f);
        if (auto it = pm.featuresMap.find(f); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    }
    return act;
}

// --features value → tokens (comma/space separated).
std::vector<std::string> feature_request_tokens(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p < s.size();) {
        auto c = s.find_first_of(", ", p);
        auto tok = s.substr(p, c == std::string_view::npos ? std::string_view::npos : c - p);
        if (!tok.empty()) out.emplace_back(tok);
        if (c == std::string_view::npos) break;
        p = c + 1;
    }
    return out;
}

// The root's own features among the --features tokens.
//
// A TOKEN CONTAINING `/` IS NOT A FEATURE OF THE ROOT (#649 E8). It can only
// mean "open this feature of that dependency", which is what the same token
// means inside `[features]`, so it is taken out here and applied as a forward
// of the root (`feature_forward_request`). It used to stay in this list, where
// a root without `[features]` turned it into `-DMCPP_FEATURE_SPIKE_FW_INSTALLER`
// and a root with the table reported it as an undeclared feature; neither
// opened the dependency's feature.
export std::vector<std::string> parse_feature_request(std::string_view s) {
    std::vector<std::string> out;
    for (auto& tok : feature_request_tokens(s))
        if (tok.find('/') == std::string::npos) out.push_back(std::move(tok));
    return out;
}

// The `<dependency key>/<feature>` tokens of --features, in the keyspace of a
// `[features]` forward. A token with an empty half is kept whole and named by
// the caller, rather than being dropped as the manifest parser drops it: on a
// command line the user typed it just now.
export std::vector<std::string> feature_forward_request_tokens(std::string_view s) {
    std::vector<std::string> out;
    for (auto& tok : feature_request_tokens(s))
        if (tok.find('/') != std::string::npos) out.push_back(std::move(tok));
    return out;
}

} // namespace mcpp::build

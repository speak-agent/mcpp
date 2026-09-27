// mcpp.build.prepare.manifest_merge — the conditional merge every
// package's manifest passes through before it is captured into the graph:
// `[target.<selector>]` build inputs, dependencies and tools folded into the
// flat tables, and `[build].defines` folded into the flag lists.
// Re-exported by mcpp.build.prepare.

export module mcpp.build.prepare.manifest_merge;

import std;
import mcpp.manifest;
import mcpp.diag;
import mcpp.xlings.address_set;
import mcpp.pack.prebuilt;
import mcpp.build.prepare_inputs;

namespace mcpp::build {

// `[target.<sel>.xlings…]` — the TARGET axis of the tool plane (SPEC-004 §4).
//
// Folded into `m.xlings` exactly as the conditional build inputs fold into
// `m.buildConfig`, so every downstream reader — the two provisioning passes,
// `fillXpkgDirs`, the materialised `.xlings.json` — stays on one flat list and
// none of them has to learn that a second axis exists.
//
// THE HOST AXIS IS NOT TOUCHED. Top-level `[xlings]` has already resolved its
// platform-keyed values against the host by the time this runs; folding here
// puts the target's entries beside them.
//
// DEDUP IS BY PACKAGE, NOT BY ADDRESS. `xim:glibc` and `xim:glibc@2.40` are two
// addresses for ONE install, and keeping both asks xlings for the same package
// twice at two versions — which is not a build that fails, it is a build whose
// environment depends on iteration order. Where both axes name a package, the
// conditional entry wins, because it is the more specific statement; that is
// the rule the flag half of this merge follows by appending after the base
// entries. A disagreement is reported, because it is the one case where the
// author wrote two things and only one of them can happen.
export void merge_conditional_xlings(mcpp::manifest::Manifest& m,
                                     const mcpp::manifest::ConditionalConfig& cc)
{
    // ONE DEFINITION OF IDENTITY, and it is not local to this merge. It used
    // to be `parse_address(a).target` — the bare name, so `xim:cuda` and a
    // hypothetical `scode:cuda` collided, and the graph split in prepare_build
    // compared whole address strings instead. See
    // mcpp.xlings.address_set for what the two definitions cost.
    auto package_of = [](std::string_view address) {
        return mcpp::xlings::addrset::package_key(address);
    };
    for (auto const& a : cc.xlings.deps) {
        const auto pkg = package_of(a);
        auto it = std::ranges::find_if(m.xlings.deps, [&](const std::string& e) {
            return package_of(e) == pkg;
        });
        if (it == m.xlings.deps.end()) { m.xlings.deps.push_back(a); continue; }
        if (*it != a)
            mcpp::diag::warning("xlings/axis-override", std::format(
                "'{}' is declared on both tool axes, as '{}' and as '{}'. The "
                "[target.<selector>] entry is the more specific statement and "
                "is the one used. Declare a tool that runs on the build machine "
                "in the top-level [xlings.workspace], and what the produced "
                "code is compiled against under [target.<selector>.xlings."
                "workspace] — see docs/05 section 2.13.", pkg, *it, a));
        *it = a;
    }
    // Keyed by PACKAGE, so the same override applies without a second search.
    for (auto const& [pkg, pin] : cc.xlings.workspace)
        m.xlings.workspace.insert_or_assign(pkg, pin);
    // Keyed by ADDRESS. `insert_or_assign` rather than `try_emplace` for the
    // same reason: the address that survived above is the conditional one.
    for (auto const& [addr, w] : cc.xlings.depWhen)
        m.xlings.depWhen.insert_or_assign(addr, w);
    for (auto const& [f, addrs] : cc.xlings.featureDeps) {
        auto& dst = m.xlings.featureDeps[f];
        for (auto const& a : addrs) {
            const auto pkg = package_of(a);
            auto it = std::ranges::find_if(dst, [&](const std::string& e) {
                return package_of(e) == pkg;
            });
            if (it == dst.end()) dst.push_back(a); else *it = a;
        }
    }
    for (auto const& [addr, pin] : cc.xlings.featurePins)
        m.xlings.featurePins.insert_or_assign(addr, pin);
}

// A `[target.<selector>.xlings…]` selector MUST NOT name a RESOLVED layer.
//
// Not a style rule, a schedule one. The five resolved layer keys (`c-abi`,
// `compiler`, …) are answered by dependency RESOLUTION, so a predicate naming
// one is held back to the second merge pass (`merge_layer_conditional_config`),
// which runs after every package's build.mcpp has already run and after the
// root's tool provisioning. An entry admitted there would be declared and never installed,
// and the failure it produces is the worst-shaped one there is: the build
// succeeds and the tool is simply absent.
//
// `accelerator` IS ADMITTED, and used to be refused here with the rest. It is
// not resolved from anything: it is `--accel`, or `[build] accel`, read before
// the first package is looked up, so a payload predicated on it is merged in
// the FIRST pass and installed like any other. Refusing it had a cost paid on
// every build of every project with a device island — the vendor toolkit is
// declared unconditionally or not at all, so a CPU-only build downloaded
// gigabytes for a device it was not compiling for.
//
// Refused rather than deferred, and refused at the earliest point that can
// see the predicate. The gate plane answers the case this shape is reached
// for: `[feature-xlings.<f>]` selects a tool by what the project asked for,
// and a feature is known before anything is provisioned.
export std::optional<std::string>
layer_predicated_xlings_refusal(const mcpp::manifest::Manifest& m)
{
    for (auto const& cc : m.conditionalConfigs) {
        if (cc.xlings.empty()) continue;
        if (!cfgpred::uses_layer(cc.predicate)) continue;
        std::string named;
        for (auto const& a : cc.xlings.deps) {
            if (!named.empty()) named += ", ";
            named += a;
        }
        for (auto const& [f, addrs] : cc.xlings.featureDeps)
            for (auto const& a : addrs) {
                if (!named.empty()) named += ", ";
                named += std::format("{} (feature '{}')", a, f);
            }
        return std::format(
            "[target.'{}'] declares tools ({}), but its predicate names a "
            "target-side layer. A layer is answered by dependency resolution, "
            "which happens after tools are installed and after build programs "
            "run, so a tool conditioned on one would be declared and never "
            "installed. Condition it on the target instead "
            "(`[target.'cfg(os = \"linux\")'.xlings.workspace]`), on the "
            "accelerator (`[target.'cfg(accelerator = \"cuda\")'.xlings"
            ".workspace]`, which IS answered before provisioning), or on a "
            "feature (`[feature-xlings.<feature>]`). See docs/05 section 2.13.",
            cc.predicate, named);
    }
    return std::nullopt;
}

// Two declarations of one dependency, compared by the identity their keys
// normalise to rather than by the keys themselves: `fw` and `mcpplibs.fw` are
// one package under two map keys (`selector.stableMapKey`), and a comparison
// of keys would leave both entries in the map for the resolver to see.
bool same_dependency_identity(const mcpp::manifest::DependencySpec& a,
                              const mcpp::manifest::DependencySpec& b) {
    if (a.shortName.empty() || b.shortName.empty()) return false;
    return a.namespace_ == b.namespace_ && a.shortName == b.shortName;
}

void replace_dependencies(
    std::map<std::string, mcpp::manifest::DependencySpec>& into,
    const std::map<std::string, mcpp::manifest::DependencySpec>& from)
{
    for (auto const& [key, spec] : from) {
        std::erase_if(into, [&](auto const& entry) {
            return entry.first == key || same_dependency_identity(entry.second, spec);
        });
        into[key] = spec;
    }
}

// L1 cfg merge for ONE package's manifest (root or ANY dependency — path,
// git, or version/registry): append the matching conditional
// cflags/cxxflags/ldflags and sources (G1b) to its buildConfig. Sources also
// update the legacy modules.sources mirror — the scanner walks that.
//
// #229: this is the SINGLE funnel for cfg-conditional sources/flags — every
// package's manifest passes through exactly one call to this function,
// always immediately BEFORE that manifest is captured into `packages[]` via
// makePackageRoot()/propagateLinkFlags() (which snapshot buildConfig into
// privateBuild/linkUsage and into the root's propagated ldflags — merging
// any later than that point is silently lost for flags, though not for
// sources, which the modgraph scan re-reads live). Three call sites, one per
// loading branch, together cover every package exactly once: the root
// (before its own makePackageRoot), the path/git-dep branch, and
// loadVersionDep() (shared by the main per-dependency loop, the
// multi-version mangling secondary, and the SemVer-merge re-fetch — all three
// of ITS callers get the merge for free from the one call inside it).
// The dependency MAPS ride the same funnel (#359). They used to be merged by
// a hand-written loop at the root call site only, with a comment declaring a
// dependency's own conditional deps "out of scope". That was the #229 shape
// one level up: three call sites merged build inputs, ONE of them also merged
// deps, and nothing said why. A package's `[target.windows.dependencies]` is
// its own statement about itself and means the same thing whether the package
// is the root or someone's dependency.
// The resolved triple travels INSIDE `ctx` (cfgpred::Ctx::triple). It used to
// be a third parameter here too, which is how a bare-triple predicate came to
// disagree with a cfg() one about the same native build — see the note on Ctx.
export void merge_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx)
{
    // Recorded before the first merge; see Manifest::beforeConditionalMerge.
    if (!m.beforeConditionalMerge)
        m.beforeConditionalMerge = std::make_shared<const mcpp::manifest::Manifest>(m);
    // A DISTRIBUTION package may carry a leg's link line twice: as `ldflags`
    // (GNU spelling, which is all an older mcpp reads) and as the neutral
    // `[target.<pred>.runtime]` pair, which mcpp renders per dialect. Applying
    // both would put `-L` on a native `cl.exe` command line, which is exactly
    // what the neutral form exists to avoid — so where the neutral form is
    // present it REPLACES the ldflags rather than adding to them.
    //
    // Scoped to distribution packages on purpose: a hand-written manifest that
    // states both may well mean both (`ldflags` also carries things like
    // `-Wl,--as-needed`), and silently dropping half of it would be its own
    // silent failure.
    const bool generatedPackage = mcpp::pack::is_distribution_package(m);

    for (auto const& cc : m.conditionalConfigs) {
        // THE TWO PASSES MUST BE DISJOINT, AND `matches()` ALONE DOES NOT
        // MAKE THEM SO. A layer key answers false here because `layersKnown` is
        // false — but `cfg(any(linux, c-abi = "musl"))` still matches on its
        // triple leg, and the second pass would match it again and `append()`
        // the same inputs twice. Membership, not the answer, decides ownership:
        // a predicate that NAMES a layer belongs to the second pass entirely.
        if (cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        const bool neutralWins = generatedPackage
                              && (!cc.linkLibraryDirs.empty() || !cc.libraries.empty()
                                  || !cc.frameworks.empty());
        // One append() for every field the axis may carry (#258). Matching
        // sections land AFTER the base entries, so a conditional rule beats
        // a broader unconditional one under GNU last-wins — which is what
        // makes an off-OS REMOVAL expressible (`-U` after the base `-D`).
        if (neutralWins) {
            // Drop the LIBRARY REFERENCES, not the whole ldflags list.
            //
            // Clearing it outright was a measured regression: a PE/MinGW shared
            // leg's ldflags also carry `-Wl,-Bdynamic`, without which `-static`
            // leaves ld in static-only mode and it refuses the import library
            // with `have you installed the static version of the mathkit
            // library?`. e2e 257 caught it.
            //
            // The neutral form replaces exactly what it can express — a library
            // and where to find it. Anything else in that block says something
            // it cannot say, and must survive.
            auto inputs = cc.inputs;
            std::erase_if(inputs.ldflags, [](std::string_view f) {
                return f.starts_with("-L") || f.starts_with("-l")
                    || f.starts_with("/LIBPATH:");
            });
            mcpp::manifest::append(m.buildConfig, inputs);
        } else {
            mcpp::manifest::append(m.buildConfig, cc.inputs);
        }
        // The neutral half goes where `render_link_intent_flags` will find it.
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
        for (auto const& f : cc.frameworks)
            m.runtimeConfig.linkIntent.frameworks.push_back(f);
        merge_conditional_xlings(m, cc);
        // `[target.<sel>.abi]`: recorded for every package; rendered only for
        // the root, where prepare_build reads it. Last matching section wins,
        // the rule every other conditional scalar follows.
        if (cc.abiThreadsDeclared) {
            m.buildConfig.abiThreads = cc.abiThreads;
            m.buildConfig.abiThreadsDeclared = true;
        }
        if (cc.abiExceptionsDeclared) {
            m.buildConfig.abiExceptions = cc.abiExceptions;
            m.buildConfig.abiExceptionsDeclared = true;
        }
        // `[target.<sel>] requires_abi` / `.feature-requires-abi` (A6): a
        // requirement on the TARGET axis, unioned in -- not overwritten --
        // because more than one matching selector may ask for the same
        // member, and every one of them is a true statement. The selector
        // text rides along so the unmet-requirement check can name what
        // asked, the same courtesy `[package] requires_abi` gets by naming
        // "the package" and a feature's entry by naming the feature.
        if (cc.requiresAbiThreads)
            m.targetRequiresAbiThreads.push_back(cc.predicate);
        if (cc.requiresAbiExceptions)
            m.targetRequiresAbiExceptions.push_back(cc.predicate);
        for (auto const& [f, val] : cc.featureRequiresAbiThreads)
            if (val) m.targetFeatureRequiresAbiThreads[f].push_back(cc.predicate);
        for (auto const& [f, val] : cc.featureRequiresAbiExceptions)
            if (val) m.targetFeatureRequiresAbiExceptions[f].push_back(cc.predicate);
        // `modules.sources` is the scanner's own view and is not part of
        // BuildInputs, so conditional sources are mirrored into it here.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        // A matching conditional declaration of a dependency REPLACES the
        // declaration of the same identity, and a later matching section
        // replaces an earlier one: the rule every conditional scalar above
        // follows (#634, A1). This used to be `insert()`, which kept the
        // unconditional entry, so `linkage = "shared"` written for one row was
        // dropped on that row without a word. No manifest among 509 scanned
        // declared one dependency in both tables, so no build that worked
        // changes; the declaring table rides on the spec (`declaredIn`) into
        // the resolution record.
        replace_dependencies(m.dependencies, cc.dependencies);
        replace_dependencies(m.devDependencies, cc.devDependencies);
        replace_dependencies(m.buildDependencies, cc.buildDependencies);
        // #359: `[target.<sel>.feature-deps.<feature>]`. The feature is
        // registered by the parser regardless of the predicate; only what it
        // pulls in is conditional.
        for (auto const& [fname, deps] : cc.featureDeps)
            replace_dependencies(m.featureDeps[fname], deps);
        // `[target.<sel>.targets.<name>] kind`: the row's form of a library
        // target, applied before resolution, so the link-form resolution
        // reads it exactly as it reads `[targets.<name>] kind`. `load` has
        // already refused a name that is not a library target.
        //
        // `linkage` (#642 E1) is the row's default form, and a row's statement
        // REPLACES the statement it follows, whichever of the two each one is:
        // a default after `kind = "shared"` returns the target to the library
        // form a consumer may choose from, and a `kind` after a default clears
        // the default. Last matching section wins, as for every conditional
        // scalar.
        for (auto const& [name, row] : cc.targetKinds) {
            for (auto& t : m.targets) {
                if (t.name != name) continue;
                t.kindDeclaredBy = row.statement;
                t.kindFromRow = true;
                if (!row.linkage.empty()) {
                    t.kind = mcpp::manifest::Target::Library;
                    t.linkageDefault = row.linkage;
                    t.linkageDeclaredBy = row.statement;
                } else {
                    t.kind = row.kind;
                    t.linkageDefault.clear();
                    t.linkageDeclaredBy.clear();
                }
            }
        }
    }
}

// The macro name a `defines` entry or a `-D` word defines: the text before
// the first `=`, or the whole text when there is no value.
std::string_view define_name(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}

// Desugar `[build].defines` into `-D<x>` on both C and C++ flag channels.
//
// ORDER (both halves are load-bearing): this must run AFTER every merge that
// contributes `defines` (workspace inheritance, then the package's own table,
// then a matching `[target.'cfg(...)'.build]`), and BEFORE the manifest is
// snapshotted into packages[] / fingerprinted, because that snapshot (not the
// manifest) is what the P1689 scan, the compile edges and compute_fingerprint
// actually read. `makePackageRoot` refuses a manifest whose `defines` are
// still unfolded.
//
// `defines` IS A SET KEYED BY MACRO NAME (SPEC-004 §8). A later entry for a
// name replaces the earlier one in place, so a member that restates an
// inherited `NAME=value` produces one `-DNAME=value` word instead of two
// words and a redefinition diagnostic; an entry `!NAME` removes the name.
// A list is not enough for this, because the compiler resolves a repeated
// `-D` by warning (an error under `-Werror`) and a `-U` written in `cxxflags`
// precedes every folded `-D` and so cannot remove one.
//
// The key covers every `-D<NAME>` word already in the flag lists as well:
// those written in `cflags`/`cxxflags` of the same tables, and those folded
// by an earlier call (the layer-conditional pass calls this again with only
// its own entries). A name this call defines or removes supersedes them, so
// the package's compile lines carry at most one definition per name.
//
// Idempotent: clearing the vector after folding makes repeated calls harmless.
// Both `cflags` and `cxxflags` get the macro; assembly units pick it up for
// free via the -D/-U/-I subset the ninja backend filters out of packageCflags.
// A define is a value, so it enters the flag list as one word
// (`flag_element`): `N="x"` reaches the compiler as `-DN="x"` on every host.
export void fold_build_defines_into_flags(mcpp::manifest::BuildConfig& bc) {
    if (bc.defines.empty()) return;

    std::vector<std::string> resolved;          // entries, first-seen order
    std::vector<std::string> named;             // every name this call touches
    auto touch = [&](std::string_view name) {
        if (std::ranges::find(named, name) == named.end())
            named.emplace_back(name);
    };
    for (auto const& d : bc.defines) {
        if (d.starts_with('!')) {
            const auto name = std::string_view(d).substr(1);
            std::erase_if(resolved, [&](const std::string& e) {
                return define_name(e) == name;
            });
            touch(name);
            continue;
        }
        const auto name = define_name(d);
        touch(name);
        auto it = std::ranges::find_if(resolved, [&](const std::string& e) {
            return define_name(e) == name;
        });
        if (it != resolved.end()) *it = d;
        else resolved.push_back(d);
    }

    auto superseded = [&](const std::string& element) {
        auto words = mcpp::manifest::flag_words(element);
        if (words.size() != 1 || !words.front().starts_with("-D")) return false;
        const auto name = define_name(std::string_view(words.front()).substr(2));
        return std::ranges::find(named, name) != named.end();
    };
    std::erase_if(bc.cflags, superseded);
    std::erase_if(bc.cxxflags, superseded);

    for (auto const& d : resolved) {
        const auto element = mcpp::manifest::flag_element("-D" + d);
        bc.cflags.push_back(element);
        bc.cxxflags.push_back(element);
    }
    bc.defines.clear();
}

// The post-condition of the normalisation pipeline, as the snapshot checks it:
// every `defines` entry has been folded into the flag lists. A non-empty list
// here means a merge ran after the fold, and the entries would otherwise be
// dropped without a diagnostic (#690). Returns the internal-error text, or
// nothing when the manifest may be captured.
export std::optional<std::string>
unfolded_defines_error(const mcpp::manifest::Manifest& m) {
    auto const& d = m.buildConfig.defines;
    if (d.empty()) return std::nullopt;
    return std::format(
        "internal error: [build].defines of package '{}' reached the build "
        "graph unfolded ({} entr{}, first '{}'); a merge ran after "
        "fold_build_defines_into_flags (please report)",
        m.package.name.empty() ? std::string("(root)") : m.package.name,
        d.size(), d.size() == 1 ? "y" : "ies", d.front());
}

} // namespace mcpp::build

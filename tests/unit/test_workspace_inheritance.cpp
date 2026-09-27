#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.project;
import mcpp.build.prepare;
import mcpp.build.prepare.target_env;   // host_tool_declared_toolchain (#710)

// #690. The unit-level halves of the workspace-inheritance repair: the
// `[workspace.build]` key table (one statement of the inheritable subset), the
// keyed `defines` fold, and the snapshot's post-condition. The end-to-end
// halves (both graph positions, git-hosted members) are
// tests/e2e/321_workspace_inheritance.sh and
// tests/e2e/741_workspace_member_as_dependency.sh.

namespace {

// A value for every row of `kWorkspaceBuildKeys`. The table and this map must
// have the same keys: a row added without a value here fails
// `EveryTableRowIsParsedAndInherited` by name.
const std::map<std::string, std::string> kSampleScalar = {
    {"c_standard", "c17"},
    {"linkage", "static"},
    {"target", "x86_64-linux-musl"},
    {"cxx_runtime", "self-contained"},
    {"dependency_linkage", "static"},
    {"macos_deployment_target", "13.0"},
    {"ios_deployment_target", "15.0"},
};

std::string workspace_declaring_every_key() {
    std::string src =
        "[workspace]\nmembers = [\"m\"]\n\n[workspace.package]\nversion = \"0.1.0\"\n\n"
        "[workspace.build]\n";
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
        if (std::holds_alternative<std::string mcpp::manifest::BuildConfig::*>(row.field)) {
            auto it = kSampleScalar.find(std::string(row.key));
            src += std::format("{} = \"{}\"\n", row.key,
                               it == kSampleScalar.end() ? "missing-sample" : it->second);
        } else {
            src += std::format("{} = [\"ws_{}\"]\n", row.key, row.key);
        }
    }
    return src;
}

std::vector<std::string> as_strings(const std::vector<std::filesystem::path>& v) {
    std::vector<std::string> out;
    for (auto const& p : v) out.push_back(p.generic_string());
    return out;
}

} // namespace

// F3: a key the parser assigns and inherits must also be a key it accepts.
// `ios_deployment_target` was the counterexample.
TEST(WorkspaceInheritance, EveryTableRowIsParsedAndInherited) {
    auto ws = mcpp::manifest::parse_string(workspace_declaring_every_key());
    ASSERT_TRUE(ws.has_value()) << ws.error().format();
    ASSERT_TRUE(ws->workspace.inherited.buildPresent);

    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\n", "m/mcpp.toml", {.insideWorkspace = true});
    ASSERT_TRUE(member.has_value()) << member.error().format();
    const std::filesystem::path wsRoot = "/ws";
    mcpp::project::inherit_workspace_build(*member, *ws, wsRoot);

    std::size_t scalars = 0;
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
        SCOPED_TRACE(std::string(row.key));
        auto const& parsed = ws->workspace.inherited.build;
        auto const& inherited = member->buildConfig;
        if (auto const* f = std::get_if<std::vector<std::string>
                mcpp::manifest::BuildConfig::*>(&row.field)) {
            const std::vector<std::string> want{std::format("ws_{}", row.key)};
            EXPECT_EQ(parsed.**f, want);
            EXPECT_EQ(inherited.**f, want);
        } else if (auto const* f = std::get_if<std::vector<std::filesystem::path>
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            EXPECT_EQ(as_strings(parsed.**f),
                      std::vector<std::string>{std::format("ws_{}", row.key)});
            // Relative include directories are anchored at the workspace root.
            EXPECT_EQ(as_strings(inherited.**f),
                      std::vector<std::string>{
                          (wsRoot / std::format("ws_{}", row.key)).generic_string()});
        } else if (auto const* f = std::get_if<std::string
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            ++scalars;
            auto it = kSampleScalar.find(std::string(row.key));
            ASSERT_NE(it, kSampleScalar.end()) << "no sample value for a new scalar row";
            EXPECT_EQ(parsed.**f, it->second);
            EXPECT_EQ(inherited.**f, it->second);
        }
    }
    EXPECT_EQ(scalars, kSampleScalar.size());
}

TEST(WorkspaceInheritance, UnknownKeyErrorListsTheTable) {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n[workspace.build]\nno_such_key = []\n");
    ASSERT_FALSE(ws.has_value());
    const auto text = ws.error().format();
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys)
        EXPECT_NE(text.find(row.key), std::string::npos) << row.key;
}

// W7: `defines` is a set keyed by macro name.
TEST(DefinesFold, LaterEntryReplacesEarlierInPlace) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "Y", "X=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DX=2", "-DY"}));
    EXPECT_EQ(bc.cflags, (std::vector<std::string>{"-DX=2", "-DY"}));
    EXPECT_TRUE(bc.defines.empty());
}

TEST(DefinesFold, BangNameRemovesTheName) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "!X", "Z"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DZ"}));
}

TEST(DefinesFold, EntrySupersedesADashDWordInTheFlagLists) {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-DX=1", "-O2", "-DXY=1"};
    bc.cflags = {"-DX"};
    bc.defines = {"X=3"};
    mcpp::build::fold_build_defines_into_flags(bc);
    // `-DXY` is another name and stays; `-DX=1` and `-DX` are superseded.
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-O2", "-DXY=1", "-DX=3"}));
    EXPECT_EQ(bc.cflags, (std::vector<std::string>{"-DX=3"}));
}

// The layer-conditional pass folds a second time with only its own entries.
TEST(DefinesFold, SecondPassRemovesAWordTheFirstPassFolded) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "W"};
    mcpp::build::fold_build_defines_into_flags(bc);
    bc.defines = {"!X"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DW"}));
}

TEST(DefinesFold, ValueWithSpaceStaysOneWordAndKeyed) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"N=\"a b\""};
    mcpp::build::fold_build_defines_into_flags(bc);
    ASSERT_EQ(bc.cxxflags.size(), 1u);
    bc.defines = {"N=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DN=2"}));
}

// W1: the snapshot refuses a manifest whose `defines` were not folded.
TEST(SnapshotPostcondition, UnfoldedDefinesAreAnInternalError) {
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    EXPECT_FALSE(mcpp::build::unfolded_defines_error(m).has_value());
    m.buildConfig.defines = {"WORKSPACE_DEFINE=1"};
    auto err = mcpp::build::unfolded_defines_error(m);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("internal error"), std::string::npos);
    EXPECT_NE(err->find("'lib'"), std::string::npos);
    EXPECT_NE(err->find("WORKSPACE_DEFINE=1"), std::string::npos);
    mcpp::build::fold_build_defines_into_flags(m.buildConfig);
    EXPECT_FALSE(mcpp::build::unfolded_defines_error(m).has_value());
}

// #713. A member inherits the workspace root's `[xlings.workspace]` entries,
// conditional rows included; a package the member declares itself keeps the
// member's address, because the nearer declaration wins (SPEC-004 §4.5).
TEST(WorkspaceXlings, AMemberInheritsTheRootsEntriesAndItsOwnWins) {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n"
        "[xlings.workspace]\nninja = \"1.12.1\"\ncmake = \"3.30.0\"\n\n"
        "[target.'cfg(os = \"linux\")'.xlings.workspace]\npatchelf = \"0.18.0\"\n");
    ASSERT_TRUE(ws.has_value()) << ws.error().format();
    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[xlings.workspace]\ncmake = \"3.31.0\"\n",
        "m/mcpp.toml", {.insideWorkspace = true});
    ASSERT_TRUE(member.has_value()) << member.error().format();

    mcpp::project::inherit_workspace_xlings(*member, *ws);

    auto has = [&](std::string_view needle) {
        return std::ranges::any_of(member->xlings.deps, [&](const std::string& a) {
            return a.find(needle) != std::string::npos;
        });
    };
    EXPECT_TRUE(has("ninja@1.12.1"));
    EXPECT_TRUE(has("cmake@3.31.0"));
    EXPECT_FALSE(has("cmake@3.30.0"));
    // The conditional row travels as a row, decided by its selector at merge time.
    bool rowCarried = false;
    for (auto const& cc : member->conditionalConfigs)
        for (auto const& a : cc.xlings.deps)
            rowCarried = rowCarried || a.find("patchelf@0.18.0") != std::string::npos;
    EXPECT_TRUE(rowCarried);
}

// #714. An entry that says `workspace = true` and that no workspace resolved is
// refused by name, in every dependency table.
TEST(WorkspaceDependency, AnUnresolvedWorkspaceEntryIsNamed) {
    for (std::string_view table : {"dependencies", "dev-dependencies", "build-dependencies"}) {
        SCOPED_TRACE(std::string(table));
        auto m = mcpp::manifest::parse_string(std::format(
            "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[{}]\nfmt = {{ workspace = true }}\n",
            table), "m/mcpp.toml", {.insideWorkspace = true});
        ASSERT_TRUE(m.has_value()) << m.error().format();
        auto err = mcpp::project::unresolved_workspace_dependency_error(*m, "/p/m");
        ASSERT_TRUE(err.has_value());
        EXPECT_NE(err->find(std::format("[{}] fmt", table)), std::string::npos) << *err;
        EXPECT_NE(err->find("members"), std::string::npos) << *err;
    }
    auto resolved = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[dependencies]\nfmt = \"11.0.0\"\n");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_FALSE(mcpp::project::unresolved_workspace_dependency_error(*resolved, "/p/m"));
}

// #710. A host tool's toolchain is the one its own build would use: its host
// row, then `[toolchain]`, each after the root-position keys of the workspace
// that lists it. A member tool that declares nothing takes the workspace's.
TEST(HostToolToolchain, AMemberToolReadsItsWorkspaceToolchain) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("mcpp-710-{:x}", std::random_device{}());
    fs::create_directories(root / "tool");
    auto write = [](const fs::path& p, std::string_view text) {
        std::ofstream(p) << text;
    };
    write(root / "mcpp.toml",
          "[workspace]\nmembers = [\"tool\"]\n\n[toolchain]\ndefault = \"gcc@15.1.0\"\n");
    write(root / "tool" / "mcpp.toml",
          "[package]\nname = \"tool\"\nversion = \"0.1.0\"\n");
    auto tool = mcpp::manifest::load(root / "tool" / "mcpp.toml");
    ASSERT_TRUE(tool.has_value());
    EXPECT_EQ(mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux"),
              std::optional<std::string>("gcc@15.1.0"));

    // The tool's own declaration wins over the workspace's.
    write(root / "tool" / "mcpp.toml",
          "[package]\nname = \"tool\"\nversion = \"0.1.0\"\n\n"
          "[toolchain]\ndefault = \"gcc@16.1.0\"\n");
    tool = mcpp::manifest::load(root / "tool" / "mcpp.toml");
    ASSERT_TRUE(tool.has_value());
    EXPECT_EQ(mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux"),
              std::optional<std::string>("gcc@16.1.0"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

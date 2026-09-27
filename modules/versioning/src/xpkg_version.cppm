// mcpp.xpkg_version — the version grammar of xpkg package keys, as xlings
// resolves them.
//
// WHY A SECOND GRAMMAR. `mcpp.version_req` is the grammar of mcpp's own
// dependencies (Cargo's: a bare "1.2.3" means ^1.2.3). An `[xlings]` address is
// not read by mcpp at all: it goes to xlings, and xlings resolves it with ITS
// grammar. Where mcpp has to answer "which installed payload did that address
// select" -- `mcpp::xpkg_dir`, the run PATH, the sysroot fallback -- it must
// give xlings' answer, and the two grammars disagree on the case that matters
// most (mcpp#712): the bare two-segment `1.7` is a caret range to Cargo and a
// prefix range [1.7, 1.8) to xlings, and a four-segment key such as `1.7.0.1`
// is outside Cargo's grammar entirely, so it was never a candidate.
//
// THIS IS A FALLBACK, NOT THE AUTHORITY. xlings reports what it resolved each
// request to (the `install_targets` interface event, protocol 1.1), and mcpp
// records that; this grammar answers only when no record exists (an older
// xlings, or a payload installed by hand). It cannot see one input xlings uses:
// a version already active in the subos that satisfies the request is chosen
// over a higher one. With several installed versions and no record, the
// answer here is the highest match, which is what a fresh xlings install
// selects.
//
// SOURCE: xlings src/core/semver.cppm (generalized grammar, 2026.8.9.2 and
// later), ported rule for rule. The conformance vectors xlings publishes with
// its resolver are replayed against this module in tests/, so a change on
// either side that the other does not make fails a unit test instead of
// resolving a different payload.
//
// Grammar:
//   version  = field ('.' field)* ('-' prerelease)?   ('-' needs a digit before it)
//   field    = [0-9A-Za-z]+, split at digit/alpha boundaries into segments
//   '+' and everything after it is build metadata and is dropped.
//   A string with no numeric segment ("latest") is a name, not a version.
// Ordering: numeric segments numerically, alpha lexicographically, numeric
// above alpha, a missing segment is 0, a prerelease is below its release.
// Requests:
//   "1.2.3", "2.15.0.1"  written-prefix equality, at least three segments wide
//                        (1.2.3 matches 1.2.3 and 1.2.3.4, never 1.2.4)
//   "1", "1.7"           prefix range: [1, 2), [1.7, 1.8)
//   "1.0.0-rc1"          exact
//   ">=a", ">a", "<=a", "<a", space-separated conjunctions
//   "^1.2.3", "~1.2.3", "1.2.*", "1.*"

export module mcpp.xpkg_version;

import std;

export namespace mcpp::xpkg_version {

struct Segment {
    bool               isNum = true;
    unsigned long long num   = 0;
    std::string        text;           // alpha segments only
};

struct Version {
    std::vector<Segment> segs;
    int                  components = 0;   // dot-fields as written
    std::string          prerelease;       // "" = a release
};

enum class Op { Eq, Gt, Gte, Lt, Lte };

struct Constraint {
    Op      op;
    Version ver;
};

// All constraints must hold.
struct Range {
    std::vector<Constraint> constraints;
};

std::optional<Version> parse(std::string_view s);
std::strong_ordering   compare(const Version& a, const Version& b);
std::optional<Range>   parse_range(std::string_view expr);
bool                   satisfies(const Version& v, const Range& r);

// Order two keys: a parseable key outranks an unparseable one, two
// unparseable keys order lexicographically. Returns <0, 0, >0.
int compare_keys(std::string_view a, std::string_view b);

// The highest key in `available` that `request` selects, or nullopt.
// "latest" and unparseable keys are never selected.
std::optional<std::string>
select_best(std::span<const std::string> available, std::string_view request);

// The installed version an address resolves to, by the rule xlings selects
// with: a key equal to `request` first (a version whose name does not parse,
// `8.0.RC1`, is addressable only that way), then `select_best`; an empty
// request takes the highest key by `compare_keys`. nullopt when none is
// selected. The conformance vectors xlings publishes are stated against this
// function (tests/data/semver-vectors.tsv).
std::optional<std::string>
select_installed(std::span<const std::string> installed, std::string_view request);

} // namespace mcpp::xpkg_version

namespace mcpp::xpkg_version {

namespace {

Segment number(unsigned long long v) { return Segment{true, v, {}}; }

std::strong_ordering compare_segment(const Segment& a, const Segment& b) {
    if (a.isNum && b.isNum) return a.num <=> b.num;
    if (a.isNum != b.isNum)
        return a.isNum ? std::strong_ordering::greater : std::strong_ordering::less;
    return a.text <=> b.text;
}

unsigned long long num_at(const Version& v, std::size_t i) {
    return i < v.segs.size() && v.segs[i].isNum ? v.segs[i].num : 0;
}

Version num_version(std::initializer_list<unsigned long long> nums) {
    Version v;
    for (auto n : nums) v.segs.push_back(number(n));
    v.components = static_cast<int>(v.segs.size());
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// Equality is written-prefix, floored at three segments: "15.1.0" matches
// 15.1.0 and 15.1.0.5 but not 15.1.1, and "1.2" as an Eq token matches 1.2 and
// 1.2.0 but not 1.2.5. The ordering operators compare the whole version.
bool check(const Version& v, const Constraint& c) {
    if (c.op == Op::Eq) {
        const auto width = std::max<std::size_t>(c.ver.segs.size(), 3);
        static const Segment zero = number(0);
        for (std::size_t i = 0; i < width; ++i) {
            const auto& l = i < v.segs.size() ? v.segs[i] : zero;
            const auto& r = i < c.ver.segs.size() ? c.ver.segs[i] : zero;
            if (compare_segment(l, r) != 0) return false;
        }
        return v.prerelease == c.ver.prerelease;
    }
    const auto cmp = compare(v, c.ver);
    switch (c.op) {
        case Op::Gt:  return cmp > 0;
        case Op::Gte: return cmp >= 0;
        case Op::Lt:  return cmp < 0;
        case Op::Lte: return cmp <= 0;
        case Op::Eq:  break;
    }
    return false;
}

// [lo, next of the first or second segment)
Range prefix_range(const Version& lo, bool firstSegment) {
    Range r;
    auto hi = firstSegment ? num_version({num_at(lo, 0) + 1})
                           : num_version({num_at(lo, 0), num_at(lo, 1) + 1});
    r.constraints.push_back({Op::Gte, lo});
    r.constraints.push_back({Op::Lt, std::move(hi)});
    return r;
}

std::optional<Constraint> parse_token(std::string_view tok) {
    tok = trim(tok);
    if (tok.empty()) return std::nullopt;
    Op op = Op::Eq;
    if      (tok.starts_with(">=")) { op = Op::Gte; tok.remove_prefix(2); }
    else if (tok.starts_with(">"))  { op = Op::Gt;  tok.remove_prefix(1); }
    else if (tok.starts_with("<=")) { op = Op::Lte; tok.remove_prefix(2); }
    else if (tok.starts_with("<"))  { op = Op::Lt;  tok.remove_prefix(1); }
    auto v = parse(trim(tok));
    if (!v) return std::nullopt;
    return Constraint{op, std::move(*v)};
}

} // namespace

std::optional<Version> parse(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    if (auto plus = s.find('+'); plus != std::string_view::npos) {
        s = s.substr(0, plus);
        if (s.empty()) return std::nullopt;
    }
    std::string_view numpart = s, prepart;
    if (auto dash = s.find('-'); dash != std::string_view::npos) {
        bool digitBefore = false;
        for (std::size_t i = 0; i < dash; ++i)
            if (s[i] >= '0' && s[i] <= '9') { digitBefore = true; break; }
        if (digitBefore) { numpart = s.substr(0, dash); prepart = s.substr(dash + 1); }
    }
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto is_alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };

    Version v;
    bool sawNumeric = false;
    std::size_t start = 0;
    while (start <= numpart.size()) {
        const auto dot = numpart.find('.', start);
        const auto field = numpart.substr(
            start, dot == std::string_view::npos ? numpart.size() - start : dot - start);
        if (field.empty()) return std::nullopt;
        ++v.components;
        std::size_t i = 0;
        while (i < field.size()) {
            std::size_t j = i;
            if (is_digit(field[i])) {
                while (j < field.size() && is_digit(field[j])) ++j;
                if (j - i > 19) return std::nullopt;
                unsigned long long n = 0;
                for (auto c : field.substr(i, j - i)) n = n * 10 + static_cast<unsigned>(c - '0');
                v.segs.push_back(number(n));
                sawNumeric = true;
            } else if (is_alpha(field[i])) {
                while (j < field.size() && is_alpha(field[j])) ++j;
                v.segs.push_back(Segment{false, 0, std::string(field.substr(i, j - i))});
            } else {
                return std::nullopt;
            }
            i = j;
        }
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (v.segs.empty() || !sawNumeric) return std::nullopt;
    v.prerelease = std::string(prepart);
    return v;
}

std::strong_ordering compare(const Version& a, const Version& b) {
    static const Segment zero = number(0);
    const auto n = std::max(a.segs.size(), b.segs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& l = i < a.segs.size() ? a.segs[i] : zero;
        const auto& r = i < b.segs.size() ? b.segs[i] : zero;
        if (auto c = compare_segment(l, r); c != 0) return c;
    }
    if (!a.prerelease.empty() && !b.prerelease.empty()) return a.prerelease <=> b.prerelease;
    if (a.prerelease.empty() != b.prerelease.empty())
        return a.prerelease.empty() ? std::strong_ordering::greater
                                    : std::strong_ordering::less;
    return std::strong_ordering::equal;
}

bool satisfies(const Version& v, const Range& r) {
    return std::ranges::all_of(r.constraints,
        [&](const Constraint& c) { return check(v, c); });
}

std::optional<Range> parse_range(std::string_view expr) {
    expr = trim(expr);
    if (expr.empty()) return std::nullopt;

    if (expr.starts_with("^")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        std::size_t k = 0;
        while (k + 1 < v->segs.size() && v->segs[k].isNum && v->segs[k].num == 0) ++k;
        Version hi;
        for (std::size_t i = 0; i < k; ++i) hi.segs.push_back(number(num_at(*v, i)));
        hi.segs.push_back(number(num_at(*v, k) + 1));
        hi.components = static_cast<int>(hi.segs.size());
        Range r;
        r.constraints.push_back({Op::Gte, *v});
        r.constraints.push_back({Op::Lt, std::move(hi)});
        return r;
    }
    if (expr.starts_with("~")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        return prefix_range(*v, /*firstSegment=*/false);
    }
    if (auto star = expr.find('*'); star != std::string_view::npos) {
        auto prefix = expr.substr(0, star);
        while (!prefix.empty() && prefix.back() == '.') prefix.remove_suffix(1);
        if (prefix.empty()) return std::nullopt;
        auto v = parse(prefix);
        if (!v) return std::nullopt;
        return prefix_range(*v, /*firstSegment=*/v->components == 1);
    }
    if (expr.starts_with(">") || expr.starts_with("<")) {
        Range r;
        std::size_t pos = 0;
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            if (pos >= expr.size()) break;
            const auto start = pos;
            while (pos < expr.size() && (expr[pos] == '>' || expr[pos] == '<' || expr[pos] == '=')) ++pos;
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            while (pos < expr.size() && expr[pos] != ' ') ++pos;
            auto c = parse_token(expr.substr(start, pos - start));
            if (!c) return std::nullopt;
            r.constraints.push_back(std::move(*c));
        }
        if (r.constraints.empty()) return std::nullopt;
        return r;
    }
    auto v = parse(expr);
    if (!v) return std::nullopt;
    if (v->components >= 3 || !v->prerelease.empty()) {
        Range r;
        r.constraints.push_back({Op::Eq, *v});
        return r;
    }
    return prefix_range(*v, /*firstSegment=*/v->components == 1);
}

int compare_keys(std::string_view a, std::string_view b) {
    auto va = parse(a), vb = parse(b);
    if (va && vb) {
        const auto c = compare(*va, *vb);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (va && !vb) return 1;
    if (!va && vb) return -1;
    return a < b ? -1 : (a > b ? 1 : 0);
}

std::optional<std::string>
select_best(std::span<const std::string> available, std::string_view request) {
    auto range = parse_range(request);
    if (!range) return std::nullopt;
    std::optional<std::string> best;
    for (auto const& key : available) {
        if (key == "latest") continue;
        auto v = parse(key);
        if (!v || !satisfies(*v, *range)) continue;
        if (!best || compare_keys(key, *best) > 0) best = key;
    }
    return best;
}

std::optional<std::string>
select_installed(std::span<const std::string> installed, std::string_view request) {
    if (!request.empty()) {
        if (std::ranges::find(installed, request) != installed.end())
            return std::string(request);
        return select_best(installed, request);
    }
    std::optional<std::string> best;
    for (auto const& key : installed)
        if (!best || compare_keys(key, *best) > 0) best = key;
    return best;
}

} // namespace mcpp::xpkg_version

#pragma once
// Funscript parsing and sampling (docs/concepts/HAPTICS_FUNSCRIPT_RESEARCH.md §1):
// {"actions":[{"at":ms,"pos":0..100}], "inverted":bool, "range":0..100}.
// Between points the position moves in a straight line.

#include "../../browser/mini_json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace funscript {

struct Action {
    double at = 0.0;   // ms from the start of the media
    double pos = 0.0;  // 0 bottom .. 100 top
};

struct Script {
    std::vector<Action> actions;  // sorted by `at`, unique times
    std::string error;
    bool ok() const { return actions.size() >= 2; }
    double endMs() const { return actions.empty() ? 0.0 : actions.back().at; }
};

inline Script parse(const std::string& text) {
    Script s;
    pvrjson::Value root;
    if (!pvrjson::parse(text, root) || root.type != pvrjson::Value::Type::Object) {
        s.error = "not a valid funscript (JSON)";
        return s;
    }
    const pvrjson::Value* acts = root.get("actions");
    if (!acts || acts->type != pvrjson::Value::Type::Array) {
        s.error = "funscript has no actions";
        return s;
    }
    const pvrjson::Value* inv = root.get("inverted");
    const bool inverted = inv && inv->type == pvrjson::Value::Type::Bool && inv->boolean;
    for (const auto& a : acts->array) {
        const pvrjson::Value* at = a.get("at");
        const pvrjson::Value* pos = a.get("pos");
        if (!at || !pos || at->type != pvrjson::Value::Type::Number || pos->type != pvrjson::Value::Type::Number) continue;
        // Finite, not negative, within ~4 months (1e10 ms): "1e999" parses as infinity and would turn
        // positions into NaN on the device.
        if (!std::isfinite(at->number) || !std::isfinite(pos->number) || at->number < 0.0 || at->number > 1e10) continue;
        if (s.actions.size() >= 4000000) break;  // far beyond any real script; bounds the memory
        double p = std::clamp(pos->number, 0.0, 100.0);
        if (inverted) p = 100.0 - p;
        s.actions.push_back({at->number, p});
    }
    std::stable_sort(s.actions.begin(), s.actions.end(), [](const Action& x, const Action& y) { return x.at < y.at; });
    // Two points at the same time: keep the last one.
    std::vector<Action> unique;
    for (const Action& a : s.actions) {
        if (!unique.empty() && unique.back().at == a.at) unique.back() = a;
        else unique.push_back(a);
    }
    s.actions.swap(unique);
    if (!s.ok()) s.error = "funscript has fewer than two actions";
    return s;
}

// First action strictly after `ms` (actions.size() if none).
inline size_t nextIndex(const Script& s, double ms) {
    return static_cast<size_t>(std::upper_bound(s.actions.begin(), s.actions.end(), ms,
                                                [](double t, const Action& a) { return t < a.at; }) -
                               s.actions.begin());
}

// Position 0..100 at `ms` (held at the first / last point outside the script).
inline double positionAt(const Script& s, double ms) {
    if (s.actions.empty()) return 0.0;
    const size_t k = nextIndex(s, ms);
    if (k == 0) return s.actions.front().pos;
    if (k >= s.actions.size()) return s.actions.back().pos;
    const Action& a = s.actions[k - 1];
    const Action& b = s.actions[k];
    const double f = (ms - a.at) / std::max(1.0, b.at - a.at);
    return a.pos + (b.pos - a.pos) * std::clamp(f, 0.0, 1.0);
}

// Average movement speed (position units per second) over [fromMs, toMs].
inline double speedBetween(const Script& s, double fromMs, double toMs) {
    if (toMs <= fromMs || s.actions.size() < 2) return 0.0;
    double travel = 0.0;
    double prevT = fromMs;
    double prevP = positionAt(s, fromMs);
    for (size_t k = nextIndex(s, fromMs); k < s.actions.size() && s.actions[k].at < toMs; ++k) {
        travel += std::fabs(s.actions[k].pos - prevP);
        prevT = s.actions[k].at;
        prevP = s.actions[k].pos;
    }
    travel += std::fabs(positionAt(s, toMs) - prevP);
    (void)prevT;
    return travel / ((toMs - fromMs) / 1000.0);
}

// Heat over windows of `windowMs`: [startMs, endMs, heat 0..1], speed 400/s = 1.
inline std::vector<std::array<double, 3>> heat(const Script& s, double durationMs, double windowMs = 2000.0) {
    std::vector<std::array<double, 3>> out;
    const double end = std::max(durationMs, s.endMs());
    if (!s.ok() || end <= 0.0) return out;
    windowMs = std::max(windowMs, end / 1500.0);  // the host takes at most 2000 segments
    for (double t = 0.0; t < end; t += windowMs) {
        const double h = std::clamp(speedBetween(s, t, std::min(end, t + windowMs)) / 400.0, 0.0, 1.0);
        if (!out.empty() && std::fabs(out.back()[2] - h) < 0.03) out.back()[1] = std::min(end, t + windowMs);
        else out.push_back({t, std::min(end, t + windowMs), h});
    }
    return out;
}

}  // namespace funscript
